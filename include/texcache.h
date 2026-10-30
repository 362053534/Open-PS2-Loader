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
} cache_entry_t;


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

// ===== BG 计时埋点声明 开始 =====
// 实机诊断开关。为 0 时下面的埋点宏展开为空，L3 不导出。
#define BG_PROF 1

// GetTimerSystemTime 的单位是总线时钟。147456 tick = 1 ms。
#define BG_PROF_HZ 147456000
#define BG_PROF_CAP 4096

#ifndef BG_PROF_NOW
#define BG_PROF_NOW() GetTimerSystemTime()
#endif
#ifndef BG_PROF_LOCK
#define BG_PROF_LOCK() DIntr()
#endif
#ifndef BG_PROF_UNLOCK
#define BG_PROF_UNLOCK() EIntr()
#endif
#ifndef BG_PROF_PATH
#define BG_PROF_PATH "mass0:/bg_timing.txt"
#endif

enum {
    BG_EV_KEY = 0,
    BG_EV_VISIT,
    BG_EV_RELOAD,
    BG_EV_QUEUE,
    BG_EV_IO_START,
    BG_EV_IO_DROP,
    BG_EV_LOOSE_DONE,
    BG_EV_PACK_LOCK,
    BG_EV_PICK,
    BG_EV_SEEK_DONE,
    BG_EV_READ_DONE,
    BG_EV_DECODE_DONE,
    BG_EV_COMPACT_DONE,
    BG_EV_PUBLISH,
    BG_EV_DRAW,
    BG_EV_COUNT
};

void bgProfMark(int ev, const char *suffix, const char *id, int arg);
void bgProfDump(void);

#if BG_PROF
#define BG_PROF_MARK(ev, suf, id, arg) bgProfMark((ev), (suf), (id), (arg))
#define BG_PROF_DUMP() bgProfDump()
#else
#define BG_PROF_MARK(ev, suf, id, arg) ((void)0)
#define BG_PROF_DUMP() ((void)0)
#endif
// ===== BG 计时埋点声明 结束 =====

#endif
