/*
 * Port of neutrino's patch_rc_uya (rickgaiser, commit 088aad39), extended for OPL SMB:
 * stake the 321792-byte hole at 0x4C900 before OPL network modules allocate IOP RAM.
 */

#include <loadcore.h>
#include <sysmem.h>
#include <tamtypes.h>

#include "ioplib.h"

#define MODNAME "rcuyapatch"
IRX_ID(MODNAME, 1, 1);

#define UYA_BUF_SIZE 321792
#define UYA_BUF_ADDR ((void *)0x4c900)

typedef void *(*fp_AllocSysMemory)(int mode, int size, void *ptr);
typedef int (*fp_FreeSysMemory)(void *ptr);

static fp_AllocSysMemory org_AllocSysMemory;
static fp_FreeSysMemory org_FreeSysMemory;
static void *uya_hole;

static void *hooked_AllocSysMemory(int mode, int size, void *ptr)
{
    if (size == UYA_BUF_SIZE) {
        if (uya_hole != NULL)
            return uya_hole;
        return org_AllocSysMemory(ALLOC_ADDRESS, size, UYA_BUF_ADDR);
    }

    return org_AllocSysMemory(mode, size, ptr);
}

static int hooked_FreeSysMemory(void *ptr)
{
    /* Keep the staked hole for the game's lifetime. */
    if (uya_hole != NULL && ptr == uya_hole)
        return 0;

    return org_FreeSysMemory(ptr);
}

int _start(int argc, char **argv)
{
    iop_library_t *lib_sysmem;

    (void)argc;
    (void)argv;

    lib_sysmem = ioplib_getByName("sysmem");
    if (lib_sysmem == NULL)
        return MODULE_NO_RESIDENT_END;

    org_AllocSysMemory = ioplib_hookExportEntry(lib_sysmem, 4, hooked_AllocSysMemory);
    if (org_AllocSysMemory == NULL)
        return MODULE_NO_RESIDENT_END;

    org_FreeSysMemory = ioplib_hookExportEntry(lib_sysmem, 5, hooked_FreeSysMemory);
    ioplib_relinkExports(lib_sysmem);

    /* Claim 0x4C900 before SMSTCPIP/SMAP/smbinit (or other OPL modules) fragment IOP RAM. */
    uya_hole = org_AllocSysMemory(ALLOC_ADDRESS, UYA_BUF_SIZE, UYA_BUF_ADDR);

    return MODULE_RESIDENT_END;
}
