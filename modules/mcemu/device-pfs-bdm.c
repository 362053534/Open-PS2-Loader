/*
   Copyright 2006-2008, Romz
   Copyright 2010, Polo
   Licenced under Academic Free License version 3.0
   Review OpenUsbLd README & LICENSE files for further details.
   */

#include "mcemu.h"

/* 将 PFS 文件中的 VMC 页转换为 APA 硬盘物理扇区。 */
static u32 Mcpage_to_Apasector(int mc_num, u32 mc_page)
{
    register int i;
    register u32 sector_to_read, lbound, ubound;

    ubound = 0;
    sector_to_read = 0;

    for (i = 0; i < 10; i++) {
        lbound = ubound;
        ubound += vmcSpec[mc_num].blocks[i].count;

        if ((mc_page >= (lbound << 4)) && (mc_page < (ubound << 4)))
            sector_to_read = vmcSpec[mc_num].parts[vmcSpec[mc_num].blocks[i].subpart].start + (vmcSpec[mc_num].blocks[i].number << 4) + (mc_page - (lbound << 4));
    }

    return sector_to_read;
}

int DeviceWritePage(int mc_num, void *buf, u32 page_num)
{
    u32 lba = Mcpage_to_Apasector(mc_num, page_num);

    DPRINTF("writing page 0x%lx at lba 0x%lx\n", page_num, lba);
    bdm_writeSector(lba, 1, buf);

    return 1;
}

int DeviceReadPage(int mc_num, void *buf, u32 page_num)
{
    u32 lba = Mcpage_to_Apasector(mc_num, page_num);

    DPRINTF("reading page 0x%lx at lba 0x%lx\n", page_num, lba);
    bdm_readSector(lba, 1, buf);

    return 1;
}

void DeviceShutdown(void)
{
}
