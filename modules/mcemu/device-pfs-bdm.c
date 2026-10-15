/*
   Copyright 2006-2008, Romz
   Copyright 2010, Polo
   Licenced under Academic Free License version 3.0
   Review OpenUsbLd README & LICENSE files for further details.
   */

#include "mcemu.h"

/* 将 PFS 文件中的 VMC 页转换为 APA 硬盘物理扇区；页未被块链覆盖或越界时返回 0，防止误写扇区 0。 */
static int Mcpage_to_Apasector(int mc_num, u32 mc_page, u32 *lba)
{
    register int i;
    register u32 lbound, ubound;

    ubound = 0;

    if (mc_page >= vmcSpec[mc_num].cspec.CardSize)
        return 0;

    for (i = 0; i < PFS_VMC_MAX_BLOCKS; i++) {
        lbound = ubound;
        ubound += vmcSpec[mc_num].blocks[i].count;

        if ((mc_page >= (lbound << 4)) && (mc_page < (ubound << 4))) {
            u32 subpart = vmcSpec[mc_num].blocks[i].subpart;
            u32 offset = (vmcSpec[mc_num].blocks[i].number << 4) + (mc_page - (lbound << 4));

            if (subpart >= PFS_VMC_MAX_PARTS || offset >= vmcSpec[mc_num].parts[subpart].length)
                return 0;

            *lba = vmcSpec[mc_num].parts[subpart].start + offset;
            return 1;
        }
    }

    return 0;
}

/* 擦除前预检查：从 page_num 起的 count 页必须全部可映射，否则一页都不写。 */
int DeviceCheckPages(int mc_num, u32 page_num, u32 count)
{
    u32 lba, i;

    if (page_num >= vmcSpec[mc_num].cspec.CardSize || count > vmcSpec[mc_num].cspec.CardSize - page_num)
        return 0;

    for (i = 0; i < count; i++) {
        if (!Mcpage_to_Apasector(mc_num, page_num + i, &lba))
            return 0;
    }

    return 1;
}

int DeviceWritePage(int mc_num, void *buf, u32 page_num)
{
    u32 lba;

    if (!Mcpage_to_Apasector(mc_num, page_num, &lba)) {
        DPRINTF("write page 0x%lx unmapped, refused\n", page_num);
        return 0;
    }

    DPRINTF("writing page 0x%lx at lba 0x%lx\n", page_num, lba);
    bdm_writeSector(lba, 1, buf);

    return 1;
}

int DeviceReadPage(int mc_num, void *buf, u32 page_num)
{
    u32 lba;

    if (!Mcpage_to_Apasector(mc_num, page_num, &lba)) {
        DPRINTF("read page 0x%lx unmapped, refused\n", page_num);
        return 0;
    }

    DPRINTF("reading page 0x%lx at lba 0x%lx\n", page_num, lba);
    bdm_readSector(lba, 1, buf);

    return 1;
}

void DeviceShutdown(void)
{
}
