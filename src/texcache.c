#include "include/opl.h"
#include "include/texcache.h"
#include "include/textures.h"
#include "include/ioman.h"
#include "include/gui.h"
#include "include/util.h"
#include "include/renderman.h"

int ForceRefreshPrevTexCache = 0;
int PrevCacheID_COV = -2;
int PrevCacheID_ICO = -2;
int PrevCacheID_BG = -2;

//int LoadFrames_COV = 0;
//int LoadFrames_ICO = 0;
//int LoadFrames_BG = 0;
//int LoadFrames = 0; // 加载超过一定时间，则一直跳过加载
//int RestartLoadTexFrames = 0;
//int RestartLoadTexDelay = 15;

int TexStopLoadDelay = 28; // 按住按键超过这个帧数才停止加载ART
int ButtonFrames = 0; // 与TexLoadDalay配合使用，快速移动光标时不会连续加载ART图

typedef struct
{
    image_cache_t *cache;
    cache_entry_t *entry;
    item_list_t *list;
    // only for comparison if the deferred action is still valid
    int cacheUID;
    char *value;
} load_image_request_t;

// Io handled action...
static void cacheLoadImage(void *data)
{
    load_image_request_t *req = data;

    // Safeguards...
    if (!req || !req->entry || !req->cache)
        return;

    item_list_t *handler = req->list;
    if (!handler)
        return;

    // the cache entry was already reused!
    if (req->cacheUID != req->entry->UID)
        return;

    // seems okay. we can proceed
    GSTEXTURE *texture = &req->entry->texture;
    texFree(texture);

    if (handler->itemGetImage(handler, req->cache->prefix, req->cache->isPrefixRelative, req->value, req->cache->suffix, texture, GS_PSM_CT24) < 0)
        req->entry->lastUsed = 0;
    else
        req->entry->lastUsed = guiFrameId;

    req->entry->qr = NULL;

    free(req);
}

void cacheInit()
{
    ioRegisterHandler(IO_CACHE_LOAD_ART, &cacheLoadImage);
}

void cacheEnd()
{
    // nothing to do... others have to destroy the cache via cacheDestroyCache
}

static void cacheClearItem(cache_entry_t *item, int freeTxt)
{
    if (freeTxt && item->texture.Mem) {
        rmUnloadTexture(&item->texture);
        free(item->texture.Mem);
        if (item->texture.Clut)
            free(item->texture.Clut);
    }

    memset(item, 0, sizeof(cache_entry_t));
    item->texture.Mem = NULL;
    item->texture.Vram = 0;
    item->texture.Clut = NULL;
    item->texture.VramClut = 0;
    //item->texture.ClutStorageMode = GS_CLUT_STORAGE_CSM1; // Default
    item->qr = NULL;
    item->lastUsed = -1;
    item->UID = 0;
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

GSTEXTURE *cacheGetTexture(image_cache_t *cache, item_list_t *list, int *cacheId, int *UID, char *value)
{
    // under the cache pre-delay (to avoid filling cache while moving around)
    if (!guiInactiveFrames) {
        ButtonFrames++; // 按住按键的时间
        //if (RestartLoadTexFrames)
        //    RestartLoadTexFrames = 0;
    } else {
        if (ButtonFrames)
            ButtonFrames = 0;
        //// 连续按按键触发跳过加载后，停下一段时间，才会重新开始加载ART图
        //if (RestartLoadTexFrames++ >= RestartLoadTexDelay) {
        //    RestartLoadTexFrames = RestartLoadTexDelay;
        //    LoadFrames_COV = 0;
        //    LoadFrames_ICO = 0;
        //    LoadFrames_BG = 0;
        //    LoadFrames = 0;
        //}
    }

    //// 根据图像类型，记录加载时间
    //if (!strncmp("COV", cache->suffix, 3)) {
    //    if (*cacheId == -1)
    //        LoadFrames_COV++;
    //    else {
    //        if (LoadFrames_COV)
    //            LoadFrames_COV = 0;
    //    }
    //    LoadFrames = LoadFrames_COV;
    //} else if (!strncmp("ICO", cache->suffix, 3)) {
    //    if (*cacheId == -1)
    //        LoadFrames_ICO++;
    //    else {
    //        if (LoadFrames_ICO)
    //            LoadFrames_ICO = 0;
    //    }
    //    LoadFrames = LoadFrames_ICO;
    //} else if (!strncmp("BG", cache->suffix, 2)) {
    //    //// debug  打印debug信息
    //    // char debugFileDir[64];
    //    // strcpy(debugFileDir, "smb:debug-TexCache.txt");
    //    // FILE *debugFile = fopen(debugFileDir, "ab+");
    //    // if (debugFile != NULL) {
    //    //     fprintf(debugFile, "BG cacheId:%d  LoadFrames_BG:%d\r\n", *cacheId, LoadFrames_BG);
    //    //     fclose(debugFile);
    //    // }
    //    if (*cacheId == -1)
    //        LoadFrames_BG++;
    //    else {
    //        if (LoadFrames_BG)
    //            LoadFrames_BG = 0;
    //    }
    //    LoadFrames = LoadFrames_BG;
    //} else {
    //    if (LoadFrames)
    //        LoadFrames = 0;
    //}

    GSTEXTURE *prevCache = NULL;
    // 切换设备页签时，上次图缓存需要清掉
    if (ForceRefreshPrevTexCache) {
        ForceRefreshPrevTexCache = 0;
        PrevCacheID_COV = -2;
        PrevCacheID_ICO = -2;
        PrevCacheID_BG = -2;
    } else {
        // 根据图像类型，赋值上一次的缓存
        if (!strncmp("COV", cache->suffix, 3)) {
            if (PrevCacheID_COV >= 0)
                prevCache = &(&cache->content[PrevCacheID_COV])->texture;
        } else if (!strncmp("ICO", cache->suffix, 3)) {
            if (PrevCacheID_ICO >= 0)
                prevCache = &(&cache->content[PrevCacheID_ICO])->texture;
        } else if (!strncmp("BG", cache->suffix, 2)) {
            if (PrevCacheID_BG >= 0)
                prevCache = &(&cache->content[PrevCacheID_BG])->texture; // 缓存队列满了后，会返回NULL
        }
    }

    // -2代表无图像，-1代表正在查找图像，0-9代表缓存编号
    if (*cacheId == -2) {
        // 根据图像类型，将缓存分类保存，替代NULL时的默认图(防止闪烁)
        if (!strncmp("COV", cache->suffix, 3)) {
            PrevCacheID_COV = *cacheId;
        } else if (!strncmp("ICO", cache->suffix, 3)) {
            PrevCacheID_ICO = *cacheId;
        } else if (!strncmp("BG", cache->suffix, 2)) {
            PrevCacheID_BG = *cacheId;
        }
        return NULL;
    } else if (*cacheId != -1) {
        cache_entry_t *entry = &cache->content[*cacheId];
        if (entry->UID == *UID) {
            if (entry->qr) {
                return prevCache;
            } else if (entry->lastUsed == 0) {
                *cacheId = -2;
                // 根据图像类型，将缓存分类保存，替代NULL时的默认图(防止闪烁)
                if (!strncmp("COV", cache->suffix, 3)) {
                    PrevCacheID_COV = *cacheId;
                } else if (!strncmp("ICO", cache->suffix, 3)) {
                    PrevCacheID_ICO = *cacheId;
                } else if (!strncmp("BG", cache->suffix, 2)) {
                    PrevCacheID_BG = *cacheId;
                }
                return NULL;
            } else {
                entry->lastUsed = guiFrameId;
                // 根据图像类型，将缓存分类保存，替代NULL时的默认图(防止闪烁)
                if (!strncmp("COV", cache->suffix, 3)) {
                    PrevCacheID_COV = *cacheId;
                } else if (!strncmp("ICO", cache->suffix, 3)) {
                    PrevCacheID_ICO = *cacheId;
                } else if (!strncmp("BG", cache->suffix, 2)) {
                    PrevCacheID_BG = *cacheId;
                }
                return &entry->texture;
            }
        }

        *cacheId = -1;
    }

    // under the cache pre-delay (to avoid filling cache while moving around)
    if ((ButtonFrames >= TexStopLoadDelay)/* || (LoadFrames >= TexStopLoadDelay)*/) // 按住按键超时，或加载超时，则停止加载ART
        return prevCache;

    cache_entry_t *currEntry, *oldestEntry = NULL;
    int i, rtime = guiFrameId;

    // 寻找可替换的槽
    for (i = 0; i < cache->count; i++) {
        currEntry = &cache->content[i];
        // 可用槽，但需保护正在使用的
        if ((!currEntry->qr) && (currEntry->lastUsed < rtime) &&
            !(prevCache && (&currEntry->texture == prevCache))) {
            oldestEntry = currEntry;
            rtime = currEntry->lastUsed;
            *cacheId = i;
        }
    }

    if (oldestEntry) {
        load_image_request_t *req = malloc(sizeof(load_image_request_t) + strlen(value) + 1);
        req->cache = cache;
        req->entry = oldestEntry;
        req->list = list;
        req->value = (char *)req + sizeof(load_image_request_t);
        strcpy(req->value, value);
        req->cacheUID = cache->nextUID;

        cacheClearItem(oldestEntry, 1);
        oldestEntry->qr = req;
        oldestEntry->UID = cache->nextUID;

        *UID = cache->nextUID++;

        ioPutRequest(IO_CACHE_LOAD_ART, req);
    }
    return prevCache;
}
