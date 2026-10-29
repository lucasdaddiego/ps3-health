# Preview on the host

The app's screens render on a Mac (or Linux) without a TV, through ps3gfx's
preview: `tools/preview/build.zsh` hands this app to the library's
`tools/preview/build.zsh` (the clone in `~/ps3dev/src/ps3gfx` that
`sdk_setup.sh` makes, or `$PS3GFX`), which compiles `source/*.c` and
`pv_app.c` with the library's own sources against stub PSL1GHT headers, on a
software back end with a scripted controller and a sandbox for the `/dev_*`
paths, into `build/pv`. The raster, the back end, the gfx traces
(`PV_GFXTRACE`), `diff.py` and the pad scripts are the library's: see its
`tools/preview/README.md`. A checkout at another commit than the one
`sdk_setup.sh` pins is announced ("preview on ps3gfx <sha>, the pkg links
<rev>"): the pkg links the pin.

Needs: zsh, a C compiler (`cc`), Python 3 with Pillow for the PNG step
(`PYTHON=...` picks the interpreter, as in `build.sh`).

```sh
zsh tools/preview/build.zsh            # build/pv; PV_CFLAGS adds cc flags
zsh tools/preview/run.zsh              # every scenario; or one: run.zsh drive
zsh tools/preview/shots.zsh            # the README screenshots, into docs/
python3 ~/ps3dev/src/ps3gfx/tools/preview/diff.py A B [D]   # pixel diff of two snapshot folders
```

`run.zsh` writes its snapshots to `build/out/*.png`; the scenario names and pad
scripts are in its `case`. Everything under `build/` is ignored by git. The
job threads run in real time, so a screen that shows a running job (`mem_job`)
can change from run to run: run twice and compare to see which snapshots
repeat.

## The app's part: pv_app.c

The library's world refuses every LV2 syscall and answers the network
offline. `pv_app.c` defines the strong `pv_syscall`: the drive (syscalls 600,
601, 609 and 616, answered from a console's sector dumps), the temperatures
(383), the fan duty (409), the six SPUs of the SPU kit (165, 169 to 173,
177, 178, and the image import and close), and the kernel's 1 MB pages of
the memory tests (348, 349: host memory from a pool, `PV_KMEM_MB`).
`stub/sys/spu.h`, `stub/lv2/spu.h` and `stub/sys/memory.h` (its address is
64 bits on the host) stand in for PSL1GHT's headers (`build.zsh` adds `stub/` to
the include path), so the driver's calls reach `pv_syscall`. One emulated
group: the start runs each thread's start at once, in the caller's thread.
The local-store mode runs to its end there, with the kit's kernel over a host
buffer of the area's size, and fills the reports as the SPU program does (lo
0x1980, the kit's at the pinned commit; the ticks a model of 0.1 s). The heat
mode puts its start report, then a host thread counts every report on the
preview's clock until the terminate. The join gives cause 2, or cause 4 and
the value after a terminate. A thread's exit status (165) answers only for a
thread that ended by itself, else LV2's ESTAT (0x8001000f); a thread that
ended before a terminate keeps its status here, which no console round has
checked yet.

The audio port and the Network module's world are here too. `stub/audio/audio.h`
stands in for PSL1GHT's `audio/audio.h` (the port config's two addresses are
64-bit, a host pointer does not fit the console's 32-bit ones); `pv_app.c`
answers the calls with a ring in host memory and a reader thread that moves
the read index on the preview's clock, so the module's fill thread runs as on
the console. The library's `net/netctl.h` has the address alone:
`stub/pv_health.h` sets its include guard and declares netctl as PSL1GHT has
it (the codes, the union), plus `SOCK_RAW` and `IPPROTO_ICMP`; `pv_app.c`
defines `netCtlGetInfo`, the sockets of the ping (an ICMP echo answered after
an IPv4 header, or a TCP connect refused by the router) and the DNS lookup,
which replace the library's weak offline ones. The preview's clock moves by
frames (1/60 s), so a ping takes whole frames: 16.7 ms, 33.3 ms. The layouts
are what the scenarios check, not the times.

The Clock module's time server and the Display module's TV page are here
too: `netSocket` gives a UDP socket when `PV_NTP` is set, `netSendTo` keeps
the 48-byte request, `netPoll` and `netRecvFrom` answer after `PV_NTP` frames
with a stratum 2 reply whose receive and transmit times are the preview's
clock plus `PV_NTP_OFF_MS` (the preview's wall clock moves by whole seconds,
so an offset comes out short by the frames in flight). `stub/pv_health.h`
declares `videoDeviceInfo` and `videoGetDeviceInfo` as PSL1GHT has them
(the library's `sysutil/video.h` has the video state alone), and `pv_app.c`
answers with an HDMI TV, or the rc of `PV_TV_RC`.

The library's stubs have `sysModuleLoad` and no `sysModuleUnload`, which
`net_end` and `audio_end` call: `stub/pv_health.h` declares it and `build.zsh` force-includes
it (`-include`; a `stub/sysmodule/sysmodule.h` here would come after the
library's on the include path), and `pv_app.c` defines it. The boot marks
need one more: `sysLv2FsRename`, for the move to `boot.old.txt` at the cap,
declared and defined the same way. It refuses an existing target, as lv2
does. `boot.txt` goes to the sandbox like every other file.

## Drive dumps

The emulated drive reads `identify.bin`, `smart.bin`, `thresh.bin`,
`selftest.bin`, `errlog.bin`, `devstat.bin`, `phy.bin` and `devinfo.bin` from
`$PV_DRIVE`, by default `~/.local/share/ps3/drive/`. Copy them from the
console's app folder (`/dev_hdd0/tmp/ps3_health/`, FTP). Without them the Drive
scenarios show the probe screen.

**Never commit the dumps.** `identify.bin` holds the drive's full serial
number. `shots.zsh` renders from a masked copy (serial, world wide name and
the reserved words 236-254 cleared, checksum fixed), so the screenshots show
`XXXXXXXXXXXXXXX`.

## Switches

`PV_SCRIPT`, `PV_ROOT`, `PV_OUT`, `PV_RES`, `PV_STICKS`, `PV_STAT`,
`PV_GFXTRACE` and `PV_MAXFRAMES` are the library's (its README). `pv_app.c`
adds:

| Variable | Effect |
|---|---|
| `PV_DRIVE` | the dumps (above) |
| `PV_ST=F3` | the self-test status byte (F3: running, 70 % done) |
| `PV_RISE=1` | temperatures that rise under load |
| `PV_TEMP_OFF=n` | add n degrees to every reading |
| `PV_383_RC=80010003` | syscall 383 refused with this rc |
| `PV_383_FAIL_AT=s` | syscall 383 refused (0x80010003) from second s of the preview's clock on |
| `PV_SPU_FAIL=n` | SPU syscall n (165 to 178) fails with 0x80010002 (177 while heat counts: the SPUs that could not be stopped) |
| `PV_SPU_LATE=i` | SPU i never reports: the driver's late path (the terminate, the join) |
| `PV_SPU_LATE=i,0x200` | SPU i ends at once with that status, before its first report (0x200 a bad argument, 0x300 a bad mode) |
| `PV_SPU_STALL=i,n` | local store: SPU i stops after pass n, without its last report (the driver's stop at 5 s); heat: SPU i stops counting after n s (the load test's stop) |
| `PV_HEAT_RATE=n` | heat: loops per microsecond per SPU (default 355, the design's estimate) |
| `PV_LS_FAULT=i` | one bit stuck at 1 in one word of SPU i's local store |
| `PV_AUDIO_CH=2` | the 8-channel audio port is refused: the module opens 2 channels |
| `PV_KMEM_MB=n` | the kernel's pool of 1 MB pages for syscalls 348 and 349 (the memory tests' blocks), default 256; 0 refuses them: the malloc fallback (`run.zsh mem_malloc`) |
| `PV_KMEM_STAT=1` | print the kernel pages still taken at exit: 0 after any test (`run.zsh mem_order`: the XDR test, then the quick check) |
| `PV_WAV=file` | the audio blocks as the port reads them, raw 32-bit float, interleaved (`run.zsh audio` checks it for clicks and stale blocks) |
| `PV_NET=wired` | netctl: `wired` (the default, 1000 Mbit/s full duplex), `wired100`, `wifi` (78 %, channel 6), `wifiweak` (31 %), `down` (no address) |
| `PV_PING=n[,k]` | the router answers each ping after n frames, every kth lost; `none`: no answer. Unset: no socket, offline |
| `PV_ICMP=refuse` | the raw socket is refused: the ping times TCP connects |
| `PV_DNS_MS=n` | a lookup of example.com (the Network module's name) answers after n ms; any other name, or unset: it fails |
| `PV_NTP=n` | the time server answers the Clock module's packet after n frames; `none`: no answer (the module's timeout); `kod`: a kiss-o'-death (RATE). Unset: no UDP socket. The server must be an address in `ntp.txt` (the resolver gives no list on the host) |
| `PV_NTP_OFF_MS=n` | the time server's clock minus the console's, in ms (default 0) |
| `PV_TV_RC=hex` | the TV data read (`videoGetDeviceInfo`) is refused with that rc; unset: an HDMI TV at 1080p, full-range RGB, Rec. 709 primaries, gamma 2.20, one HDCP key, eight modes |
