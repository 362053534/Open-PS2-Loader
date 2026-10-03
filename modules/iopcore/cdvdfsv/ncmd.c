/*
  Copyright 2009, jimmikaelkael
  Licenced under Academic Free License version 3.0
  Review open-ps2-loader README & LICENSE files for further details.
*/

#include "cdvdfsv-internal.h"

typedef struct
{
    u32 lsn;
    u32 sectors;
    void *buf;
    sceCdRMode mode;
    void *eeaddr1;
    void *eeaddr2;
} RpcCdvd_t;

typedef struct
{
    u32 lsn;
    u32 sectors;
    void *buf;
    int cmd;
    sceCdRMode mode;
    u32 pad;
} RpcCdvdStream_t;

typedef struct
{
    u32 bufmax;
    u32 bankmax;
    void *buf;
    u32 pad;
} RpcCdvdStInit_t;

typedef struct
{
    u32 lsn;      // sector location to start reading from
    u32 sectors;  // number of sectors to read
    void *buf;    // buffer address to read to ( bit0: 0=EE, 1=IOP )
} RpcCdvdchain_t; // (EE addresses must be on 64byte alignment)

typedef struct
{ // size = 144
    u32 b1len;
    u32 b2len;
    void *pdst1;
    void *pdst2;
    u8 buf1[64];
    u8 buf2[64];
} cdvdfsv_readee_t;

static sceCdRMode cdvdfsv_Stmode;

static SifRpcServerData_t cdvdNcmds_rpcSD;
static u8 cdvdNcmds_rpcbuf[1024];

static void *cbrpc_cdvdNcmds(int fno, void *buf, int size);
static inline void cdvd_readee(void *buf);
static inline void cdvdSt_read(void *buf);
static inline void cdvd_Stsubcmdcall(void *buf);
static inline void cdvd_readiopm(void *buf);
static inline void cdvd_readchain(void *buf);
static inline void rpcNCmd_cdreadDiskID(void *buf);
static inline void rpcNCmd_cdgetdisktype(void *buf);

enum CD_NCMD_CMDS {
    CD_NCMD_READ = 1,
    CD_NCMD_CDDAREAD,
    CD_NCMD_DVDREAD,
    CD_NCMD_GETTOC,
    CD_NCMD_SEEK,
    CD_NCMD_STANDBY,
    CD_NCMD_STOP,
    CD_NCMD_PAUSE,
    CD_NCMD_STREAM,
    CD_NCMD_CDDASTREAM,
    CD_NCMD_NCMD = 0x0C,
    CD_NCMD_READIOPMEM,
    CD_NCMD_DISKREADY,
    CD_NCMD_READCHAIN,
    CD_NCMD_READDISKID = 0x11,
    CD_NCMD_DISKTYPE = 0x17,

    CD_NCMD_COUNT
};

enum CDVD_ST_CMDS {
    CDVD_ST_CMD_START = 1,
    CDVD_ST_CMD_READ,
    CDVD_ST_CMD_STOP,
    CDVD_ST_CMD_SEEK,
    CDVD_ST_CMD_INIT,
    CDVD_ST_CMD_STAT,
    CDVD_ST_CMD_PAUSE,
    CDVD_ST_CMD_RESUME,
    CDVD_ST_CMD_SEEKF
};

/* 说明：曾在这里试过"1 扇区余数读补 30ms"的实验（REM1_MIN_US），
 * 实机验证对 USB 卡 OP 无效，而且汉化版每轮都有这样一笔 1 扇区读（246 笔/OP），
 * 每笔白白多付最多 30ms —— 已整体回退。相关结论见 notes/gundam_seed_ce_op_hang_analysis.md */

#ifdef FSV_MERGE_SMALLREAD
/* ===== 实验（2026-10-04）：把每轮开头的小额读并进相邻读，从设备侧消掉它 =====
 *
 * 实测：游戏自己发的读是「汉化版 1+64+64+63、原版 3+64+64+61 扇区」（PCSX2 的
 * DvdRead 日志一行 = 游戏的一次 sceCdRead）。cdvdfsv 按 CDVDMAN_FS_SECTORS(8)
 * 切块发给设备，于是设备侧每轮各有一笔小额读：
 *     汉化版  1 + 8×8 + 8×8 + 7×8+7    （小额读 = 1 / 7 扇区）
 *     原版    3 + 8×8 + 8×8 + 7×8+5    （小额读 = 3 / 5 扇区）
 * 本开关让 cdvdfsv 每笔设备读至少 FSV_MERGE_SECTORS 扇区：多读出来的扇区放进
 * carry，紧接的顺序请求直接取用 —— 小额读不再单独出现在设备侧（USB/BOT 每笔
 * 命令都要一次 CBW/CSW 往返，小额读最不划算）。
 *
 * 用法（切换 N 值会自动重编 ncmd.o，见 cdvdfsv/Makefile）：
 *     make FSV_MERGE_SMALLREAD=8    每笔设备读凑成 8 扇区（=CDVDMAN_FS_SECTORS，
 *                                   正好把汉化版那笔 1 扇区读并进下一笔）
 *     make FSV_MERGE_SMALLREAD=16   每笔设备读 16 扇区（摊薄 USB 每笔命令开销）
 *     make FSV_MERGE_SMALLREAD=32   32 扇区（静态缓冲 64KB，注意 IOP 内存是否够）
 *
 * 判读：置 8 后能播 => H2 成立（就是那笔 1 扇区读的问题）；
 *       置 8 无效、16/32 有效 => 设备读块太小（命令开销），也是设备侧问题；
 *       都无效 => 转 H1（换 SMB/MX4SIO/HDD，或重编码压低开头需求）。
 *
 * 注意：只对 sector_size==2048 的读生效；请求不是严格接在 carry 后面时丢弃 carry；
 *       过读最多 FSV_MERGE_SECTORS-1 扇区，越界部分由 device-bdm 补零（USB 侧安全）。 */
#define FSV_MERGE_SECTORS FSV_MERGE_SMALLREAD

#if FSV_MERGE_SECTORS < CDVDMAN_FS_SECTORS
#undef FSV_MERGE_SECTORS
#define FSV_MERGE_SECTORS CDVDMAN_FS_SECTORS
#endif

static u8 fsv_mg_buf[FSV_MERGE_SECTORS * 2048] __attribute__((aligned(64)));
static u32 fsv_mg_lsn;   /* carry 里第一扇区对应的 LSN */
static u32 fsv_mg_count; /* carry 里的有效扇区数 */
static u32 fsv_mg_off;   /* carry 数据在 fsv_mg_buf 里的起始扇区号 */
static u32 fsv_mg_end;   /* 上一笔请求结束的 LSN（判断是不是顺序流） */

/* 读 sectors(<=CDVDMAN_FS_SECTORS) 个扇区到 dst，并把多读的留给下一笔。
 * 只有"紧接上一笔"的顺序流才会放大读块，零散的小额读保持原样（不拖慢普通加载）。 */
static inline void fsv_read_merged(u32 lsn, u32 sectors, u8 *dst)
{
    u32 serve = 0, need, fetch;
    int sequential = (lsn == fsv_mg_end);

    fsv_mg_end = lsn + sectors;

    if (sectors > FSV_MERGE_SECTORS) { /* 大块读不走合并，避免缓冲装不下 */
        while (sceCdRead(lsn, sectors, (void *)dst, NULL) == 0)
            sceCdSync(0);
        sceCdSync(0);
        return;
    }

    if (fsv_mg_count && (lsn == fsv_mg_lsn)) { /* carry 正好接上才用 */
        serve = (sectors < fsv_mg_count) ? sectors : fsv_mg_count;
        memcpy(dst, fsv_mg_buf + fsv_mg_off * 2048, serve * 2048);
        fsv_mg_lsn += serve;
        fsv_mg_count -= serve;
        fsv_mg_off += serve;
        if (fsv_mg_count == 0)
            fsv_mg_off = 0;
    } else {
        fsv_mg_count = 0;
        fsv_mg_off = 0;
    }

    need = sectors - serve;
    if (need == 0)
        return;

    fetch = need;
    if (sequential && (need < FSV_MERGE_SECTORS))
        fetch = FSV_MERGE_SECTORS;

    if ((serve == 0) && (fetch == sectors)) { /* 整块正好够：直接读进 dst */
        while (sceCdRead(lsn, fetch, (void *)dst, NULL) == 0)
            sceCdSync(0);
        sceCdSync(0);
        return;
    }

    /* 走到这里 carry 一定是空的（还有剩的话 need 就会是 0），直接读进缓冲开头 */
    while (sceCdRead(lsn + serve, fetch, (void *)fsv_mg_buf, NULL) == 0)
        sceCdSync(0);
    sceCdSync(0);

    memcpy(dst + serve * 2048, fsv_mg_buf, need * 2048);
    if (fetch > need) { /* 多读的留在缓冲里，记下偏移，不搬数据 */
        fsv_mg_lsn = lsn + serve + need;
        fsv_mg_count = fetch - need;
        fsv_mg_off = need;
    } else {
        fsv_mg_count = 0;
        fsv_mg_off = 0;
    }
}
#endif

/* 设备读的统一入口（原先直接写 while (sceCdRead(...)) sceCdSync(0); sceCdSync(0);）。
 * 打开 FSV_MERGE_SMALLREAD 后，2048 字节扇区的读走上面的合并逻辑。 */
static inline void fsv_read_sectors(u32 lsn, u32 sectors, u8 *dst, u16 sector_size)
{
#ifdef FSV_MERGE_SMALLREAD
    if (sector_size == 2048) {
        fsv_read_merged(lsn, sectors, dst);
        return;
    }
#else
    (void)sector_size;
#endif

    while (sceCdRead(lsn, sectors, (void *)dst, NULL) == 0)
        sceCdSync(0);
    sceCdSync(0);
}

//--------------------------------------------------------------
static inline void cdvd_readee(void *buf)
{ // Read Disc data to EE mem buffer
    u8 curlsn_buf[16];
    u32 nbytes, nsectors, sectors_to_read, size_64b, size_64bb, bytesent, temp;
    u16 sector_size;
    int flag_64b;
    void *fsvRbuf = (void *)cdvdfsv_buf;
    void *eeaddr_64b, *eeaddr2_64b;
    cdvdfsv_readee_t readee;
    RpcCdvd_t *r = (RpcCdvd_t *)buf;
    u32 orig_lsn;
    u32 orig_sectors;

    if (r->sectors == 0) {
        *(int *)buf = 0;
        return;
    }

    orig_lsn = r->lsn;
    orig_sectors = r->sectors;
    /* 调试用：配合 cdvdman 的 bdm read 日志，看卡住时是哪一笔 EE 请求没结束。 */
    DPRINTF("readee: start lsn=%u sectors=%u\n", orig_lsn, orig_sectors);

    sector_size = 2048;

    if (r->mode.datapattern == SCECdSecS2328)
        sector_size = 2328;
    if (r->mode.datapattern == SCECdSecS2340)
        sector_size = 2340;


    r->eeaddr1 = (void *)((u32)r->eeaddr1 & 0x1fffffff);
    r->eeaddr2 = (void *)((u32)r->eeaddr2 & 0x1fffffff);
    r->buf = (void *)((u32)r->buf & 0x1fffffff);

    sceCdDiskReady(0);

    sectors_to_read = r->sectors;
    bytesent = 0;

    memset((void *)curlsn_buf, 0, 16);

    readee.pdst1 = (void *)r->buf;
    eeaddr_64b = (void *)(((u32)r->buf + 0x3f) & 0xffffffc0); // get the next 64-bytes aligned address

    if ((u32)r->buf & 0x3f)
        readee.b1len = (((u32)r->buf & 0xffffffc0) - (u32)r->buf) + 64; // get how many bytes needed to get a 64 bytes alignment
    else
        readee.b1len = 0;

    nbytes = r->sectors * sector_size;

    temp = (u32)r->buf + nbytes;
    eeaddr2_64b = (void *)(temp & 0xffffffc0);
    temp -= (u32)eeaddr2_64b;
    readee.pdst2 = eeaddr2_64b; // get the end address on a 64 bytes align
    readee.b2len = temp;        // get bytes remainder at end of 64 bytes align
    fsvRbuf += temp;

    if (readee.b1len)
        flag_64b = 0; // 64 bytes alignment flag
    else {
        if (temp)
            flag_64b = 0;
        else
            flag_64b = 1;
    }

    while (1) {
        do {
            if ((sectors_to_read == 0) || (sceCdGetError() == SCECdErABRT)) {
                sysmemSendEE((void *)&readee, (void *)r->eeaddr1, sizeof(cdvdfsv_readee_t));

                *((u32 *)&curlsn_buf[0]) = nbytes;
                sysmemSendEE((void *)curlsn_buf, (void *)r->eeaddr2, 16);

                *(int *)buf = nbytes;
                DPRINTF("readee: done lsn=%u sectors=%u nbytes=%u err=%d\n", orig_lsn, orig_sectors, nbytes, sceCdGetError());
                return;
            }

            if (flag_64b == 0) { // not 64 bytes aligned buf
                // The data of the last sector of the chunk will be used to correct buffer alignment.
                if (sectors_to_read < CDVDMAN_FS_SECTORS - 1)
                    nsectors = sectors_to_read;
                else
                    nsectors = CDVDMAN_FS_SECTORS - 1;
                temp = nsectors + 1;
            } else { // 64 bytes aligned buf
                if (sectors_to_read < CDVDMAN_FS_SECTORS)
                    nsectors = sectors_to_read;
                else
                    nsectors = CDVDMAN_FS_SECTORS;
                temp = nsectors;
            }

            fsv_read_sectors(r->lsn, temp, (u8 *)fsvRbuf, sector_size);

            size_64b = nsectors * sector_size;
            size_64bb = size_64b;

            if (!flag_64b) {
                if (sectors_to_read == r->sectors) // check that was the first read. Data read will be skewed by readee.b1len bytes into the adjacent sector.
                    memcpy((void *)readee.buf1, (void *)fsvRbuf, readee.b1len);

                if ((sectors_to_read == nsectors) && (readee.b1len)) // For the last sector read.
                    size_64bb = size_64b - 64;
            }

            if (size_64bb > 0) {
                sysmemSendEE((void *)(fsvRbuf + readee.b1len), (void *)eeaddr_64b, size_64bb);
                bytesent += size_64bb;
            }

            // *((u32 *)&curlsn_buf[0]) = bytesent;
            // sysmemSendEE((void *)curlsn_buf, (void *)r->eeaddr2, 16);

            sectors_to_read -= nsectors;
            r->lsn += nsectors;
            eeaddr_64b += size_64b;

        } while ((flag_64b) || (sectors_to_read));

        // At the very last pass, copy readee.b2len bytes from the last sector, to complete the alignment correction.
        memcpy((void *)readee.buf2, (void *)(fsvRbuf + size_64b - readee.b2len), readee.b2len);
    }

    *(int *)buf = bytesent;
}

//-------------------------------------------------------------------------
static inline void cdvdSt_read(void *buf)
{
    RpcCdvdStream_t *St = (RpcCdvdStream_t *)buf;
    u32 err;
    int r, rpos, remaining;
    void *ee_addr;

    for (rpos = 0, ee_addr = St->buf, remaining = St->sectors; remaining > 0; ee_addr += r * 2048, rpos += r, remaining -= r) {
        if ((r = sceCdStRead(remaining, (void *)((u32)ee_addr | 0x80000000), 0, &err)) < 1)
            break;
    }

    *(int *)buf = (rpos & 0xFFFF) | (err << 16);
}

//-------------------------------------------------------------------------
static inline void cdvd_Stsubcmdcall(void *buf)
{ // call a Stream Sub function (below) depending on stream cmd sent
    RpcCdvdStream_t *St = (RpcCdvdStream_t *)buf;

    switch (St->cmd) {
        case CDVD_ST_CMD_START:
            *(int *)buf = sceCdStStart(((RpcCdvdStream_t *)buf)->lsn, &cdvdfsv_Stmode);
            break;
        case CDVD_ST_CMD_READ:
            cdvdSt_read(buf);
            break;
        case CDVD_ST_CMD_STOP:
            *(int *)buf = sceCdStStop();
            break;
        case CDVD_ST_CMD_SEEK:
            *(int *)buf = sceCdStSeek(((RpcCdvdStream_t *)buf)->lsn);
            break;
        case CDVD_ST_CMD_INIT:
            *(int *)buf = sceCdStInit(((RpcCdvdStInit_t *)buf)->bufmax, ((RpcCdvdStInit_t *)buf)->bankmax, ((RpcCdvdStInit_t *)buf)->buf);
            break;
        case CDVD_ST_CMD_STAT:
            *(int *)buf = sceCdStStat();
            break;
        case CDVD_ST_CMD_PAUSE:
            *(int *)buf = sceCdStPause();
            break;
        case CDVD_ST_CMD_RESUME:
            *(int *)buf = sceCdStResume();
            break;
        case CDVD_ST_CMD_SEEKF:
            *(int *)buf = sceCdStSeekF(((RpcCdvdStream_t *)buf)->lsn);
            break;
        default:
            *(int *)buf = 0;
            break;
    };
}

static inline void cdvd_readiopm(void *buf)
{
    u32 readpos;

    while (sceCdRead(((RpcCdvd_t *)buf)->lsn, ((RpcCdvd_t *)buf)->sectors, ((RpcCdvd_t *)buf)->buf, NULL) == 0)
        sceCdSync(0);
    while (sceCdSync(1)) {
        readpos = sceCdGetReadPos();
        sysmemSendEE(&readpos, ((RpcCdvd_t *)buf)->eeaddr2, sizeof(readpos));
        DelayThread(8000);
    }
}

//-------------------------------------------------------------------------
static inline void cdvd_readchain(void *buf)
{
    int i;
    u32 nsectors, tsectors, lsn, addr, readpos;

    RpcCdvdchain_t *ch = (RpcCdvdchain_t *)buf;

    for (i = 0, readpos = 0; i < 64; i++, ch++) {

        if ((ch->lsn == -1) || (ch->sectors == -1) || ((u32)ch->buf == -1))
            break;

        lsn = ch->lsn;
        tsectors = ch->sectors;
        addr = (u32)ch->buf & 0xfffffffc;

        if ((u32)ch->buf & 1) { // IOP addr
            while (sceCdRead(lsn, tsectors, (void *)addr, NULL) == 0)
                sceCdSync(0);
            sceCdSync(0);

            readpos += tsectors * 2048;
        } else { // EE addr
            while (tsectors > 0) {
                nsectors = (tsectors > CDVDMAN_FS_SECTORS) ? CDVDMAN_FS_SECTORS : tsectors;

                while (sceCdRead(lsn, nsectors, cdvdfsv_buf, NULL) == 0)
                    sceCdSync(0);
                sceCdSync(0);
                sysmemSendEE(cdvdfsv_buf, (void *)addr, nsectors * 2048);

                lsn += nsectors;
                tsectors -= nsectors;
                addr += nsectors * 2048;
                readpos += nsectors * 2048;
            }
        }

        // The pointer to the read position variable within EE RAM is stored at ((RpcCdvdchain_t *)buf)[65].sectors.
        sysmemSendEE(&readpos, (void *)((RpcCdvdchain_t *)buf)[65].sectors, sizeof(readpos));
    }
}

//-------------------------------------------------------------------------
static inline void rpcNCmd_cdreadDiskID(void *buf)
{
    u8 *p = (u8 *)buf;

    memset(p, 0, 10);
    *(int *)buf = sceCdReadDiskID((unsigned int *)&p[4]);
}

//-------------------------------------------------------------------------
static inline void rpcNCmd_cdgetdisktype(void *buf)
{
    u8 *p = (u8 *)buf;
    *(int *)&p[4] = sceCdGetDiskType();
    *(int *)&p[0] = 1;
}

//-------------------------------------------------------------------------
static void *cbrpc_cdvdNcmds(int fno, void *buf, int size)
{ // CD NCMD RPC callback
    int sc_param;

    sceCdSC(CDSC_IO_SEMA, &fno);

    switch (fno) {
        case CD_NCMD_READ:
        case CD_NCMD_CDDAREAD:
        case CD_NCMD_DVDREAD:
            cdvd_readee(buf);
            break;
        case CD_NCMD_GETTOC:
            /*u32 eeaddr = *(u32 *)buf;
            DPRINTF("cbrpc_cdvdNcmds GetToc eeaddr=%08x\n", (int)eeaddr);
            char toc[2064];
            memset(toc, 0, 2064);
            int result = sceCdGetToc((u8 *)toc);
            *(int *)buf = result;
            if (result)
                sysmemSendEE(toc, (void *)eeaddr, 2064);*/
            *(int *)buf = 1;
            break;
        case CD_NCMD_SEEK:
            *(int *)buf = sceCdSeek(*(u32 *)buf);
            break;
        case CD_NCMD_STANDBY:
            *(int *)buf = sceCdStandby();
            break;
        case CD_NCMD_STOP:
            *(int *)buf = sceCdStop();
            break;
        case CD_NCMD_PAUSE:
            *(int *)buf = sceCdPause();
            break;
        case CD_NCMD_STREAM:
            cdvd_Stsubcmdcall(buf);
            break;
        case CD_NCMD_READIOPMEM:
            cdvd_readiopm(buf);
            break;
        case CD_NCMD_DISKREADY:
            *(int *)buf = sceCdDiskReady(0);
            break;
        case CD_NCMD_READCHAIN:
            cdvd_readchain(buf);
            break;
        case CD_NCMD_READDISKID:
            rpcNCmd_cdreadDiskID(buf);
            break;
        case CD_NCMD_DISKTYPE:
            rpcNCmd_cdgetdisktype(buf);
            break;
        default:
            DPRINTF("cbrpc_cdvdNcmds unknown rpc fno=%x buf=%x size=%x\n", fno, (int)buf, size);
            *(int *)buf = 0;
            break;
    }

    sc_param = 0;
    sceCdSC(CDSC_IO_SEMA, &sc_param);

    return buf;
}

void cdvdfsv_register_ncmd_rpc(SifRpcDataQueue_t *rpc_DQ)
{
    sceSifRegisterRpc(&cdvdNcmds_rpcSD, 0x80000595, &cbrpc_cdvdNcmds, cdvdNcmds_rpcbuf, NULL, NULL, rpc_DQ);
}
