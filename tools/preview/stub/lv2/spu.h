/* PS3 Health's stub of PSL1GHT's lv2/spu.h for the host preview. The image
 * calls are SPRX imports on the console (the firmware's liblv2), not
 * syscalls: ../pv_app.c defines them. */
#ifndef PV_LV2_SPU_H
#define PV_LV2_SPU_H
#include <ppu-types.h>
#define SPU_IMAGE_PROTECT 0x00
typedef struct { u32 type, entryPoint, segments, segmentCount; } sysSpuImage;
s32 sysSpuImageImport(sysSpuImage *image, const void *elf, u32 type);
s32 sysSpuImageClose(sysSpuImage *image);
#endif
