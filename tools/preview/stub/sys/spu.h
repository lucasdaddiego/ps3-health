/* PS3 Health's stub of PSL1GHT's sys/spu.h for the host preview: the types,
 * constants and calls of the SPU driver (source/spu_run.c). The calls are
 * inline LV2 syscalls, as on the console, so they reach pv_syscall:
 * ../pv_app.c answers them and runs the SPU kit's local-store kernel in
 * place of the SPUs. */
#ifndef PV_SYS_SPU_H
#define PV_SYS_SPU_H
#include <stdint.h>
#include <ppu-types.h>
#include <ppu-lv2.h>
#include <lv2/spu.h>

typedef u32 sys_spu_group_t;
typedef u32 sys_spu_thread_t;

#define SPU_THREAD_GROUP_TYPE_NORMAL           0x0
#define SPU_THREAD_GROUP_JOIN_GROUP_EXIT       0x0001
#define SPU_THREAD_GROUP_JOIN_ALL_THREADS_EXIT 0x0002
#define SPU_THREAD_GROUP_JOIN_TERMINATED       0x0004
#define SPU_THREAD_ATTR_NONE                   0x00

typedef struct { u64 arg0, arg1, arg2, arg3; } sysSpuThreadArgument;
typedef struct { const char *name; u32 nsize; u32 option; } sysSpuThreadAttribute;
typedef struct { u32 nsize; const char *name; u32 type; union { u32 ct; } option; } sysSpuThreadGroupAttribute;

#define PV_P(x) ((u64)(uintptr_t)(x))
static inline s32 sysSpuInitialize(u32 spus, u32 rawspus) { lv2syscall2(169, spus, rawspus); return_to_user_prog(s32); }
static inline s32 sysSpuThreadGroupCreate(sys_spu_group_t *group, u32 num, u32 prio, sysSpuThreadGroupAttribute *attr)
{ lv2syscall4(170, PV_P(group), num, prio, PV_P(attr)); return_to_user_prog(s32); }
static inline s32 sysSpuThreadGroupDestroy(sys_spu_group_t group) { lv2syscall1(171, group); return_to_user_prog(s32); }
static inline s32 sysSpuThreadInitialize(sys_spu_thread_t *thread, sys_spu_group_t group, u32 spu, sysSpuImage *image,
                                         sysSpuThreadAttribute *attr, sysSpuThreadArgument *arg)
{ lv2syscall6(172, PV_P(thread), group, spu, PV_P(image), PV_P(attr), PV_P(arg)); return_to_user_prog(s32); }
static inline s32 sysSpuThreadGroupStart(sys_spu_group_t group) { lv2syscall1(173, group); return_to_user_prog(s32); }
static inline s32 sysSpuThreadGroupTerminate(sys_spu_group_t group, u32 value) { lv2syscall2(177, group, value); return_to_user_prog(s32); }
static inline s32 sysSpuThreadGroupJoin(sys_spu_group_t group, u32 *cause, u32 *status)
{ lv2syscall3(178, group, PV_P(cause), PV_P(status)); return_to_user_prog(s32); }
static inline s32 sysSpuThreadGetExitStatus(sys_spu_thread_t thread, s32 *status) { lv2syscall2(165, thread, PV_P(status)); return_to_user_prog(s32); }
#endif
