#include <tamtypes.h>
#include <ps2lib_err.h>
#include <kernel.h>
#include <sifrpc.h>
#include <string.h>

#include "cd_igr_rpc.h"

int oplIGRGetSMB2Diag(smb2_diag_t *diag)
{
    SifRpcClientData_t _igr_cd __attribute__((aligned(64)));
    smb2_diag_t rpcData __attribute__((aligned(64)));
    int r;

    if (!diag)
        return -E_SIF_RPC_CALL;

    _igr_cd.server = NULL;
    while ((r = SifBindRpc(&_igr_cd, 0x80000598, 0)) >= 0 && (!_igr_cd.server))
        nopdelay();

    if (r < 0)
        return -E_SIF_RPC_BIND;

    if (SifCallRpc(&_igr_cd, 2, 0, &rpcData, sizeof(rpcData), &rpcData, sizeof(rpcData), NULL, NULL) < 0)
        return -E_SIF_RPC_CALL;

    memcpy(diag, &rpcData, sizeof(rpcData));
    return 0;
}

int oplIGRShutdown(int poff)
{
    SifRpcClientData_t _igr_cd __attribute__((aligned(64)));
    int r;
    s32 poffData __attribute__((aligned(64)));

    _igr_cd.server = NULL;
    while ((r = SifBindRpc(&_igr_cd, 0x80000598, 0)) >= 0 && (!_igr_cd.server))
        nopdelay();

    if (r < 0)
        return -E_SIF_RPC_BIND;

    *(s32 *)UNCACHED_SEG(&poffData) = poff;
    if (SifCallRpc(&_igr_cd, 1, SIF_RPC_M_NOWBDC, &poffData, sizeof(poffData), NULL, 0, NULL, NULL) < 0)
        return -E_SIF_RPC_CALL;

    return 0;
}
