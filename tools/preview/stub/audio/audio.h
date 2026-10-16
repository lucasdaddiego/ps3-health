/* PS3 Health's stub of PSL1GHT's audio/audio.h for the host preview: the
 * types, constants and calls of the Audio module (source/mod_audio.c).
 * ../pv_app.c answers the calls with a ring that the preview's clock reads.
 * The two addresses of the port's config are 64-bit here: a host pointer
 * does not fit the console's 32-bit ones. */
#ifndef PV_AUDIO_H
#define PV_AUDIO_H
#include <ppu-types.h>

#define AUDIO_BLOCK_SAMPLES  256
#define AUDIO_STATUS_READY   1
#define AUDIO_STATUS_RUN     2
#define AUDIO_STATUS_CLOSE   0x1010
#define AUDIO_PORT_2CH       2
#define AUDIO_PORT_8CH       8
#define AUDIO_PORT_INITLEVEL 0x1000
#define AUDIO_BLOCK_8        8
#define AUDIO_BLOCK_16       16
#define AUDIO_BLOCK_32       32

typedef struct {
    u64 numChannels, numBlocks, attrib;
    float level;
} audioPortParam;

typedef struct {
    u64 readIndex;                           /* the address of the u64 block index the system reads */
    u32 status;
    u64 channelCount, numBlocks;
    u32 portSize;
    u64 audioDataStart;                      /* the address of block 0 */
} audioPortConfig;

s32 audioInit(void);
s32 audioQuit(void);
s32 audioPortOpen(audioPortParam *param, u32 *portNum);
s32 audioPortStart(u32 portNum);
s32 audioPortStop(u32 portNum);
s32 audioGetPortConfig(u32 portNum, audioPortConfig *config);
s32 audioPortClose(u32 portNum);
#endif
