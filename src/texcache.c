#include "include/opl.h"
#include "include/texcache.h"
#include "include/textures.h"
#include "include/ioman.h"
#include "include/gui.h"
#include "include/util.h"
#include "include/renderman.h"
#include "include/pad.h"
#include <pthread.h>

extern GSGLOBAL *gsGlobal;

int ForceRefreshPrevTexCache = 0;
int forceSkipQr = 0;
int texLoading = 0;

static int PrevCacheID = -2;
static int PrevCacheID_COV = -2;
static int PrevCacheID_ICO = -2;
static int PrevCacheID_BG = -2;

//int artQrCount = 0; // 给加入Qr缓存队列的Art图计数
//int artQrDone = 0; // 代表一轮Art图已全部进入Qr队列
static int buttonPressedOnce = 0;  // 快速连按时，每次按键只重置CD帧数一次
static int cdFrames = 30;         // 一轮Art图Qr后的CD时间(帧数)
static int skipQr = 0;             // 判断是否可以跳过请求Qr队列
static int cdFramesCount = 0; // 手动重复按键
// 光标改变时递增；active COV 请求不能从 IO worker 中途摘除时，
// 仍可在解码完成前后判断它是否属于最新目标。
static u32 artRequestGeneration = 1;

//int buttonFrames = 0; // 按住按键的帧数，用来跳过cdFrames
//static u64 prevGuiFrameId = 0; // 和guiFrameId进行比对，判断是否完成了一轮Qr
static char *curStartUp = NULL;
static int findBGCount = 0; // 寻找背景图的次数
static int usePthread = 0;  // 使用pthread多线程方法加载图片
//static int texLoadingTimeOut = 0;  // 用于判断加载计数异常时，将texLoading置为0

// 申请线程
pthread_t tid1;
pthread_t tid2;
pthread_t tid3;
pthread_attr_t attr;
pthread_mutex_t texLoadingMutex = PTHREAD_MUTEX_INITIALIZER;

// 线程是否已创建
int pthread_created_BG = 0;
int pthread_created_COV = 0;
int pthread_created_ICO = 0;

// 尝试添加线程wait信号量
static ee_sema_t wakeupIdSema;

typedef struct
{
    int qr;
    s32 wakeupId;
    image_cache_t *cache;
    item_list_t *list;
    int cacheId;
    int cacheUID;
    char *value;
    int itemId;
    // quiet==1 表示该请求来自 Coverflow 专用取图路径(cacheGetTextureQuiet)。
    // Coverflow 每帧为多张封面排队，且不使用 cacheGetTexture 那套“单封面防抖”的
    // cdFramesCount 冷却状态；而 cdFramesCount 只在常规 cacheGetTexture 内被推进/清零。
    // 在 Coverflow 主界面上封面走的是 quiet 路径，一旦 cdFramesCount 被其它路径(如进出
    // 详情页)置成非 0 就再没人把它清零，导致 cacheLoadImage1 永久跳过所有封面加载、
    // 封面被反复排队又跳过(texLoading 卡住不归零)。因此 quiet 请求必须无视 cdFramesCount。
    int quiet;
    // 只有 Coverflow COV 请求参与目标代际判断；ICO 和普通列表保持原有生命周期。
    int trackGeneration;
    // 在渲染线程入队时决定是否压缩低分辨率 BG，worker 不直接读取可能
    // 正在切换的 gsGlobal 指针。
    int compactBackground;
} load_image_request_t;
load_image_request_t req1 = {0};
load_image_request_t req2 = {0};
load_image_request_t req3 = {0};

static void cacheClearItem(cache_entry_t *item, int freeTxt)
{
    if (!item)
        return;

    if (freeTxt) {
        if (item->texture.Mem) {
            rmUnloadTexture(&item->texture);
            free(item->texture.Mem);
            item->texture.Mem = NULL; // Must be allocated by loader
        }
        if (item->texture.Clut) {
            free(item->texture.Clut);
            item->texture.Clut = NULL; // Default, can be set by loader
        }
    }

    memset(item, 0, sizeof(cache_entry_t));
    item->texture.Width = 0;            // Must be set by loader
    item->texture.Height = 0;           // Must be set by loader
    item->texture.PSM = GS_PSM_CT24;    // Must be set by loader
    item->texture.ClutPSM = 0;          // Default, can be set by loader
    item->texture.TBW = 0;              // gsKit internal value
    item->texture.Vram = 0;             // VRAM allocation handled by texture manager
    item->texture.VramClut = 0;         // VRAM allocation handled by texture manager
    item->texture.Filter = GS_FILTER_LINEAR; // Default
    // item->texture.ClutStorageMode = GS_CLUT_STORAGE_CSM1; // Default
    //  Do not load the texture to VRAM directly, only load it to EE RAM
    item->texture.Delayed = 1;

    item->qr = 0;
    item->lastUsed = 0;
    item->UID = -1;
    item->texFound = -1;
}

static void cacheDecreaseLoading(void)
{
    pthread_mutex_lock(&texLoadingMutex);
    if (texLoading > 0)
        texLoading--;
    pthread_mutex_unlock(&texLoadingMutex);
}

static int cacheShouldCompactBackground(image_cache_t *cache)
{
    return cache && !strncmp(cache->suffix, "BG", 2) && gsGlobal &&
           gsGlobal->DoubleBuffering == GS_SETTING_ON && gsGlobal->PSM == GS_PSM_CT24;
}

static void cacheCancelImageRequest(void *data)
{
    load_image_request_t *ioReq = (load_image_request_t *)data;
    if (!ioReq)
        return;

    if (ioReq->cache && ioReq->cache->content && ioReq->cacheId >= 0 && ioReq->cacheId < ioReq->cache->count) {
        cache_entry_t *entry = &ioReq->cache->content[ioReq->cacheId];

        // UID一致才允许释放槽位，避免旧请求清掉后来复用该槽位的新请求。
        if (entry->UID == ioReq->cacheUID) {
            entry->qr = 0;
            entry->lastUsed = 0;
            entry->requestGeneration = 0;
            entry->texFound = -1;
        }
    }

    cacheDecreaseLoading();
    free(ioReq);
}

void cacheCancelPendingArtRequests(void)
{
    pthread_mutex_lock(&texLoadingMutex);
    artRequestGeneration++;
    if (artRequestGeneration == 0)
        artRequestGeneration = 1;
    int wasLoading = texLoading > 0;
    pthread_mutex_unlock(&texLoadingMutex);

    if (usePthread)
        return;

    // 光标移动瞬间仍有图片未显示时，保留原有的30帧连按保护。
    if (wasLoading && !ForceRefreshPrevTexCache && !padGetRepeating())
        cdFramesCount = 1;

    ioRemoveRequestsWithCleanup(IO_CACHE_LOAD_ART, cacheCancelImageRequest);
}

static void cacheQueueImageRequest(image_cache_t *cache, int cacheId, item_list_t *list, char *value, int itemId, int quiet)
{
    load_image_request_t *req = calloc(1, sizeof(load_image_request_t));
    if (!req) {
        cache->content[cacheId].qr = 0;
        return;
    }

    req->cache = cache;
    req->cacheId = cacheId;
    req->cacheUID = cache->content[cacheId].UID;
    req->list = list;
    req->value = value;
    req->itemId = itemId;
    req->qr = 1;
    req->quiet = quiet;
    req->trackGeneration = quiet && !strncmp(cache->suffix, "COV", 3);
    req->compactBackground = cacheShouldCompactBackground(cache);

    pthread_mutex_lock(&texLoadingMutex);
    cache->content[cacheId].requestGeneration = req->trackGeneration ? artRequestGeneration : 0;
    if (texLoading >= 0)
        texLoading++;
    else
        texLoading = 1;
    pthread_mutex_unlock(&texLoadingMutex);

    // 入队失败时必须同步回滚，否则启动流程会一直等待不存在的请求。
    if (ioPutRequest(IO_CACHE_LOAD_ART, req) != IO_OK)
        cacheCancelImageRequest(req);
}

// 加载其他图片时用的线程函数
static void cacheLoadImage1(void *data)
{
    load_image_request_t *ioReq = (load_image_request_t *)data;
    // Safeguards...
    if (!ioReq->cache || !ioReq->cache->content) {
        cacheDecreaseLoading();
        free(ioReq);
        return;
    }

    item_list_t *handler = ioReq->list;
    if (!handler) {
        cacheDecreaseLoading();
        ioReq->cache->content[ioReq->cacheId].qr = 0;
        ioReq->cache->content[ioReq->cacheId].requestGeneration = 0;
        free(ioReq);
        return;
    }

    // 普通 cacheGetTexture 请求仍保留原有的 worker 侧冷却保护：请求从 IO 队列
    // 取出后，光标可能已经变化，menusys 来不及再把它移除；此时丢弃旧 ART，避免
    // 普通列表在快速切换时为已离开的游戏做无谓的解码。Coverflow 的 quiet 请求
    // 不依赖这套单封面冷却，所以显式绕过 cdFramesCount。两条路径都必须响应
    // forceSkipQr（cacheEnd 的退出保护）。
    if ((cdFramesCount && !ioReq->quiet) || forceSkipQr) {
        cacheDecreaseLoading();
        ioReq->cache->content[ioReq->cacheId].qr = 0;
        ioReq->cache->content[ioReq->cacheId].requestGeneration = 0;
        free(ioReq);
        return;
    }

    cache_entry_t *entry = &ioReq->cache->content[ioReq->cacheId];

    // 加载图片。itemGetImage() 可能已经无法中途取消；结果先留在原槽位，
    // 但在发布 texFound=1 之前必须重新确认该请求仍属于最新目标。
    int result = handler->itemGetImage(handler, ioReq->cache->prefix, ioReq->cache->isPrefixRelative, ioReq->value, ioReq->cache->suffix, &entry->texture, GS_PSM_CT24, ioReq->itemId);

    if (result >= 0 && ioReq->compactBackground)
        texCompactBackground(&entry->texture);

    // 光标变化会递增 artRequestGeneration；Coverflow 当前可见/预取路径再次
    // 查询仍需要的 active 槽位时，会把 requestGeneration 更新回当前代。
    // 没有被重新查询的旧请求在此处丢弃，不进入 cache，也不会在后续触发 GS bind。
    int keepResult = 0;
    int sameEntry = 0;
    pthread_mutex_lock(&texLoadingMutex);
    sameEntry = (entry->UID == ioReq->cacheUID);
    if (sameEntry && entry->qr &&
        (!ioReq->trackGeneration || entry->requestGeneration == artRequestGeneration)) {
        // 普通列表和 ICO 请求保持原有发布规则；只有 Coverflow COV 请求需要
        // 通过代际确认它仍属于新的可见/预取范围。
        keepResult = 1;
        if (result < 0) {
            entry->lastUsed = 0;
            entry->texFound = 0;
            //*ioReq->cacheId = -2;
        } else {
            entry->lastUsed = guiFrameId;
            entry->texFound = 1;
        }
        entry->requestGeneration = 0;
        entry->qr = 0;
    } else if (sameEntry && entry->qr) {
        entry->qr = 2; // 丢弃期间禁止主线程复用此槽位
    }
    pthread_mutex_unlock(&texLoadingMutex);

    if (!keepResult) {
        if (sameEntry && ioReq->trackGeneration)
            cacheClearItem(entry, 1);
        cacheDecreaseLoading();
        ioReq->qr = 0;
        free(ioReq);
        return;
    }

    cacheDecreaseLoading();
    ioReq->qr = 0;
    free(ioReq);
    return;
}
// Io handled action...
static void *cacheLoadImage(void *data)
{
    load_image_request_t *ioReq = (load_image_request_t *)data;
    while (1) {
        if (forceSkipQr)
            return NULL;

        WaitSema(ioReq->wakeupId);
        if (forceSkipQr)
            return NULL;

        // Safeguards...
        if (!ioReq->cache || !ioReq->cache->content) {
            pthread_mutex_lock(&texLoadingMutex);
            if (texLoading > 0)
                texLoading--;
            pthread_mutex_unlock(&texLoadingMutex);
            // 重置状态
            ioReq->qr = 0;
            continue;
        }

        item_list_t *handler = ioReq->list;
        if (!handler) {
            ioReq->cache->content[ioReq->cacheId].qr = 0;
            pthread_mutex_lock(&texLoadingMutex);
            if (texLoading > 0)
                texLoading--;
            pthread_mutex_unlock(&texLoadingMutex);
            // 重置状态
            ioReq->qr = 0;
            continue;
        }

        // 光标指向的游戏ID和后台加载的art图片不符时，或者已经处于CD(按住和快速点击)时，停止加载图片，避免卡顿
        if (cdFramesCount || forceSkipQr) {
            ioReq->cache->content[ioReq->cacheId].qr = 0;
            pthread_mutex_lock(&texLoadingMutex);
            if (texLoading > 0)
                texLoading--;
            pthread_mutex_unlock(&texLoadingMutex);
            // 重置状态
            ioReq->qr = 0;
            continue;
        }

        // 加载图片
        int result = handler->itemGetImage(handler, ioReq->cache->prefix, ioReq->cache->isPrefixRelative, ioReq->value, ioReq->cache->suffix, &ioReq->cache->content[ioReq->cacheId].texture, GS_PSM_CT24, ioReq->itemId);

        if (result >= 0 && ioReq->compactBackground)
            texCompactBackground(&ioReq->cache->content[ioReq->cacheId].texture);

        if (result < 0) {
            ioReq->cache->content[ioReq->cacheId].lastUsed = 0;
            ioReq->cache->content[ioReq->cacheId].texFound = 0;
            //*ioReq->cacheId = -2;
        } else {
            ioReq->cache->content[ioReq->cacheId].lastUsed = guiFrameId;
            ioReq->cache->content[ioReq->cacheId].texFound = 1;
        }
        pthread_mutex_lock(&texLoadingMutex);
        if (texLoading > 0)
            texLoading--;
        pthread_mutex_unlock(&texLoadingMutex);
        ioReq->cache->content[ioReq->cacheId].qr = 0;
        // 重置状态
        ioReq->qr = 0;
    }
    return NULL;
}

void flushBatchRequests(void)
{
    // 左右切页签强制刷新缓存的变量，需要判断当前游戏所有图片是否都处理完毕
    if (ForceRefreshPrevTexCache > 1)
        ForceRefreshPrevTexCache = 0;

    // 线程异常时，将线程取消后重新创建(补救措施,大概率没用作用)
    //if (texLoading && !getKeyPressed(KEY_UP) && !getKeyPressed(KEY_DOWN) && !getKeyPressed(KEY_L1) && !getKeyPressed(KEY_R1)) {
    //    if (++texLoadingTimeOut >= 600) { // 没有按住按键，且加载超过10秒时，重置texLoading
    //        if (usePthread) {
    //            if (pthread_created_BG)
    //                pthread_cancel(tid1);
    //            if (pthread_created_COV)
    //                pthread_cancel(tid2);
    //            if (pthread_created_ICO)
    //                pthread_cancel(tid3);

    //            // 线程分离，如果不需要pthread_join
    //             pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    //            if (pthread_created_BG)
    //                pthread_create(&tid1, &attr, cacheLoadImage, &req1);
    //            if (pthread_created_COV)
    //                pthread_create(&tid2, &attr, cacheLoadImage, &req2);
    //            if (pthread_created_ICO)
    //                pthread_create(&tid3, &attr, cacheLoadImage, &req3);

    //            texLoading = 0;
    //        } else {
    //            ;
    //        }
    //    }
    //} else
    //    texLoadingTimeOut = 0;

    //// 有堆积的图片待加载
    //if (batchRequestCount > 0 && !texLoading) {
    //    //// debug  打印debug信息
    //    //char debugFileDir[64];
    //    //strcpy(debugFileDir, "smb:debug-TexCacheAllArtIoOnce.txt");
    //    //FILE *debugFile = fopen(debugFileDir, "ab+");
    //    //if (debugFile != NULL) {
    //    //    fprintf(debugFile, "batchRequestCount:%d   guiFrameId:%d  curStartUp:%s\r\n", batchRequestCount, guiFrameId, curStartUp);
    //    //    fclose(debugFile);
    //    //}

    //    //  使用官方的多线程方法
    //    ioRequestCount = batchRequestCount;
    //    batchRequestCount = 0;
    //    texLoading = 1;

    //    ioPutRequest(IO_CUSTOM_SIMPLEACTION, &cacheLoadImage);

    //    //// 使用ptheard来推送
    //    ////pthread_mutex_lock(&mutex);
    //    //ioRequestCount = batchRequestCount;
    //    //batchRequestCount = 0;
    //    //texLoading = 1;
    //    //pthread_cond_signal(&cond);
    //    ////pthread_mutex_unlock(&mutex);
    //}
}

void cacheInit()
{
    ioRegisterHandler(IO_CACHE_LOAD_ART, &cacheLoadImage1);
    if (usePthread) {
        wakeupIdSema.init_count = 0;
        wakeupIdSema.max_count = 1;
        wakeupIdSema.option = 0;

        req1.wakeupId = CreateSema(&wakeupIdSema);
        req2.wakeupId = CreateSema(&wakeupIdSema);
        req3.wakeupId = CreateSema(&wakeupIdSema);

        // 初始化pthread线程属性
        pthread_attr_init(&attr);

        //// 线程分离，如果不需要pthread_join
        // pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

        // 设置合适的栈空间，防止爆栈等错误
        pthread_attr_setstacksize(&attr, 1024 * 1024); // kb
    }

    //// 使用pthread的多线程方法
    //pthread_t tid;
    //pthread_attr_t attr;
    //pthread_attr_init(&attr);

    ////// 线程分离，如果不需要pthread_join
    ////pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    //// 设置合适的栈空间，防止爆栈等错误
    //pthread_attr_setstacksize(&attr, 1 * 1024 * 1024); // 1mb

    //// 创建线程
    //pthread_create(&tid, &attr, cacheLoadImage, NULL);
    //pthread_attr_destroy(&attr);
}

void cacheEnd()
{
    forceSkipQr = 1;
    if (usePthread) {
        // 等待所有线程wait
        int waitTime = 0;
        while (1) {
            pthread_mutex_lock(&texLoadingMutex);
            int loading = texLoading;
            pthread_mutex_unlock(&texLoadingMutex);
            if (loading <= 0)
                break;
            waitTime++;
            usleep(1000);
            if (waitTime >= 4000) // 设置4秒超时时间，不让程序卡死
                break;
        }
        // 设置退出标志，并全部唤醒
        if (pthread_created_BG)
            SignalSema(req1.wakeupId);
        if (pthread_created_COV)
            SignalSema(req2.wakeupId);
        if (pthread_created_ICO)
            SignalSema(req3.wakeupId);

        if (waitTime < 4000) {
            // 销毁pthread所有资源
            if (pthread_created_BG)
                pthread_join(tid1, NULL);
            if (pthread_created_COV)
                pthread_join(tid2, NULL);
            if (pthread_created_ICO)
                pthread_join(tid3, NULL);
            pthread_attr_destroy(&attr);
            pthread_mutex_destroy(&texLoadingMutex);
        }
        DeleteSema(req1.wakeupId);
        DeleteSema(req2.wakeupId);
        DeleteSema(req3.wakeupId);
    }
    // nothing to do... others have to destroy the cache via cacheDestroyCache
}

image_cache_t *cacheInitCache(int userId, const char *prefix, int isPrefixRelative, const char *suffix, int count)
{
    image_cache_t *cache = (image_cache_t *)malloc(sizeof(image_cache_t));
    cache->userId = userId;
    cache->count = count;
    cache->prefix = NULL;
    int length;
    if (prefix) {
        length = strlen(prefix) + 1;
        cache->prefix = (char *)malloc(length * sizeof(char));
        memcpy(cache->prefix, prefix, length);
    }
    cache->isPrefixRelative = isPrefixRelative;
    length = strlen(suffix) + 1;
    cache->suffix = (char *)malloc(length * sizeof(char));
    memcpy(cache->suffix, suffix, length);
    cache->nextUID = 1;
    cache->content = (cache_entry_t *)malloc(count * sizeof(cache_entry_t));

    int i;
    for (i = 0; i < count; ++i)
        cacheClearItem(&cache->content[i], 0);

    return cache;
}

void cacheDestroyCache(image_cache_t *cache)
{
    int i;
    for (i = 0; i < cache->count; ++i) {
        cacheClearItem(&cache->content[i], 1);
    }

    free(cache->prefix);
    free(cache->suffix);
    free(cache->content);
    free(cache);
}

GSTEXTURE *cacheGetTexture(image_cache_t *cache, item_list_t *list, int *cacheId, int *UID, char *value, int itemId)
{
    // 默认情况下，触发重复按键时，就会跳过所有Qr
    if (padGetRepeating()) {
        findBGCount = 0;
        cdFramesCount = 0; // 强制结束连按CD
    } else
        buttonPressedOnce = 0;
    skipQr = gScrollSpeed > 0 ? padGetRepeating() : 0;

    // 启动id变化时，说明光标有移动（可能用UID判断，效率更高更合理，之后再改。UID一开始是-1，然后再分配一个正整数）
    if (curStartUp != value) {
        // 移动光标时，如果有IO请求，就会跳过Qr，后台也会停止继续加载队列中的图片
        if (curStartUp && !ForceRefreshPrevTexCache && texLoading > 0) {
            if (!padGetRepeating())
                cdFramesCount = 1; // 触发连按CD
            else
                skipQr = 1; // 按住时，还有图片请求，就跳过本次Qr
        }
        curStartUp = value;
    }

    if (cdFramesCount) {
        //if (cdFramesCount == 1) {
        //    buttonPressedOnce = 1;
        //    cdFrames = 50; // 第一次触发时的CD会长一点，需要考虑loadtex的卡顿时间
        //    //// debug  打印debug信息
        //    //char debugFileDir[64];
        //    //strcpy(debugFileDir, "smb:debug-TexCacheIoPut.txt");
        //    //FILE *debugFile = fopen(debugFileDir, "ab+");
        //    //if (debugFile != NULL) {
        //    //    fprintf(debugFile, "artQrCount:%d   UID:%d   cacheID:%d\r\ncurStartUp:%s_%s\r\n\r\n", artQrCount ,* UID, *cacheId, curStartUp, cache->suffix);
        //    //    fclose(debugFile);
        //    //}
        //}

        // 连按CD期间，再次按键，重置帧数
        if (getKeyPressed(KEY_UP) || getKeyPressed(KEY_DOWN) || getKeyPressed(KEY_L1) || getKeyPressed(KEY_R1)) {
            cdFramesCount = 1;
            //// 按下按键只重置一次的变量
            //if (!buttonPressedOnce) {
            //    buttonPressedOnce = 1;
            //    cdFrames = 50;
            //}
        } else
            buttonPressedOnce = 0;

        // CD期间跳过Qr，防止卡顿，CD结束后恢复原状
        if (cdFramesCount++ <= cdFrames)
            skipQr = 1;
        else
            cdFramesCount = 0;

        // 下次第一个加载背景图，如果没有就重试N次后退出
        if (!cdFramesCount) {
            if (cache->suffix[0] != 'B') {
                cdFramesCount = 10000;
                if (++findBGCount >= MENU_MIN_INACTIVE_FRAMES) {
                    findBGCount = 0;
                    cdFramesCount = 0;
                } else
                    skipQr = 1;
            } else
                findBGCount = 0;
        }

        // CD期间进入了自动连按状态，矫正一次Qr，结束cdFramesCount
        if (gScrollSpeed > 0 && padGetRepeating()) {
            findBGCount = 0;
            cdFramesCount = 0;
            skipQr = 1;
        }
    }

    if (forceSkipQr)
        skipQr = 1;

    // 切换设备页签时，上次图缓存需要清掉
    if (ForceRefreshPrevTexCache) {
        ForceRefreshPrevTexCache++;

        // 重置上次的缓存ID
        PrevCacheID_COV = PrevCacheID_ICO = PrevCacheID_BG = PrevCacheID = -2;
    } else {
        // 根据图像类型，赋值上一次的缓存
        if (!strncmp("BG", cache->suffix, 2))
            PrevCacheID = PrevCacheID_BG;
        else if (!strncmp("COV", cache->suffix, 3))
            PrevCacheID = PrevCacheID_COV;
        else if (!strncmp("ICO", cache->suffix, 3))
            PrevCacheID = PrevCacheID_ICO;
        else
            PrevCacheID = -2;
    }

    // -2代表无图像，-1代表正在查找图像，0-9代表缓存编号
    if (*cacheId == -2) {
        // 根据图像类型，将缓存分类保存，替代NULL时的默认图(防止闪烁)
        if (!strncmp("BG", cache->suffix, 2))
            PrevCacheID_BG = *cacheId;
        else if (!strncmp("COV", cache->suffix, 3))
            PrevCacheID_COV = *cacheId;
        else if (!strncmp("ICO", cache->suffix, 3))
            PrevCacheID_ICO = *cacheId;
        return NULL;
    } else if (*cacheId != -1) {
        cache_entry_t *entry = &cache->content[*cacheId];
        if (entry->UID == *UID) {
            if (entry->qr) {
                return PrevCacheID < 0 ? NULL : &cache->content[PrevCacheID].texture;
            } else if (entry->texFound == 0) {
                *cacheId = -2;
                // 根据图像类型，将缓存分类保存，替代NULL时的默认图(防止闪烁)
                if (!strncmp("COV", cache->suffix, 3))
                    PrevCacheID_COV = *cacheId;
                else if (!strncmp("ICO", cache->suffix, 3))
                    PrevCacheID_ICO = *cacheId;
                else if (!strncmp("BG", cache->suffix, 2))
                    PrevCacheID_BG = *cacheId;
                return NULL;
            } else if (entry->texFound == 1) {
                if (entry->texture.Mem) {
                    // entry->lastUsed = guiFrameId;
                    //  根据图像类型，将缓存分类保存，替代NULL时的默认图(防止闪烁)
                    if (!strncmp("BG", cache->suffix, 2))
                        PrevCacheID_BG = *cacheId;
                    else if (!strncmp("COV", cache->suffix, 3))
                        PrevCacheID_COV = *cacheId;
                    else if (!strncmp("ICO", cache->suffix, 3))
                        PrevCacheID_ICO = *cacheId;
                    return &entry->texture;
                }
            }
        }
        *cacheId = -1;
    }

    if (skipQr)
        return PrevCacheID < 0 ? NULL : &cache->content[PrevCacheID].texture;

    cache_entry_t *currEntry, *oldestEntry = NULL;
    int i;
    int cacheId_temp = -1;
    u64 rtime = guiFrameId;

    // 寻找可替换的槽
    for (i = 0; i < cache->count; i++) {
        currEntry = &cache->content[i];
        // 可用槽。多槽 cache 保留上一帧的 fallback；单槽 BG cache 必须允许
        // 直接替换上一张背景，否则 PrevCacheID 会永久占住唯一槽位。
        if (!currEntry->qr && (currEntry->lastUsed < rtime) &&
            (cache->count == 1 || PrevCacheID != i)) {
            oldestEntry = currEntry;
            rtime = currEntry->lastUsed;
            cacheId_temp = i;
        }
    }

    if (oldestEntry) {
        if (PrevCacheID == cacheId_temp && cache->count == 1) {
            // cacheClearItem() 会立即释放旧背景；不要把已清空的唯一槽位
            // 当作 fallback 返回给本帧的渲染路径。
            PrevCacheID = -1;
            if (!strncmp("BG", cache->suffix, 2))
                PrevCacheID_BG = -1;
            else if (!strncmp("COV", cache->suffix, 3))
                PrevCacheID_COV = -1;
            else if (!strncmp("ICO", cache->suffix, 3))
                PrevCacheID_ICO = -1;
        }

        if (!usePthread) {
            *cacheId = cacheId_temp; // 指针赋值放在for循环外面，只赋值一次，防止竞态
            // 使用官方的多线程方法
            cacheClearItem(oldestEntry, 1);
            oldestEntry->qr = 1;
            // UID没有分配时，才重新分配UID，也许可以解决一些BUG？
            if (*UID == -1)
                oldestEntry->UID = *UID = cache->nextUID++;
            else
                oldestEntry->UID = *UID;

            cacheQueueImageRequest(cache, *cacheId, list, value, itemId, 0); // quiet=0：常规单封面路径
        } else {
            //  加载图片
            if (!strncmp("BG", cache->suffix, 2)) {
                if (!req1.qr) {
                    *cacheId = cacheId_temp; // 指针赋值放在for循环外面，只赋值一次，防止竞态
                    cacheClearItem(oldestEntry, 1);
                    oldestEntry->qr = 1;
                    // UID没有分配时，才重新分配UID，也许可以解决一些BUG？
                    if (*UID == -1)
                        oldestEntry->UID = *UID = cache->nextUID++;
                    else
                        oldestEntry->UID = *UID;

                    //  使用pthread的多线程方法
                    pthread_mutex_lock(&texLoadingMutex);
                    if (texLoading >= 0)
                        texLoading++;
                    else
                        texLoading = 1;
                    pthread_mutex_unlock(&texLoadingMutex);
                    req1.cache = cache;
                    req1.cacheId = *cacheId;
                    req1.list = list;
                    req1.value = value;
                    req1.itemId = itemId;
                    req1.qr = 1;
                    req1.compactBackground = cacheShouldCompactBackground(cache);
                    if (!pthread_created_BG) {
                        pthread_created_BG = 1;
                        pthread_create(&tid1, &attr, cacheLoadImage, &req1);
                    }
                    SignalSema(req1.wakeupId);
                }
            } else if (!strncmp("COV", cache->suffix, 3)) {
                if (!req2.qr) {
                    *cacheId = cacheId_temp; // 指针赋值放在for循环外面，只赋值一次，防止竞态
                    cacheClearItem(oldestEntry, 1);
                    oldestEntry->qr = 1;
                    // UID没有分配时，才重新分配UID，也许可以解决一些BUG？
                    if (*UID == -1)
                        oldestEntry->UID = *UID = cache->nextUID++;
                    else
                        oldestEntry->UID = *UID;

                    //  使用pthread的多线程方法
                    pthread_mutex_lock(&texLoadingMutex);
                    if (texLoading >= 0)
                        texLoading++;
                    else
                        texLoading = 1;
                    pthread_mutex_unlock(&texLoadingMutex);
                    req2.cache = cache;
                    req2.cacheId = *cacheId;
                    req2.list = list;
                    req2.value = value;
                    req2.itemId = itemId;
                    req2.qr = 1;
                    if (!pthread_created_COV) {
                        pthread_created_COV = 1;
                        pthread_create(&tid2, &attr, cacheLoadImage, &req2);
                    }
                    SignalSema(req2.wakeupId);
                }
            } else if (!strncmp("ICO", cache->suffix, 3)) {
                if (!req3.qr) {
                    *cacheId = cacheId_temp; // 指针赋值放在for循环外面，只赋值一次，防止竞态
                    cacheClearItem(oldestEntry, 1);
                    oldestEntry->qr = 1;
                    // UID没有分配时，才重新分配UID，也许可以解决一些BUG？
                    if (*UID == -1)
                        oldestEntry->UID = *UID = cache->nextUID++;
                    else
                        oldestEntry->UID = *UID;

                    //  使用pthread的多线程方法
                    pthread_mutex_lock(&texLoadingMutex);
                    if (texLoading >= 0)
                        texLoading++;
                    else
                        texLoading = 1;
                    pthread_mutex_unlock(&texLoadingMutex);
                    req3.cache = cache;
                    req3.cacheId = *cacheId;
                    req3.list = list;
                    req3.value = value;
                    req3.itemId = itemId;
                    req3.qr = 1;
                    if (!pthread_created_ICO) {
                        pthread_created_ICO = 1;
                        pthread_create(&tid3, &attr, cacheLoadImage, &req3);
                    }
                    SignalSema(req3.wakeupId);
                }
            } else {
                *cacheId = cacheId_temp; // 指针赋值放在for循环外面，只赋值一次，防止竞态
                cacheClearItem(oldestEntry, 1);
                oldestEntry->qr = 1;
                // UID没有分配时，才重新分配UID，也许可以解决一些BUG？
                if (*UID == -1)
                    oldestEntry->UID = *UID = cache->nextUID++;
                else
                    oldestEntry->UID = *UID;

                // 【第二十八问修复：texLoading 计数泄漏导致封面永久停载】
                // 非 BG/COV/ICO 的 art（典型是 INFO 详情页的 SCR/SCR2 截图）即使在
                // usePthread 模式下也走官方 IO 队列(IO_CACHE_LOAD_ART)。
                // 旧代码在此手写了一份加载逻辑：先 texLoading++，再 calloc(req)，
                // 然后 ioPutRequest(...) 且【忽略返回值】。IO 请求池是定长的
                // (ioman.c: MAX_IO_REQUESTS=16)，在 INFO 页快速上下浏览、同时 coverflow
                // 预取也在抢占请求槽时，请求池会被占满，ioPutRequest 返回
                // IO_ERR_IO_BLOCKED —— 请求既没入队、其处理函数(cacheLoadImage1)也永不执行，
                // 于是这一次的 texLoading++ 再也没有对应的 --，造成 texLoading 只增不减地
                // 永久泄漏（同时泄漏 calloc 出的 req、并把该缓存槽 qr 永久钉在 1）。
                // 一旦 texLoading 卡在 >0，cacheGetTexture 顶部的
                // `curStartUp != value && texLoading > 0` 门控会在每次光标移动时触发
                // cdFramesCount 冷却 / skipQr，从此不再派发任何新封面加载 —— 现象就是
                // “按圈返回列表后封面全停、按刷新也不恢复、只有切主题重建缓存才恢复”。
                // 修法：统一改用已有的 cacheQueueImageRequest()（与 !usePthread 分支完全一致）。
                // 它对 calloc 失败与 ioPutRequest 失败都会同步回滚 texLoading 与槽位状态
                // (cacheCancelImageRequest)，从根本上杜绝该泄漏。
                cacheQueueImageRequest(cache, *cacheId, list, value, itemId, 0); // quiet=0：常规路径(死代码分支)
            }
        }

        // prevGuiFrameId = guiFrameId;
        // artQrCount++;

        //// debug  打印debug信息
        //char debugFileDir[64];
        //strcpy(debugFileDir, "smb:debug-oldestEntry.txt");
        //FILE *debugFile = fopen(debugFileDir, "ab+");
        //if (debugFile != NULL) {
        //    fprintf(debugFile, "进了oldestEntry\r\n");
        //    fclose(debugFile);
        //}
    }
    return PrevCacheID < 0 ? NULL : &cache->content[PrevCacheID].texture;
}

// 只查询现有缓存/回退纹理，不触发新的加载请求。
// Coverflow 的 BG 在渲染背景阶段只走这里，真正的请求由 drawCoverFlow 在 ICO 请求之后提交。
GSTEXTURE *cacheGetTextureNoRequest(image_cache_t *cache, item_list_t *list, int *cacheId, int *UID, char *value, int itemId)
{
    (void)list;
    (void)value;
    (void)itemId;

    if (!cache || !cache->content || !cacheId || !UID || *cacheId == -2)
        return NULL;

    int previousId = PrevCacheID_BG;
    if (ForceRefreshPrevTexCache || previousId < 0 || previousId >= cache->count)
        previousId = -1;

    if (*cacheId >= 0 && *cacheId < cache->count) {
        cache_entry_t *entry = &cache->content[*cacheId];
        if (entry->UID == *UID) {
            if (entry->qr)
                return previousId < 0 ? NULL : &cache->content[previousId].texture;

            if (entry->texFound == 0) {
                *cacheId = -2;
                return NULL;
            }

            if (entry->texFound == 1 && entry->texture.Mem) {
                PrevCacheID_BG = *cacheId;
                return &entry->texture;
            }
        }
        *cacheId = -1;
    }

    return previousId < 0 ? NULL : &cache->content[previousId].texture;
}

// Coverflow 专用取图函数。
// 常规的 cacheGetTexture() 依赖 curStartUp / skipQr / cdFramesCount / PrevCacheID_*
// 等一整套"每帧只取选中项这一张封面"的全局状态；Coverflow 每帧需要为多张封面取图，
// 会把这些启发式打乱，导致封面永远排不进加载、只显示占位图。
// 本函数刻意不触碰上述任何全局状态，改用与通用分支相同的 cacheQueueImageRequest()
// 多请求加载路径，因此可在同一帧安全地为多张封面并行取图/排队加载。
// 命中返回纹理；未命中则按 queueRequest 决定是否排队后台加载并返回 NULL。
static GSTEXTURE *cacheGetTextureQuietInternal(image_cache_t *cache, item_list_t *list, int *cacheId, int *UID, char *value, int itemId, int queueRequest)
{
    if (!cache || !cache->content || !value)
        return NULL;

    // 已确认该项没有对应 art 文件：直接返回，避免反复排队
    if (*cacheId == -2)
        return NULL;

    // 已分配槽位：检查是否命中。动画期间即使不允许新请求，已经在队列中的
    // 请求仍然通过 qr 路径正常等待，已经加载的纹理也继续续期。
    if (*cacheId >= 0 && *cacheId < cache->count) {
        cache_entry_t *entry = &cache->content[*cacheId];
        if (entry->UID == *UID) {
            if (entry->qr) {
                // 当前帧仍需要这张图：只有 COV active 请求需要重新标记为最新目标。
                // 这样翻页后仍在新可见/预取范围内的请求可以继续完成；
                // 没有再次被查询的旧 COV 请求则会在 worker 返回时被丢弃。
                if (!strncmp(cache->suffix, "COV", 3)) {
                    pthread_mutex_lock(&texLoadingMutex);
                    entry->requestGeneration = artRequestGeneration;
                    pthread_mutex_unlock(&texLoadingMutex);
                }
                return NULL; // 正在后台加载
            }
            if (entry->texFound == 1 && entry->texture.Mem) {
                entry->lastUsed = guiFrameId; // 命中：续期，防止本帧被其它封面复用
                return &entry->texture;
            }
            if (entry->texFound == 0) {
                *cacheId = -2; // 确认无此 art，标记缺失，后续不再排队
                return NULL;
            }
            // texFound == -1：上次加载被 CD/skipQr 中断，只有允许请求时才能重试
        }
        *cacheId = -1; // UID 不匹配（槽被别的封面抢走）→ 重新查找
    }

    // Coverflow 翻页/单步动画期间只允许查询已有缓存，不分配新槽位，也不入队。
    if (!queueRequest)
        return NULL;

    // 需要加载：挑一个空闲/最旧、且未在加载中的槽
    cache_entry_t *oldest = NULL;
    int slot = -1;
    int protectedId = -1;
    u64 rtime = guiFrameId;
    int i;
    if (!strncmp(cache->suffix, "BG", 2))
        protectedId = PrevCacheID_BG;

    for (i = 0; i < cache->count; i++) {
        cache_entry_t *e = &cache->content[i];
        if (!e->qr && i != protectedId && e->lastUsed < rtime) {
            oldest = e;
            rtime = e->lastUsed;
            slot = i;
        }
    }
    if (oldest) {
        *cacheId = slot;
        cacheClearItem(oldest, 1); // 注意：会把 qr 清 0、texFound 置 -1
        oldest->qr = 1;
        if (*UID == -1)
            oldest->UID = *UID = cache->nextUID++;
        else
            oldest->UID = *UID;
        oldest->lastUsed = guiFrameId; // 本帧占位，避免同帧其它封面复用同一槽
        cacheQueueImageRequest(cache, *cacheId, list, value, itemId, 1); // quiet=1：Coverflow 路径
    }
    return NULL;
}

GSTEXTURE *cacheGetTextureQuiet(image_cache_t *cache, item_list_t *list, int *cacheId, int *UID, char *value, int itemId)
{
    return cacheGetTextureQuietInternal(cache, list, cacheId, UID, value, itemId, 1);
}

GSTEXTURE *cacheGetTextureQuietNoRequest(image_cache_t *cache, item_list_t *list, int *cacheId, int *UID, char *value, int itemId)
{
    return cacheGetTextureQuietInternal(cache, list, cacheId, UID, value, itemId, 0);
}
