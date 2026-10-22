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

extern void *_gp;

static int bgmThreadID, bgmIoThreadID;
static int outSema, inSema;
static unsigned char terminateFlag, bgmIsPlaying;
static unsigned char rdPtr, wrPtr;
static char bgmBuffer[BGM_RING_BUFFER_COUNT][BGM_RING_BUFFER_SIZE];
static volatile unsigned char bgmThreadRunning, bgmIoThreadRunning;

static u8 bgmThreadStack[BGM_THREAD_STACK_SIZE] __attribute__((aligned(16)));
static u8 bgmIoThreadStack[BGM_THREAD_STACK_SIZE] __attribute__((aligned(16)));

static OggVorbis_File *vorbisFile;

static void bgmThread(void *arg)
{
    bgmThreadRunning = 1;

    while (!terminateFlag) {
        SleepThread();

        // 退出时不再把缓冲里剩余的块播完，避免等待audsrv而拖慢甚至卡住关机/启动游戏
        while (!terminateFlag && PollSema(outSema) == outSema) {
            audsrv_wait_audio(BGM_RING_BUFFER_SIZE);
            audsrv_play_audio(bgmBuffer[rdPtr], BGM_RING_BUFFER_SIZE);
            rdPtr = (rdPtr + 1) % BGM_RING_BUFFER_COUNT;

            SignalSema(inSema);
        }
    }

    audsrv_stop_audio();

    rdPtr = 0;
    wrPtr = 0;

    bgmThreadRunning = 0;
    bgmIsPlaying = 0;
}

static void bgmIoThread(void *arg)
{
    int partsToRead, decodeTotal, bitStream, i;

    bgmIoThreadRunning = 1;
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

    bgmIoThreadRunning = 0;
    terminateFlag = 1;
    WakeupThread(bgmThreadID);
}

// 判断当前主题是否会使用设置里的背景音乐路径：内置主题总是使用；
// 第三方主题自带sound/bgm.ogg时用自带的，没有时回退到设置里的路径。用stat判断，不开句柄
int bgmUsesDefaultPath(void)
{
    char thmBgm[256];
    struct stat st;
    int themeID = thmGetGuiValue();

    if (themeID < THM_NUM_BUILTIN)
        return 1;

    snprintf(thmBgm, sizeof(thmBgm), "%ssound/bgm.ogg", thmGetFilePath(themeID));
    return stat(thmBgm, &st) != 0;
}

static int bgmLoad(void)
{
    FILE *bgmFile;
    char bgmPath[256];

    vorbisFile = malloc(sizeof(OggVorbis_File));
    memset(vorbisFile, 0, sizeof(OggVorbis_File));

    int themeID = thmGetGuiValue();
    if (themeID >= THM_NUM_BUILTIN) {
        char *thmPath = thmGetFilePath(themeID);
        snprintf(bgmPath, sizeof(bgmPath), "%ssound/bgm.ogg", thmPath);
    } else
        snprintf(bgmPath, sizeof(bgmPath), gDefaultBGMPath);

    bgmFile = fopen(bgmPath, "rb");
    // 第三方主题没有自带bgm.ogg时，回退到设置里的背景音乐路径
    if (bgmFile == NULL && themeID >= THM_NUM_BUILTIN && gDefaultBGMPath[0] != '\0') {
        snprintf(bgmPath, sizeof(bgmPath), "%s", gDefaultBGMPath);
        bgmFile = fopen(bgmPath, "rb");
    }
    if (bgmFile == NULL) {
        LOG("BGM: Failed to open Ogg file %s\n", bgmPath);
        return -ENOENT;
    }

    if (ov_open_callbacks(bgmFile, vorbisFile, NULL, 0, OV_CALLBACKS_DEFAULT) < 0) {
        LOG("BGM: Input does not appear to be an Ogg bitstream.\n");
        // 解码初始化失败时vorbisfile不会接管文件，ov_clear也不会关闭它，必须手动关闭，
        // 否则每次重载主题都会泄露一个文件句柄，累积后封面等图片全部无法读取
        fclose(bgmFile);
        return -ENOENT;
    }

    return 0;
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
    bgmThreadRunning = 0;
    bgmIoThreadRunning = 0;

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
            DeleteThread(bgmThreadID);
            result = bgmIoThreadID;
        }
    } else {
        result = bgmThreadID;
        DeleteSema(inSema);
        DeleteSema(outSema);
    }

    return result;
}

static void bgmDeinit(void)
{
    DeleteSema(inSema);
    DeleteSema(outSema);
    DeleteThread(bgmThreadID);
    DeleteThread(bgmIoThreadID);

    // Vorbisfile takes care of fclose.
    ov_clear(vorbisFile);
    free(vorbisFile);
    vorbisFile = NULL;
}

static void bgmShutdownDelayCallback(s32 alarm_id, u16 time, void *common)
{
    iWakeupThread((int)common);
}

void bgmStart(void)
{
    struct audsrv_fmt_t audsrvFmt;

    if (!audio_initialized) {
        LOG("BGM: %s: ERROR: not initialized!\n", __FUNCTION__);
        return;
    }

    // 需要使用设置里的路径但路径为空时直接返回，不创建线程、不尝试打开文件
    if (gDefaultBGMPath[0] == '\0' && bgmUsesDefaultPath())
        return;

    int ret = bgmInit();
    if (ret >= 0) {
        if (bgmLoad() != 0) {
            bgmDeinit();
            return;
        }

        vorbis_info *vi = ov_info(vorbisFile, -1);
        ov_pcm_seek(vorbisFile, 0);

        audsrvFmt.channels = vi->channels;
        audsrvFmt.freq = vi->rate;
        audsrvFmt.bits = 16;

        audsrv_set_format(&audsrvFmt);

        bgmIsPlaying = 1;
        // 在主线程中先置位运行标志：线程与主线程同优先级，StartThread后不会立即执行，
        // 若此时快速调用bgmStop，会因标志仍为0而不等待线程退出就释放资源，导致死机
        bgmThreadRunning = 1;
        bgmIoThreadRunning = 1;

        StartThread(bgmIoThreadID, NULL);
        StartThread(bgmThreadID, NULL);
    }
}

void bgmStop(void)
{
    int threadId;

    if (!audio_initialized) {
        LOG("BGM: %s: ERROR: not initialized!\n", __FUNCTION__);
        return;
    }

    LOG("BGM: terminating threads...\n");

    terminateFlag = 1;
    WakeupThread(bgmThreadID);
    // 播放线程退出时不再归还空块，需在此唤醒可能阻塞在WaitSema(inSema)上的解码线程，使其检查terminateFlag后退出
    SignalSema(inSema);

    threadId = GetThreadId();
    while (bgmIoThreadRunning) {
        SetAlarm(200 * 16, &bgmShutdownDelayCallback, (void *)threadId);
        SleepThread();
    }
    while (bgmThreadRunning) {
        SetAlarm(200 * 16, &bgmShutdownDelayCallback, (void *)threadId);
        SleepThread();
    }

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
