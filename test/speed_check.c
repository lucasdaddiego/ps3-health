/* Real files in an isolated directory; faults affect only this test process.
 * speed_impl.h is extracted from source/ata.c by test_speed.py. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
typedef int32_t s32;
typedef int64_t s64;
typedef uint64_t u64;
#define SYS_O_RDONLY O_RDONLY
#define SYS_O_WRONLY O_WRONLY
#define SYS_O_CREAT O_CREAT
#define SYS_O_EXCL O_EXCL
static int fail_open, fail_write, refused, opens, unlinks, writes, reads;
static int journal_start(const char *name) { (void)name; return !refused; }
static void journal_done(const char *name, s32 rc, int ok) { (void)name; (void)rc; (void)ok; }
static void sysGetCurrentTime(u64 *s, u64 *ns) { *s = 0x123; *ns = 0x456; }
static s32 sysLv2FsOpen(const char *p, int flags, s32 *fd, int mode, const void *arg, u64 n)
{
    (void)arg; (void)n;
    opens++;
    if (fail_open) return -9;
    int f = open(p, flags, mode);
    if (f < 0) return errno == EEXIST ? (s32)0x80010014 : -9;
    *fd = f;
    return 0;
}
static s32 sysLv2FsWrite(s32 fd, const void *p, u64 n, u64 *done)
{
    writes++;
    if (fail_write) { *done = 0; return -9; }
    ssize_t r = write(fd, p, n); *done = r > 0 ? (u64)r : 0;
    return r < 0 ? -9 : 0;
}
static s32 sysLv2FsRead(s32 fd, void *p, u64 n, u64 *done)
{
    reads++;
    ssize_t r = read(fd, p, n); *done = r > 0 ? (u64)r : 0;
    return r < 0 ? -9 : 0;
}
static s32 sysLv2FsClose(s32 fd) { return close(fd); }
static s32 sysLv2FsFsync(s32 fd) { return fsync(fd); }
static s32 sysLv2FsUnlink(const char *p) { unlinks++; return unlink(p); }
#include "speed_impl.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
static void sentinel(const char *p)
{
    FILE *f = fopen(p, "wb"); CHECK(f != NULL);
    CHECK(fputs("user data", f) >= 0); CHECK(fclose(f) == 0);
}
static void preserved(const char *p)
{
    char b[16] = {0}; FILE *f = fopen(p, "rb"); CHECK(f != NULL);
    CHECK(fread(b, 1, sizeof b, f) == 9); CHECK(!strcmp(b, "user data")); fclose(f);
}
static int run(const char *dir)
{
    double mb, w, r;
    opens = unlinks = writes = reads = 0;
    return file_speed_test(dir, "speed", &mb, &w, &r);
}
int main(void)
{
    char dir[] = "/tmp/ps3-speed-files-XXXXXX", old[256], collision[256];
    CHECK(mkdtemp(dir) != NULL);
    snprintf(old, sizeof old, "%s/speed.tmp", dir); sentinel(old);
    snprintf(collision, sizeof collision, "%s/ps3-health-speed-123-456-0.tmp", dir); sentinel(collision);
    CHECK(run(dir) == 0);
    CHECK(unlinks == 1 && writes == 64 && reads == 64);
    preserved(old); preserved(collision);

    fail_open = 1;
    CHECK(run(dir) != 0 && unlinks == 0); preserved(old); preserved(collision);
    fail_open = 0; fail_write = 1;
    CHECK(run(dir) != 0 && unlinks == 1); preserved(old); preserved(collision);
    fail_write = 0;

    for (int i = 1; i < 16; i++) {
        snprintf(collision, sizeof collision, "%s/ps3-health-speed-123-456-%d.tmp", dir, i);
        sentinel(collision);
    }
    CHECK(run(dir) != 0 && unlinks == 0 && opens == 16);
    for (int i = 0; i < 16; i++) {
        snprintf(collision, sizeof collision, "%s/ps3-health-speed-123-456-%d.tmp", dir, i);
        preserved(collision); CHECK(unlink(collision) == 0);
    }
    char longdir[128]; memset(longdir, 'x', sizeof longdir - 1); longdir[127] = 0;
    CHECK(run(longdir) != 0 && opens == 0 && unlinks == 0);
    refused = 1;
    CHECK(run(dir) != 0 && opens == 0 && unlinks == 0);
    preserved(old); CHECK(unlink(old) == 0); CHECK(rmdir(dir) == 0);
    puts("PASS speed test: existing files, collisions, open/write failures, long paths, journal refusal");
    return 0;
}
