#ifndef __TEX_CACHE_H
#define __TEX_CACHE_H

#include "include/iosupport.h"

/// A single cache entry...
typedef struct
{
    GSTEXTURE texture;

    // NULL not queued, otherwise queue request record
    int qr;

    // frame counter the icon was used the last time - oldest get rewritten first in case new icon is requested and cache is full. negative numbers mean
    // slot is free and can be used right now
    u64 lastUsed;

    int UID;

    // 该槽位的 active Coverflow 请求属于哪一代目标；光标变化时递增，
    // 过期的 active COV 完成后不再发布到 cache/GS。
    u32 requestGeneration;

    int texFound; // 图片解析完毕，防止解析到一半就进行使用，也许会造成死机？

    // 随机 BG：这张图所属游戏在图包里的 BG 张数，加载完成时由选图顺手带出；
    // CACHE_BG_COUNT_LOOSE 表示取的是散图（重载也是同一张），-1 表示不知道。
    int artCount;
} cache_entry_t;

// BG 来自散文件：散图优先于图包，重载拿到的还是同一张，命中缓存时直接用
#define CACHE_BG_COUNT_LOOSE (-2)


/// One texture cache instance
typedef struct
{
    /// User specified ID, not used in any way by the cache code (not even initialized!)
    int userId;

    /// count of entries (copy of the requested cache size upon cache initialization)
    int count;

    /// directory prefix for this cache (if any)
    char *prefix;
    int isPrefixRelative;
    char *suffix;

    int nextUID;

    /// the cache entries itself
    cache_entry_t *content;
} image_cache_t;

/** Initializes the cache subsystem.
 */
void cacheInit();

/** Terminates the cache. Does nothing currently. Users of this code have to destroy caches via cacheDestroyCache
 */
void cacheEnd();

/** Initializes a single cache
 */
image_cache_t *cacheInitCache(int userId, const char *prefix, int isPrefixRelative, const char *suffix, int count);

/** Destroys a given cache (unallocates all memory stored there, disconnects the pixmaps from the usage points).
 */
void cacheDestroyCache(image_cache_t *cache);

GSTEXTURE *cacheGetTexture(image_cache_t *cache, item_list_t *list, int *cacheId, int *UID, char *value, int itemId);

// Coverflow 专用：不触碰"单封面"全局状态，可在同一帧为多张封面并行取图/排队加载。
GSTEXTURE *cacheGetTextureQuiet(image_cache_t *cache, item_list_t *list, int *cacheId, int *UID, char *value, int itemId);
// Coverflow 翻页动画期间：只查询命中缓存或已有请求，不分配新槽位、不创建新加载请求。
GSTEXTURE *cacheGetTextureQuietNoRequest(image_cache_t *cache, item_list_t *list, int *cacheId, int *UID, char *value, int itemId);

extern int ForceRefreshPrevTexCache;
extern int forceSkipQr;
extern int texLoading;
void flushBatchRequests(void);
void cacheCancelPendingArtRequests(void);

#endif
