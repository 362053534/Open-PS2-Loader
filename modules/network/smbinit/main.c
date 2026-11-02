#include <loadcore.h>
#include <irx.h>

#include "oplsmb.h"
#include "smbauth.h"

#define MODNAME "smb_init"
IRX_ID(MODNAME, 1, 1);

int _start(int argc, char *argv[])
{
    smb_NegotiateProt(&SmbInitHashPassword);

    /* 散列已经留在 cdvdman。这份带 MD4 的模块卸掉，腾出游戏存档要的 IOP 堆。 */
    return MODULE_NO_RESIDENT_END;
}
