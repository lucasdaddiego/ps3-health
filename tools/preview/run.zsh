#!/bin/zsh
# Preview scenarios: each runs the app in a fresh sandbox with a pad script.
#   zsh tools/preview/run.zsh [scenario]     (default: all; names in the case below)
# Snapshots go to build/out/*.png. The drive answers from the dumps in
# $PV_DRIVE (default ~/.local/share/ps3-health/drive); without them the Drive
# scenarios show the probe screen.
P=${0:A:h}
OUT=$P/build/out
DRIVE=${PV_DRIVE:-$HOME/.local/share/ps3-health/drive}
[[ -x $P/build/pv ]] || { print -u2 "run build.zsh first"; exit 1 }
[[ -f $DRIVE/identify.bin ]] || print -u2 "no dumps in $DRIVE: the Drive scenarios show the probe screen"
mkdir -p $OUT
J_DONE=$'done error_log rc=0x00000000 ok=1\ndone gpl_devstat rc=0x00000000 ok=1\ndone gpl_phy rc=0x00000000 ok=1\ndone fan_policy rc=0x00000000 ok=1\ndone spu_import rc=0x00000000 ok=1\n'
J_SPU=$'done spu_import rc=0x00000000 ok=1\n'   # the SPU prompt answered: the Cooling scenarios before 2.1.0 ask only for the fan
J_FAN=$'done fan_policy rc=0x00000000 ok=1\n'    # the fan prompt answered: the SPU prompt comes at the first load test
MEM='4:DOWN 6:RIGHT 8:RIGHT 10:CROSS'   # home to the Memory module
COOL='4:RIGHT 6:CROSS'                  # home to the Cooling module
SBX=$P/build/sbx
DEF_SAFE='67 38 1853 1042'
# trend.txt of the test SSD: readings kept by hand, in local time (the preview's clock adds 2026-10-08 09:00)
TREND_SEED=$'drive Dahua V800 2.5 inch SATA 1TB SSD\n2026-10-06 16:52 918 38 72018 100 0\n2026-10-06 19:45 921 36 72132 100 0\n2026-10-07 19:52 934 36 72607 100 0\n'
scn() {   # name script [VAR=value ...]; SAFE, STATE, JOURNAL, TREND, DEMO from the caller
    local name=$1 script=$2
    shift 2
    local sbx=$P/build/sbx/$name
    [[ -d $sbx ]] && chmod -R u+w $sbx
    rm -rf $sbx
    mkdir -p $sbx/dev_hdd0/tmp/ps3_health $sbx/dev_flash/vsh/etc $sbx/dev_usb000
    print 'release:04.9300:' > $sbx/dev_flash/vsh/etc/version.txt
    [[ -n $SAFE ]] && print -r -- "$SAFE" > $sbx/dev_hdd0/tmp/ps3_health/safe_area.txt
    [[ -n $STATE ]] && print -rn -- "$STATE" > $sbx/dev_hdd0/tmp/ps3_health/state.txt
    [[ -n $JOURNAL ]] && print -rn -- "$JOURNAL" > $sbx/dev_hdd0/tmp/ps3_health/journal.txt
    [[ -n $TREND ]] && print -rn -- "$TREND" > $sbx/dev_hdd0/tmp/ps3_health/trend.txt
    [[ -n $TREND_RO ]] && chmod 200 $sbx/dev_hdd0/tmp/ps3_health/trend.txt
    [[ -n $NOFS ]] && chmod 555 $sbx/dev_hdd0/tmp/ps3_health
    if [[ -n $DEMO ]]; then mkdir -p $sbx/dev_hdd0/tmp/ps3_health/demo; cp $DRIVE/*.bin $sbx/dev_hdd0/tmp/ps3_health/demo/ 2>/dev/null; fi
    env PV_ROOT=$sbx PV_DRIVE=$DRIVE PV_OUT=$OUT PV_SCRIPT="$script" "$@" $P/build/pv 2>&1 | grep -v '^snap' | head -5
    print -- "$name: exit ${pipestatus[1]}"
}
again() {   # name script: the next session in the sandbox of the last scn of name, as that run left it
    env PV_ROOT=$P/build/sbx/$1 PV_DRIVE=$DRIVE PV_OUT=$OUT PV_SCRIPT="$2" $P/build/pv 2>&1 | grep -v '^snap' | head -5
    print -- "$1 again: exit ${pipestatus[1]}"
}
case ${1:-all} in
first|all)
    SAFE= STATE= JOURNAL= scn first "3:SNAP=calib_first 4:SQUARE 8:SNAP=calib_single 9:CIRCLE 14:SNAP=home_empty 15:EXIT" ;|
drive|all)
    SAFE=$DEF_SAFE STATE= JOURNAL= scn drive "4:CROSS 12:SNAP=drive_start 14:CROSS 80:SNAP=prompt_errlog 82:CROSS 160:SNAP=prompt_gpl 162:CROSS 260:SNAP=drive_attr 262:R1 270:SNAP=drive_attr2 272:RIGHT 280:SNAP=drive_summary 282:LEFT 296:TRIANGLE 306:SNAP=selftest_choice 308:CIRCLE 316:CIRCLE 326:SNAP=home_drive 328:TRIANGLE 340:SNAP=help_drive 342:CIRCLE 350:SNAP=home_after_help 351:EXIT" ;|
running|all)
    SAFE=$DEF_SAFE STATE= JOURNAL=$J_DONE scn running "4:CROSS 12:CROSS 120:SNAP=drive_running 122:TRIANGLE 132:SNAP=selftest_screen 133:EXIT" PV_ST=F3 ;|
probe|all)
    SAFE=$DEF_SAFE STATE= JOURNAL= scn probe "4:CROSS 12:CROSS 120:SNAP=probe 121:EXIT" PV_DRIVE=/nonexistent ;|
demo|all)
    SAFE=$DEF_SAFE STATE= JOURNAL= TREND=$TREND_SEED DEMO=1 scn demo "4:CROSS 12:TRIANGLE 80:SNAP=demo_attr 82:RIGHT 84:RIGHT 92:SNAP=demo_trend 93:EXIT"
    print -rn -- "$TREND_SEED" | cmp -s - $P/build/sbx/demo/dev_hdd0/tmp/ps3_health/trend.txt && print "demo: trend.txt unchanged" || print "demo: trend.txt CHANGED" ;|
cool|all)
    SAFE=$DEF_SAFE STATE= JOURNAL=$J_SPU scn cool "4:RIGHT 6:CROSS 14:SNAP=prompt_fan 16:CROSS 60:SNAP=cool_main 62:CROSS 3000:SNAP=cool_load 7400:SNAP=cool_result 7401:EXIT" PV_RISE=1 ;|
pad|all)
    SAFE=$DEF_SAFE STATE= JOURNAL= scn pad "4:RIGHT 6:RIGHT 8:CROSS 20-30:L2+UP+TRIANGLE+START 25:SNAP=pad_live 40-42:SELECT 41:CROSS 100:SNAP=pad_rest 300-302:SELECT 301:TRIANGLE 310-900:SPIN 600:SNAP=pad_circle 1000:SNAP=pad_after 1001:EXIT" PV_STICKS=3,-2,0,1 ;|
display|all)
    SAFE=$DEF_SAFE STATE= JOURNAL= scn display "4:DOWN 6:CROSS 14:SNAP=display_main 16:CROSS 24:SNAP=calib 26:CIRCLE 30:TRIANGLE 36:SNAP=pat_white 38:RIGHT 40:RIGHT 42:RIGHT 44:RIGHT 46:RIGHT 50:SNAP=pat_ramp 52:RIGHT 56:SNAP=pat_grid 58:RIGHT 62:SNAP=pat_stripes 64:RIGHT 68:SNAP=pat_lag 69:EXIT" ;|
mem|all)
    SAFE=$DEF_SAFE STATE= JOURNAL= scn mem "4:DOWN 6:RIGHT 8:RIGHT 10:CROSS 18:SNAP=mem_main 20:CROSS 40:SNAP=mem_job 2000:SNAP=mem_done 2001:EXIT" ;|
memrsx|all)
    SAFE=$DEF_SAFE STATE= JOURNAL= scn memrsx "4:DOWN 6:RIGHT 8:RIGHT 10:CROSS 20:TRIANGLE 1000:SNAP=mem_rsx 1002:CIRCLE 1010:SNAP=home_memrsx 1011:EXIT"
    grep -h "RSX memory" $P/build/sbx/memrsx/dev_hdd0/tmp/ps3_health/report-*.txt ;|
net|all)
    SAFE=$DEF_SAFE STATE= JOURNAL= scn net "4:DOWN 5:RIGHT 7:CROSS 15:SNAP=net_main 17:CROSS 400:SNAP=net_usb 402:TRIANGLE 500:SNAP=net_after 501:EXIT" ;|
home|all)
    SAFE=$DEF_SAFE JOURNAL= STATE=$'drive|1|2026-10-08|Dahua V800 1TB|Health OK, 934 h (38 d)\ncooling|2|2026-10-08|Cell 52>74 C, RSX 55>77 C|fan 33>45%, 2:00 load\npad|1|2026-10-08|Drift L 0.8%, R 1.2%|Circle L 96%, R 94%\ndisplay|1|2026-10-08|1920x1080p 16:9|Visible area 1786 x 1004\ntransfer|1|2026-10-08|USB 18/22 MB/s|net 94, LAN 812 Mbit/s\nmemory|3|2026-10-08|XDR 200 MB: 2 errors|RSX 196 MB: 0 errors\n' scn home "3:SNAP=home_filled 4:RIGHT 6:SNAP=home_sel 7:EXIT" ;|
p720|all)
    SAFE=$DEF_SAFE STATE= JOURNAL=$J_DONE scn p720 "3:SNAP=home_720 4:CROSS 12:CROSS 120:SNAP=drive_720 121:EXIT" PV_RES=720 ;|
memstop|all)
    SAFE=$DEF_SAFE STATE= JOURNAL= scn memstop "4:DOWN 6:RIGHT 8:RIGHT 10:CROSS 20:CROSS 40:CIRCLE 80:SNAP=mem_stopped 82:CIRCLE 90:SNAP=home_memstop 91:EXIT" ;|
coolstop|all)
    SAFE=$DEF_SAFE STATE= JOURNAL=$J_SPU scn coolstop "4:RIGHT 6:CROSS 16:CROSS 62:CROSS 300:CIRCLE 320:SNAP=cool_stopped 322:CIRCLE 330:SNAP=home_coolstop 331:EXIT" PV_RISE=1 ;|
demohome|all)
    SAFE=$DEF_SAFE STATE= JOURNAL= DEMO=1 scn demohome "4:CROSS 12:TRIANGLE 80:CIRCLE 90:CROSS 98:TRIANGLE 160:CIRCLE 170:SNAP=home_after_demo 171:EXIT"
    ls $P/build/sbx/demohome/dev_hdd0/tmp/ps3_health | grep report ;|
nofs|all)
    SAFE=$DEF_SAFE STATE= JOURNAL= NOFS=1 scn nofs "3:SNAP=nofs_home 4:SQUARE 6:SQUARE 8:SNAP=nofs_square 10:SELECT 10:SNAP=nofs_select 14:SNAP=nofs_usb 16:DOWN 17:RIGHT 19:CROSS 19:SNAP=nofs_open 27:CROSS 60:SNAP=nofs_net 61:EXIT" ;|
cool383|all)
    SAFE=$DEF_SAFE STATE= JOURNAL= scn cool383 "4:RIGHT 6:CROSS 16:CROSS 60:SNAP=cool_383 62:CIRCLE 70:EXIT" PV_383_RC=80010003
    grep -nE "cooling|383" $P/build/sbx/cool383/dev_hdd0/tmp/ps3_health/report-*.txt ;|
coolclamp|all)   # readings under the graph; readings past the stop at the first sample (96 and 100 C): the test ends at 0:00
    SAFE=$DEF_SAFE STATE= JOURNAL=$J_SPU scn coolcold "4:RIGHT 6:CROSS 16:CROSS 62:CROSS 1600:CIRCLE 1620:SNAP=cool_cold 1621:EXIT" PV_TEMP_OFF=-30
    SAFE=$DEF_SAFE STATE= JOURNAL=$J_SPU scn coolhot "4:RIGHT 6:CROSS 16:CROSS 62:CROSS 200:SNAP=cool_hot_first 201:EXIT" PV_TEMP_OFF=45
    grep -h -A2 "^SPU leg" $SBX/coolhot/dev_hdd0/tmp/ps3_health/report-*.txt ;|
selfexit|all)
    SAFE=$DEF_SAFE STATE= JOURNAL=$J_DONE scn selfexit "4:CROSS 12:CROSS 120:TRIANGLE 130:SELECT 140:SNAP=selftest_left 141:EXIT" PV_ST=F3
    ls $P/build/sbx/selfexit/dev_usb000 ;|
speedexit|all)
    SAFE=$DEF_SAFE STATE= JOURNAL=$J_DONE scn speedexit "4:CROSS 12:CROSS 120:R2 122:R2 200:TRIANGLE 210:SNAP=speed_left 211:EXIT" ;|
reprobe|all)
    SAFE=$DEF_SAFE STATE= JOURNAL= DEMO=1 scn reprobe "4:CROSS 12:CROSS 120:CIRCLE 130:CROSS 140:SNAP=reprobe_start 142:TRIANGLE 200:SNAP=reprobe_demo 201:EXIT" PV_DRIVE=/nonexistent ;|
help|all)
    SAFE=$DEF_SAFE STATE= JOURNAL= scn help "3:TRIANGLE 14:SNAP=help_empty 15:EXIT"
    SAFE='160 90 1760 990' STATE= JOURNAL= scn helpmin "3:SNAP=home_min 4:TRIANGLE 14:SNAP=help_min 15:EXIT" ;|
helpkept|all)
    SAFE=$DEF_SAFE STATE= JOURNAL=$J_DONE scn helpkept "4:CROSS 12:CROSS 120:CIRCLE 130:EXIT"
    again helpkept "3:TRIANGLE 14:SNAP=help_kept 15:EXIT" ;|
net_frozen|all)
    SAFE=$DEF_SAFE STATE= JOURNAL=$'start net_start\n' scn net_frozen "4:DOWN 5:RIGHT 7:CROSS 7:SNAP=net_frozen 9:TRIANGLE 40:SQUARE 80:SNAP=net_frozen_tests 81:EXIT"
    grep -h "froze" $P/build/sbx/net_frozen/dev_hdd0/tmp/ps3_health/report-*.txt
    cat $P/build/sbx/net_frozen/dev_hdd0/tmp/ps3_health/journal.txt ;|
copy_frozen|all)
    SAFE=$DEF_SAFE STATE= JOURNAL=$'start usb_copy\n' scn copy_frozen "3:SELECT 3:SNAP=copy_frozen_select 20:SNAP=copy_frozen 22:SQUARE 24:SQUARE 26:DOWN 27:RIGHT 29:CROSS 60:CIRCLE 70:SELECT 100:SNAP=copy_cleared 101:EXIT"
    cat $P/build/sbx/copy_frozen/dev_hdd0/tmp/ps3_health/journal.txt
    ls $P/build/sbx/copy_frozen/dev_usb000/ps3_health
    cat $P/build/sbx/copy_frozen/dev_usb000/ps3_health/journal.txt
    SAFE=$DEF_SAFE STATE= JOURNAL=$'start usb_copy\n' NOFS=1 scn copy_frozen_nofs "3:SELECT 10:SNAP=copy_frozen_nofs 11:EXIT"
    cat $P/build/sbx/copy_frozen_nofs/dev_hdd0/tmp/ps3_health/journal.txt ;|
trend|all)   # the open adds a line to the seed, CROSS another; SELECT copies trend.txt
    SAFE=$DEF_SAFE STATE= JOURNAL=$J_DONE TREND=$TREND_SEED scn trend "4:CROSS 12:CROSS 120:RIGHT 122:RIGHT 130:SNAP=trend 132:CROSS 240:SNAP=trend_reread 242:SELECT 270:SNAP=trend_usb 271:EXIT"
    cat $P/build/sbx/trend/dev_hdd0/tmp/ps3_health/trend.txt
    cmp -s $P/build/sbx/trend/dev_hdd0/tmp/ps3_health/trend.txt $P/build/sbx/trend/dev_usb000/ps3_health/trend.txt &&
        print "trend: the copy on the stick is the same" || print "trend: the copy on the stick DIFFERS" ;|
trend_1600|all)
    SAFE='160 90 1760 990' STATE= JOURNAL=$J_DONE TREND=$TREND_SEED scn trend_1600 "4:CROSS 12:CROSS 120:RIGHT 122:RIGHT 130:SNAP=trend_1600 131:EXIT" ;|
trend_newdrive|all)   # another drive's file: the open starts it again
    SAFE=$DEF_SAFE STATE= JOURNAL=$J_DONE TREND=$'drive Samsung SSD 870 EVO 1TB\n2026-09-01 10:00 100 30 5000 99 0\n' scn trend_newdrive "4:CROSS 12:CROSS 120:RIGHT 122:RIGHT 130:SNAP=trend_newdrive 131:EXIT"
    cat $P/build/sbx/trend_newdrive/dev_hdd0/tmp/ps3_health/trend.txt ;|
trend_one|all)   # no file yet
    SAFE=$DEF_SAFE STATE= JOURNAL=$J_DONE scn trend_one "4:CROSS 12:CROSS 120:RIGHT 122:RIGHT 130:SNAP=trend_one 131:EXIT"
    cat $P/build/sbx/trend_one/dev_hdd0/tmp/ps3_health/trend.txt ;|
trend_cut|all)   # over 8 KB: the first read and the tail; two unreadable lines; a last CR line with no line end (a cut write)
    seed=$'drive Dahua V800 2.5 inch SATA 1TB SSD\n2024-12-30 10:00 0 30 0 100 0\n'
    for i in {0..299}; do
        printf -v l '2025-%02d-%02d 10:00 %d %d %d 100 0\n' $((1 + i / 28)) $((1 + i % 28)) $((3 * i + 3)) $((30 + i % 9)) $((240 * i + 240))
        seed+=$l
        (( i == 290 )) && seed+=$'not a trend line\n2025-11-12 10:00 873 31\n'
    done
    seed+=$'2025-11-21 10:00 903 33 72240 100 0\r'
    SAFE=$DEF_SAFE STATE= JOURNAL=$J_DONE TREND=$seed scn trend_cut "4:CROSS 12:CROSS 120:RIGHT 122:RIGHT 130:SNAP=trend_cut 132:R1 134:DOWN 140:SNAP=trend_cut_scroll 141:EXIT"
    print "trend_cut: $(wc -c < $P/build/sbx/trend_cut/dev_hdd0/tmp/ps3_health/trend.txt | tr -d ' ') bytes" ;|
trend_ro|all)   # a trend.txt that does not read (mode 200): it stays as it is, and the page shows the rc
    SAFE=$DEF_SAFE STATE= JOURNAL=$J_DONE TREND=$TREND_SEED TREND_RO=1 scn trend_ro "4:CROSS 12:CROSS 120:RIGHT 122:RIGHT 130:SNAP=trend_ro 131:EXIT"
    chmod 600 $P/build/sbx/trend_ro/dev_hdd0/tmp/ps3_health/trend.txt
    print -rn -- "$TREND_SEED" | cmp -s - $P/build/sbx/trend_ro/dev_hdd0/tmp/ps3_health/trend.txt && print "trend_ro: trend.txt unchanged" || print "trend_ro: trend.txt CHANGED" ;|
spu_prompt|all)   # the SPU prompt at the first SQUARE, CROSS runs the test; the second SQUARE asks nothing
    SAFE=$DEF_SAFE STATE= JOURNAL= scn spu_prompt "$MEM 18:SQUARE 26:SNAP=spu_prompt 28:CROSS 1000:SQUARE 2000:SNAP=spu_prompt_after 2001:EXIT"
    sed 's/^/  /' $SBX/spu_prompt/dev_hdd0/tmp/ps3_health/journal.txt ;|
mem_ls|all)   # J_DONE: no prompt; the report after CIRCLE
    SAFE=$DEF_SAFE STATE= JOURNAL=$J_DONE scn mem_ls "$MEM 18:SQUARE 1000:SNAP=mem_ls 1002:CIRCLE 1010:SNAP=home_mem_ls 1011:EXIT"
    grep -h -A1 "SPU local store" $SBX/mem_ls/dev_hdd0/tmp/ps3_health/report-*.txt ;|
mem_ls_fault|all)   # one bit stuck at 1 in SPU 3's local store
    SAFE=$DEF_SAFE STATE= JOURNAL=$J_DONE scn mem_ls_fault "$MEM 18:SQUARE 1000:SNAP=mem_ls_fault 1002:CIRCLE 1010:SNAP=home_mem_ls_fault 1011:EXIT" PV_LS_FAULT=3
    grep -h -A2 "SPU local store" $SBX/mem_ls_fault/dev_hdd0/tmp/ps3_health/report-*.txt ;|
mem_ls_skip|all)   # CIRCLE at the prompt; SQUARE again shows the skip and asks nothing
    SAFE=$DEF_SAFE STATE= JOURNAL= scn mem_ls_skip "$MEM 18:SQUARE 26:CIRCLE 34:SNAP=mem_ls_skip 36:SQUARE 44:SNAP=mem_ls_skip_again 45:EXIT"
    sed 's/^/  /' $SBX/mem_ls_skip/dev_hdd0/tmp/ps3_health/journal.txt ;|
mem_ls_late|all)   # SPU 2 never reports: after 2 s the six are terminated and joined
    SAFE=$DEF_SAFE STATE= JOURNAL=$J_DONE scn mem_ls_late "$MEM 18:SQUARE 1000:SNAP=mem_ls_late 1002:CIRCLE 1010:EXIT" PV_SPU_LATE=2
    grep -h -A1 "SPU local store" $SBX/mem_ls_late/dev_hdd0/tmp/ps3_health/report-*.txt
    sed -n '/spu_init/,$s/^/  /p' $SBX/mem_ls_late/dev_hdd0/tmp/ps3_health/journal.txt ;|
mem_ls_stall|all)   # SPU 3 stops after pass 5: after 5 s the six are terminated and joined
    SAFE=$DEF_SAFE STATE= JOURNAL=$J_DONE scn mem_ls_stall "$MEM 18:SQUARE 1000:SNAP=mem_ls_stall 1002:CIRCLE 1010:EXIT" PV_SPU_STALL=3,5
    grep -h -A1 "SPU local store" $SBX/mem_ls_stall/dev_hdd0/tmp/ps3_health/report-*.txt
    sed -n '/spu_init/,$s/^/  /p' $SBX/mem_ls_stall/dev_hdd0/tmp/ps3_health/journal.txt ;|
mem_ls_nofs|all)   # no writable app folder: no journal, so no SPU call and no prompt
    SAFE=$DEF_SAFE STATE= JOURNAL= NOFS=1 scn mem_ls_nofs "$MEM 18:SQUARE 26:SNAP=mem_ls_nofs 27:EXIT" ;|
mem_ls_badarg|all)   # SPU 2 ends at once with 0x200 (a bad argument): the late path reads the statuses and names it
    SAFE=$DEF_SAFE STATE= JOURNAL=$J_DONE scn mem_ls_badarg "$MEM 18:SQUARE 1000:SNAP=mem_ls_badarg 1002:CIRCLE 1010:EXIT" PV_SPU_LATE=2,0x200
    grep -h -A1 "SPU local store" $SBX/mem_ls_badarg/dev_hdd0/tmp/ps3_health/report-*.txt
    sed -n '/ls_join/,$s/^/  /p' $SBX/mem_ls_badarg/dev_hdd0/tmp/ps3_health/journal.txt ;|
mem_ls_destroy|all)   # the destroy fails: the next SQUARE names it (no group runs), and makes no call
    SAFE=$DEF_SAFE STATE= JOURNAL=$J_DONE scn mem_ls_destroy "$MEM 18:SQUARE 1000:SNAP=mem_ls_destroy 1002:SQUARE 1010:SNAP=mem_ls_destroy_again 1012:CIRCLE 1020:EXIT" PV_SPU_FAIL=171
    grep -h -A1 "SPU local store" $SBX/mem_ls_destroy/dev_hdd0/tmp/ps3_health/report-*.txt | tail -2
    sed -n '/spu_init/,$s/^/  /p' $SBX/mem_ls_destroy/dev_hdd0/tmp/ps3_health/journal.txt ;|
cool_spu|all)   # the SPU prompt at the first load test, CROSS: the six SPUs beside the PPU threads and the RSX, 2 minutes
    SAFE=$DEF_SAFE STATE= JOURNAL=$J_FAN scn cool_spu "$COOL 50:CROSS 58:SNAP=cool_spu_prompt 60:CROSS 3060:SNAP=cool_spu_load 7400:SNAP=cool_spu 7402:CIRCLE 7410:SNAP=home_cool_spu 7411:EXIT" PV_RISE=1
    grep -h -A1 "^SPU leg" $SBX/cool_spu/dev_hdd0/tmp/ps3_health/report-*.txt | tail -2
    sed -n '/spu_import/,$s/^/  /p' $SBX/cool_spu/dev_hdd0/tmp/ps3_health/journal.txt ;|
cool_hot|all)   # the RSX passes 75 C near 0:34: the test stops by itself, a yellow result
    SAFE=$DEF_SAFE STATE= JOURNAL=$J_DONE scn cool_hot "$COOL 50:CROSS 1800:SNAP=cool_hot_load 2400:SNAP=cool_hot 2402:CIRCLE 2410:SNAP=home_cool_hot 2411:EXIT" PV_RISE=1 PV_TEMP_OFF=12
    grep -h -E "^(load test|SPU leg|stop|verdict)" $SBX/cool_hot/dev_hdd0/tmp/ps3_health/report-*.txt | tail -4 ;|
cool_stall|all)   # SPU 3 stops counting after 20 s: the test stops at the next sample
    SAFE=$DEF_SAFE STATE= JOURNAL=$J_DONE scn cool_stall "$COOL 50:CROSS 1800:SNAP=cool_stall 1802:CIRCLE 1810:SNAP=home_cool_stall 1811:EXIT" PV_RISE=1 PV_SPU_STALL=3,20
    grep -h -E "^(SPU leg|stop|verdict)" $SBX/cool_stall/dev_hdd0/tmp/ps3_health/report-*.txt | tail -3 ;|
cool_383fail|all)   # 383 fails from 40 s on: two failed reads in a row stop the test
    SAFE=$DEF_SAFE STATE= JOURNAL=$J_DONE scn cool_383fail "$COOL 50:CROSS 2900:SNAP=cool_383fail 2902:CIRCLE 2910:SNAP=home_cool_383fail 2911:EXIT" PV_RISE=1 PV_383_FAIL_AT=40
    grep -h -E "^(now|load test|stop|verdict)" $SBX/cool_383fail/dev_hdd0/tmp/ps3_health/report-*.txt | tail -4 ;|
cool_spu_fail|all)   # the group's start fails: the load test runs without the SPUs
    SAFE=$DEF_SAFE STATE= JOURNAL=$J_DONE scn cool_spu_fail "$COOL 50:CROSS 600:SNAP=cool_spu_fail_load 602:CIRCLE 620:SNAP=cool_spu_fail 621:EXIT" PV_RISE=1 PV_SPU_FAIL=173
    grep -h "^SPU leg" $SBX/cool_spu_fail/dev_hdd0/tmp/ps3_health/report-*.txt | tail -1
    sed -n '/spu_import/,$s/^/  /p' $SBX/cool_spu_fail/dev_hdd0/tmp/ps3_health/journal.txt ;|
cool_frozen|all)   # cool_run froze before: no SPU call and no job screen, the load test without them; Memory's SPU test still runs
    SAFE=$DEF_SAFE STATE= JOURNAL="${J_DONE}froze cool_run"$'\n' scn cool_frozen "$COOL 50:CROSS 600:SNAP=cool_frozen_load 602:CIRCLE 620:SNAP=cool_frozen 622:CIRCLE 630:DOWN 632:RIGHT 634:CROSS 650:SQUARE 1600:SNAP=cool_frozen_mem 1602:CIRCLE 1610:EXIT" PV_RISE=1
    grep -h -E "^SPU (leg|local store)" $SBX/cool_frozen/dev_hdd0/tmp/ps3_health/report-*.txt | tail -2
    sed -n '/froze/,$s/^/  /p' $SBX/cool_frozen/dev_hdd0/tmp/ps3_health/journal.txt ;|
cool_skip|all)   # CIRCLE at the SPU prompt: the load test runs without the SPUs, and TRIANGLE asks nothing
    SAFE=$DEF_SAFE STATE= JOURNAL=$J_FAN scn cool_skip "$COOL 50:CROSS 58:CIRCLE 400:SNAP=cool_skip_load 402:CIRCLE 420:TRIANGLE 440:SNAP=cool_skip_again 442:CIRCLE 460:SNAP=cool_skip 461:EXIT" PV_RISE=1
    grep -h "^SPU leg" $SBX/cool_skip/dev_hdd0/tmp/ps3_health/report-*.txt | tail -1
    sed 's/^/  /' $SBX/cool_skip/dev_hdd0/tmp/ps3_health/journal.txt ;|
cool_stuck|all)   # the terminate fails while the SPUs count: they stay up (red line); after SQUARE twice Memory's SQUARE asks nothing and runs nothing
    SAFE=$DEF_SAFE STATE= JOURNAL=$J_DONE scn cool_stuck "$COOL 50:CROSS 300:CIRCLE 340:SNAP=cool_stuck 342:CIRCLE 350:SQUARE 352:SQUARE 360:DOWN 362:RIGHT 364:CROSS 380:SQUARE 400:SNAP=cool_stuck_mem 402:SQUARE 420:SNAP=cool_stuck_mem_again 422:CIRCLE 430:EXIT" PV_RISE=1 PV_SPU_FAIL=177
    grep -h -E "^SPU (leg|local store)" $SBX/cool_stuck/dev_hdd0/tmp/ps3_health/report-*.txt | sort -u
    sed 's/^/  /' $SBX/cool_stuck/dev_hdd0/tmp/ps3_health/journal.txt ;|
cool_stuck_again|all)   # CROSS again while those SPUs still count: no load on top (the cards dimmed), the red line, the curve and "SPUs stuck" stay
    SAFE=$DEF_SAFE STATE= JOURNAL=$J_DONE scn cool_stuck_again "$COOL 50:CROSS 300:CIRCLE 342:CROSS 400:SNAP=cool_stuck_again 402:CIRCLE 410:SNAP=home_cool_stuck_again 411:EXIT" PV_RISE=1 PV_SPU_FAIL=177
    grep -h -E "^(load test|SPU leg|verdict)" $SBX/cool_stuck_again/dev_hdd0/tmp/ps3_health/report-*.txt
    cut -d'|' -f1,2,5 $SBX/cool_stuck_again/dev_hdd0/tmp/ps3_health/state.txt ;|
esac
${PYTHON:-python3} -I - "$OUT" <<'PY'
import glob, os, sys
from PIL import Image
for p in glob.glob(sys.argv[1] + '/*.ppm'):
    Image.open(p).save(p[:-4] + '.png'); os.remove(p)
PY
print "$(ls $OUT | wc -l | tr -d ' ') snapshots in ${OUT#${P:h:h}/}"
