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

int artQrCount = 0; // 给加入Qr缓存队列的Art图计数
int artQrDone = 0; // 代表一轮Art图已全部进入Qr队列
int prevGuiFrameId = 0; // 和guiFrameId进行比对，判断是否完成了一轮Qr
int cdFrames = 0; // 一轮Art图Qr后的CD时间(帧数)
int buttonFrames = 0; // 按住按键的帧数，用来跳过cdFrames
int skipQr = 0; // 判断是否可以跳过请求Qr队列

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
    if ((ForceRefreshPrevTexCache > 1) && (prevGuiFrameId != guiFrameId))
        ForceRefreshPrevTexCache = 0;

    // 已经完成一轮Qr
    if (artQrCount && (prevGuiFrameId != guiFrameId))
        artQrDone = 1;

    if (artQrDone) {
        // Qr之后会CD一段时间，才能再次Qr
        if (guiFrameId - prevGuiFrameId - 1 <= cdFrames) {
            if (gScrollSpeed > 0) {
                // CD的时候再次按键，会重新计算CD
                if (!guiInactiveFrames) {
                    prevGuiFrameId = guiFrameId;
                    buttonFrames++;
                    skipQr = 1;
                } else {
                    // 按住按键超过CD时间，再次松开，直接结束CD
                    if (buttonFrames > cdFrames) {
                        buttonFrames = 0;
                        artQrCount = 0;
                        artQrDone = 0;
                        skipQr = 0;
                    } else {
                        buttonFrames = 0;
                        skipQr = 1;
                    }
                }
            } else { // 慢速光标的Qr处理方式
                // CD的时候再次按键，会重新计算CD
                if (!guiInactiveFrames) {
                    prevGuiFrameId = guiFrameId;
                    if (++buttonFrames > cdFrames)
                        skipQr = 0;
                    else
                        skipQr = 1;
                } else {
                    // 按住按键超过CD时间，再次松开，直接结束CD
                    if (buttonFrames > cdFrames) {
                        buttonFrames = 0;
                        artQrCount = 0;
                        artQrDone = 0;
                        skipQr = 0;
                    } else {
                        buttonFrames = 0;
                        skipQr = 1;
                    }
                }
            }
        } else {
            // CD结束后，重置变量
            buttonFrames = 0;
            artQrCount = 0;
            artQrDone = 0;
            skipQr = 0;
        }
    }

    GSTEXTURE *prevCache = NULL;
    // 切换设备页签时，上次图缓存需要清掉
    if (ForceRefreshPrevTexCache) {
        if (ForceRefreshPrevTexCache == 1)
            prevGuiFrameId = guiFrameId;
        // 根据图像类型，赋值上一次的缓存
        if (!strncmp("COV", cache->suffix, 3)) {
            if (PrevCacheID_COV >= 0)
                PrevCacheID_COV = -1;
        } else if (!strncmp("ICO", cache->suffix, 3)) {
            if (PrevCacheID_ICO >= 0)
                PrevCacheID_ICO = -1;
        } else if (!strncmp("BG", cache->suffix, 2)) {
            if (PrevCacheID_BG >= 0)
                PrevCacheID_BG = -1;
        }
        ForceRefreshPrevTexCache++;
    } else {
        // 根据图像类型，赋值上一次的缓存
        if (!strncmp("COV", cache->suffix, 3)) {
            if (PrevCacheID_COV >= 0)
                prevCache = &cache->content[PrevCacheID_COV].texture;
        } else if (!strncmp("ICO", cache->suffix, 3)) {
            if (PrevCacheID_ICO >= 0)
                prevCache = &cache->content[PrevCacheID_ICO].texture;
        } else if (!strncmp("BG", cache->suffix, 2)) {
            if (PrevCacheID_BG >= 0)
                prevCache = &cache->content[PrevCacheID_BG].texture; // 缓存队列满了后，会返回NULL
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

    if (skipQr)
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

        prevGuiFrameId = guiFrameId;
        artQrCount++;

        ioPutRequest(IO_CACHE_LOAD_ART, req);
        //// debug  打印debug信息
        //char debugFileDir[64];
        //strcpy(debugFileDir, "smb:debug-TexCache.txt");
        //FILE *debugFile = fopen(debugFileDir, "ab+");
        //if (debugFile != NULL) {
        //    fprintf(debugFile, "guiFrameId:%d  ArtCount:%d\r\n", guiFrameId, artQrCount);
        //    fclose(debugFile);
        //}
    }
    return prevCache;
}
