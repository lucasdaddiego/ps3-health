/* PS3 Health: the hardware of a PS3 checked from a HEN app. Nine modules
 * behind a home screen: Drive, Cooling, Controller, Display, Audio, Network,
 * Transfer, Memory, Clock. */
#include <stdlib.h>
#include "app.h"
#include <ps3gfx/ui.h>
#include "report.h"
#include "version.h"

int fs_ok, fs_mk, fs_op, fs_wr;

/* Every exit path: START and Quit Game from the PS button menu end the loop,
 * and exit() runs this once, after main returned (no other exit() comes
 * after atexit). So no job thread is left: run_job joins its thread before
 * it returns, the drive poll is settled before a quit, the load threads are
 * joined before the load screen returns, the audio thread before the audio
 * module returns. Each boot mark here names the step about to run. */
static void leave(void)
{
    boot_mark("leave");
    spu_last_resort();                       /* a group of SPU threads still up: one more terminate */
    drive_close(&D);
    boot_mark("audio_end");
    audio_end();                             /* the module ends its port on every way out; this is the guard */
    boot_mark("net_end");
    net_end();                               /* the network ends before the picture */
    boot_mark("ui_end");
    ui_end();
    boot_mark("left");
}

/* The boot marks of the start path name the step that ended: a start that
 * stops shows its last step in boot.txt. */
int main(void)
{
    boot_mark("main");
    ui_init(&(const ui_app){"PS3 Health", APP_VERSION, APP_DIR});
    boot_mark("ui_init");
    fs_ok = fs_selftest(APP_DIR, &fs_mk, &fs_op, &fs_wr) == 0;
    boot_mark("fs_selftest");
    D.cpu_temp = D.rsx_temp = -1;
    if (!safe_load() && fs_ok) safe_calibrate(1);   /* the first start: the visible area of this TV */
    boot_mark("calibrate");
    state_load();
    boot_mark("state");
    console_firmware();
    boot_mark("firmware");
    console_timezone();
    atexit(leave);
    boot_mark("loop");
    int sel = 0;
    while (!quit) {
        sel = home_screen(sel);
        if (sel < 0) break;
        ui_module = modules[sel].name;
        int r = modules[sel].open();
        write_report();                      /* every module's result reaches the file */
        ui_module = NULL;
        ui_demo = 0;
        if (r < 0) break;
    }
    return 0;                                /* exit() runs leave() */
}
