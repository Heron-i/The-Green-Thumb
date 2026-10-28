/* soil_probe: on-Pi bring-up check for the STEMMA seesaw soil sensor.
 *
 * Purpose: the unit tests prove the driver against a fake bus; this program
 * proves the real hardware path, which no test on a dev machine can reach.
 * It runs the same driver code the soil_sensor process will use and prints
 * a PASS/FAIL checklist, exiting nonzero if any check fails:
 *   1. the I2C bus device opens              (I2C enabled, permissions OK)
 *   2. a seesaw answers at 0x36 with ID 0x55 (wiring, power, right chip)
 *   3. one moisture read                     (raw count)
 *   4. N samples on a fixed period           (sampling interval and jitter)
 *
 * Linux model: /dev/i2c-1 exists only once I2C is enabled (raspi-config ->
 * Interface Options -> I2C), and opening it needs root or the `i2c` group.
 * Opening the bus and selecting an address succeed even with nothing wired,
 * so check 2 is the first point that proves a device is really there.
 *
 * The real soil period is 60 s; the default here is 1 s so bring-up is
 * quick and you can watch the count move (grip the probe, dip it in water).
 *
 * Usage: soil_probe [-d /dev/i2c-1] [-n samples] [-p period_ms]
 * Wiring (Pi header, shares the bus with the BH1750 at 0x23): VIN -> 3.3V
 * pin 1, GND -> pin 6, SDA -> GPIO2 pin 3, SCL -> GPIO3 pin 5. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "i2c_bus.h"
#include "soil_sensor.h"

/* Shortest period accepted: comfortably above a worst-case read
 * (4 x 5 ms conversion + 3 x 10 ms retry gaps = 50 ms). */
#define SOIL_PROBE_MIN_PERIOD_MS 100

/* Sampling jitter target from DESIGN.md ("Sampling jitter": p99 < 10 ms). */
#define JITTER_P99_LIMIT_US 10000

typedef struct {
    const char *dev;
    int         samples;
    long        period_ms;
} probe_opts;

/* Print one checklist line, e.g. "[2/4] seesaw answers at 0x36 ... PASS". */
static void report(int step, const char *what, int ok)
{
    printf("[%d/4] %-34s %s\n", step, what, ok ? "PASS" : "FAIL");
}

/* Parse -d/-n/-p into `opts`. Returns 0, or -1 after printing usage. */
static int parse_opts(int argc, char **argv, probe_opts *opts)
{
    opts->dev = "/dev/i2c-1";
    opts->samples = 30;
    opts->period_ms = 1000;

    int option;
    while ((option = getopt(argc, argv, "d:n:p:")) != -1) {
        switch (option) {
        case 'd': opts->dev = optarg; break;
        case 'n': opts->samples = atoi(optarg); break;
        case 'p': opts->period_ms = atol(optarg); break;
        default:  goto usage;
        }
    }
    if (opts->samples < 2 || opts->period_ms < SOIL_PROBE_MIN_PERIOD_MS) {
        goto usage;
    }
    return 0;

usage:
    fprintf(stderr, "usage: %s [-d /dev/i2c-1] [-n samples>=2] [-p period_ms>=%d]\n",
            argv[0], SOIL_PROBE_MIN_PERIOD_MS);
    return -1;
}

/* Check 1: open the bus and bind GT_SOIL_I2C_ADDR. On failure, explain the
 * likely cause from errno (open() is the call that fails here). */
static int check_bus(const char *dev, gt_i2c_bus *bus)
{
    errno = 0;
    int ok = gt_i2c_linux_open(dev, GT_SOIL_I2C_ADDR, bus) == GT_OK;
    report(1, "I2C bus opens", ok);
    if (!ok) {
        int open_errno = errno;
        fprintf(stderr, "      %s: %s\n", dev, open_errno ? strerror(open_errno) : "open failed");
        if (open_errno == ENOENT) {
            fprintf(stderr, "      hint: enable I2C (sudo raspi-config -> Interface Options -> I2C), reboot\n");
        } else if (open_errno == EACCES) {
            fprintf(stderr, "      hint: sudo usermod -aG i2c $USER, then log out and back in\n");
        }
    }
    return ok;
}

/* Check 2: initialise the sensor. The driver's reset write is the first
 * real bus transaction, then it requires hardware ID 0x55. The driver owns
 * `bus` from here on and closes it on failure. */
static gt_soil_sensor *check_device(gt_i2c_bus bus)
{
    gt_soil_sensor *sensor = gt_soil_open_bus(bus);
    report(2, "seesaw answers at 0x36 (ID 0x55)", sensor != NULL);
    if (sensor == NULL) {
        fprintf(stderr, "      hint: run `i2cdetect -y 1`; expect 36. Nothing shown -> check wiring/power;\n"
                        "            36 shown -> it answered but the ID was not 0x55 (another chip?)\n");
    }
    return sensor;
}

/* Check 3: one moisture read through the driver, printed as a raw count. */
static int check_read(gt_soil_sensor *sensor)
{
    uint32_t raw = 0;
    int ok = gt_soil_read(sensor, &raw) == GT_OK;
    report(3, "single read", ok);
    if (ok) {
        printf("      raw=%u  (roughly 200 dry/air .. 2000 wet)\n", (unsigned)raw);
    } else {
        fprintf(stderr, "      read failed after %d retries (I/O error or 0xFFFF)\n",
                GT_SOIL_RETRIES);
    }
    return ok;
}

static int64_t mono_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

/* Sleep until absolute CLOCK_MONOTONIC time `us`. Absolute deadlines keep
 * the schedule from drifting: a late wake-up doesn't push later samples
 * back, as a relative sleep(period) after each read would. */
static void sleep_until_us(int64_t us)
{
    struct timespec deadline = { .tv_sec = us / 1000000, .tv_nsec = (long)(us % 1000000) * 1000 };
    /* clock_nanosleep returns the error number itself, not -1/errno. */
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL) == EINTR) {
    }
}

static int cmp_i64(const void *left, const void *right)
{
    int64_t left_value = *(const int64_t *)left, right_value = *(const int64_t *)right;
    return (left_value > right_value) - (left_value < right_value);
}

/* Nearest-rank percentile of a sorted array, e.g. pct = 99. */
static int64_t percentile(const int64_t *sorted, int count, int pct)
{
    int rank = (pct * count + 99) / 100;   /* ceil(pct/100 * count) */
    return sorted[rank > 0 ? rank - 1 : 0];
}

/* Running totals for check 4, updated once per sample. Intervals are the
 * time between one sample's wake-up and the next one's, in us. */
typedef struct {
    int64_t  prev_us;     /* wake-up time of the previous sample */
    int64_t  ivl_min, ivl_max, ivl_sum;
    uint32_t raw_min, raw_max;   /* over successful reads only */
    uint64_t raw_sum;
    int      errors, good;       /* failed / successful reads */
} sample_stats;

/* Start every total at zero, and each min at its type's largest value so
 * the first sample always replaces it. */
static void init_sample_stats(sample_stats *stats)
{
    memset(stats, 0, sizeof *stats);
    stats->ivl_min = INT64_MAX;
    stats->raw_min = UINT32_MAX;
}

/* Sleep until `deadline_us`, then take and print one sample, folding it
 * into `stats`. `start_us` is when sample 0 was due, used for the t_ms
 * column. Returns how late the wake-up was, in us. */
static int64_t read_scheduled_sample(gt_soil_sensor *sensor, int sample_index, int64_t start_us,
                                     int64_t deadline_us, sample_stats *stats)
{
    sleep_until_us(deadline_us);
    int64_t now_us = mono_us();
    int64_t dev_us = now_us - deadline_us;   /* >= 0: wake-ups are never early */

    if (sample_index > 0) {
        int64_t interval_us = now_us - stats->prev_us;
        stats->ivl_min = interval_us < stats->ivl_min ? interval_us : stats->ivl_min;
        stats->ivl_max = interval_us > stats->ivl_max ? interval_us : stats->ivl_max;
        stats->ivl_sum += interval_us;
    }
    stats->prev_us = now_us;

    uint32_t raw;
    double t_ms = (double)(now_us - start_us) / 1000.0;
    if (gt_soil_read(sensor, &raw) != GT_OK) {
        stats->errors++;
        printf("      %4d %10.1f %7s %8lld\n", sample_index, t_ms, "ERR", (long long)dev_us);
    } else {
        stats->good++;
        stats->raw_min = raw < stats->raw_min ? raw : stats->raw_min;
        stats->raw_max = raw > stats->raw_max ? raw : stats->raw_max;
        stats->raw_sum += raw;
        printf("      %4d %10.1f %7u %8lld\n", sample_index, t_ms, (unsigned)raw,
               (long long)dev_us);
    }
    return dev_us;
}

/* Print the reads / raw / interval / jitter summary under check 4's
 * PASS/FAIL line. `sorted_dev_us` is every sample's lateness, sorted
 * ascending. `const` means this only reads `stats` and the array. */
static void print_sampling_summary(const sample_stats *stats, const int64_t *sorted_dev_us,
                                   int sample_count, long period_ms)
{
    int64_t p50 = percentile(sorted_dev_us, sample_count, 50);
    int64_t p99 = percentile(sorted_dev_us, sample_count, 99);
    int64_t dev_max = sorted_dev_us[sample_count - 1];

    printf("      reads:    %d ok, %d failed\n", stats->good, stats->errors);
    if (stats->good > 0) {
        printf("      raw:      min %u  mean %.1f  max %u\n", (unsigned)stats->raw_min,
               (double)stats->raw_sum / stats->good, (unsigned)stats->raw_max);
    }
    printf("      interval: min %.3f  mean %.3f  max %.3f ms (target %ld)\n",
           stats->ivl_min / 1000.0, stats->ivl_sum / 1000.0 / (sample_count - 1),
           stats->ivl_max / 1000.0, period_ms);
    printf("      jitter:   p50 %lld  p99 %lld  max %lld us (p99 limit %d)\n",
           (long long)p50, (long long)p99, (long long)dev_max, JITTER_P99_LIMIT_US);

    /* Not a FAIL: an undisturbed pot really is steady. But a count that never
     * moves can mean a dead sensor, so make the user look. */
    if (stats->good >= 2 && stats->raw_min == stats->raw_max) {
        printf("      warning: every sample read raw=%u; grip the probe and rerun\n",
               (unsigned)stats->raw_min);
    }
}

/* Check 4: take `count` samples, one per `period_ms`, on an absolute
 * schedule. Each sample is timestamped just before its I2C read, which is
 * the point DESIGN.md measures jitter at. Prints every sample so the count
 * can be watched live, then a summary. Passes if every read succeeded and
 * p99 deviation from the schedule is under 10 ms. */
static int check_sampling(gt_soil_sensor *sensor, int count, long period_ms)
{
    /* int64_t: a signed 64-bit integer, wide enough for microsecond times. */
    int64_t *dev_us = malloc((size_t)count * sizeof *dev_us);
    if (dev_us == NULL) {
        report(4, "sampling interval", 0);
        return 0;
    }

    int64_t period_us = (int64_t)period_ms * 1000;
    int64_t start = mono_us() + period_us;
    sample_stats stats;
    init_sample_stats(&stats);

    printf("      sampling %d x %ld ms (grip the probe / dip it in water to see raw follow)\n",
           count, period_ms);
    printf("      %4s %10s %7s %8s\n", "i", "t_ms", "raw", "dev_us");

    for (int sample_index = 0; sample_index < count; sample_index++) {
        dev_us[sample_index] = read_scheduled_sample(sensor, sample_index, start,
                                                     start + sample_index * period_us, &stats);
    }

    qsort(dev_us, (size_t)count, sizeof *dev_us, cmp_i64);
    int64_t p99 = percentile(dev_us, count, 99);
    int ok = stats.errors == 0 && p99 < JITTER_P99_LIMIT_US;
    report(4, "sampling interval", ok);
    print_sampling_summary(&stats, dev_us, count, period_ms);
    free(dev_us);
    return ok;
}

int main(int argc, char **argv)
{
    /* Line-buffer stdout so PASS/FAIL lines and stderr hints stay in order
     * even when the output is piped to a log (e.g. `| tee probe.log`). */
    setvbuf(stdout, NULL, _IOLBF, 0);

    probe_opts opts;
    if (parse_opts(argc, argv, &opts) != 0) {
        return 2;
    }

    gt_i2c_bus bus;
    if (!check_bus(opts.dev, &bus)) {
        return 1;
    }
    gt_soil_sensor *sensor = check_device(bus);
    if (sensor == NULL) {
        return 1;
    }
    int ok = check_read(sensor) && check_sampling(sensor, opts.samples, opts.period_ms);

    gt_soil_close(sensor);
    return ok ? 0 : 1;
}
