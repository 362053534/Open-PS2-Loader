/*
 Copyright 2022, Thanks to SP193
 Licenced under Academic Free License version 3.0
 Review OpenPS2Loader README & LICENSE files for further details.
 */

#include <audsrv.h>
#include <vorbis/vorbisfile.h>

#include "include/sound.h"
#include "include/opl.h"
#include "include/ioman.h"
#include "include/themes.h"
#include "include/ethsupport.h"
#include <sys/stat.h>

// Silence unused variable warnings from vorbisfile.h
static ov_callbacks OV_CALLBACKS_NOCLOSE __attribute__((unused));
static ov_callbacks OV_CALLBACKS_STREAMONLY __attribute__((unused));
static ov_callbacks OV_CALLBACKS_STREAMONLY_NOCLOSE __attribute__((unused));

/*--    Theme Sound Effects    ----------------------------------------------------------------------------------------------
---------------------------------------------------------------------------------------------------------------------------*/

extern unsigned char boot_adp[];
extern unsigned int size_boot_adp;

extern unsigned char cancel_adp[];
extern unsigned int size_cancel_adp;

extern unsigned char confirm_adp[];
extern unsigned int size_confirm_adp;

extern unsigned char cursor_adp[];
extern unsigned int size_cursor_adp;

extern unsigned char message_adp[];
extern unsigned int size_message_adp;

extern unsigned char transition_adp[];
extern unsigned int size_transition_adp;

extern unsigned char bd_connect_adp[];
extern unsigned int size_bd_connect_adp;

extern unsigned char bd_disconnect_adp[];
extern unsigned int size_bd_disconnect_adp;

struct sfxEffect
{
    const char *name;
    void *buffer;
    int size;
    int builtin;
    int duration_ms;
};

static struct sfxEffect sfx_files[SFX_COUNT] = {
    {"boot.adp"},
    {"cancel.adp"},
    {"confirm.adp"},
    {"cursor.adp"},
    {"message.adp"},
    {"transition.adp"},
    {"bd_connect.adp"},
    {"bd_disconnect.adp"},
};

static struct audsrv_adpcm_t sfx[SFX_COUNT];
static int audio_initialized = 0;

// Returns 0 if the specified file was read. The sfxEffect structure will not be updated unless the file is successfully read.
static int sfxRead(const char *full_path, struct sfxEffect *sfx)
{
    int adpcm;
    void *buffer;
    int ret, size;

    LOG("SFX: sfxRead('%s')\n", full_path);

    adpcm = open(full_path, O_RDONLY);
    if (adpcm < 0) {
        LOG("SFX: %s: Failed to open adpcm file %s\n", __FUNCTION__, full_path);
        return -ENOENT;
    }

    size = lseek(adpcm, 0, SEEK_END);
    lseek(adpcm, 0L, SEEK_SET);

    buffer = memalign(64, size);
    if (buffer == NULL) {
        LOG("SFX: Failed to allocate memory for SFX\n");
        close(adpcm);
        return -ENOMEM;
    }

    ret = read(adpcm, buffer, size);
    close(adpcm);

    if (ret != size) {
        LOG("SFX: Failed to read SFX: %d (expected %d)\n", ret, size);
        free(buffer);
        return -EIO;
    }

    sfx->buffer = buffer;
    sfx->size = size;
    sfx->builtin = 0;

    return 0;
}

static int sfxCalculateSoundDuration(int nSamples)
{
    float sampleRate = 44100; // 44.1kHz

    // Return duration in milliseconds
    return (nSamples / sampleRate) * 1000;
}

static void sfxInitDefaults(void)
{
    int i;

    for (i = 0; i < SFX_COUNT; i++)
        sfx_files[i].builtin = 1;

    sfx_files[SFX_BOOT].buffer = boot_adp;
    sfx_files[SFX_BOOT].size = size_boot_adp;
    sfx_files[SFX_CANCEL].buffer = cancel_adp;
    sfx_files[SFX_CANCEL].size = size_cancel_adp;
    sfx_files[SFX_CONFIRM].buffer = confirm_adp;
    sfx_files[SFX_CONFIRM].size = size_confirm_adp;
    sfx_files[SFX_CURSOR].buffer = cursor_adp;
    sfx_files[SFX_CURSOR].size = size_cursor_adp;
    sfx_files[SFX_MESSAGE].buffer = message_adp;
    sfx_files[SFX_MESSAGE].size = size_message_adp;
    sfx_files[SFX_TRANSITION].buffer = transition_adp;
    sfx_files[SFX_TRANSITION].size = size_transition_adp;
    sfx_files[SFX_BD_CONNECT].buffer = bd_connect_adp;
    sfx_files[SFX_BD_CONNECT].size = size_bd_connect_adp;
    sfx_files[SFX_BD_DISCONNECT].buffer = bd_disconnect_adp;
    sfx_files[SFX_BD_DISCONNECT].size = size_bd_disconnect_adp;
}

// Returns 0 (AUDSRV_ERR_NOERROR) if the sound was loaded successfully.
static int sfxLoad(struct sfxEffect *sfxData, audsrv_adpcm_t *sfx)
{
    int ret;

    // Calculate duration based on number of samples
    sfxData->duration_ms = sfxCalculateSoundDuration(((u32 *)sfxData->buffer)[3]);
    // Estimate duration based on filesize, if the ADPCM header was 0
    if (sfxData->duration_ms == 0)
        sfxData->duration_ms = sfxData->size / 47;

    ret = audsrv_load_adpcm(sfx, sfxData->buffer, sfxData->size);
    if (sfxData->builtin == 0) {
        free(sfxData->buffer);
        sfxData->buffer = NULL; // Mark the buffer as freed.
    }

    return ret;
}

// Returns number of audio files successfully loaded, < 0 if an unrecoverable error occurred.
int sfxInit(int bootSnd)
{
    char sound_path[256];
    char full_path[256];
    int ret, loaded;
    int thmSfxEnabled = 0;
    int i = 1;

    if (!audio_initialized) {
        LOG("SFX: %s: ERROR: not initialized!\n", __FUNCTION__);
        return -1;
    }

    audsrv_adpcm_init();
    sfxInitDefaults();
    audioSetVolume();

    // 内置主题（默认/ Coverflow）没有主题目录，跳过自定义音效
    int themeID = thmGetGuiValue();
    if (themeID >= THM_NUM_BUILTIN) {
        // Get theme path for sfx
        char *thmPath = thmGetFilePath(themeID);
        snprintf(sound_path, sizeof(sound_path), "%ssound", thmPath);

        // Check for custom sfx folder
        DIR *dir = opendir(sound_path);
        if (dir != NULL) {
            thmSfxEnabled = 1;
            closedir(dir);
        }
    }

    loaded = 0;
    i = bootSnd ? 0 : 1;
    for (; i < SFX_COUNT; i++) {
        if (thmSfxEnabled) {
            snprintf(full_path, sizeof(full_path), "%s/%s", sound_path, sfx_files[i].name);
            ret = sfxRead(full_path, &sfx_files[i]);
            if (ret != 0) {
                LOG("SFX: %s could not be loaded. Using default sound %d.\n", full_path, ret);
            }
        } else
            snprintf(full_path, sizeof(full_path), "builtin/%s", sfx_files[i].name);

        ret = sfxLoad(&sfx_files[i], &sfx[i]);
        if (ret == 0) {
            LOG("SFX: Loaded %s, size=%d, duration=%dms\n", full_path, sfx_files[i].size, sfx_files[i].duration_ms);
            loaded++;
        } else {
            LOG("SFX: failed to load %s, error %d\n", full_path, ret);
        }
    }

    return loaded;
}

int sfxGetSoundDuration(int id)
{
    if (!audio_initialized) {
        LOG("SFX: %s: ERROR: not initialized!\n", __FUNCTION__);
        return 0;
    }

    return sfx_files[id].duration_ms;
}

void sfxPlay(int id)
{
    if (!audio_initialized) {
        LOG("SFX: %s: ERROR: not initialized!\n", __FUNCTION__);
        return;
    }

    if (gEnableSFX) {
        audsrv_ch_play_adpcm(id, &sfx[id]);
    }
}

/*--    Theme Background Music    -------------------------------------------------------------------------------------------
---------------------------------------------------------------------------------------------------------------------------*/

#define BGM_RING_BUFFER_COUNT 16
#define BGM_RING_BUFFER_SIZE  4096
#define BGM_THREAD_BASE_PRIO  33 // 原为0x40；作为解码线程优先级，低于主线程(31)、高于图片IO线程(34)，避免浏览封面时背景音乐卡顿
#define BGM_THREAD_STACK_SIZE 0x1000
#define BGM_THREAD_EXIT_PRIO  1 // 线程退出前临时提到的最高优先级：保证通知主线程后自己立即跑完退出流程，主线程醒来时它已处于DORMANT

// 送入audsrv的块大小按"每次喂给声卡的数据量"对齐，使单次wait_audio的等待约等于一个块的时长(约21毫秒)，
// 不会长时间占住audsrv的RPC线程而拖慢界面音效。须与ps2sdk中audsrv的缓冲放大倍数(RINGBUF_LOW_RATE_FEED_COUNT=20)配套
#define BGM_AUDSRV_FEED_SAMPLES   512   // audsrv每次喂给声卡的输出采样点数
#define BGM_AUDSRV_OUTPUT_RATE    48000 // audsrv输出采样率(Hz)
#define BGM_AUDSRV_CHUNK_FEED_NUM 2     // 每块包含的"喂给量"个数
#define BGM_SAMPLE_BYTES          2     // 解码输出为16位
#define BGM_MS_PER_SEC            1000  // 每秒毫秒数
#define BGM_WHOLE_BLOCK_MAX_MS    32    // 整个解码块的播放时长不超过它就整块送入，不再拆分(如44.1kHz立体声约23毫秒)
#define BGM_TAIL_MERGE_DIVISOR    2     // 末尾零头不足半个块(块大小/该值)时并入上一段一起送，避免单独发起一次很小的请求

#define BGM_MEMORY_LOAD_MAX_SIZE (2 * 1024 * 1024) // 自定义背景音乐文件不超过该大小(字节)时整个读入内存播放

extern void *_gp;

static int bgmThreadID, bgmIoThreadID;
static int outSema, inSema, exitSema; // exitSema：两个音乐线程退出时各发一个信号，bgmStop据此等待它们真正退出
static unsigned char terminateFlag, bgmIsPlaying;
// 加载在IO线程上异步进行：bgmLoading从提交任务到任务结束为1；bgmLoadSema在任务结束时发信号，供bgmStop等待；
// bgmThreadsStarted表示任务已成功启动两个音乐线程(任务失败时它自己已清理资源，bgmStop无需再清理)
static volatile unsigned char bgmLoading, bgmThreadsStarted;
static int bgmLoadSema = -1; // 只创建一次，不随每次播放销毁，避免与正在等待的bgmStop冲突
static unsigned char rdPtr, wrPtr;
static int bgmChunkSize = BGM_RING_BUFFER_SIZE; // 每次送入audsrv的字节数，在bgmStart中按采样率和声道数计算
static char bgmBuffer[BGM_RING_BUFFER_COUNT][BGM_RING_BUFFER_SIZE];

static u8 bgmThreadStack[BGM_THREAD_STACK_SIZE] __attribute__((aligned(16)));
static u8 bgmIoThreadStack[BGM_THREAD_STACK_SIZE] __attribute__((aligned(16)));

static OggVorbis_File *vorbisFile;

static void bgmThread(void *arg)
{
    while (!terminateFlag) {
        SleepThread();

        // 退出时不再把缓冲里剩余的块播完，避免等待audsrv而拖慢甚至卡住关机/启动游戏
        while (!terminateFlag && PollSema(outSema) == outSema) {
            // 把一个解码块按bgmChunkSize拆成小段送入，每段先等待audsrv腾出对应空间。
            // 剩余不足"一段加半段"时就一次送完(零头并入最后一段，整块送入时也走这个分支)；
            // 块大小与每段大小都是帧字节数的整数倍，不会错位
            int offset = 0;
            while (!terminateFlag && offset < BGM_RING_BUFFER_SIZE) {
                int len = BGM_RING_BUFFER_SIZE - offset;
                if (len >= bgmChunkSize + bgmChunkSize / BGM_TAIL_MERGE_DIVISOR)
                    len = bgmChunkSize;

                audsrv_wait_audio(len);
                audsrv_play_audio(bgmBuffer[rdPtr] + offset, len);
                offset += len;
            }
            rdPtr = (rdPtr + 1) % BGM_RING_BUFFER_COUNT;

            SignalSema(inSema);
        }
    }

    audsrv_stop_audio();

    rdPtr = 0;
    wrPtr = 0;

    bgmIsPlaying = 0;

    // 先提到最高优先级再通知主线程：否则主线程(优先级更高)会在通知的瞬间抢占，
    // 在本线程还没退出时就删除线程并复用同一块栈
    ChangeThreadPriority(GetThreadId(), BGM_THREAD_EXIT_PRIO);
    SignalSema(exitSema);
}

static void bgmIoThread(void *arg)
{
    int partsToRead, decodeTotal, bitStream, i;

    do {
        WaitSema(inSema);
        partsToRead = 1;

        while ((wrPtr + partsToRead < BGM_RING_BUFFER_COUNT) && (PollSema(inSema) == inSema))
            partsToRead++;

        // 一次攒了多个空块时要把这些块全部解码(二维数组连续存放，且上面的条件保证不会越过数组末尾)，
        // 原来只解码第一块却前移partsToRead块，多出的块会播放旧数据，造成重复或卡顿
        decodeTotal = BGM_RING_BUFFER_SIZE * partsToRead;
        int bufferPtr = 0;
        do {
            int ret = ov_read(vorbisFile, bgmBuffer[wrPtr] + bufferPtr, decodeTotal, 0, 2, 1, &bitStream);
            if (ret > 0) {
                bufferPtr += ret;
                decodeTotal -= ret;
            } else if (ret < 0) {
                LOG("BGM: I/O error while reading.\n");
                terminateFlag = 1;
                break;
            } else if (ret == 0)
                ov_pcm_seek(vorbisFile, 0);
        } while (decodeTotal > 0);

        wrPtr = (wrPtr + partsToRead) % BGM_RING_BUFFER_COUNT;
        for (i = 0; i < partsToRead; i++)
            SignalSema(outSema);
        WakeupThread(bgmThreadID);
    } while (!terminateFlag && gEnableBGM);

    terminateFlag = 1;
    WakeupThread(bgmThreadID);

    // 同播放线程：提到最高优先级后再通知，保证通知后立即退出
    ChangeThreadPriority(GetThreadId(), BGM_THREAD_EXIT_PRIO);
    SignalSema(exitSema);
}

// 内嵌的默认背景音乐(audio/default_bgm.ogg，由bin2c生成)
extern unsigned char default_bgm_ogg[];
extern unsigned int size_default_bgm_ogg;

// 背景音乐的内存读取状态(内嵌的默认音乐，或整个读入内存的自定义音乐)。同一时间只有一个播放实例，用静态变量即可
static struct
{
    const unsigned char *data;
    long size;
    long pos;
    unsigned char *owned; // 需要释放的内存(自定义音乐读入的缓冲)；内嵌音乐在ELF里，为NULL
} memBgm;

// 以下四个回调让vorbisfile直接从内存读取数据，语义与fread/fseek/ftell/fclose一致
static size_t memBgmRead(void *ptr, size_t size, size_t nmemb, void *datasource)
{
    size_t items;

    if (size == 0 || nmemb == 0)
        return 0;

    items = (size_t)(memBgm.size - memBgm.pos) / size;
    if (items > nmemb)
        items = nmemb;

    memcpy(ptr, memBgm.data + memBgm.pos, items * size);
    memBgm.pos += (long)(items * size);
    return items;
}

static int memBgmSeek(void *datasource, ogg_int64_t offset, int whence)
{
    ogg_int64_t target;

    if (whence == SEEK_SET)
        target = offset;
    else if (whence == SEEK_CUR)
        target = memBgm.pos + offset;
    else if (whence == SEEK_END)
        target = memBgm.size + offset;
    else
        return -1;

    if (target < 0 || target > memBgm.size)
        return -1;

    memBgm.pos = (long)target;
    return 0;
}

static int memBgmClose(void *datasource)
{
    // 内嵌音乐在ELF里无需释放；自定义音乐读入的缓冲在这里释放(ov_clear时调用)
    if (memBgm.owned != NULL) {
        free(memBgm.owned);
        memBgm.owned = NULL;
    }
    memBgm.data = NULL;
    return 0;
}

static long memBgmTell(void *datasource)
{
    return memBgm.pos;
}

static const ov_callbacks memBgmCallbacks = {memBgmRead, memBgmSeek, memBgmClose, memBgmTell};

// 从内存中的Ogg数据初始化解码器，成功返回0
// owned不为NULL时表示data是malloc出来的缓冲，成功后由解码器在ov_clear时释放，失败时在这里释放
static int bgmOpenMemory(const unsigned char *data, long size, unsigned char *owned)
{
    memBgm.data = data;
    memBgm.size = size;
    memBgm.pos = 0;
    memBgm.owned = owned;

    memset(vorbisFile, 0, sizeof(OggVorbis_File));
    if (ov_open_callbacks(&memBgm, vorbisFile, NULL, 0, memBgmCallbacks) < 0) {
        LOG("BGM: Ogg data in memory is invalid.\n");
        // 解码初始化失败时vorbisfile不会调用关闭回调，缓冲需要在这里释放
        free(owned);
        memBgm.owned = NULL;
        memBgm.data = NULL;
        memset(vorbisFile, 0, sizeof(OggVorbis_File));
        return -ENOENT;
    }

    return 0;
}

// 用文件路径打开并初始化解码器，成功返回0。失败时不留下打开的句柄，vorbisFile保持清零，可继续尝试下一个来源
// 文件不超过 BGM_MEMORY_LOAD_MAX_SIZE 时整个读入内存播放(播放期间不再访问存储设备)，否则或内存不足时从文件流式读取
static int bgmOpenPath(const char *path)
{
    FILE *bgmFile;
    long fileSize = -1;

    if (path[0] == '\0')
        return -ENOENT;

    bgmFile = fopen(path, "rb");
    if (bgmFile == NULL) {
        LOG("BGM: Failed to open Ogg file %s\n", path);
        return -ENOENT;
    }

    if (fseek(bgmFile, 0, SEEK_END) == 0)
        fileSize = ftell(bgmFile);
    if (fseek(bgmFile, 0, SEEK_SET) != 0)
        fileSize = -1;

    if (fileSize > 0 && fileSize <= BGM_MEMORY_LOAD_MAX_SIZE) {
        unsigned char *buffer = malloc(fileSize);

        if (buffer != NULL) {
            if (fread(buffer, 1, fileSize, bgmFile) == (size_t)fileSize) {
                fclose(bgmFile);
                LOG("BGM: Loaded %s into memory (%ld bytes)\n", path, fileSize);
                return bgmOpenMemory(buffer, fileSize, buffer);
            }

            // 读取不完整：放弃内存方式，回到文件开头改为流式读取
            LOG("BGM: Failed to read %s into memory, falling back to streaming.\n", path);
            free(buffer);
            if (fseek(bgmFile, 0, SEEK_SET) != 0) {
                fclose(bgmFile);
                return -ENOENT;
            }
        } else
            LOG("BGM: Not enough memory for %s (%ld bytes), falling back to streaming.\n", path, fileSize);
    }

    memset(vorbisFile, 0, sizeof(OggVorbis_File));
    if (ov_open_callbacks(bgmFile, vorbisFile, NULL, 0, OV_CALLBACKS_DEFAULT) < 0) {
        LOG("BGM: Input does not appear to be an Ogg bitstream.\n");
        // 解码初始化失败时vorbisfile不会接管文件，ov_clear也不会关闭它，必须手动关闭，
        // 否则每次重载主题都会泄露一个文件句柄，累积后封面等图片全部无法读取
        fclose(bgmFile);
        memset(vorbisFile, 0, sizeof(OggVorbis_File));
        return -ENOENT;
    }

    return 0;
}

// 从内嵌的默认背景音乐初始化解码器，成功返回0
static int bgmOpenEmbedded(void)
{
    return bgmOpenMemory(default_bgm_ogg, (long)size_default_bgm_ogg, NULL);
}

// 按优先级选择背景音乐：
// 1. 设置里的自定义路径有效时，任何主题都播放它
// 2. 第三方主题自带sound/bgm.ogg时播放自带的
// 3. 内置主题播放内嵌的默认背景音乐；第三方主题没有自带文件时不播放(不回退到内嵌音乐)
static int bgmLoad(void)
{
    char bgmPath[256];
    int themeID = thmGetGuiValue();

    vorbisFile = malloc(sizeof(OggVorbis_File));
    memset(vorbisFile, 0, sizeof(OggVorbis_File));

    if (bgmOpenPath(gDefaultBGMPath) == 0)
        return 0;

    if (themeID >= THM_NUM_BUILTIN) {
        snprintf(bgmPath, sizeof(bgmPath), "%ssound/bgm.ogg", thmGetFilePath(themeID));
        return bgmOpenPath(bgmPath);
    }

    return bgmOpenEmbedded();
}

// 在指定根目录中查找第一个后缀为.ogg的文件(不区分大小写)，找到返回0
// 只有一个出口，无论是否找到都会先closedir，避免目录句柄泄露
static int bgmSearchDir(const char *root, char *path, int size)
{
    struct dirent *pdirent;
    DIR *pdir;
    int found = -1;

    pdir = opendir(root);
    if (pdir == NULL)
        return -1;

    while ((pdirent = readdir(pdir)) != NULL) {
        int len = strlen(pdirent->d_name);

        if (pdirent->d_type == DT_DIR || len < 5)
            continue;
        if (strcasecmp(&pdirent->d_name[len - 4], ".ogg") != 0)
            continue;
        // 路径超过缓冲区长度时跳过，避免截断后无法打开
        if (strlen(root) + len >= (size_t)size)
            continue;

        snprintf(path, size, "%s%s", root, pdirent->d_name);
        found = 0;
        break;
    }

    closedir(pdir);
    return found;
}

// 自动搜索背景音乐：按 APA → BDM → SMB → 记忆卡 的顺序，
// 搜索各设备根目录(SMB需共享已打开)，找到第一个ogg文件即返回0
int bgmSearchFile(char *path, int size)
{
    char root[16];
    const char *smbRoot;
    int i;

    // APA/BDM未挂载时opendir会直接失败返回，不会分配句柄，无需事先判断状态
    if (bgmSearchDir("pfs0:/", path, size) == 0)
        return 0;

    for (i = 0; i < MAX_BDM_DEVICES; i++) {
        snprintf(root, sizeof(root), "mass%d:/", i);
        if (bgmSearchDir(root, path, size) == 0)
            return 0;
    }

    smbRoot = ethGetShareRoot();
    if (smbRoot != NULL && bgmSearchDir(smbRoot, path, size) == 0)
        return 0;

    if (bgmSearchDir("mc0:/", path, size) == 0 || bgmSearchDir("mc1:/", path, size) == 0)
        return 0;

    return -1;
}

static int bgmInit(void)
{
    ee_thread_t thread;
    ee_sema_t sema;
    int result;

    terminateFlag = 0;
    rdPtr = 0;
    wrPtr = 0;

    sema.max_count = BGM_RING_BUFFER_COUNT;
    sema.init_count = BGM_RING_BUFFER_COUNT;
    sema.attr = 0;
    sema.option = (u32) "bgm-in-sema";
    inSema = CreateSema(&sema);

    if (inSema >= 0) {
        sema.max_count = BGM_RING_BUFFER_COUNT;
        sema.init_count = 0;
        sema.attr = 0;
        sema.option = (u32) "bgm-out-sema";
        outSema = CreateSema(&sema);

        if (outSema < 0) {
            DeleteSema(inSema);
            return outSema;
        }

        // 两个线程各发一个信号，计数上限为2
        sema.max_count = 2;
        sema.init_count = 0;
        sema.attr = 0;
        sema.option = (u32) "bgm-exit-sema";
        exitSema = CreateSema(&sema);

        if (exitSema < 0) {
            DeleteSema(inSema);
            DeleteSema(outSema);
            return exitSema;
        }
    } else
        return inSema;

    thread.func = &bgmThread;
    thread.stack = bgmThreadStack;
    thread.stack_size = sizeof(bgmThreadStack);
    thread.gp_reg = &_gp;
    thread.initial_priority = BGM_THREAD_BASE_PRIO - 1; // 播放线程比解码线程高一级，可随时抢占解码线程及时送数据
    thread.attr = 0;
    thread.option = 0;

    // BGM thread will start in DORMANT state.
    bgmThreadID = CreateThread(&thread);

    if (bgmThreadID >= 0) {
        thread.func = &bgmIoThread;
        thread.stack = bgmIoThreadStack;
        thread.stack_size = sizeof(bgmIoThreadStack);
        thread.gp_reg = &_gp;
        thread.initial_priority = BGM_THREAD_BASE_PRIO;
        thread.attr = 0;
        thread.option = 0;

        // BGM I/O thread will start in DORMANT state.
        bgmIoThreadID = CreateThread(&thread);
        if (bgmIoThreadID >= 0) {
            result = 0;
        } else {
            DeleteSema(inSema);
            DeleteSema(outSema);
            DeleteSema(exitSema);
            DeleteThread(bgmThreadID);
            result = bgmIoThreadID;
        }
    } else {
        result = bgmThreadID;
        DeleteSema(inSema);
        DeleteSema(outSema);
        DeleteSema(exitSema);
    }

    return result;
}

static void bgmDeinit(void)
{
    DeleteSema(inSema);
    DeleteSema(outSema);
    DeleteSema(exitSema);
    DeleteThread(bgmThreadID);
    DeleteThread(bgmIoThreadID);

    // Vorbisfile takes care of fclose.
    // 加载任务发现已请求停止而没有加载时，vorbisFile为NULL
    if (vorbisFile != NULL) {
        ov_clear(vorbisFile);
        free(vorbisFile);
        vorbisFile = NULL;
    }
}

// 在IO线程上执行：打开并解码初始化背景音乐，成功则启动两个音乐线程。
// 读文件(整个读入内存或打开流)可能耗时很久，放在IO队列里，主线程才能继续画界面，加载图标正常转动
static void bgmLoadAndPlay(void)
{
    struct audsrv_fmt_t audsrvFmt;
    int loaded = 0;

    // bgmStop已请求停止时不再加载
    if (!terminateFlag)
        loaded = (bgmLoad() == 0);

    // 加载期间bgmStop被调用时(读文件可能很久)，加载完也不再启动音乐线程
    if (!loaded || terminateFlag) {
        bgmDeinit();
        bgmIsPlaying = 0;
    } else {
        vorbis_info *vi = ov_info(vorbisFile, -1);
        ov_pcm_seek(vorbisFile, 0);

        audsrvFmt.channels = vi->channels;
        audsrvFmt.freq = vi->rate;
        audsrvFmt.bits = 16;

        audsrv_set_format(&audsrvFmt);

        // 每段 = 2倍audsrv的喂给量(8kHz单声道为340字节，约21毫秒)；不能超过解码块大小。
        // 整个解码块本身就播放不超过约32毫秒的格式(如44.1kHz立体声)等待很短，保持整块送入不拆分
        bgmChunkSize = BGM_AUDSRV_CHUNK_FEED_NUM * ((BGM_AUDSRV_FEED_SAMPLES * audsrvFmt.freq) / BGM_AUDSRV_OUTPUT_RATE) * audsrvFmt.channels * BGM_SAMPLE_BYTES;
        if (BGM_RING_BUFFER_SIZE * BGM_MS_PER_SEC <= BGM_WHOLE_BLOCK_MAX_MS * audsrvFmt.freq * audsrvFmt.channels * BGM_SAMPLE_BYTES)
            bgmChunkSize = BGM_RING_BUFFER_SIZE;
        if (bgmChunkSize <= 0 || bgmChunkSize > BGM_RING_BUFFER_SIZE)
            bgmChunkSize = BGM_RING_BUFFER_SIZE;

        bgmThreadsStarted = 1;

        // 必须先启动播放线程：本任务运行在IO线程(优先级34)，解码线程(33)一启动就会抢占并立刻去唤醒播放线程，
        // 如果播放线程还没启动，这次唤醒就丢了，播放线程随后先睡眠再也等不到唤醒，而解码线程填满缓冲后又在等它，双方互等没有声音。
        // 先启动播放线程(32)，它会抢占本任务并进入睡眠，之后解码线程的唤醒就不会丢
        StartThread(bgmThreadID, NULL);
        StartThread(bgmIoThreadID, NULL);
    }

    bgmLoading = 0;
    SignalSema(bgmLoadSema);
}

void bgmStart(void)
{
    if (!audio_initialized) {
        LOG("BGM: %s: ERROR: not initialized!\n", __FUNCTION__);
        return;
    }

    if (bgmLoadSema < 0) {
        ee_sema_t sema;

        sema.max_count = 1;
        sema.init_count = 0;
        sema.attr = 0;
        sema.option = (u32) "bgm-load-sema";
        bgmLoadSema = CreateSema(&sema);
        if (bgmLoadSema < 0)
            return;
    }

    // 上一次的音乐线程已启动但自己退出了(解码出错)，没人调用bgmStop：先回收，避免泄露
    if (bgmThreadsStarted)
        bgmStop();

    int ret = bgmInit();
    if (ret >= 0) {
        // 清掉上一次可能残留的信号，避免bgmStop误以为这次加载已经结束
        while (PollSema(bgmLoadSema) == bgmLoadSema)
            ;

        // 立即标记为播放中，防止加载期间被重复启动；加载失败时由任务清零
        bgmIsPlaying = 1;
        bgmThreadsStarted = 0;
        bgmLoading = 1;

        // 提交到IO队列；IO队列被阻塞(如正在启动游戏)或请求槽已满时无法排队，退回在当前线程同步执行
        if (ioPutRequest(IO_CUSTOM_SIMPLEACTION, &bgmLoadAndPlay) != IO_OK)
            bgmLoadAndPlay();
    }
}

void bgmStop(void)
{
    if (!audio_initialized) {
        LOG("BGM: %s: ERROR: not initialized!\n", __FUNCTION__);
        return;
    }

    LOG("BGM: terminating threads...\n");

    terminateFlag = 1;

    // 加载任务还在IO队列里或正在执行时，先等它结束(它看到terminateFlag会跳过加载或不再启动线程)
    if (bgmLoading)
        WaitSema(bgmLoadSema);

    // 加载失败或被中止时，任务已自行清理，没有音乐线程需要停
    if (!bgmThreadsStarted) {
        LOG("BGM: stopped (not started).\n");
        return;
    }
    bgmThreadsStarted = 0;

    WakeupThread(bgmThreadID);
    // 播放线程退出时不再归还空块，需在此唤醒可能阻塞在WaitSema(inSema)上的解码线程，使其检查terminateFlag后退出
    SignalSema(inSema);

    // 用信号量等待两个音乐线程各自退出。不能用SleepThread：主线程的唤醒通道与垂直同步中断共用，
    // 会吞掉垂直同步的唤醒并让界面一直睡在rmEndFrame里；信号量也不会留下未取消的闹钟
    WaitSema(exitSema);
    WaitSema(exitSema);

    bgmDeinit();

    LOG("BGM: stopped.\n");
}

int isBgmPlaying(void)
{
    int ret = (int)bgmIsPlaying;

    return ret;
}

// HACK: BGM stutters while perfroming certain tasks, mute during these operations and unmute once completed.
void bgmMute(void)
{
    if (audio_initialized)
        audsrv_set_volume(0);
}

void bgmUnMute(void)
{
    if (audio_initialized)
        audsrv_set_volume(gBGMVolume);
}

/*--    General Audio    ------------------------------------------------------------------------------------------------------
-----------------------------------------------------------------------------------------------------------------------------*/

void audioInit(void)
{
    if (!audio_initialized) {
        if (audsrv_init() != 0) {
            LOG("AUDIO: Failed to initialize audsrv\n");
            LOG("AUDIO: Audsrv returned error string: %s\n", audsrv_get_error_string());
            return;
        }
        audio_initialized = 1;
    }
}

void audioEnd(void)
{
    if (!audio_initialized) {
        LOG("AUDIO: %s: ERROR: not initialized!\n", __FUNCTION__);
        return;
    }

    if (gEnableBGM && isBgmPlaying())
        bgmStop();

    audsrv_quit();
    audio_initialized = 0;
}

void audioSetVolume(void)
{
    int i;

    if (!audio_initialized) {
        LOG("AUDIO: %s: ERROR: not initialized!\n", __FUNCTION__);
        return;
    }

    for (i = 1; i < SFX_COUNT; i++)
        audsrv_adpcm_set_volume(i, gSFXVolume);

    audsrv_adpcm_set_volume(0, gBootSndVolume);
    audsrv_set_volume(gBGMVolume);
}
