#!/bin/zsh
# Make $PS3DEV (default ~/ps3dev) an SDK whose apps start on this console:
# the ps3dev nightly-2026-07-26 bundle (host tools, ppu-gcc 7.2) with PSL1GHT's
# 2020 runtime (6e565a7), pinned to a full commit hash: a newer PSL1GHT runtime
# does not start on this console. Then ps3gfx, the renderer and the frame the
# app draws through, at the commit PS3GFX_REV pins, installed into the bundle's
# portlibs/ppu: the only thing the app uses from there.
# Needs: zsh, curl, git, make, shasum (macOS arm64/x86_64 or Linux x86_64).
# PS3DEV_ARCHIVE=<folder>: an offline source instead of the internet, with
# bundles/<the bundle file>, src/PSL1GHT.bundle and src/ps3gfx.bundle (git
# bundles); the checksum and the two pins are checked the same way.
# PS3DEV_SKIP_PS3GFX=1: no ps3gfx step (CI: the job installs its own checkout).
#
# A step whose stamp names its pin is skipped: the runtime when
# $PS3DEV/.runtime-2020 names PSL1GHT_REV, ps3gfx when
# portlibs/ppu/share/ps3gfx/COMMIT is PS3GFX_REV and $PS3DEV/src/ps3gfx is a
# clone at that commit. So a run on a finished SDK clones, builds and installs
# nothing: it only sets the exports.
#
# Apps linked with the bundle's own runtime (PSL1GHT 2021+) die before main on
# HFW 4.93 + PS3HEN 3.6.0: black screen, back to the XMB after ~10 s, even a
# main() that only sleeps 60 s (2026-10-06). The same one-line app with the
# 2020 runtime runs. The 2021 changes are the heap rewrite (d2ea732) and a
# pre-main strdup for chdir/getcwd (99dd0b9); which one is to blame is untested.
#
# Run it, or source it to keep PS3DEV, PSL1GHT and PATH in the shell (the
# console loop). The body is an anonymous function with ERR_RETURN and its own
# options: a failure returns from it and never closes the shell that sources
# it, and the temporary folder goes in every case. The two pins stay at column
# 0: build.sh and the other apps' scripts read PS3GFX_REV with sed.
() {
emulate -L zsh -o err_return
local bundle sum from src stamp PSL1GHT_REV PS3GFX_REV tmp=$(mktemp -d)
{
export PS3DEV=${PS3DEV:-$HOME/ps3dev}
export PSL1GHT=$PS3DEV
export PATH=$PS3DEV/bin:$PS3DEV/ppu/bin:$PATH

if [[ ! -x $PS3DEV/ppu/bin/ppu-gcc ]]; then
  [[ ${PS3DEV:t} == ps3dev ]] || { print -u2 "PS3DEV must end in /ps3dev (the bundle extracts as ps3dev/)"; return 1 }
  case "$(uname -s)-$(uname -m)" in
    Darwin-arm64)  bundle=ps3dev-macos-ARM64.tar.gz; sum=cf69b6b520a871479dea2b734edcd5023e5ee472f7d28907fc6aaa6ecdf6ae77 ;;
    Darwin-x86_64) bundle=ps3dev-macos-X64.tar.gz;   sum=30c6189ed0ecfaa0af1941299215c3cc2063ec6babed3dcb83aaa41cc59b39c7 ;;
    Linux-x86_64)  bundle=ps3dev-linux-X64.tar.gz;   sum=dcbed747e094c6a382dae5b0aacc322a1d4390d9f2849a7d750a96ea398ee8ab ;;
    *) print -u2 "no ps3dev nightly-2026-07-26 bundle for $(uname -s) $(uname -m)"; return 1 ;;
  esac
  if [[ -n $PS3DEV_ARCHIVE ]]; then cp $PS3DEV_ARCHIVE/bundles/$bundle $tmp/$bundle
  else curl -fL --retry 3 -o $tmp/$bundle https://github.com/ps3dev/ps3dev/releases/download/nightly-2026-07-26/$bundle; fi
  [[ $(shasum -a 256 $tmp/$bundle | cut -d' ' -f1) == $sum ]] \
    || { print -u2 "bundle checksum mismatch for $bundle"; return 1 }
  mkdir -p ${PS3DEV:h}
  rm -f $PS3DEV/.runtime-2020                # the bundle brings its own runtime: the 2020 one goes in again
  tar xzf $tmp/$bundle -C ${PS3DEV:h}
fi

PSL1GHT_REV=6e565a70e927f55813babe86dc6f64149535f908   # 2020-11-25
if [[ -f $PS3DEV/.runtime-2020 && $(<$PS3DEV/.runtime-2020) == "PSL1GHT ${PSL1GHT_REV[1,7]},"* ]]; then
  print "$PS3DEV: runtime PSL1GHT ${PSL1GHT_REV[1,7]} (2020), already installed"
else
  from=https://github.com/ps3dev/PSL1GHT; [[ -n $PS3DEV_ARCHIVE ]] && from=$PS3DEV_ARCHIVE/src/PSL1GHT.bundle
  git clone -q $from $tmp/psl1ght
  git -C $tmp/psl1ght checkout -q $PSL1GHT_REV
  make -C $tmp/psl1ght/ppu --no-print-directory > $tmp/psl1ght.log 2>&1 || { tail -20 $tmp/psl1ght.log; return 1 }
  make -C $tmp/psl1ght/ppu install --no-print-directory > /dev/null
  print "PSL1GHT ${PSL1GHT_REV[1,7]}, $(date -u +%F)" > $PS3DEV/.runtime-2020
  print "$PS3DEV: runtime PSL1GHT ${PSL1GHT_REV[1,7]} (2020)"
fi

# ps3gfx, the library the app draws through (github.com/lucasdaddiego/ps3gfx):
# a clone at the pinned commit, kept in $PS3DEV/src so that the host preview
# builds against the same sources, then its make install into portlibs/ppu (the
# archive, the <ps3gfx/*.h> headers, OFL.txt and the COMMIT stamp that build.sh
# checks).
PS3GFX_REV=047e850cff3ef438a20e95a157d777f513e65b42   # ps3gfx main, 2026-10-31: image.h, a PNG or JPEG into texels (Couch 0.6.0's reader photo; Health's ELF byte-identical, image.o is not linked); 2026-10-17 (second round): the second review's fixes (white texels at alpha 0, the pad read through a job, the trimmed icons and their new ids, a gfx_prim per 64 glyphs, the five-store rain quads, the rain's sum on request (p.rain.check), the glass from the band's own cloud samples, the copy's first dcbf only; every ELF changes); 2026-10-17: a review's fixes (the journal: names to their first space, a failed record written again; gesture fires only for its own buttons, read_pad releases a gone pad's buttons; a byte past ASCII is '?'; ui_shot by the RSX copy; fx.h at repeated points; spu_fx's loops unrolled; every ELF changes); 2026-10-16: gfx_copy_to_main, the RSX copy to main memory (Health's RSX test reads through it; every ELF changes); ui_init times its steps (ui_init_us; Health's ELF changes); 2026-10-15: the journal reads its whole history, fs reports fsync and close errors (Health's ELF changes); 2026-10-14: bilinear icons at every size, crisper spu_fx clouds; the anti-aliased line helpers; 2026-10-13: Couch 0.3.0's additions (the display digits, the Phosphor icons, fx.h, the effects program spu_fx), after the SPU kit; Health's ELF byte-identical
src=$PS3DEV/src/ps3gfx stamp=$PS3DEV/portlibs/ppu/share/ps3gfx/COMMIT
if [[ -n $PS3DEV_SKIP_PS3GFX ]]; then
  print "$PS3DEV: no ps3gfx step (PS3DEV_SKIP_PS3GFX)"
elif [[ -f $stamp && $(<$stamp) == $PS3GFX_REV && $(git -C $src rev-parse -q --verify HEAD 2>/dev/null) == $PS3GFX_REV ]]; then
  print "$PS3DEV: ps3gfx ${PS3GFX_REV[1,7]} in portlibs/ppu, already installed"
else
  rm -rf $src
  from=https://github.com/lucasdaddiego/ps3gfx; [[ -n $PS3DEV_ARCHIVE ]] && from=$PS3DEV_ARCHIVE/src/ps3gfx.bundle
  git clone -q $from $src && git -C $src checkout -q $PS3GFX_REV
  [[ $(git -C $src rev-parse HEAD) == $PS3GFX_REV ]] || { print -u2 "ps3gfx is not at $PS3GFX_REV"; return 1 }
  make --no-print-directory -C $src install > /dev/null
  print "$PS3DEV: ps3gfx ${PS3GFX_REV[1,7]} in portlibs/ppu"
fi
} always { rm -rf $tmp }
}
