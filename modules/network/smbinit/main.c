#include <loadcore.h>
#include <irx.h>

#include "oplsmb.h"
#include "smbauth.h"

#define MODNAME "smb_init"
IRX_ID(MODNAME, 1, 1);

int _start(int argc, char *argv[])
{
    smb_NegotiateProt(&SmbInitHashPassword);

    /* 认证用的 MD4 和 DES 随这个模块卸掉，不占游戏存档要的 IOP 堆。 */
    return MODULE_NO_RESIDENT_END;
}
