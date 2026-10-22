/* light_probe: on-Pi bring-up check for the BH1750 light sensor.
 *
 * Purpose: the unit tests prove the driver against a fake bus; this program
 * proves the real hardware path, which no test on a dev machine can reach.
 * It runs the same driver code the light_sensor process will use and prints
 * a PASS/FAIL checklist, exiting nonzero if any check fails:
 *   1. the I2C bus device opens        (I2C enabled, permissions OK)
 *   2. a BH1750 ACKs at 0x23           (wiring, ADDR pin, power)
 *   3. one measurement reads           (basic C read, raw count + lux)
 *   4. N samples on a fixed period     (sampling interval and jitter)
 *
 * Linux model: /dev/i2c-1 exists only once I2C is enabled (raspi-config ->
 * Interface Options -> I2C), and opening it needs root or the `i2c` group.
 * Opening the bus and selecting an address succeed even with nothing wired,
 * so check 2 is the first point that proves a device is really there.
 *
 * Usage: light_probe [-d /dev/i2c-1] [-n samples] [-p period_ms]
 * Wiring (Pi header): VCC -> 3.3V pin 1, GND -> pin 6, SDA -> GPIO2 pin 3,
 * SCL -> GPIO3 pin 5, ADDR -> GND or unconnected (0x23; 3.3V gives 0x5C). */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "i2c_bus.h"
#include "light_sensor.h"

typedef struct {
    const char *dev;
    int         samples;
    long        period_ms;
} probe_opts;

/* Print one checklist line, e.g. "[2/4] BH1750 answers at 0x23 ... PASS". */
static void report(int step, const char *what, int ok)
{
    printf("[%d/4] %-34s %s\n", step, what, ok ? "PASS" : "FAIL");
}

/* Parse -d/-n/-p into `o`. Returns 0, or -1 after printing usage. */
static int parse_opts(int argc, char **argv, probe_opts *o)
{
    o->dev = "/dev/i2c-1";
    o->samples = 60;
    o->period_ms = 1000;

    int c;
    while ((c = getopt(argc, argv, "d:n:p:")) != -1) {
        switch (c) {
        case 'd': o->dev = optarg; break;
        case 'n': o->samples = atoi(optarg); break;
        case 'p': o->period_ms = atol(optarg); break;
        default:  goto usage;
        }
    }
    /* The period must outlast one high-res conversion, or reads would return
     * the same measurement twice. */
    if (o->samples < 2 || o->period_ms < GT_BH1750_MEAS_MAX_MS) {
        goto usage;
    }
    return 0;

usage:
    fprintf(stderr, "usage: %s [-d /dev/i2c-1] [-n samples>=2] [-p period_ms>=%d]\n",
            argv[0], GT_BH1750_MEAS_MAX_MS);
    return -1;
}

/* Check 1: open the bus and bind GT_LIGHT_I2C_ADDR. On failure, explain the
 * likely cause from errno (open() is the call that fails here). */
static int check_bus(const char *dev, gt_i2c_bus *bus)
{
    errno = 0;
    int ok = gt_i2c_linux_open(dev, GT_LIGHT_I2C_ADDR, bus) == GT_OK;
    report(1, "I2C bus opens", ok);
    if (!ok) {
        int e = errno;
        fprintf(stderr, "      %s: %s\n", dev, e ? strerror(e) : "open failed");
        if (e == ENOENT) {
            fprintf(stderr, "      hint: enable I2C (sudo raspi-config -> Interface Options -> I2C), reboot\n");
        } else if (e == EACCES) {
            fprintf(stderr, "      hint: sudo usermod -aG i2c $USER, then log out and back in\n");
        }
    }
    return ok;
}

/* Check 2: initialise the sensor. The driver's POWER_ON write is the first
 * real bus transaction, so a NULL here means nothing ACKed at 0x23. The
 * driver owns `bus` from here on and closes it on failure. */
static gt_light_sensor *check_device(gt_i2c_bus bus)
{
    gt_light_sensor *s = gt_light_open_bus(bus);
    report(2, "BH1750 answers at 0x23", s != NULL);
    if (s == NULL) {
        fprintf(stderr, "      hint: run `i2cdetect -y 1`; expect 23. Nothing shown -> check wiring/power;\n"
                        "            5c shown -> ADDR pin is high, tie it to GND\n");
    }
    return s;
}

/* Check 3: one measurement through the driver, printed as raw and lux. */
static int check_read(gt_light_sensor *s)
{
    uint32_t raw = 0;
    int ok = gt_light_read(s, &raw) == GT_OK;
    report(3, "single read", ok);
    if (ok) {
        printf("      raw=%u  lux=%.1f\n", (unsigned)raw, gt_light_raw_to_lux(raw));
    } else {
        fprintf(stderr, "      read failed after %d retries\n", GT_LIGHT_RETRIES);
    }
    return ok;
}

/* Sampling jitter target from DESIGN.md ("Sampling jitter": p99 < 10 ms). */
#define JITTER_P99_LIMIT_US 10000

static int64_t mono_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

/* Sleep until absolute CLOCK_MONOTONIC time `us`. Absolute deadlines keep
 * the schedule from drifting: a late wake-up doesn't push later samples
 * back, as a relative sleep(period) after each read would. */
static void sleep_until_us(int64_t us)
{
    struct timespec t = { .tv_sec = us / 1000000, .tv_nsec = (long)(us % 1000000) * 1000 };
    /* clock_nanosleep returns the error number itself, not -1/errno. */
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL) == EINTR) {
    }
}

static int cmp_i64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

/* Nearest-rank percentile of a sorted array, e.g. pct = 99. */
static int64_t percentile(const int64_t *sorted, int n, int pct)
{
    int rank = (pct * n + 99) / 100;   /* ceil(pct/100 * n) */
    return sorted[rank > 0 ? rank - 1 : 0];
}

/* Check 4: take `n` samples, one per `period_ms`, on an absolute schedule.
 * Each sample is timestamped just before its I2C read, which is the point
 * DESIGN.md measures jitter at. Prints every sample so lux can be watched
 * live (cover the sensor, shine a light), then a summary. Passes if every
 * read succeeded and p99 deviation from the schedule is under 10 ms. */
static int check_sampling(gt_light_sensor *s, int n, long period_ms)
{
    int64_t *dev_us = malloc((size_t)n * sizeof *dev_us);
    if (dev_us == NULL) {
        report(4, "sampling interval", 0);
        return 0;
    }

    int64_t period_us = (int64_t)period_ms * 1000;
    int64_t start = mono_us() + period_us;
    int64_t prev = 0, ivl_min = INT64_MAX, ivl_max = 0, ivl_sum = 0;
    uint32_t raw_min = UINT32_MAX, raw_max = 0;
    double lux_sum = 0;
    int errors = 0, good = 0;

    printf("      sampling %d x %ld ms (cover the sensor / shine a light to see lux follow)\n",
           n, period_ms);
    printf("      %4s %10s %7s %9s %8s\n", "i", "t_ms", "raw", "lux", "dev_us");

    for (int i = 0; i < n; i++) {
        int64_t deadline = start + i * period_us;
        sleep_until_us(deadline);
        int64_t now = mono_us();
        dev_us[i] = now - deadline;   /* >= 0: wake-ups are never early */

        if (i > 0) {
            int64_t ivl = now - prev;
            ivl_min = ivl < ivl_min ? ivl : ivl_min;
            ivl_max = ivl > ivl_max ? ivl : ivl_max;
            ivl_sum += ivl;
        }
        prev = now;

        uint32_t raw;
        double t_ms = (double)(now - start) / 1000.0;
        if (gt_light_read(s, &raw) != GT_OK) {
            errors++;
            printf("      %4d %10.1f %7s %9s %8lld\n", i, t_ms, "ERR", "-", (long long)dev_us[i]);
            continue;
        }
        good++;
        raw_min = raw < raw_min ? raw : raw_min;
        raw_max = raw > raw_max ? raw : raw_max;
        lux_sum += gt_light_raw_to_lux(raw);
        printf("      %4d %10.1f %7u %9.1f %8lld\n", i, t_ms, (unsigned)raw,
               gt_light_raw_to_lux(raw), (long long)dev_us[i]);
    }

    qsort(dev_us, (size_t)n, sizeof *dev_us, cmp_i64);
    int64_t p50 = percentile(dev_us, n, 50);
    int64_t p99 = percentile(dev_us, n, 99);
    int64_t dmax = dev_us[n - 1];
    free(dev_us);

    int ok = errors == 0 && p99 < JITTER_P99_LIMIT_US;
    report(4, "sampling interval", ok);
    printf("      reads:    %d ok, %d failed\n", good, errors);
    if (good > 0) {
        printf("      lux:      min %.1f  mean %.1f  max %.1f\n",
               gt_light_raw_to_lux(raw_min), lux_sum / good, gt_light_raw_to_lux(raw_max));
    }
    printf("      interval: min %.3f  mean %.3f  max %.3f ms (target %ld)\n",
           ivl_min / 1000.0, ivl_sum / 1000.0 / (n - 1), ivl_max / 1000.0, period_ms);
    printf("      jitter:   p50 %lld  p99 %lld  max %lld us (p99 limit %d)\n",
           (long long)p50, (long long)p99, (long long)dmax, JITTER_P99_LIMIT_US);

    /* Not a FAIL: a dark closet really is constant. But a stuck 0 or 65535
     * often means a dead or saturated sensor, so make the user look. */
    if (good >= 2 && raw_min == raw_max) {
        printf("      warning: every sample read raw=%u; vary the light and rerun\n",
               (unsigned)raw_min);
    }
    return ok;
}

int main(int argc, char **argv)
{
    /* Line-buffer stdout so PASS/FAIL lines and stderr hints stay in order
     * even when the output is piped to a log (e.g. `| tee probe.log`). */
    setvbuf(stdout, NULL, _IOLBF, 0);

    probe_opts o;
    if (parse_opts(argc, argv, &o) != 0) {
        return 2;
    }

    gt_i2c_bus bus;
    if (!check_bus(o.dev, &bus)) {
        return 1;
    }
    gt_light_sensor *s = check_device(bus);
    if (s == NULL) {
        return 1;
    }
    int ok = check_read(s) && check_sampling(s, o.samples, o.period_ms);

    gt_light_close(s);
    return ok ? 0 : 1;
}
