/*
 * Port of neutrino's patch_rc_uya (rickgaiser, commit 088aad39), extended for OPL SMB:
 * stake the 321792-byte hole at 0x4C900 before OPL network modules allocate IOP RAM.
 */

#include <intrman.h>
#include <loadcore.h>
#include <sysmem.h>
#include <tamtypes.h>

#include "ioplib.h"

#define MODNAME "rcuyapatch"
IRX_ID(MODNAME, 1, 2);

#define UYA_BUF_SIZE 321792
#define UYA_BUF_ADDR ((void *)0x4c900)

typedef void *(*fp_AllocSysMemory)(int mode, int size, void *ptr);
typedef int (*fp_FreeSysMemory)(void *ptr);

static fp_AllocSysMemory org_AllocSysMemory;
static void *uya_hole;

static void *hooked_AllocSysMemory(int mode, int size, void *ptr)
{
    if (size == UYA_BUF_SIZE &&
        (mode == ALLOC_FIRST || mode == ALLOC_LAST ||
         (mode == ALLOC_ADDRESS && ptr == UYA_BUF_ADDR))) {
        void *reserved;
        int oldstate;

        /* 预留块只能交出一次；并发或重复申请必须由 sysmem 检查地址是否仍被占用。 */
        CpuSuspendIntr(&oldstate);
        reserved = uya_hole;
        uya_hole = NULL;
        CpuResumeIntr(oldstate);

        if (reserved != NULL)
            return reserved;
        return org_AllocSysMemory(ALLOC_ADDRESS, size, UYA_BUF_ADDR);
    }

    return org_AllocSysMemory(mode, size, ptr);
}

int _start(int argc, char **argv)
{
    iop_library_t *lib_sysmem;
    fp_AllocSysMemory previous_alloc;
    fp_FreeSysMemory free_sys_memory;

    (void)argc;
    (void)argv;

    lib_sysmem = ioplib_getByName("sysmem");
    if (ioplib_getTableSize(lib_sysmem) <= 5)
        return MODULE_NO_RESIDENT_END;

    org_AllocSysMemory = lib_sysmem->exports[4];
    free_sys_memory = lib_sysmem->exports[5];

    /* 预留失败时不能安装半生效的钩子，也不能退到游戏仍会误用的其他地址。 */
    uya_hole = org_AllocSysMemory(ALLOC_ADDRESS, UYA_BUF_SIZE, UYA_BUF_ADDR);
    if (uya_hole == NULL) {
        Kprintf(MODNAME ": cannot reserve multiplayer buffer at 0x4c900\n");
        return MODULE_NO_RESIDENT_END;
    }

    previous_alloc = ioplib_hookExportEntry(lib_sysmem, 4, hooked_AllocSysMemory);
    if (previous_alloc == NULL) {
        free_sys_memory(uya_hole);
        uya_hole = NULL;
        return MODULE_NO_RESIDENT_END;
    }
    org_AllocSysMemory = previous_alloc;
    ioplib_relinkExports(lib_sysmem);

    /* 游戏获得缓冲区后可正常释放；不再通过 FreeSysMemory 假报成功来永久扣留内存。 */
    return MODULE_RESIDENT_END;
}
