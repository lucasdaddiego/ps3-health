/* The boot log: APP_DIR/boot.txt, a few marks on the start path and on the
 * exit path, so a start that stops can be found. One line per mark: its name
 * and sysGetSystemTime() in microseconds (the time since the console booted),
 * "main 41234567". The "main" line begins each start. A start mark names the
 * step that ended (ui_init: the picture is up); an exit mark names the step
 * about to run (net_end: the network's end begins), and "left" ends the exit.
 * Only sysLv2Fs calls (open with append and create, write, fsync, close, and
 * at the "main" mark lseek, mkdir, unlink, rename), no newlib stdio and no
 * heap, so a mark is safe before ui_init and in leave().
 * A mark that cannot open the file is skipped: it never stops the app. The
 * "main" mark makes the folder on the first start. When the file is over
 * 32 KB, the "main" mark first moves it to boot.old.txt (unlink, rename) and
 * starts a new one: the last start, which may be the one that stopped, stays
 * on the disk. A rename that fails empties the file, so the cap holds. */
#include <stdio.h>                           /* SEEK_END only */
#include <string.h>
#include <sys/file.h>
#include <lv2/systime.h>
#include "app.h"

#define BOOT_FILE APP_DIR "/boot.txt"
#define BOOT_OLD APP_DIR "/boot.old.txt"
#define BOOT_CAP 32768

static s32 boot_open(s32 how, s32 *fd)
{
    return sysLv2FsOpen(BOOT_FILE, SYS_O_WRONLY | SYS_O_CREAT | how, fd, 0666, NULL, 0);
}

void boot_mark(const char *name)
{
    char line[48], d[24];
    int n = 0, k = 0, first = !strcmp(name, "main");
    u64 t = (u64)sysGetSystemTime(), w, size = 0;
    s32 fd;
    while (name[n] && n < 24) line[n] = name[n], n++;
    line[n++] = ' ';
    do d[k++] = (char)('0' + t % 10);
    while ((t /= 10) && k < (int)sizeof d);
    while (k) line[n++] = d[--k];
    line[n++] = '\n';
    if (boot_open(SYS_O_APPEND, &fd)) {
        if (!first) return;                  /* no folder: only the start's first mark makes it */
        sysLv2FsMkdir(APP_DIR, 0777);
        if (boot_open(SYS_O_APPEND, &fd)) return;
    }
    if (first && sysLv2FsLSeek64(fd, 0, SEEK_END, &size) == 0 && size > BOOT_CAP) {
        sysLv2FsClose(fd);
        sysLv2FsUnlink(BOOT_OLD);            /* lv2's rename does not replace a file */
        if (boot_open(sysLv2FsRename(BOOT_FILE, BOOT_OLD) ? SYS_O_TRUNC : SYS_O_APPEND, &fd)) return;
    }
    sysLv2FsWrite(fd, line, n, &w);
    sysLv2FsFsync(fd);
    sysLv2FsClose(fd);
}
