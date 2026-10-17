/* PS3 Health's stub of PSL1GHT's sys/memory.h for the host preview: the
 * kernel's 1 MB pages of the memory tests (source/mod_mem.c). The calls are
 * inline LV2 syscalls, as on the console, so they reach pv_syscall:
 * ../pv_app.c answers 348 and 349 from a pool of host memory. The address is
 * 64 bits here (u32 on the console): a host pointer does not fit in 32. */
#ifndef PV_SYS_MEMORY_H
#define PV_SYS_MEMORY_H
#include <stddef.h>
#include <ppu-types.h>
#include <ppu-lv2.h>

typedef u64 sys_mem_addr_t;

#define SYS_MEMORY_PAGE_SIZE_1M  0x0000000000000400ULL
#define SYS_MEMORY_PAGE_SIZE_64K 0x0000000000000200ULL

static inline s32 sysMemoryAllocate(size_t size, u64 flags, sys_mem_addr_t *alloc_addr)
{
    lv2syscall3(348, size, flags, (u64)(uintptr_t)alloc_addr);
    return_to_user_prog(s32);
}

static inline s32 sysMemoryFree(sys_mem_addr_t start_addr)
{
    lv2syscall1(349, start_addr);
    return_to_user_prog(s32);
}
#endif
