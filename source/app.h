/* The app: eight modules behind a home screen, one report, one journal. */
#ifndef APP_H
#define APP_H

#include "ata.h"
#include <ps3gfx/fs.h>
#include <ps3gfx/journal.h>
#include <ps3gfx/spu_kit.h>

/* A module: a tile on the home screen and its own screens. open() returns 0
 * to go back home, -1 to exit the app. report() appends its section with
 * rep_out() and writes nothing when it has no result. */
typedef struct {
    const char *name;        /* the tile and the title bar */
    const char *key;         /* the key in state.txt */
    const char *desc;        /* one sentence in the tile: what the module checks */
    int (*open)(void);
    void (*report)(void);
} module;

#define MODULE_COUNT 8
extern const module modules[MODULE_COUNT];

/* The last result of each module, kept in APP_DIR/state.txt for the home
 * screen: a dot (DOT_*), two lines of at most 44 characters, the date. */
enum { DOT_NONE, DOT_OK, DOT_WARN, DOT_BAD };
typedef struct {
    int dot;
    char l1[48], l2[48], when[16];
} tile_state;
void state_load(void);
void state_set(const char *key, int dot, const char *l1, const char *l2);
void state_hold(void);             /* state_set keeps its tiles in memory until state_flush: one write for several tiles */
void state_flush(void);

void boot_mark(const char *name);  /* one line in APP_DIR/boot.txt (boot.c); "main" begins a start */

extern int fs_ok;                  /* APP_DIR is writable: the journal works */
extern int fs_mk, fs_op, fs_wr;    /* the file test return codes, for the home screen when fs_ok is 0 */

int home_screen(int sel);          /* the module chosen, -1 to exit */
int help_screen(void);             /* TRIANGLE on the home screen: the QR code and a short guide; 0 back, -1 exit */
int quick_screen(void);            /* R1 on the home screen (quick.c): the short tests in one run; 0 back, -1 exit */
void help_keep(void);              /* the QR text of the drive result to APP_DIR/compat.txt, for Help in a later session */

/* modules */
extern drive_state D;
int mod_drive_open(void);
void mod_drive_report(void);
int mod_drive_probed(void);        /* 1 when the drive (or the demo data) was read in this session */
int mod_display_open(void);
void mod_display_report(void);
int mod_pad_open(void);
void mod_pad_report(void);
int mod_mem_open(void);
void mod_mem_report(void);
int mod_net_open(void);
void mod_net_report(void);
void net_init(void);               /* the network's start, one journaled call, then the address (mod_net.c); every network test calls it */
int net_is_up(void);               /* 1 once the start succeeded */
int net_start_frozen(void);        /* 1 when the start froze this console before (or was skipped): no network call runs */
void net_end(void);                /* leave(): the network's end, one journaled call; nothing when it never started */
int mod_network_open(void);
void mod_network_report(void);
int mod_audio_open(void);
void mod_audio_report(void);
void audio_end(void);              /* leave() and the module's exit: the sound's end, one journaled call; nothing when it never started */
int mod_cool_open(void);
void mod_cool_report(void);

/* The SPU driver (spu_run.c), the PPU side of ps3gfx's SPU kit: one group of
 * six SPU threads per use, each call journaled. The image names spu_init,
 * spu_import and spu_close are shared; each test has its family of names
 * (Memory's local-store test, Cooling's heat load). */
typedef struct {
    const char *group, *start, *run, *terminate, *join, *exit_status, *destroy;   /* exit_status NULL: not read */
} spu_names;
typedef struct {
    const char *failed;                  /* the first call that failed or was not made (a journal name, group_still_up or group_not_destroyed); NULL when none */
    s32 rc;                              /* its rc */
    int skipped, frozen, declined;       /* that call was not made; it froze this console before; it was skipped at the prompt */
    int ran, started, late, stuck;       /* the group started; the first reports within 2 s; one did not come; the group could not be stopped */
    int terminated, joined;              /* the terminate was made (term_rc); the join returned (cause, status) */
    s32 term_rc;
    u32 cause, status;
    s32 exit_status[SPU_KIT_SPUS];       /* -1: not read */
    spu_kit_rec rec[SPU_KIT_SPUS];       /* the six reports at the end */
    u64 us;                              /* from spu_up to the end */
    char why[64];                        /* why the run did not reach its end, for the screen and the report; empty when it did */
} spu_result;
extern const spu_names spu_ls;           /* the Memory module's local-store test: ls_group ... ls_destroy */
extern const spu_names spu_cool;         /* the Cooling module's heat load: cool_group ... cool_destroy, no exit status (heat never ends by itself) */
const char *spu_blocked(const spu_names *f);   /* the call that keeps the family from starting (a journal name, group_still_up or group_not_destroyed), or NULL */
int spu_ask(const spu_names *f);         /* the first-run prompt, once (none when f is blocked): 1 run, 0 skipped, -1 exit */
int spu_up(const spu_names *f, u32 mode, spu_result *r);   /* 1 when six SPUs run; 0: no group is left running (or it is stuck), r says why */
const volatile spu_kit_rec *spu_reports(void);   /* the six live reports of the group (heat: count moves every SPU_KIT_HEAT_BATCH loops) */
int spu_stuck(void);                     /* 1 once a group could not be stopped (r->stuck of any test): its SPUs may still run until the app exits */
int spu_wait_done(s64 us);               /* ls: 1 when the six reports have done = 1 within us */
void spu_down(spu_result *r);            /* after spu_up returned 1: the end of the group, the reports into r */
void spu_last_resort(void);              /* leave(): a group still up gets one more journaled terminate */

/* The quick check (quick.c) runs each module's short test in one job: the
 * *_quick functions run in the job thread (progress, job_cancel), the
 * *_quick_end functions in the UI thread after it: they set the tile, and
 * give the line of the verdict screen and its dot (DOT_*). */
void drive_quick(void);
int drive_quick_end(char *line, int n);
int cool_quick_ask(void);          /* the fan prompt, once: 1 go on, -1 exit */
void cool_quick(void);
int cool_quick_end(char *line, int n);
int mem_quick_ask(void);           /* the SPU prompt, once: 1 go on, -1 exit */
void mem_quick(void);
int mem_quick_end(char *line, int n);
void network_quick(void);
int network_quick_end(char *line, int n);

/* The Cooling module's soak test borrows the Memory module's tests
 * (mod_mem.c): one pattern p over n blocks of 1 MB, written then verified
 * (the errors, the first wrong word's address in *first; stop ends it
 * early), and the SPU local-store test once (1 when the six ended it, the
 * wrong reads in *errors). */
long mem_soak_pass(void *const *blocks, int n, int p, volatile int *stop, unsigned long *first);
int mem_soak_ls(spu_result *r, unsigned *errors);
/* The blocks of a memory test (mod_mem.c): up to max of 1 MB, the kernel's
 * pages that mem_give hands back (malloc's when the kernel refuses), less
 * keep given back at once as headroom; *kernel for mem_give. The count. */
int mem_take(void **blocks, int max, int keep, int *kernel);
void mem_give(void *const *blocks, int n, int kernel);

#endif
