/*
  Copyright 2009-2010, jimmikaelkael
  Licenced under Academic Free License version 3.0
  Review Open PS2 Loader README & LICENSE files for further details.
*/

#include "internal.h"
#include "../../isofs/zso.h"

#define MODNAME "cdvd_driver"
IRX_ID(MODNAME, 1, 1);

//------------------ Patch Zone ----------------------
struct CDVDMAN_SETTINGS_TYPE cdvdman_settings = {
    CDVDMAN_SETTINGS_DEFAULT_COMMON,
    CDVDMAN_SETTINGS_DEFAULT_DEVICE_SETTINGS};

//----------------------------------------------------
extern struct irx_export_table _exp_cdvdman;
extern struct irx_export_table _exp_cdvdstm;
extern struct irx_export_table _exp_smsutils;
extern struct irx_export_table _exp_oplutils;
#ifdef __USE_DEV9
extern struct irx_export_table _exp_dev9;
#endif

// reader function interface, raw reader impementation by default
int DeviceReadSectorsCached(u32 sector, void *buffer, unsigned int count);
int (*DeviceReadSectorsPtr)(u32 sector, void *buffer, unsigned int count) = &DeviceReadSectors;

// internal functions prototypes
static void oplShutdown(int poff);
static int cdvdman_writeSCmd(u8 cmd, const void *in, u16 in_size, void *out, u16 out_size);
static unsigned int event_alarm_cb(void *args);
static void cdvdman_signal_read_end(void);
static void cdvdman_signal_read_end_intr(void);
static void cdvdman_startThreads(void);
static void cdvdman_create_semaphores(void);
static int cdvdman_read(u32 lsn, u32 sectors, u16 sector_size, void *buf);

// Sector cache to improve IO
static u8 MAX_SECTOR_CACHE = 0;
static u8 *sector_cache = NULL;
static u32 cur_sector = 0xFFFFFFFF;

struct cdvdman_cb_data
{
    void (*user_cb)(int reason);
    int reason;
};

cdvdman_status_t cdvdman_stat;
static struct cdvdman_cb_data cb_data;

int cdrom_io_sema;
static int cdrom_rthread_sema;
static int cdvdman_scmdsema;
int cdvdman_searchfilesema;
static int cdvdman_ReadingThreadID;

static StmCallback_t Stm0Callback = NULL;
static iop_sys_clock_t gCallbackSysClock;

// buffers
u8 cdvdman_buf[CDVDMAN_BUF_SECTORS * 2048];

#define CDVDMAN_MODULE_VERSION 0x225
static int cdvdman_debug_print_flag = 0;

unsigned char sync_flag;
unsigned char cdvdman_cdinited = 0;
static unsigned int ReadPos = 0; /* Current buffer offset in 2048-byte sectors. */

/* 1 深排队：音乐占着读线程时，语音 sceCdRead 先收下，避免直接返回 0。 */
static u8 cdread_pending;
static u8 cdread_io_busy;
static u8 cdread_outstand;
static u32 cdread_pending_lba;
static u32 cdread_pending_sectors;
static u16 cdread_pending_size;
static void *cdread_pending_buf;

#ifdef __USE_DEV9
static int POFFThreadID;
#endif

typedef void (*oplShutdownCb_t)(void);
static oplShutdownCb_t vmcShutdownCb = NULL;

void initCache()
{
    u8 cache_size = cdvdman_settings.common.zso_cache;
    if (cache_size && sector_cache == NULL) {
        sector_cache = AllocSysMemory(ALLOC_FIRST, cache_size * 2048, NULL);
        if (sector_cache)
            MAX_SECTOR_CACHE = cache_size;
    }
}

void oplRegisterShutdownCallback(oplShutdownCb_t cb)
{
    vmcShutdownCb = cb;
}

static void oplShutdown(int poff)
{
    u32 stat;

    DeviceLock();
    if (vmcShutdownCb != NULL)
        vmcShutdownCb();
    DeviceUnmount();
    if (poff) {
        DeviceStop();
#ifdef __USE_DEV9
        Dev9CardStop();
#endif
        sceCdPowerOff(&stat);
    }
}

//-------------------------------------------------------------------------
#ifdef __USE_DEV9
static void cdvdman_poff_thread(void *arg)
{
    SleepThread();

    oplShutdown(1);
}
#endif

int cdvdman_init(void)
{
#ifdef __USE_DEV9
    iop_thread_t ThreadData;
#endif

    if (cdvdman_cdinited)
        return 1;

    cdvdman_stat.err = SCECdErNO;

    /* 失败时不锁定初始化状态，后续sceCdInit仍可重新传输碎片表。 */
    if (!cdvdman_fs_init())
        return 0;

#ifdef __USE_DEV9
    if (cdvdman_settings.common.flags & IOPCORE_ENABLE_POFF) {
        ThreadData.attr = TH_C;
        ThreadData.option = 0xABCD0001;
        ThreadData.priority = 1;
        ThreadData.stacksize = 0x1000;
        ThreadData.thread = &cdvdman_poff_thread;
        StartThread(POFFThreadID = CreateThread(&ThreadData), NULL);
    }
#endif

    cdvdman_cdinited = 1;
    return 1;
}

int sceCdInit(int init_mode)
{
    return cdvdman_init();
}

//-------------------------------------------------------------------------
static unsigned int cdvdemu_read_end_cb(void *arg)
{
    iSetEventFlag(cdvdman_stat.intr_ef, 0x1000);
    return 0;
}

void *ziso_alloc(u32 size)
{
    return AllocSysMemory(0, size, NULL);
}

/*
  This small improvement will mostly benefit ZSO files.
  For the same size of an ISO sector, we can have more than one ZSO blocks.
  If we do a consecutive read of many ISO sectors we will have a huge amount of ZSO sectors ready.
  Therefore reducing IO access for ZSO files.
*/
int DeviceReadSectorsCached(u32 lsn, void *buffer, unsigned int sectors)
{
    if (sectors < MAX_SECTOR_CACHE) { // if MAX_SECTOR_CACHE is 0 then it will act as disabled and passthrough
        if (cur_sector == 0xFFFFFFFF || lsn < cur_sector || (lsn - cur_sector) + sectors > MAX_SECTOR_CACHE) {
            int res = DeviceReadSectors(lsn, sector_cache, MAX_SECTOR_CACHE);
            if (res != SCECdErNO)
                return res; // 读失败
            cur_sector = lsn;
        }
        int pos = lsn - cur_sector;
        if (pos >= 0) {
            memcpy(buffer, &(sector_cache[pos * 2048]), 2048 * sectors);
            return SCECdErNO;
        }
    }
    return DeviceReadSectors(lsn, buffer, sectors);
}

#ifdef SMB_DRIVER
// 普通 ISO 专用扇区缓存，和 ZSO 的 DeviceReadSectorsCached 分开，避免改压缩读路径。
#define ISO_SECTOR_CACHE_MAX_WAYS 2
static u8 iso_cache_size = 0;
static u8 iso_cache_ways = 0;
static u8 *iso_sector_cache = NULL;
static u32 iso_cache_lsn[ISO_SECTOR_CACHE_MAX_WAYS];
static u8 iso_cache_count[ISO_SECTOR_CACHE_MAX_WAYS];
static u8 iso_cache_mru = 0;

static void iso_sector_cache_reset(void)
{
    int i;

    for (i = 0; i < ISO_SECTOR_CACHE_MAX_WAYS; i++) {
        iso_cache_lsn[i] = 0xFFFFFFFF;
        iso_cache_count[i] = 0;
    }
    iso_cache_mru = 0;
}

static int iso_sector_cache_try_alloc(u8 cache_size, u8 ways)
{
    u8 *buf;

    if (!cache_size || !ways)
        return 0;

    buf = AllocSysMemory(ALLOC_FIRST, (int)ways * cache_size * 2048, NULL);
    if (!buf)
        return 0;

    iso_sector_cache = buf;
    iso_cache_size = cache_size;
    iso_cache_ways = ways;
    iso_sector_cache_reset();
    return 1;
}

static void initIsoSectorCache(void)
{
    u8 cache_size = cdvdman_settings.common.zso_cache;

    if (iso_sector_cache != NULL || cache_size == 0)
        return;

    // 旧配置里常见 8/16，接不住「1 扇区后再读 LSN+16」的语音块。
    if (cache_size < 32)
        cache_size = 32;

    while (cache_size >= 8) {
        if (iso_sector_cache_try_alloc(cache_size, ISO_SECTOR_CACHE_MAX_WAYS))
            return;
        if (iso_sector_cache_try_alloc(cache_size, 1))
            return;
        cache_size >>= 1;
    }
}

static int iso_sector_cache_find(u32 lsn, unsigned int sectors)
{
    int i;
    u32 off;

    for (i = 0; i < iso_cache_ways; i++) {
        if (iso_cache_lsn[i] == 0xFFFFFFFF || lsn < iso_cache_lsn[i])
            continue;
        off = lsn - iso_cache_lsn[i];
        if (off < iso_cache_count[i] && sectors <= (u32)(iso_cache_count[i] - off))
            return i;
    }
    return -1;
}

static int iso_sector_cache_pick_victim(void)
{
    int i;

    for (i = 0; i < iso_cache_ways; i++) {
        if (iso_cache_lsn[i] == 0xFFFFFFFF)
            return i;
    }
    for (i = 0; i < iso_cache_ways; i++) {
        if (i != iso_cache_mru)
            return i;
    }
    return 0;
}

static int DeviceReadSectorsIsoCached(u32 lsn, void *buffer, unsigned int sectors)
{
    int way, res;
    unsigned int fetch;
    u8 *way_buf;

    // 大块顺序读（音乐 32 扇区）直接穿透，避免把语音窗口挤掉。
    if (!iso_cache_size || !iso_sector_cache || sectors >= iso_cache_size)
        return DeviceReadSectors(lsn, buffer, sectors);

    way = iso_sector_cache_find(lsn, sectors);
    if (way >= 0) {
        memcpy(buffer, iso_sector_cache + ((way * iso_cache_size) + (lsn - iso_cache_lsn[way])) * 2048, sectors * 2048);
        iso_cache_mru = (u8)way;
        return SCECdErNO;
    }

    way = iso_sector_cache_pick_victim();
    way_buf = iso_sector_cache + way * iso_cache_size * 2048;
    fetch = iso_cache_size;
    if (mediaLsnCount && lsn < mediaLsnCount && (mediaLsnCount - lsn) < fetch)
        fetch = mediaLsnCount - lsn;
    if (fetch < sectors)
        return DeviceReadSectors(lsn, buffer, sectors);

    res = DeviceReadSectors(lsn, way_buf, fetch);
    if (res != SCECdErNO) {
        iso_cache_lsn[way] = 0xFFFFFFFF;
        iso_cache_count[way] = 0;
        return DeviceReadSectors(lsn, buffer, sectors);
    }

    iso_cache_lsn[way] = lsn;
    iso_cache_count[way] = (u8)fetch;
    iso_cache_mru = (u8)way;
    memcpy(buffer, way_buf, sectors * 2048);
    return SCECdErNO;
}
#endif

/*
  For ZSO we need to be able to read at arbitrary offsets with arbitrary sizes.
  Since we can only do sector-based reads, this funtions acts as a wrapper.
  It will do at most 3 IO reads, most of the time only 1.
*/
int read_raw_data(u8 *addr, u32 size, u32 offset, u32 shift)
{
    u32 o_size = size;
    u32 lba = offset / (2048 >> shift); // avoid overflow by shifting sector size instead of offset
    u32 pos = (offset << shift) & 2047; // doesn't matter if it overflows since we only care about the 11 LSB anyways

    // prevent caching if already reading into ZSO index cache
    int (*ReadSectors)(u32 lsn, void *buffer, unsigned int sectors) = (addr == (u8 *)ziso_idx_cache) ? &DeviceReadSectors : &DeviceReadSectorsCached;

    // read first block if not aligned to sector size
    if (pos) {
        int r = MIN(size, (2048 - pos));
        if (ReadSectors(lba, ziso_tmp_buf, 1) != SCECdErNO)
            return o_size - size;
        memcpy(addr, ziso_tmp_buf + pos, r);
        size -= r;
        lba++;
        addr += r;
    }

    // read intermediate blocks if more than one block is left
    u32 n_blocks = size / 2048;
    if (size % 2048)
        n_blocks++;
    if (n_blocks > 1) {
        int r = 2048 * (n_blocks - 1);
        if (ReadSectors(lba, addr, n_blocks - 1) != SCECdErNO)
            return o_size - size;
        size -= r;
        addr += r;
        lba += n_blocks - 1;
    }

    // read remaining data
    if (size) {
        if (ReadSectors(lba, ziso_tmp_buf, 1) != SCECdErNO)
            return o_size - size;
        memcpy(addr, ziso_tmp_buf, size);
        size = 0;
    }

    // return remaining size
    return o_size - size;
}

int DeviceReadSectorsCompressed(u32 lsn, void *addr, unsigned int count)
{
    unsigned int sectors;

    if (lsn >= ziso_total_block) {
        memset(addr, 0, count * 2048);
        return SCECdErNO;
    }

    sectors = MIN(count, ziso_total_block - lsn);
    if (ziso_read_sector(addr, lsn, sectors) != sectors)
        return SCECdErREAD;

    // 超出ZSO逻辑末尾的部分补零，兼容游戏的末尾探测读取。
    if (sectors < count)
        memset((u8 *)addr + sectors * 2048, 0, (count - sectors) * 2048);

    return SCECdErNO;
}

static int probed = 0;
static int ProbeZSO(u8 *buffer)
{
    if (DeviceReadSectors(0, buffer, 1) != SCECdErNO)
        return 0;
    probed = 1;
    if (*(u32 *)buffer == ZSO_MAGIC) {
        // initialize ZSO
        ziso_init((ZISO_header *)buffer, *(u32 *)(buffer + sizeof(ZISO_header)));
        // initialize cache
        initCache();
        // redirect sector reader
        DeviceReadSectorsPtr = &DeviceReadSectorsCompressed;
    }
#ifdef SMB_DRIVER
    else {
        // 普通 ISO 走独立缓存，不复用 ZSO 的 DeviceReadSectorsCached。
        initIsoSectorCache();
        if (iso_cache_size)
            DeviceReadSectorsPtr = &DeviceReadSectorsIsoCached;
    }
#endif
    return 1;
}

static int cdvdman_read_sectors(u32 lsn, unsigned int sectors, void *buf)
{
    unsigned int remaining;
    void *ptr;
    int endOfMedia = 0;

    DPRINTF("cdvdman_read lsn=%lu sectors=%u buf=%p\n", lsn, sectors, buf);

    // PVD容量仅作为辅助边界，底层能够完整读取时兼容D9转D5等魔改镜像。
    if (mediaLsnCount) {
        if (lsn >= mediaLsnCount) {
            DPRINTF("cdvdman_read eom lsn=%d sectors=%d leftsectors=%d MaxLsn=%d \n", lsn, sectors, mediaLsnCount - lsn, mediaLsnCount);
            endOfMedia = 2;
        } else if (sectors > mediaLsnCount - lsn) {
            DPRINTF("cdvdman_read eom lsn=%d sectors=%d leftsectors=%d MaxLsn=%d \n", lsn, sectors, mediaLsnCount - lsn, mediaLsnCount);
            endOfMedia = 1;
        }
    }

    if (probed == 0) { // Probe for ZSO before first read
        // check for ZSO
        if (!ProbeZSO(buf)) // we need to pass the buffer so we have somewhere to read the first sector to identify ZSO before allocating any extra RAM
            return 1;
    }

    cdvdman_stat.err = SCECdErNO;
    for (ptr = buf, remaining = sectors; remaining > 0;) {
        unsigned int SectorsToRead = remaining;

        if (cdvdman_settings.common.flags & IOPCORE_COMPAT_ACCU_READS) {
            // Limit transfers to a maximum length of 8, with a restricted transfer rate.
            iop_sys_clock_t TargetTime;

            if (SectorsToRead > 8)
                SectorsToRead = 8;

            TargetTime.hi = 0;
            TargetTime.lo = (cdvdman_settings.common.media == 0x12 ? 81920 : 33512) * SectorsToRead;
            // SP193: approximately 2KB/3600KB/s = 555us required per 2048-byte data sector at 3600KB/s, so 555 * 36.864 = 20460 ticks per sector with a 36.864MHz clock.
            /* AKuHAK: 3600KB/s is too fast, it is CD 24x - theoretical maximum on CD
               However, when setting SCECdSpinMax we will get 900KB/s (81920) for CD, and 2200KB/s (33512) for DVD */
            ClearEventFlag(cdvdman_stat.intr_ef, ~0x1000);
            SetAlarm(&TargetTime, &cdvdemu_read_end_cb, NULL);
        }

        cdvdman_stat.err = DeviceReadSectorsPtr(lsn, ptr, SectorsToRead);
        if (cdvdman_stat.err != SCECdErNO) {
            if (cdvdman_settings.common.flags & IOPCORE_COMPAT_ACCU_READS)
                CancelAlarm(&cdvdemu_read_end_cb, NULL);
            break;
        }

        /* PS2LOGO Decryptor algorithm; based on misfire's code (https://github.com/mlafeldt/ps2logo)
           The PS2 logo is stored within the first 12 sectors, scrambled.
           This algorithm exploits the characteristic that the value used for scrambling will be recorded,
           when it is XOR'ed against a black pixel. The first pixel is black, hence the value of the first byte
           was the value used for scrambling. */
        if (lsn < 13) {
            u32 j;
            u8 *logo = (u8 *)ptr;
            static u8 key = 0;
            if (lsn == 0) // First sector? Copy the first byte as the value for unscrambling the logo.
                key = logo[0];
            if (key != 0) {
                for (j = 0; j < (SectorsToRead * 2048); j++) {
                    logo[j] ^= key;
                    logo[j] = (logo[j] << 3) | (logo[j] >> 5);
                }
            }
        }

        ptr = (void *)((u8 *)ptr + (SectorsToRead * 2048));
        remaining -= SectorsToRead;
        lsn += SectorsToRead;
        ReadPos += SectorsToRead * 2048;

        if (cdvdman_settings.common.flags & IOPCORE_COMPAT_ACCU_READS) {
            // Sleep until the required amount of time has been spent.
            WaitEventFlag(cdvdman_stat.intr_ef, 0x1000, WEF_AND, NULL);
        }
    }

    // If we had a read that went past the end of media, after reading what we can, set the end of media error.
    if (endOfMedia && cdvdman_stat.err != SCECdErNO)
        cdvdman_stat.err = endOfMedia == 2 ? SCECdErIPI : SCECdErEOM;

    return (cdvdman_stat.err == SCECdErNO ? 0 : 1);
}

static int cdvdman_read(u32 lsn, u32 sectors, u16 sector_size, void *buf)
{
    cdvdman_stat.status = SCECdStatRead;

    // OPL only has 2048 bytes no matter what. For other sizes we have to copy to the offset and prepoluate the sector header data (the extra bytes.)
    u32 offset = 0;

    if (sector_size == 2340)
        offset = 12; // head - sub - data(2048) -- edc-ecc

    buf = (void *)PHYSADDR(buf);
    if ((u32)(buf) & 3 || (sector_size != 2048)) {
        // For transfers to unaligned buffers, a double-copy is required to avoid stalling the device's DMA channel.
        WaitSema(cdvdman_searchfilesema);

        u32 nsectors, nbytes;
        u32 rpos = lsn;

        while (sectors > 0) {
            nsectors = sectors;
            if (nsectors > CDVDMAN_BUF_SECTORS)
                nsectors = CDVDMAN_BUF_SECTORS;

            // For other sizes we can only read one sector at a time.
            // There are only very few games (CDDA games, EA Tiburon) that will be affected
            if (sector_size != 2048)
                nsectors = 1;

            cdvdman_read_sectors(rpos, nsectors, cdvdman_buf);

            rpos += nsectors;
            sectors -= nsectors;
            nbytes = nsectors * sector_size;

            // Copy the data for buffer.
            // For any sector other than 2048 one sector at a time is copied.
            memcpy((void *)((u32)buf + offset), cdvdman_buf, nbytes);

            // For these custom sizes we need to manually fix the header.
            // For 2340 we have 12bytes. 4 are position.
            if (sector_size == 2340) {
                u8 *header = (u8 *)buf;
                // position.
                sceCdlLOCCD p;
                sceCdIntToPos(rpos - 1, &p); // to get current pos.
                header[0] = p.minute;
                header[1] = p.second;
                header[2] = p.sector;
                header[3] = 0; // p.track for cdda only non-zero

                // Subheader and copy of subheader.
                header[4] = header[8] = 0;
                header[5] = header[9] = 0;
                header[6] = header[10] = 0x8;
                header[7] = header[11] = 0;
            }
            buf = (void *)((u8 *)buf + nbytes);
        }
        SignalSema(cdvdman_searchfilesema);
    } else
        cdvdman_read_sectors(lsn, sectors, buf);

    ReadPos = 0; /* Reset the buffer offset indicator. */

    cdvdman_stat.status = SCECdStatPause;

    return 1;
}

//-------------------------------------------------------------------------
u32 sceCdGetReadPos(void)
{
    DPRINTF("sceCdGetReadPos\n");

    return ReadPos;
}

// Must be called from a thread context, with interrupts disabled.
static int cdvdman_common_lock(int IntrContext)
{
    if (sync_flag)
        return 0;

    if (IntrContext)
        iClearEventFlag(cdvdman_stat.intr_ef, ~1);
    else
        ClearEventFlag(cdvdman_stat.intr_ef, ~1);

    sync_flag = 1;

    return 1;
}

int cdvdman_AsyncRead(u32 lsn, u32 sectors, u16 sector_size, void *buf)
{
    int IsIntrContext, OldState;

    IsIntrContext = QueryIntrContext();

    CpuSuspendIntr(&OldState);

    if (sync_flag) {
        /* 只在读线程真正堵在设备 I/O 时排队；收尾/回调窗口仍拒绝，避免幽灵读。 */
        if (!cdread_io_busy || cdread_pending) {
            CpuResumeIntr(OldState);
            DPRINTF("cdvdman_AsyncRead: exiting (sync_flag)...\n");
            return 0;
        }

        cdread_pending_lba = lsn;
        cdread_pending_sectors = sectors;
        cdread_pending_size = sector_size;
        cdread_pending_buf = buf;
        cdread_pending = 1;
        cdread_outstand++;
        CpuResumeIntr(OldState);
        return 1;
    }

    if (!cdvdman_common_lock(IsIntrContext)) {
        CpuResumeIntr(OldState);
        DPRINTF("cdvdman_AsyncRead: exiting (sync_flag)...\n");
        return 0;
    }

    cdread_io_busy = 1;
    cdread_outstand = 1;
    cdvdman_stat.cdread_lba = lsn;
    cdvdman_stat.cdread_sectors = sectors;
    cdvdman_stat.sector_size = sector_size;
    cdvdman_stat.cdread_buf = buf;

    CpuResumeIntr(OldState);

    if (IsIntrContext)
        iSignalSema(cdrom_rthread_sema);
    else
        SignalSema(cdrom_rthread_sema);

    return 1;
}

void cdvdman_cancel_pending_read(void)
{
    int OldState;

    CpuSuspendIntr(&OldState);
    if (cdread_pending) {
        cdread_pending = 0;
        if (cdread_outstand > 0)
            cdread_outstand--;
    }
    CpuResumeIntr(OldState);
}

int cdvdman_SyncRead(u32 lsn, u32 sectors, u16 sector_size, void *buf)
{
    int IsIntrContext, OldState;

    IsIntrContext = QueryIntrContext();

    CpuSuspendIntr(&OldState);

    if (!cdvdman_common_lock(IsIntrContext)) {
        CpuResumeIntr(OldState);
        DPRINTF("cdvdman_SyncRead: exiting (sync_flag)...\n");
        return 0;
    }

    CpuResumeIntr(OldState);

    cdvdman_read(lsn, sectors, sector_size, buf);

    cdvdman_cb_event(SCECdFuncRead);
    sync_flag = 0;
    SetEventFlag(cdvdman_stat.intr_ef, 9);

    return 1;
}

//-------------------------------------------------------------------------
static void cdvdman_initDiskType(void)
{
    cdvdman_stat.err = SCECdErNO;

    cdvdman_stat.disc_type_reg = (int)cdvdman_settings.common.media;
    DPRINTF("DiskType=0x%x\n", cdvdman_settings.common.media);
}

//-------------------------------------------------------------------------
u32 sceCdPosToInt(sceCdlLOCCD *p)
{
    register u32 result;

    result = ((u32)p->minute >> 4) * 10 + ((u32)p->minute & 0xF);
    result *= 60;
    result += ((u32)p->second >> 4) * 10 + ((u32)p->second & 0xF);
    result *= 75;
    result += ((u32)p->sector >> 4) * 10 + ((u32)p->sector & 0xF);
    result -= 150;

    DPRINTF("%s({0x%X, 0x%X, 0x%X, 0x%X}) = %d\n", __FUNCTION__, p->minute, p->second, p->sector, p->track, result);

    return result;
}

//-------------------------------------------------------------------------
sceCdlLOCCD *sceCdIntToPos(u32 i, sceCdlLOCCD *p)
{
    register u32 sc, se, mi;

    i += 150;
    se = i / 75;
    sc = i - se * 75;
    mi = se / 60;
    se = se - mi * 60;
    p->sector = (sc - (sc / 10) * 10) + (sc / 10) * 16;
    p->second = (se / 10) * 16 + se - (se / 10) * 10;
    p->minute = (mi / 10) * 16 + mi - (mi / 10) * 10;

    return p;
}

//-------------------------------------------------------------------------
sceCdCBFunc sceCdCallback(sceCdCBFunc func)
{
    int oldstate;
    void *old_cb;

    DPRINTF("sceCdCallback %p\n", func);

    if (sceCdSync(1))
        return NULL;

    CpuSuspendIntr(&oldstate);

    old_cb = cb_data.user_cb;
    cb_data.user_cb = func;

    CpuResumeIntr(oldstate);

    return old_cb;
}

//-------------------------------------------------------------------------
int sceCdSC(int code, int *param)
{
    int result;

    DPRINTF("sceCdSC(0x%X, 0x%X)\n", code, *param);

    switch (code) {
        case CDSC_GET_INTRFLAG:
            result = cdvdman_stat.intr_ef;
            break;
        case CDSC_IO_SEMA:
            if (*param) {
                WaitSema(cdrom_io_sema);
            } else
                SignalSema(cdrom_io_sema);

            result = *param; // EE N-command code.
            break;
        case CDSC_GET_VERSION:
            result = CDVDMAN_MODULE_VERSION;
            break;
        case CDSC_GET_DEBUG_STATUS:
            *param = (int)&cdvdman_debug_print_flag;
            result = 0xFF;
            break;
        case CDSC_SET_ERROR:
            result = cdvdman_stat.err = *param;
            break;
        case CDSC_OPL_SHUTDOWN:
            oplShutdown(*param);
            result = 1;
            break;
        default:
            DPRINTF("sceCdSC unknown, code=0x%X param=0x%X \n", code, *param);
            result = 1; // dummy result
    }

    return result;
}

//-------------------------------------------------------------------------
static int cdvdman_writeSCmd(u8 cmd, const void *in, u16 in_size, void *out, u16 out_size)
{
    int i;
    u8 *p;

    WaitSema(cdvdman_scmdsema);

    if (CDVDreg_SDATAIN & 0x80) {
        SignalSema(cdvdman_scmdsema);
        return 0;
    }

    if (!(CDVDreg_SDATAIN & 0x40)) {
        do {
            (void)CDVDreg_SDATAOUT;
        } while (!(CDVDreg_SDATAIN & 0x40));
    }

    if (in_size > 0) {
        for (i = 0; i < in_size; i++) {
            p = (void *)((const u8 *)in + i);
            CDVDreg_SDATAIN = *p;
        }
    }

    CDVDreg_SCOMMAND = cmd;
    (void)CDVDreg_SCOMMAND;

    while (CDVDreg_SDATAIN & 0x80) {
        ;
    }

    i = 0;
    if (!(CDVDreg_SDATAIN & 0x40)) {
        do {
            if (i >= out_size) {
                break;
            }
            p = (void *)((u8 *)out + i);
            *p = CDVDreg_SDATAOUT;
            i++;
        } while (!(CDVDreg_SDATAIN & 0x40));
    }

    if (!(CDVDreg_SDATAIN & 0x40)) {
        do {
            (void)CDVDreg_SDATAOUT;
        } while (!(CDVDreg_SDATAIN & 0x40));
    }

    SignalSema(cdvdman_scmdsema);

    return 1;
}

//--------------------------------------------------------------
int cdvdman_sendSCmd(u8 cmd, const void *in, u16 in_size, void *out, u16 out_size)
{
    int r, retryCount = 0;

retry:

    r = cdvdman_writeSCmd(cmd & 0xff, in, in_size, out, out_size);
    if (r == 0) {
        DelayThread(2000);
        if (++retryCount <= 2500)
            goto retry;
    }

    DelayThread(2000);

    return 1;
}

//--------------------------------------------------------------
void cdvdman_cb_event(int reason)
{
    if (cb_data.user_cb != NULL) {
        cb_data.reason = reason;

        DPRINTF("cdvdman_cb_event reason: %d - setting cb alarm...\n", reason);

        if (QueryIntrContext())
            iSetAlarm(&gCallbackSysClock, &event_alarm_cb, &cb_data);
        else
            SetAlarm(&gCallbackSysClock, &event_alarm_cb, &cb_data);
    } else {
        cdvdman_signal_read_end();
    }
}

static unsigned int event_alarm_cb(void *args)
{
    struct cdvdman_cb_data *cb_data = args;

    cdvdman_signal_read_end_intr();
    if (cb_data->user_cb != NULL) // This interrupt does not occur immediately, hence check for the callback again here.
        cb_data->user_cb(cb_data->reason);
    return 0;
}

//-------------------------------------------------------------------------
/* Use these to signal that the reading process is complete.
   Do not run the user callback after the drive can be deemed ready,
   as this may break games that were not designed to expect the callback to be run
   after the drive becomes visibly ready via the libcdvd API.
   Hence if a user callback is registered, signal completion from
   within the interrupt handler, before the user callback is run. */
static void cdvdman_signal_read_end(void)
{
    int OldState;

    CpuSuspendIntr(&OldState);
    /* 还有排队/下一条未完成时，sceCdSync 必须继续等。 */
    if (cdread_outstand > 1) {
        cdread_outstand--;
        CpuResumeIntr(OldState);
        return;
    }
    cdread_outstand = 0;
    sync_flag = 0;
    CpuResumeIntr(OldState);
    SetEventFlag(cdvdman_stat.intr_ef, 9);
}

static void cdvdman_signal_read_end_intr(void)
{
    if (cdread_outstand > 1) {
        cdread_outstand--;
        return;
    }
    cdread_outstand = 0;
    sync_flag = 0;
    iSetEventFlag(cdvdman_stat.intr_ef, 9);
}

static void cdvdman_cdread_Thread(void *args)
{
    int OldState;
    int kick;

    while (1) {
        WaitSema(cdrom_rthread_sema);

        do {
            cdvdman_read(cdvdman_stat.cdread_lba, cdvdman_stat.cdread_sectors, cdvdman_stat.sector_size, cdvdman_stat.cdread_buf);

            CpuSuspendIntr(&OldState);
            cdread_io_busy = 0;
            kick = 0;
            if (cdread_pending) {
                cdvdman_stat.cdread_lba = cdread_pending_lba;
                cdvdman_stat.cdread_sectors = cdread_pending_sectors;
                cdvdman_stat.sector_size = cdread_pending_size;
                cdvdman_stat.cdread_buf = cdread_pending_buf;
                cdread_pending = 0;
                cdread_io_busy = 1;
                kick = 1;
            }
            CpuResumeIntr(OldState);

            /* This streaming callback is not compatible with the original SONY stream channel 0 (IOP) callback's design.
           The original is run from the interrupt handler, but we want it to run
           from a threaded environment because our interrupt is emulated. */
            if (Stm0Callback != NULL) {
                cdvdman_signal_read_end();

                /* Check that the streaming callback was not cleared, as this pointer may get changed between function calls.
                   As per the original semantics, once it is cleared, then it should not be called. */
                if (Stm0Callback != NULL)
                    Stm0Callback();
            } else
                cdvdman_cb_event(SCECdFuncRead); // Only runs if streaming is not in action.
        } while (kick);
    }
}

//-------------------------------------------------------------------------
static void cdvdman_startThreads(void)
{
    iop_thread_t thread_param;

    cdvdman_stat.status = SCECdStatPause;
    cdvdman_stat.err = SCECdErNO;

    thread_param.thread = &cdvdman_cdread_Thread;
    thread_param.stacksize = 0x1000;
    thread_param.priority = 0x0f;
    thread_param.attr = TH_C;
    thread_param.option = 0xABCD0000;

    cdvdman_ReadingThreadID = CreateThread(&thread_param);
    StartThread(cdvdman_ReadingThreadID, NULL);
}

//-------------------------------------------------------------------------
static void cdvdman_create_semaphores(void)
{
    iop_sema_t smp;

    smp.initial = 1;
    smp.max = 1;
    smp.attr = 0;
    smp.option = 0;

    cdvdman_scmdsema = CreateSema(&smp);
    smp.initial = 0;
    cdrom_rthread_sema = CreateSema(&smp);
}

//-------------------------------------------------------------------------
static int intrh_cdrom(void *common)
{
    if (CDVDreg_PWOFF & CDL_DATA_RDY)
        CDVDreg_PWOFF = CDL_DATA_RDY;

    if (CDVDreg_PWOFF & CDL_DATA_END) {
        // If IGR is enabled: Do not acknowledge the interrupt here. The EE-side IGR code will monitor and acknowledge it.
        if (cdvdman_settings.common.flags & IOPCORE_ENABLE_POFF) {
            CDVDreg_PWOFF = CDL_DATA_END; // Acknowldge power-off request.
        }
        iSetEventFlag(cdvdman_stat.intr_ef, 0x14); // Notify FILEIO and CDVDFSV of the power-off event.

// Call power-off callback here. OPL doesn't handle one, so do nothing.
#ifdef __USE_DEV9
        if (cdvdman_settings.common.flags & IOPCORE_ENABLE_POFF) {
            // If IGR is disabled, switch off the console.
            iWakeupThread(POFFThreadID);
        }
#endif
    } else
        CDVDreg_PWOFF = CDL_DATA_COMPLETE; // Acknowledge interrupt

    return 1;
}

static inline void InstallIntrHandler(void)
{
    RegisterIntrHandler(IOP_IRQ_CDVD, 1, &intrh_cdrom, NULL);
    EnableIntr(IOP_IRQ_CDVD);

    // Acknowledge hardware events (e.g. poweroff)
    if (CDVDreg_PWOFF & CDL_DATA_END)
        CDVDreg_PWOFF = CDL_DATA_END;
    if (CDVDreg_PWOFF & CDL_DATA_RDY)
        CDVDreg_PWOFF = CDL_DATA_RDY;
}

static int FanSpeedChange(u8 fan_speed)
{
    u8 result[16];

    return cdvdman_sendSCmd(0x28, &fan_speed, 1, result, sizeof(result));
}

/* SCMD 0x03, Mechacon subcommand 0x28: one input and one output byte. */
static int FanSpeedChange_2(u8 fan_speed)
{
    u8 rdbuf[1];
    u8 wrbuf[2];

    wrbuf[0] = 0x28;
    wrbuf[1] = fan_speed;

    return cdvdman_sendSCmd(0x03, wrbuf, 2, rdbuf, 1);
}

int _start(int argc, char **argv)
{
    // register exports
    RegisterLibraryEntries(&_exp_cdvdman);
    RegisterLibraryEntries(&_exp_cdvdstm);

    RegisterLibraryEntries(&_exp_smsutils);
#ifdef __USE_DEV9
    RegisterLibraryEntries(&_exp_dev9);
    dev9d_init();
#endif

    DeviceInit();

    RegisterLibraryEntries(&_exp_oplutils);

    // Setup the callback timer.
    USec2SysClock((cdvdman_settings.common.flags & IOPCORE_COMPAT_ACCU_READS) ? 5000 : 0, &gCallbackSysClock);

    // create SCMD/searchfile semaphores
    cdvdman_create_semaphores();

    // start cdvdman threads
    cdvdman_startThreads();

    // register cdrom device driver
    cdvdman_initdev();
    InstallIntrHandler();

    // hook MODLOAD's exports
    hookMODLOAD();

    // init disk type stuff
    cdvdman_initDiskType();

    FanSpeedChange_2(0x00);
    return MODULE_RESIDENT_END;
}

//-------------------------------------------------------------------------
void SetStm0Callback(StmCallback_t callback)
{
    Stm0Callback = callback;
}

//-------------------------------------------------------------------------
int _shutdown(void)
{
    DeviceDeinit();

    return 0;
}
