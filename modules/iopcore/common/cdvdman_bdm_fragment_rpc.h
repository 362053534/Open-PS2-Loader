#ifndef CDVDMAN_BDM_FRAGMENT_RPC_H
#define CDVDMAN_BDM_FRAGMENT_RPC_H

#include <sifrpc.h>

/* cdvdman 导出名表末尾追加，序号 115，不移动已有序号。
 * BDM / BDM ATA（含 APA HDL 实际选用的 bdm_ata_cdvdman）把碎片表服务挂到
 * 已启动的 cdvdfsv 队列上。SMB 与仍被编进 ELF 但启动时不选用的 legacy
 * hdd_cdvdman 提供同名空 stub，只为共用的 cdvdfsv 导入能在所有变体上解析。 */
void cdvdman_register_bdm_fragment_rpc(SifRpcDataQueue_t *qd);

#endif
