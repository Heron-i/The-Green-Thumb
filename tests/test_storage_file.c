/* Loopback tests for storage over real files: gt_file_ops_linux (the
 * Linux file layer, src/storage_file.c) and gt_storage_open, in a
 * directory the test makes itself.
 *
 * Purpose: prove what tests/fake_file.h can't, namely that the real file
 * system behaves as the seam promises (storage_io.h). Files are created,
 * written, read back, sized, cut, renamed, listed and deleted by name
 * inside one directory. A short read and a missing file are GT_ERR_IO, and
 * a name that would reach outside the directory is refused. Then storage
 * itself runs end to end on disk: a clean shutdown and reopen keeps every
 * record, and garbage bytes written onto a segment by hand are cut off by
 * recovery at the next open.
 *
 * Each test makes its own directory with mkdtemp under /tmp (as the IPC
 * loopback tests do: Windows-mounted drives in WSL2 are unreliable) and
 * removes it after. Run under make test / asan / memcheck. Implements
 * SLB-01 through SLB-09.
 *
 * What these tests cannot prove: durability. fsync returning GT_OK only
 * shows the call works; the on-Pi power pulls (Mechanism D Step 13) are
 * the evidence that the card keeps what fsync promised. */
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "log_record.h"
#include "storage.h"
#include "storage_io.h"

#define TODAY_US   ((uint64_t)(1792972800u + 3600u) * 1000000u)   /* 2026-10-26 01:00 UTC */
#define TODAY_NAME "log-20261026.bin"

/* One test's directory: "/tmp/gt-slb-XXXXXX", the X's filled in by mkdtemp,
 * which also creates the directory. */
typedef struct {
    char dir[32];
} scratch_dir;

/* Make a fresh, empty directory. Returns 1 on success, 0 on failure. */
static int scratch_init(scratch_dir *scratch)
{
    strcpy(scratch->dir, "/tmp/gt-slb-XXXXXX");
    return mkdtemp(scratch->dir) != NULL;
}

/* Remove every file in the directory, then the directory itself. The test
 * only ever makes plain files in it, so one level is enough. */
static void scratch_remove(scratch_dir *scratch)
{
    DIR *listing = opendir(scratch->dir);
    if (listing != NULL) {
        struct dirent *entry;
        /* invariant: every plain file seen so far has been unlinked */
        while ((entry = readdir(listing)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            char path[96];
            snprintf(path, sizeof path, "%s/%s", scratch->dir, entry->d_name);
            unlink(path);
        }
        closedir(listing);
    }
    rmdir(scratch->dir);
}

/* Open the Linux file ops over the scratch directory, checking it works. */
static void open_ops(const scratch_dir *scratch, const gt_file_ops **ops, void **ctx)
{
    *ops = NULL;
    *ctx = NULL;
    CHECK_EQ(gt_file_ops_linux(scratch->dir, ops, ctx), GT_OK);
    CHECK(*ops != NULL && *ctx != NULL);
}

/* 1 if `name` is among the first `count` rows of `names`. */
static int listed(char names[GT_FILE_LIST_MAX][GT_FILE_NAME_MAX], size_t count, const char *name)
{
    for (size_t name_index = 0; name_index < count; name_index++) {
        if (strcmp(names[name_index], name) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Encode a light DATA record with `seq` into `wire`. */
static void make_wire(uint8_t wire[GT_REC_SIZE], uint32_t seq)
{
    const gt_record record = {
        .type = GT_REC_DATA, .sensor_id = GT_SENSOR_LIGHT,
        .seq = seq, .timestamp_us = 5000 + seq, .value = 40 + seq,
    };
    CHECK_EQ(gt_record_encode(&record, wire), GT_OK);
}

/* Append records seq first .. last to storage. */
static void append_range(gt_storage *storage, uint32_t first, uint32_t last)
{
    /* invariant: records first .. seq - 1 have been appended */
    for (uint32_t seq = first; seq <= last; seq++) {
        uint8_t wire[GT_REC_SIZE];
        make_wire(wire, seq);
        CHECK_EQ(gt_storage_append(storage, wire, 0, NULL), GT_OK);
    }
}

/* ---- SLB-01: a missing or empty dir is refused ----
 * A directory that doesn't exist is GT_ERR_IO; NULL and "" are
 * GT_ERR_INVAL. The outputs are untouched each time. */
static void test_slb01_bad_dir_refused(void)
{
    static char sentinel;
    const gt_file_ops *ops = (const gt_file_ops *)(void *)&sentinel;
    void *ctx = &sentinel;

    CHECK_EQ(gt_file_ops_linux("/tmp/gt-slb-does-not-exist", &ops, &ctx), GT_ERR_IO);
    CHECK_EQ(gt_file_ops_linux(NULL, &ops, &ctx), GT_ERR_INVAL);
    CHECK_EQ(gt_file_ops_linux("", &ops, &ctx), GT_ERR_INVAL);
    CHECK(ops == (const gt_file_ops *)(void *)&sentinel);
    CHECK(ctx == &sentinel);
}

/* ---- SLB-02: a file is created, written and read ----
 * Two writes at explicit offsets; size and a read-back see both. */
static void test_slb02_create_write_read(void)
{
    scratch_dir scratch;
    CHECK(scratch_init(&scratch));
    const gt_file_ops *ops;
    void *ctx;
    open_ops(&scratch, &ops, &ctx);
    const uint8_t first[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    const uint8_t second[4] = { 9, 10, 11, 12 };
    uint8_t back[12];
    uint64_t size = 0;
    int handle = -1;

    CHECK_EQ(ops->open(ctx, "a.bin", GT_FILE_CREATE, &handle), GT_OK);
    CHECK_EQ(ops->pwrite(ctx, handle, 0, first, sizeof first), GT_OK);
    CHECK_EQ(ops->pwrite(ctx, handle, 8, second, sizeof second), GT_OK);
    CHECK_EQ(ops->size(ctx, handle, &size), GT_OK);
    CHECK(size == 12);
    CHECK_EQ(ops->pread(ctx, handle, 0, back, sizeof back), GT_OK);
    CHECK(memcmp(back, first, 8) == 0 && memcmp(&back[8], second, 4) == 0);
    CHECK_EQ(ops->fsync(ctx, handle), GT_OK);
    CHECK_EQ(ops->fsync_dir(ctx), GT_OK);

    ops->close(ctx, handle);
    ops->destroy(ctx);
    scratch_remove(&scratch);
}

/* ---- SLB-03: a short read and a missing file fail ----
 * Reading past the end is GT_ERR_IO, not a partial read; opening a name
 * that doesn't exist without CREATE is GT_ERR_IO and creates nothing. */
static void test_slb03_short_read_and_missing(void)
{
    scratch_dir scratch;
    CHECK(scratch_init(&scratch));
    const gt_file_ops *ops;
    void *ctx;
    open_ops(&scratch, &ops, &ctx);
    const uint8_t bytes[8] = { 0 };
    uint8_t back[8];
    int handle = -1, missing = -1;

    CHECK_EQ(ops->open(ctx, "a.bin", GT_FILE_CREATE, &handle), GT_OK);
    CHECK_EQ(ops->pwrite(ctx, handle, 0, bytes, sizeof bytes), GT_OK);
    CHECK_EQ(ops->pread(ctx, handle, 4, back, sizeof back), GT_ERR_IO);
    CHECK_EQ(ops->open(ctx, "nope.bin", 0, &missing), GT_ERR_IO);

    char names[GT_FILE_LIST_MAX][GT_FILE_NAME_MAX];
    size_t count = 0;
    CHECK_EQ(ops->list(ctx, names, &count), GT_OK);
    CHECK_EQ(count, 1);

    ops->close(ctx, handle);
    ops->destroy(ctx);
    scratch_remove(&scratch);
}

/* ---- SLB-04: ftruncate cuts the file ---- */
static void test_slb04_ftruncate_cuts(void)
{
    scratch_dir scratch;
    CHECK(scratch_init(&scratch));
    const gt_file_ops *ops;
    void *ctx;
    open_ops(&scratch, &ops, &ctx);
    const uint8_t bytes[16] = { 7 };
    uint64_t size = 0;
    int handle = -1;

    CHECK_EQ(ops->open(ctx, "a.bin", GT_FILE_CREATE, &handle), GT_OK);
    CHECK_EQ(ops->pwrite(ctx, handle, 0, bytes, sizeof bytes), GT_OK);
    CHECK_EQ(ops->ftruncate(ctx, handle, 8), GT_OK);
    CHECK_EQ(ops->size(ctx, handle, &size), GT_OK);
    CHECK(size == 8);

    ops->close(ctx, handle);
    ops->destroy(ctx);
    scratch_remove(&scratch);
}

/* ---- SLB-05: rename, list and unlink by name ----
 * list never returns "." or "..". A rename onto an existing name replaces
 * it, as storage's rename to .corrupt relies on. */
static void test_slb05_rename_list_unlink(void)
{
    scratch_dir scratch;
    CHECK(scratch_init(&scratch));
    const gt_file_ops *ops;
    void *ctx;
    open_ops(&scratch, &ops, &ctx);
    char names[GT_FILE_LIST_MAX][GT_FILE_NAME_MAX];
    size_t count = 0;
    int handle = -1;

    const char *made[] = { "a.bin", "b.bin", "d.bin" };
    for (size_t made_index = 0; made_index < 3; made_index++) {
        CHECK_EQ(ops->open(ctx, made[made_index], GT_FILE_CREATE, &handle), GT_OK);
        ops->close(ctx, handle);
    }
    CHECK_EQ(ops->rename(ctx, "a.bin", "c.bin"), GT_OK);
    CHECK_EQ(ops->rename(ctx, "d.bin", "b.bin"), GT_OK);   /* replaces b.bin */
    CHECK_EQ(ops->list(ctx, names, &count), GT_OK);
    CHECK_EQ(count, 2);
    CHECK(listed(names, count, "b.bin") && listed(names, count, "c.bin"));
    CHECK(!listed(names, count, ".") && !listed(names, count, ".."));

    CHECK_EQ(ops->unlink(ctx, "b.bin"), GT_OK);
    CHECK_EQ(ops->unlink(ctx, "b.bin"), GT_ERR_IO);   /* already gone */
    CHECK_EQ(ops->list(ctx, names, &count), GT_OK);
    CHECK_EQ(count, 1);
    CHECK(listed(names, count, "c.bin"));

    ops->destroy(ctx);
    scratch_remove(&scratch);
}

/* ---- SLB-06: names outside the directory are refused ----
 * Anything with a '/', ".", "..", "" or too long for a row is
 * GT_ERR_INVAL for every op that takes a name, and nothing is made. */
static void test_slb06_outside_names_refused(void)
{
    scratch_dir scratch;
    CHECK(scratch_init(&scratch));
    const gt_file_ops *ops;
    void *ctx;
    open_ops(&scratch, &ops, &ctx);
    char long_name[GT_FILE_NAME_MAX + 1];
    memset(long_name, 'a', GT_FILE_NAME_MAX);
    long_name[GT_FILE_NAME_MAX] = '\0';
    const char *bad_names[] = { "../escape.bin", "sub/a.bin", "/tmp/a.bin", ".", "..", "",
                                long_name };
    size_t bad_count = sizeof bad_names / sizeof bad_names[0];
    int handle = -1;

    for (size_t bad_index = 0; bad_index < bad_count; bad_index++) {
        const char *bad = bad_names[bad_index];
        CHECK_EQ(ops->open(ctx, bad, GT_FILE_CREATE, &handle), GT_ERR_INVAL);
        CHECK_EQ(ops->rename(ctx, bad, "x.bin"), GT_ERR_INVAL);
        CHECK_EQ(ops->rename(ctx, "x.bin", bad), GT_ERR_INVAL);
        CHECK_EQ(ops->unlink(ctx, bad), GT_ERR_INVAL);
    }
    char names[GT_FILE_LIST_MAX][GT_FILE_NAME_MAX];
    size_t count = 99;
    CHECK_EQ(ops->list(ctx, names, &count), GT_OK);
    CHECK_EQ(count, 0);
    CHECK(access("/tmp/escape.bin", F_OK) != 0);

    ops->destroy(ctx);
    scratch_remove(&scratch);
}

/* ---- SLB-07: storage round-trips on disk ----
 * Open an empty directory, append 1-3, shut down cleanly, close; reopen:
 * not unclean, the newest seq is 3, and the segment holds exactly the
 * header and 3 records. */
static void test_slb07_storage_round_trip(void)
{
    scratch_dir scratch;
    CHECK(scratch_init(&scratch));
    gt_storage *storage = NULL;
    gt_storage_open_info info;

    CHECK_EQ(gt_storage_open(scratch.dir, TODAY_US, &storage, &info), GT_OK);
    CHECK_EQ(info.created, 1);
    append_range(storage, 1, 3);
    CHECK_EQ(gt_storage_shutdown(storage, NULL), GT_OK);
    gt_storage_close(storage);

    storage = NULL;
    CHECK_EQ(gt_storage_open(scratch.dir, TODAY_US, &storage, &info), GT_OK);
    CHECK_EQ(info.unclean_shutdown, 0);
    CHECK_EQ(info.created, 0);
    CHECK_EQ(info.has_last, 1);
    CHECK_EQ(info.last_seq, 3);
    gt_storage_close(storage);

    char path[96];
    struct stat file_info;
    snprintf(path, sizeof path, "%s/%s", scratch.dir, TODAY_NAME);
    CHECK_EQ(stat(path, &file_info), 0);
    CHECK(file_info.st_size == GT_SEG_HEADER_SZ + 3 * GT_REC_SIZE);
    scratch_remove(&scratch);
}

/* ---- SLB-08: a torn tail on disk is recovered ----
 * Storage commits 2 records and is closed without a shutdown (a crash).
 * 10 garbage bytes are then written onto the segment by hand. The next
 * open reports an unclean shutdown, cuts exactly those bytes, and the
 * file on disk is back to the header and 2 records. */
static void test_slb08_torn_tail_recovered(void)
{
    scratch_dir scratch;
    CHECK(scratch_init(&scratch));
    gt_storage *storage = NULL;
    gt_storage_open_info info;

    CHECK_EQ(gt_storage_open(scratch.dir, TODAY_US, &storage, NULL), GT_OK);
    append_range(storage, 1, 2);
    CHECK_EQ(gt_storage_flush(storage, NULL), GT_OK);
    gt_storage_close(storage);

    char path[96];
    snprintf(path, sizeof path, "%s/%s", scratch.dir, TODAY_NAME);
    int fd = open(path, O_WRONLY | O_APPEND);
    const uint8_t garbage[10] = { 0xA7, 0xA7, 0xA7, 0xA7, 0xA7, 0xA7, 0xA7, 0xA7, 0xA7, 0xA7 };
    CHECK(fd >= 0);
    if (fd >= 0) {
        CHECK(write(fd, garbage, sizeof garbage) == (ssize_t)sizeof garbage);
        close(fd);
    }

    storage = NULL;
    CHECK_EQ(gt_storage_open(scratch.dir, TODAY_US, &storage, &info), GT_OK);
    CHECK_EQ(info.unclean_shutdown, 1);
    CHECK_EQ(info.recovery.outcome, GT_RECOVER_TRUNCATED);
    CHECK(info.recovery.bytes_removed == 10);
    CHECK_EQ(info.last_seq, 2);
    gt_storage_close(storage);

    struct stat file_info;
    CHECK_EQ(stat(path, &file_info), 0);
    CHECK(file_info.st_size == GT_SEG_HEADER_SZ + 2 * GT_REC_SIZE);
    scratch_remove(&scratch);
}

/* ---- SLB-09: storage open refuses a missing dir ----
 * GT_ERR_IO, and *storage_out is untouched. */
static void test_slb09_storage_missing_dir(void)
{
    static char sentinel;
    gt_storage *storage = (gt_storage *)(void *)&sentinel;

    CHECK_EQ(gt_storage_open("/tmp/gt-slb-does-not-exist", TODAY_US, &storage, NULL),
             GT_ERR_IO);
    CHECK(storage == (gt_storage *)(void *)&sentinel);
}

static const test_case_t tests[] = {
    {"SLB-01", "a missing or empty dir is refused", test_slb01_bad_dir_refused},
    {"SLB-02", "a file is created, written and read", test_slb02_create_write_read},
    {"SLB-03", "a short read and a missing file fail", test_slb03_short_read_and_missing},
    {"SLB-04", "ftruncate cuts the file", test_slb04_ftruncate_cuts},
    {"SLB-05", "rename, list and unlink by name", test_slb05_rename_list_unlink},
    {"SLB-06", "names outside the directory are refused", test_slb06_outside_names_refused},
    {"SLB-07", "storage round-trips on disk", test_slb07_storage_round_trip},
    {"SLB-08", "a torn tail on disk is recovered", test_slb08_torn_tail_recovered},
    {"SLB-09", "storage open refuses a missing dir", test_slb09_storage_missing_dir},
};

int main(void)
{
    return RUN_TESTS(tests);
}
