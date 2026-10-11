/* The SPU driver: the PPU side of ps3gfx's SPU kit (<ps3gfx/spu_kit.h>, the
 * program spu_kit_spu) for the Memory module's local-store test (ls) and the
 * Cooling module's heat load (heat). Each use imports the image, makes one
 * group of six SPU threads, starts it, waits up to 2 s for the six first
 * reports, and ends the group itself: the join once the six threads put
 * their last report, else a terminate first (never a join while a thread can
 * still run; heat never ends by itself), then the destroy and the image
 * close. Every LV2 call is journaled: the image by the shared names
 * spu_init, spu_import and spu_close, the group by the names of its family
 * (spu_ls: ls_*, spu_cool: cool_*), so a freeze in one test does not close
 * the other. The run entry (ls_run, cool_run) opens before the start's done
 * line and closes after the start line of the join or the terminate, so a
 * freeze while the SPUs work is recorded too. A call that froze the console
 * is never made again (SQUARE twice clears the journal). The reports are
 * read with volatile reads: the SPUs write them by DMA. */
#include <stdio.h>
#include <string.h>
#include <sys/spu.h>
#include <sys/systime.h>
#include <lv2/systime.h>
#include "app.h"
#include <ps3gfx/ui.h>

#define GROUP_PRIO 100               /* PSL1GHT's samples: 16 is the highest, 255 the lowest */
#define FIRST_US 2000000             /* the six first reports, or the group is stopped */
#define MOVE_US 100000               /* a failed terminate: the counts read twice, this far apart */

const spu_names spu_ls = {"ls_group", "ls_start", "ls_run", "ls_terminate", "ls_join", "ls_exit_status", "ls_destroy"};
const spu_names spu_cool = {"cool_group", "cool_start", "cool_run", "cool_terminate", "cool_join", NULL, "cool_destroy"};

/* Every family: a start needs the terminate of each, its way out. */
static const spu_names *const families[] = {&spu_ls, &spu_cool};

/* GROUP_IDLE: the group exists and no thread runs (joined, or never
 * started): the destroy is next. When the destroy fails, the group stays and
 * holds the SPUs, so no other group can be made in this process. */
enum { GROUP_NONE, GROUP_RUNNING, GROUP_IDLE };
static int initialized, imported, grp, run_open, stuck;   /* sysSpuInitialize ran in this process; the image; the group; the run entry; the group could not be stopped */
static sysSpuImage image;
static sys_spu_group_t group;
static sys_spu_thread_t threads[SPU_KIT_SPUS];
static spu_kit_rec recs[SPU_KIT_SPUS];             /* the reports, a 128-byte line each (the type's alignment) */
static const spu_names *gf;                        /* the family of the group */
static spu_result *R;                              /* the result the calls write into */
static s64 t0;

static u64 ea(const void *p) { return (u64)(uintptr_t)p; }

/* A journaled call: jgo before (0: it froze this console before or was
 * skipped, do not make it), jend after (the first failed call is the run's
 * failure). */
static int jgo(const char *name)
{
    if (journal_start(name)) return 1;
    if (!R->failed) {
        R->failed = name;
        R->skipped = 1;
    }
    return 0;
}

static int jend(const char *name, s32 rc)
{
    journal_done(name, rc, rc == 0);
    if (rc && !R->failed) {
        R->failed = name;
        R->rc = rc;
    }
    return rc == 0;
}

const char *spu_blocked(const spu_names *f)
{
    static const char *const image_names[] = {"spu_init", "spu_import", "spu_close"};
    const char *const own[] = {f->group, f->start, f->run, f->terminate, f->join, f->exit_status, f->destroy};
    for (unsigned i = 0; i < sizeof image_names / sizeof image_names[0]; i++)
        if (journal_state(image_names[i]) == J_FROZEN) return image_names[i];
    if (journal_state("spu_import") == J_SKIPPED) return "spu_import";
    for (unsigned i = 0; i < sizeof own / sizeof own[0]; i++)
        if (own[i] && journal_state(own[i]) == J_FROZEN) return own[i];
    for (unsigned i = 0; i < sizeof families / sizeof families[0]; i++)
        if (journal_state(families[i]->terminate) == J_FROZEN) return families[i]->terminate;
    if (grp == GROUP_RUNNING) return "group_still_up";
    if (grp == GROUP_IDLE) return "group_not_destroyed";
    return NULL;
}

const volatile spu_kit_rec *spu_reports(void) { return recs; }

/* Not group_still_up alone: a failed join after a terminate with rc 0 also
 * leaves the group up, but its threads were stopped. */
int spu_stuck(void) { return stuck; }

/* The answer and the skip are kept on spu_import, which every use journals:
 * SQUARE twice empties it, and the prompt comes back once. spu_init runs
 * once per process, so its entry can stay empty after SQUARE twice: it is
 * read only to see a CROSS whose spu_init then froze or failed, before any
 * spu_import line. Then no prompt: spu_blocked or spu_up gives the reason.
 * No prompt either when spu_blocked refuses the family: no SPU call would
 * follow and write spu_import, so the prompt would come back at every use
 * (a group still up after SQUARE twice). One answer serves both tests. */
int spu_ask(const spu_names *f)
{
    int js = journal_state("spu_import");
    if (js != J_NONE) return js != J_SKIPPED;
    if (journal_state("spu_init") != J_NONE || spu_blocked(f)) return 1;
    int r = first_run_prompt("the six SPUs (the SPU thread group)",
                             "Runs the app's own program on the six SPUs (the Cell's helper cores): each writes only a 128-byte report into the app's memory.",
                             "Memory: each SPU tests its own 256 KB local store. Cooling: the six SPUs add a heat load to the load test.",
                             "Syscalls 169 to 173, 177, 178 and 165, each journaled. A skip keeps the SPUs off: the load test runs without them.");
    if (r == 0) journal_skip("spu_import");
    return r;
}

static void image_close(void)
{
    if (imported && jgo("spu_close")) jend("spu_close", sysSpuImageClose(&image));
    imported = 0;                                    /* a failed close is not tried again: the next use imports anew */
}

/* The group: six threads of the kit's program in one mode, thread i with the
 * report recs[i]. A failed thread destroys the group inside the same
 * journaled call. */
static s32 group_make(u32 mode)
{
    static const char name[] = "ps3health";
    sysSpuThreadGroupAttribute ga;
    memset(&ga, 0, sizeof ga);
    ga.nsize = sizeof name;
    ga.name = name;
    ga.type = SPU_THREAD_GROUP_TYPE_NORMAL;
    s32 rc = sysSpuThreadGroupCreate(&group, SPU_KIT_SPUS, GROUP_PRIO, &ga);
    for (int i = 0; rc == 0 && i < SPU_KIT_SPUS; i++) {
        sysSpuThreadAttribute ta;
        memset(&ta, 0, sizeof ta);
        ta.name = name;
        ta.nsize = sizeof name;
        ta.option = SPU_THREAD_ATTR_NONE;
        sysSpuThreadArgument a = {ea(&recs[i]), 0, spu_kit_arg2(mode, (u32)i), 0};
        rc = sysSpuThreadInitialize(&threads[i], group, (u32)i, &image, &ta, &a);
        if (rc) sysSpuThreadGroupDestroy(group);
    }
    return rc;
}

/* The start, then the run entry before the start's done line: no gap
 * between the two entries while the SPUs work. */
static s32 group_start(void)
{
    s32 rc = sysSpuThreadGroupStart(group);
    if (rc == 0) run_open = jgo(gf->run);
    return rc;
}

/* The reports with done = 1: each thread of ls puts its last one just before it ends. */
static int done_count(void)
{
    int n = 0;
    for (int i = 0; i < SPU_KIT_SPUS; i++) n += ((const volatile spu_kit_rec *)&recs[i])->done == 1;
    return n;
}

/* The first reports: each thread puts one at its start. *missing: the first
 * thread without one at the last look, -1 when none. */
static int first_reports(int *missing)
{
    s64 a = sysGetSystemTime();
    for (;;) {
        int n = 0;
        *missing = -1;
        for (int i = 0; i < SPU_KIT_SPUS; i++) {
            if (((const volatile spu_kit_rec *)&recs[i])->magic == SPU_KIT_MAGIC) n++;
            else if (*missing < 0) *missing = i;
        }
        if (n == SPU_KIT_SPUS || sysGetSystemTime() - a > FIRST_US) return n;
        sysUsleep(1000);
    }
}

/* After a failed terminate: did a thread still report? The counts twice,
 * 100 ms apart (a sleep may end early: the host preview's does). The sleep
 * opens the loop, so the syscall audit sees its li r11 before the sc. */
static int counts_moved(void)
{
    u64 c[SPU_KIT_SPUS];
    for (int i = 0; i < SPU_KIT_SPUS; i++) c[i] = ((const volatile spu_kit_rec *)&recs[i])->count;
    s64 a = sysGetSystemTime();
    int naps = 0;
    do sysUsleep(10000);
    while (sysGetSystemTime() - a < MOVE_US && ++naps < 100);
    for (int i = 0; i < SPU_KIT_SPUS; i++)
        if (((const volatile spu_kit_rec *)&recs[i])->count != c[i]) return 1;
    return 0;
}

/* The run entry ends after the start line of the join or the terminate is
 * on disk: no gap between the two entries. */
static void run_close(void)
{
    if (run_open) journal_done(gf->run, 0, 1);   /* the group ran to its end without a freeze */
    run_open = 0;
}

/* After a join, for a family that reads them (ls): the six exit statuses,
 * after a terminate too. A thread that ended by itself before its first
 * report (0x200: the arguments, 0x300: the mode) is never waited for, so
 * only this read shows why. A thread the terminate stopped has no status: its
 * read fails (-1), so after a terminate the done line can have ok=0. That is
 * a thread's own result, not the run's failure. */
static void exit_statuses(spu_result *r)
{
    if (!gf->exit_status || !jgo(gf->exit_status)) return;
    s32 first = 0;
    for (int i = 0; i < SPU_KIT_SPUS; i++) {
        s32 e = sysSpuThreadGetExitStatus(threads[i], &r->exit_status[i]);
        if (e) r->exit_status[i] = -1;
        if (e && !first) first = e;
    }
    journal_done(gf->exit_status, first, first == 0);
}

static void join(spu_result *r)
{
    u32 cause = 0, status = 0;
    int go = jgo(gf->join);
    run_close();
    if (!go || !jend(gf->join, sysSpuThreadGroupJoin(group, &cause, &status))) return;
    grp = GROUP_IDLE;
    r->joined = 1;
    r->cause = cause;
    r->status = status;
    exit_statuses(r);                                /* before the destroy: the threads go with the group */
}

/* The terminate, then the join. The terminate's done line has ok=1 for
 * every rc: an error can mean the threads had ended. Then no count moved
 * in 100 ms, and the join follows; a count that moved means a thread still
 * runs: no join, no destroy, the group stays up (stuck), no other start
 * follows, and spu_stuck() gives 1 until the app exits. */
static void stop(spu_result *r)
{
    int go = jgo(gf->terminate);
    run_close();
    if (!go) return;
    s32 rc = sysSpuThreadGroupTerminate(group, SPU_KIT_TERM_VALUE);
    journal_done(gf->terminate, rc, 1);
    r->terminated = 1;
    r->term_rc = rc;
    if (rc != 0 && counts_moved()) {
        r->stuck = stuck = 1;
        return;
    }
    join(r);
}

/* After the group ended: the destroy, and the image close once no thread can run. */
static void teardown(void)
{
    if (grp == GROUP_IDLE && jgo(gf->destroy) && jend(gf->destroy, sysSpuThreadGroupDestroy(group))) grp = GROUP_NONE;
    if (grp != GROUP_RUNNING) image_close();
}

/* The end of every use: the reports and the time into r, and why the run
 * did not reach its end (a late SPU's text, written at the deadline, stays
 * unless one of these replaces it). */
static int end(spu_result *r)
{
    if (r->ran) {
        __sync_synchronize();
        memcpy(r->rec, recs, sizeof recs);
    }
    r->us = (u64)(sysGetSystemTime() - t0);
    if (r->stuck) snprintf(r->why, sizeof r->why, "the SPUs could not be stopped");
    else if (r->failed && r->skipped && !strcmp(r->failed, "group_still_up"))   /* not journal names: no journal_state */
        snprintf(r->why, sizeof r->why, "the SPUs of an earlier test are still up");
    else if (r->failed && r->skipped && !strcmp(r->failed, "group_not_destroyed"))
        snprintf(r->why, sizeof r->why, "the SPU group of an earlier test could not be destroyed");
    else if (r->failed && r->skipped && journal_state(r->failed) == J_SKIPPED) {
        r->declined = 1;
        snprintf(r->why, sizeof r->why, "skipped at the prompt");
    } else if (r->failed && r->skipped) {
        r->frozen = 1;
        snprintf(r->why, sizeof r->why, "%s froze this console before", r->failed);
    } else if (r->failed) snprintf(r->why, sizeof r->why, "%s failed: rc 0x%08x", r->failed, (unsigned)r->rc);
    return 0;
}

int spu_up(const spu_names *f, u32 mode, spu_result *r)
{
    memset(r, 0, sizeof *r);
    for (int i = 0; i < SPU_KIT_SPUS; i++) r->exit_status[i] = -1;
    R = r;
    t0 = sysGetSystemTime();
    const char *b = spu_blocked(f);
    if (b) {                                         /* no call */
        r->failed = b;
        r->skipped = 1;
        return end(r);
    }
    if (!initialized) {                              /* once per process */
        if (!jgo("spu_init") || !jend("spu_init", sysSpuInitialize(SPU_KIT_SPUS, 0))) return end(r);
        initialized = 1;
    }
    if (!jgo("spu_import") || !jend("spu_import", sysSpuImageImport(&image, spu_kit_spu, SPU_IMAGE_PROTECT))) return end(r);
    imported = 1;
    gf = f;
    memset(recs, 0, sizeof recs);                    /* before any thread runs */
    __sync_synchronize();
    if (!jgo(f->group) || !jend(f->group, group_make(mode))) {
        image_close();
        return end(r);
    }
    if (!jgo(f->start) || !jend(f->start, group_start())) {
        if (!jgo(f->destroy) || !jend(f->destroy, sysSpuThreadGroupDestroy(group))) grp = GROUP_IDLE;   /* the group stays */
        image_close();
        return end(r);
    }
    grp = GROUP_RUNNING;
    r->ran = 1;
    int missing;
    r->started = first_reports(&missing);
    r->late = r->started < SPU_KIT_SPUS;
    if (r->late) snprintf(r->why, sizeof r->why, "SPU %d did not report in %d s", missing, FIRST_US / 1000000);
    if (r->late || !run_open) {                      /* stop() ends the run entry */
        stop(r);
        s32 st = r->late ? r->exit_status[missing] : -1;   /* read after the join (ls): it ended at once */
        if (st == SPU_KIT_EXIT_ARGS || st == SPU_KIT_EXIT_MODE)
            snprintf(r->why, sizeof r->why, "SPU %d ended with status 0x%x before its first report", missing, (unsigned)st);
        teardown();
        return end(r);
    }
    return 1;
}

int spu_wait_done(s64 us)
{
    s64 a = sysGetSystemTime();
    for (;;) {
        u32 passes = 0;
        for (int i = 0; i < SPU_KIT_SPUS; i++) {
            u64 c = ((const volatile spu_kit_rec *)&recs[i])->count;
            passes += c < SPU_KIT_LS_PASSES ? (u32)c : SPU_KIT_LS_PASSES;
        }
        job_set_percent((int)(passes * 100 / (SPU_KIT_SPUS * SPU_KIT_LS_PASSES)));
        if (done_count() == SPU_KIT_SPUS) return 1;
        if (sysGetSystemTime() - a > us) return 0;
        sysUsleep(1000);
    }
}

void spu_down(spu_result *r)
{
    R = r;
    if (grp == GROUP_RUNNING) {                      /* join() or stop() ends the run entry */
        if (done_count() == SPU_KIT_SPUS) join(r);   /* each thread put its last report and ends: the join returns */
        else stop(r);
        teardown();
    }
    end(r);
}

void spu_last_resort(void)
{
    static spu_result last;
    if (grp != GROUP_RUNNING) return;
    memset(&last, 0, sizeof last);
    R = &last;
    stop(&last);                                     /* it ends the run entry too */
    teardown();
}
