/* What the library's stub headers lack for this app, force-included into
 * every file of the preview by build.zsh (-include): the library's stub/
 * comes first on the include path, so a sysmodule/sysmodule.h here would
 * never be read. pv_app.c defines them. */
#ifndef PV_HEALTH_H
#define PV_HEALTH_H
#include "ppu-types.h"
s32 sysModuleUnload(u32 id);                 /* net_end: the netctl module */
s32 sysLv2FsRename(const char *from, const char *to);   /* boot.c: boot.txt to boot.old.txt at the cap */
#endif
