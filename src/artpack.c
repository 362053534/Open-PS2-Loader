#include "include/opl.h"
#include "include/iosupport.h"
#include "include/supportbase.h"
#include "include/textures.h"
#include "include/ioman.h"
#include "include/artpack.h"
#include "include/texcache.h"
#include <ps2sdkapi.h> // lseek64
#include <png.h>
#include <pthread.h>
#include <setjmp.h>
#include <strings.h> // strcasecmp
#include <time.h> // clock（随机取图的种子）
#include <sys/stat.h> // stat / S_ISDIR（图包开关关闭时判断 ART2）

/*
 * 图包加载器（第一阶段）：ART2 目录中任意命名的 .zip（扩展名不区分大小写，排除 "." 开头的隐藏文件），取 readdir 遇到的第一个；
 *
 * 线程模型：
 *  - artPackDeviceInit/End 在设备初始化/清理的线程中执行；
 *  - artPackLoadImage 在 IO 工作线程中执行；
 *  - 所有设备状态由 artPackMutex 保护。建索引耗时较长，在锁外完成，最后在锁内发布；
 *  - 只有全局最快设备的 art.zip 保持打开，其 art.idx 条目表也常驻内存；其它设备不占用 fd，
 *    取图时按需读取 art.idx；
 *  - 最快设备的条目表只在启动时载入一次：所有设备探测完、选默认页前由 artPackPreloadBestEntries
 *    在 IO 线程载入；载入失败或之后最快设备变化时不再整表载入，取图回退为按需读 art.idx；
 *  - 最快设备的 fd 为懒加载：在其第一次取图时才打开；
 *    artPackDeviceEnd 只关闭 fd、释放内存并重新选择最快设备，不做其它文件 IO；
 *  - 每个设备的 artPackDeviceEnd 都会在卸载前关闭该 fd，避免设备卸载（如 PFS 的
 *    PDIOC_CLOSEALL）后留下失效 fd。
 *
 * 64 位定位统一使用 ps2sdk libcglue 的 lseek64（supportbase.c 读 ISO 时也是如此）。
 */

/* ---------------- ZIP 格式常量 ---------------- */

#define ZIP_SIG_LOCAL        0x04034b50
#define ZIP_SIG_CENTRAL      0x02014b50
#define ZIP_SIG_EOCD         0x06054b50
#define ZIP_SIG_ZIP64_LOC    0x07064b50
#define ZIP_SIG_ZIP64_EOCD   0x06064b50

#define ZIP_LOCAL_HDR_SIZE   30
#define ZIP_CENTRAL_HDR_SIZE 46
#define ZIP_EOCD_SIZE        22
#define ZIP_ZIP64_LOC_SIZE   20
#define ZIP_ZIP64_EOCD_SIZE  56
#define ZIP_MAX_COMMENT_LEN  0xFFFF
#define ZIP_EXTRA_ZIP64_ID   0x0001
#define ZIP_EXTRA_HDR_SIZE   4
#define ZIP_METHOD_STORED    0
#define ZIP_U16_MAX          0xFFFF
#define ZIP_U32_MAX          0xFFFFFFFFUL

// 本地头字段偏移
#define ZIP_LOCAL_OFF_METHOD    8
#define ZIP_LOCAL_OFF_COMP      18
#define ZIP_LOCAL_OFF_UNCOMP    22
#define ZIP_LOCAL_OFF_NAME_LEN  26
#define ZIP_LOCAL_OFF_EXTRA_LEN 28

// 中央目录字段偏移
#define ZIP_CEN_OFF_METHOD    10
#define ZIP_CEN_OFF_COMP      20
#define ZIP_CEN_OFF_UNCOMP    24
#define ZIP_CEN_OFF_NAME_LEN  28
#define ZIP_CEN_OFF_EXTRA_LEN 30
#define ZIP_CEN_OFF_CMT_LEN   32
#define ZIP_CEN_OFF_LOCAL     42

// EOCD / ZIP64 字段偏移
#define ZIP_EOCD_OFF_TOTAL    10
#define ZIP_EOCD_OFF_CD_SIZE  12
#define ZIP_EOCD_OFF_CD_OFF   16
#define ZIP_Z64LOC_OFF_EOCD   8
#define ZIP_Z64EOCD_OFF_TOTAL 32
#define ZIP_Z64EOCD_OFF_CD_SIZE 40
#define ZIP_Z64EOCD_OFF_CD_OFF  48

/* ---------------- 运行参数 ---------------- */

#define ART_PACK_ZIP_EXT       ".zip" // 图包扩展名（不区分大小写）
#define ART_PACK_DIR_NAME      "ART2" // 图包所在目录（与 ART2 散文件同一目录）
#define ART_PACK_IDX_EXT       ".idx" // 索引扩展名，索引名 = 图包名去掉 .zip 再加 .idx
#define ART_PACK_EMBED_IDX_NAME "art.idx" // 嵌入 zip 的索引条目名：必须是 zip 的第一个条目，不压缩存放
#define ART_PACK_HIDDEN_PREFIX "."    // 隐藏文件前缀，须排除（含 macOS 拷贝时生成的 "._" AppleDouble 假 zip）
#define ART_PACK_PATH_MAX      256
#define ART_PACK_CHUNK_SIZE    (64 * 1024)   // 中央目录分块读取大小
#define ART_PACK_MAX_ENTRIES   (256 * 1024)  // 防御异常文件导致的超大内存申请
#define ART_PACK_MAX_DATA_SIZE ART_ENTRY_SIZE_MAX // 单张 PNG 上限，即条目大小字段（22 位）能表示的最大值
#define ART_PACK_LOCAL_SLACK   256           // 一次读取时为本地头文件名+extra 预留的余量
#define ART_PACK_MAX_TEX_DIM   1024          // 与旧解码器的尺寸限制一致
#define ART_PACK_MAX_TEX_BYTES (720 * 512 * 4) // 与 textures.c 的 maxSize 一致
#define ART_PACK_PNG_EXT       ".png"

/* ---------------- art.idx 文件格式 ---------------- */

#define ART_IDX_MAGIC     "AIDX"
#define ART_IDX_MAGIC_LEN 4
#define ART_IDX_VERSION   2
#define ART_GAME_ID_LEN   12 // 含结尾 0，游戏 ID 最长 11 字符

typedef struct
{
    char magic[ART_IDX_MAGIC_LEN];
    u32 version;
    u64 archiveSize;
    u32 ps1Count; // 游戏表前 ps1Count 条是 PS1，后面 ps2Count 条是 PS2，各自按 gameId 排序
    u32 ps2Count;
    u32 entryCount;
    u32 reserved;
} __attribute__((packed)) art_idx_header_t;

// 游戏表共 ps1Count + ps2Count + 1 条，紧跟 header；最后一条是哨兵（只用 firstEntry = entryCount），
// 每个游戏的条目数 = 下一条的 firstEntry - 本条的 firstEntry
typedef struct
{
    char gameId[ART_GAME_ID_LEN]; // 已转大写
    u32 firstEntry;
} __attribute__((packed)) art_idx_game_t;

/*
 * 条目表：每条一个 64 位整数，紧跟游戏表（保持 8 字节对齐）。从低位到高位依次是
 *   序号 5 位 | 种类 3 位 | 数据大小 22 位 | 本地头偏移 34 位
 * 偏移 34 位可到 16GB，大小 22 位可到 4MB-1，序号只支持 0..31。
 * 条目只在查找某个游戏时才用下面的宏拆开，不在载入时逐条转换。
 */
typedef u64 art_idx_entry_t;

#define ART_ENTRY_INDEX_MAX  31
#define ART_ENTRY_SIZE_MAX   ((1u << 22) - 1)
#define ART_ENTRY_OFF_LIMIT  ((u64)1 << 34) // 偏移必须小于该值
#define ART_ENTRY_INDEX(e)   ((u8)((e) & 0x1F))
#define ART_ENTRY_TYPE(e)    ((u8)(((e) >> 5) & 0x7))
#define ART_ENTRY_SIZE(e)    ((u32)(((e) >> 8) & ART_ENTRY_SIZE_MAX))
#define ART_ENTRY_OFF(e)     ((u64)((e) >> 30))
#define ART_ENTRY_MAKE(off, size, type, index) \
    (((u64)(off) << 30) | ((u64)(size) << 8) | ((u64)(type) << 5) | (u64)(index))

// 条目表在 art.idx 中的偏移：header + (gameCount + 哨兵) 条游戏记录
#define ART_IDX_ENTRY_TABLE_OFF(gameCount) \
    ((u32)(sizeof(art_idx_header_t) + ((u64)(gameCount) + 1) * sizeof(art_idx_game_t)))

// 编译期检查结构尺寸，保证文件格式固定
#define ART_STATIC_ASSERT(name, cond) typedef char name[(cond) ? 1 : -1]
ART_STATIC_ASSERT(art_idx_header_size_check, sizeof(art_idx_header_t) == 32);
ART_STATIC_ASSERT(art_idx_game_size_check, sizeof(art_idx_game_t) == 16);
ART_STATIC_ASSERT(art_idx_entry_size_check, sizeof(art_idx_entry_t) == 8);

// 建索引时的临时记录
typedef struct
{
    char gameId[ART_GAME_ID_LEN];
    u8 bucket;
    u8 suffixType;
    u8 suffixIndex;
    u8 reserved;
    u32 dataSize;
    u64 localHdrOff;
} art_build_rec_t;

ART_STATIC_ASSERT(art_build_rec_fits, sizeof(art_build_rec_t) >= sizeof(art_idx_entry_t));

enum ART_PACK_BUCKET {
    ART_BUCKET_NONE = 0,
    ART_BUCKET_PS1,
    ART_BUCKET_PS2,
};

enum ART_PACK_SUFFIX {
    ART_SUF_NONE = 0,
    ART_SUF_COV,
    ART_SUF_COV2,
    ART_SUF_ICO,
    ART_SUF_LGO,
    ART_SUF_LAB,
    ART_SUF_BG,
    ART_SUF_SCR,
};

typedef struct
{
    const char *name;
    u8 type;
    u8 indexed; // 1 表示形如 NAME_xx 的带两位序号后缀
} art_suffix_def_t;

static const art_suffix_def_t artSuffixDefs[] = {
    {"COV", ART_SUF_COV, 0},
    {"COV2", ART_SUF_COV2, 0},
    {"ICO", ART_SUF_ICO, 0},
    {"LGO", ART_SUF_LGO, 0},
    {"LAB", ART_SUF_LAB, 0},
    {"BG", ART_SUF_BG, 1},
    {"SCR", ART_SUF_SCR, 1},
};

#define ART_SUFFIX_DEF_COUNT (sizeof(artSuffixDefs) / sizeof(artSuffixDefs[0]))
#define ART_SUFFIX_INDEX_DIGITS 2
// 请求后缀的序号取值：真实序号只有 0..99；下面两个特殊值表示“该类图里随机取一张”
#define ART_REQ_RANDOM      0xFF // BG / SCR
#define ART_REQ_RANDOM_SCR2 0xFE // SCR2：随机，且排除 SCR 选中的那张（SCR 也排除 SCR2 选中的）

typedef struct
{
    int active;
    char zipPath[ART_PACK_PATH_MAX];
    char idxPath[ART_PACK_PATH_MAX];
    u32 gameCount;               // PS1 + PS2 游戏数，不含哨兵
    u32 ps1Count;                // 游戏表前 ps1Count 条是 PS1
    art_idx_game_t *games;       // 常驻内存，二分查找；共 gameCount + 1 条，末尾是哨兵
    u32 entryCount;
    u32 entryTableOff;           // art.idx 中条目表偏移
    art_idx_entry_t *memEntries; // art.idx 写入失败时常驻；或作为最快设备时从 art.idx 载入
    int memEntriesFromIdx;       // ART_PACK_ENTRIES_* ：memEntries 的来源
    int rank;                    // 设备速度等级（ART_PACK_RANK_*，越小越快）
} art_pack_dev_t;

// memEntries 来源：写索引失败时的常驻副本，必须保留
#define ART_PACK_ENTRIES_RESIDENT 0
// memEntries 来源：因成为最快设备而从 art.idx 载入，不再是最快设备时可释放
#define ART_PACK_ENTRIES_FROM_IDX 1

static art_pack_dev_t artPackDevs[MODE_COUNT];
static pthread_mutex_t artPackMutex = PTHREAD_MUTEX_INITIALIZER;

// 没有任何设备启用图包时的 artPackBestMode 取值
#define ART_PACK_NO_BEST -1
// 全局选出的最快图包设备（artPackDevs 下标），由 artPackMutex 保护
static int artPackBestMode = ART_PACK_NO_BEST;

// 无效文件描述符
#define ART_PACK_FD_NONE -1
// 最快设备常驻打开的 art.zip fd 及其所属设备，由 artPackMutex 保护
static int artPackBestFd = ART_PACK_FD_NONE;
static int artPackBestFdMode = ART_PACK_NO_BEST;

static int artReadAt(int fd, u64 offset, void *buf, size_t size);

// 关闭最快设备的 art.zip fd；调用方须持有 artPackMutex
static void artPackCloseBestFd(void)
{
    if (artPackBestFd != ART_PACK_FD_NONE)
        close(artPackBestFd);
    artPackBestFd = ART_PACK_FD_NONE;
    artPackBestFdMode = ART_PACK_NO_BEST;
}

// 返回最快设备的 art.zip fd，必要时打开；失败返回 ART_PACK_FD_NONE，下次取图再重试。调用方须持有 artPackMutex
static int artPackEnsureBestFd(void)
{
    if (artPackBestFd != ART_PACK_FD_NONE && artPackBestFdMode == artPackBestMode)
        return artPackBestFd;
    artPackCloseBestFd();
    if (artPackBestMode == ART_PACK_NO_BEST || !artPackDevs[artPackBestMode].active)
        return ART_PACK_FD_NONE;
    artPackBestFd = open(artPackDevs[artPackBestMode].zipPath, O_RDONLY);
    if (artPackBestFd < 0) {
        LOG("ARTPACK: cannot open %s\n", artPackDevs[artPackBestMode].zipPath);
        artPackBestFd = ART_PACK_FD_NONE;
        return ART_PACK_FD_NONE;
    }
    artPackBestFdMode = artPackBestMode;
    return artPackBestFd;
}

// 释放某设备因曾是最快设备而从 art.idx 载入的条目表；常驻副本保留。调用方须持有 artPackMutex
static void artPackDropIdxEntries(art_pack_dev_t *dev)
{
    if (dev->memEntriesFromIdx != ART_PACK_ENTRIES_FROM_IDX)
        return;
    free(dev->memEntries);
    dev->memEntries = NULL;
    dev->memEntriesFromIdx = ART_PACK_ENTRIES_RESIDENT;
}

// 把最快设备的整个条目表一次读入内存；失败时保持 NULL，取图回退为按需读 art.idx。调用方须持有 artPackMutex
static void artPackLoadBestEntries(void)
{
    art_pack_dev_t *dev;
    art_idx_entry_t *list;
    size_t bytes;
    int fd;

    if (artPackBestMode == ART_PACK_NO_BEST)
        return;
    dev = &artPackDevs[artPackBestMode];
    if (!dev->active || dev->memEntries || dev->entryCount == 0)
        return;
    bytes = (size_t)dev->entryCount * sizeof(art_idx_entry_t);
    list = malloc(bytes);
    if (!list)
        return;
    fd = open(dev->idxPath, O_RDONLY);
    if (fd < 0 || artReadAt(fd, (u64)dev->entryTableOff, list, bytes) < 0) {
        if (fd >= 0)
            close(fd);
        free(list);
        LOG("ARTPACK: cannot load entries from %s\n", dev->idxPath);
        return;
    }
    close(fd);
    dev->memEntries = list;
    dev->memEntriesFromIdx = ART_PACK_ENTRIES_FROM_IDX;
}

// 启动时所有设备探测完后调用一次（IO 线程），把最快设备的条目表整表载入内存
void artPackPreloadBestEntries(void)
{
    pthread_mutex_lock(&artPackMutex);
    artPackLoadBestEntries();
    pthread_mutex_unlock(&artPackMutex);
}

// 在已激活的设备中选出 rank 最小者（相同时取 mode 较小者）；调用方须持有 artPackMutex。
// 只做选择与清理（释放旧设备条目表、关闭不属于新设备的 fd），不做任何文件 IO；
// 新设备的 fd 在 artPackLoadImage 中懒加载；条目表不再补载，取图按需读 art.idx
static void artPackSelectBest(void)
{
    int i;
    int oldBest = artPackBestMode;

    artPackBestMode = ART_PACK_NO_BEST;
    for (i = 0; i < MODE_COUNT; i++) {
        if (!artPackDevs[i].active)
            continue;
        // 严格小于：等级相同时保留先遇到的较小 mode
        if (artPackBestMode == ART_PACK_NO_BEST || artPackDevs[i].rank < artPackDevs[artPackBestMode].rank)
            artPackBestMode = i;
    }

    // 最快设备变化：释放旧设备从 art.idx 载入的条目表（已被 artFreeDev 清零时为空操作）
    if (oldBest != artPackBestMode && oldBest != ART_PACK_NO_BEST)
        artPackDropIdxEntries(&artPackDevs[oldBest]);
    if (artPackBestFdMode != artPackBestMode)
        artPackCloseBestFd();
}

/* ---------------- 小工具 ---------------- */

static u16 artRd16(const u8 *p)
{
    return (u16)(p[0] | (p[1] << 8));
}

static u32 artRd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u64 artRd64(const u8 *p)
{
    return (u64)artRd32(p) | ((u64)artRd32(p + 4) << 32);
}

static int artValidMode(int mode)
{
    return mode >= 0 && mode < MODE_COUNT;
}

// 读满 size 字节，失败返回 -1
static int artReadAt(int fd, u64 offset, void *buf, size_t size)
{
    if (lseek64(fd, (off64_t)offset, SEEK_SET) != (off64_t)offset)
        return -1;
    return read(fd, buf, size) == (ssize_t)size ? 0 : -1;
}

// 游戏 ID 转大写写入 out，长度不合法返回 -1
static int artNormalizeId(const char *src, size_t len, char *out)
{
    size_t i;

    if (len == 0 || len >= ART_GAME_ID_LEN)
        return -1;
    for (i = 0; i < len; i++) {
        char c = src[i];
        if (c == '/' || c == '\\' || c == '\0')
            return -1;
        out[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    memset(&out[len], 0, ART_GAME_ID_LEN - len);
    return 0;
}

// 解析后缀字符串（如 COV、BG_00、SCR_01），不区分大小写
static int artParseSuffix(const char *s, size_t len, u8 *type, u8 *index)
{
    size_t i;

    for (i = 0; i < ART_SUFFIX_DEF_COUNT; i++) {
        const art_suffix_def_t *def = &artSuffixDefs[i];
        size_t baseLen = strlen(def->name);

        if (len < baseLen || strncasecmp(s, def->name, baseLen))
            continue;

        if (!def->indexed) {
            if (len != baseLen)
                continue;
            *type = def->type;
            *index = 0;
            return 0;
        }

        if (len != baseLen + 1 + ART_SUFFIX_INDEX_DIGITS || s[baseLen] != '_')
            continue;
        if (s[baseLen + 1] < '0' || s[baseLen + 1] > '9' || s[baseLen + 2] < '0' || s[baseLen + 2] > '9')
            continue;
        *type = def->type;
        *index = (u8)((s[baseLen + 1] - '0') * 10 + (s[baseLen + 2] - '0'));
        return 0;
    }

    return -1;
}

// 主题请求的后缀映射（与 OPLM 图包条目命名规则一致）
static int artParseRequestSuffix(const char *suffix, u8 *type, u8 *index)
{
    if (!suffix)
        return -1;
    // 随机取图开启时，BG / SCR / SCR2 在该游戏已有的同类图里每次随机取一张，
    // SCR 与 SCR2 互相排除对方选过的那张（见 artPickRandomEntry）；关闭时走下面原有的固定序号映射。
    if (gArtRandom) {
        if (!strcmp(suffix, "BG")) {
            *type = ART_SUF_BG;
            *index = ART_REQ_RANDOM;
            return 0;
        }
        if (!strcmp(suffix, "SCR")) {
            *type = ART_SUF_SCR;
            *index = ART_REQ_RANDOM;
            return 0;
        }
        if (!strcmp(suffix, "SCR2")) {
            *type = ART_SUF_SCR;
            *index = ART_REQ_RANDOM_SCR2;
            return 0;
        }
    }
    if (!strcmp(suffix, "BG"))
        suffix = "BG_00";
    else if (!strcmp(suffix, "SCR"))
        suffix = "SCR_00";
    else if (!strcmp(suffix, "SCR2"))
        suffix = "SCR_01";
    return artParseSuffix(suffix, strlen(suffix), type, index);
}

// SCR / SCR2 的选图记忆：按游戏记住两张截图各自最近一次选中的序位。
// SCR 和 SCR2 是两份独立的缓存，各自命中或被淘汰互不影响，请求先后也不固定；
// 所以排除对象必须取自“对方最近一次选中的序位”，而不是“上一次请求的游戏”。
// 记忆保留最近若干个游戏，覆盖缓存里仍可能命中的游戏（命中的那张截图就是该游戏记忆里的那一张）。
#define ART_SCR_MEMO_COUNT 8
typedef struct
{
    char id[ART_GAME_ID_LEN];
    u8 bucket;
    u8 used;
    int pick[2]; // [0]=SCR，[1]=SCR2：最近一次选中的 SCR 序位，-1 表示还没选过
} art_scr_memo_t;

// 在该游戏的条目里，从 type 类的所有图中随机选出一张，返回它在条目数组中的下标；没有可选的图返回 -1。
// 条目按（种类，序号）排序，序号即使不连续也只在实际存在的图里选。
// SCR 与 SCR2 互相排除对方的记忆，保证同一个游戏的两张截图不会重复，且与谁先请求无关；
// 只有一张 SCR 时 SCR2 没有图可选，返回 -1（不显示）。调用方须持有 artPackMutex。
static int artPickRandomEntry(const art_idx_entry_t *entries, u32 entryNum, u8 type, u8 request, const char *gameId, u8 bucket)
{
    static u32 randState = 1;
    static art_scr_memo_t memos[ART_SCR_MEMO_COUNT];
    static int memoNext;
    art_scr_memo_t *memo = NULL;
    int count = 0, avoid = -1, pick, rank, slot, i;

    for (i = 0; i < (int)entryNum; i++) {
        if (ART_ENTRY_TYPE(entries[i]) == type)
            count++;
    }

    if (type == ART_SUF_SCR) {
        slot = (request == ART_REQ_RANDOM_SCR2) ? 1 : 0;
        for (i = 0; i < ART_SCR_MEMO_COUNT; i++) {
            if (memos[i].used && memos[i].bucket == bucket && !memcmp(memos[i].id, gameId, ART_GAME_ID_LEN)) {
                memo = &memos[i];
                break;
            }
        }
        if (!memo) {
            memo = &memos[memoNext];
            memoNext = (memoNext + 1) % ART_SCR_MEMO_COUNT;
            memcpy(memo->id, gameId, ART_GAME_ID_LEN);
            memo->bucket = bucket;
            memo->used = 1;
            memo->pick[0] = memo->pick[1] = -1;
        }
        avoid = memo->pick[1 - slot];
        // 图包更新后序位可能越界，此时记忆作废
        if (avoid >= count)
            avoid = -1;
    }

    // SCR2 至少要有 2 张 SCR 才显示：只有 1 张时不显示，避免与 SCR 重复
    if (count - (avoid >= 0) <= 0 || (request == ART_REQ_RANDOM_SCR2 && count < 2))
        return -1;

    randState = randState * 1664525u + 1013904223u + (u32)clock();
    pick = (int)((randState >> 8) % (u32)(count - (avoid >= 0)));
    if (avoid >= 0 && pick >= avoid)
        pick++;

    if (memo)
        memo->pick[slot] = pick;

    rank = 0;
    for (i = 0; i < (int)entryNum; i++) {
        if (ART_ENTRY_TYPE(entries[i]) == type && rank++ == pick)
            return i;
    }
    return -1;
}

/*
 * 解析条目名：PS1/<ID>/<ID>_<SUF>.png 或 PS2/<ID>/<ID>_<SUF>.png
 * 不匹配返回 -1（包括根目录下的 *_LIST.csv/txt）。
 */
static int artParseEntryName(const char *name, size_t len, art_build_rec_t *rec)
{
    static const struct
    {
        const char *dir;
        u8 bucket;
    } buckets[] = {{"PS1/", ART_BUCKET_PS1}, {"PS2/", ART_BUCKET_PS2}};
    const size_t extLen = sizeof(ART_PACK_PNG_EXT) - 1;
    char dirId[ART_GAME_ID_LEN];
    char fileId[ART_GAME_ID_LEN];
    const char *p, *slash, *file, *end;
    size_t i, dirLen = 0, idLen;

    rec->bucket = ART_BUCKET_NONE;
    for (i = 0; i < sizeof(buckets) / sizeof(buckets[0]); i++) {
        dirLen = strlen(buckets[i].dir);
        if (len > dirLen && !strncasecmp(name, buckets[i].dir, dirLen)) {
            rec->bucket = buckets[i].bucket;
            break;
        }
    }
    if (rec->bucket == ART_BUCKET_NONE)
        return -1;

    end = name + len;
    if (len < dirLen + extLen || strncasecmp(end - extLen, ART_PACK_PNG_EXT, extLen))
        return -1;
    end -= extLen;

    p = name + dirLen;
    slash = memchr(p, '/', (size_t)(end - p));
    if (!slash || artNormalizeId(p, (size_t)(slash - p), dirId) < 0)
        return -1;

    file = slash + 1;
    idLen = (size_t)(slash - p);
    if ((size_t)(end - file) <= idLen + 1 || file[idLen] != '_')
        return -1;
    if (artNormalizeId(file, idLen, fileId) < 0 || strcmp(dirId, fileId))
        return -1;

    if (artParseSuffix(file + idLen + 1, (size_t)(end - (file + idLen + 1)), &rec->suffixType, &rec->suffixIndex) < 0)
        return -1;

    memcpy(rec->gameId, dirId, ART_GAME_ID_LEN);
    return 0;
}

static int artCompareRec(const void *a, const void *b)
{
    const art_build_rec_t *ra = (const art_build_rec_t *)a;
    const art_build_rec_t *rb = (const art_build_rec_t *)b;
    int r;

    if (ra->bucket != rb->bucket)
        return ra->bucket < rb->bucket ? -1 : 1;
    r = strcmp(ra->gameId, rb->gameId);
    if (r)
        return r;
    if (ra->suffixType != rb->suffixType)
        return ra->suffixType < rb->suffixType ? -1 : 1;
    if (ra->suffixIndex != rb->suffixIndex)
        return ra->suffixIndex < rb->suffixIndex ? -1 : 1;
    return 0;
}

static void artFreeDev(art_pack_dev_t *dev)
{
    free(dev->games);
    free(dev->memEntries);
    memset(dev, 0, sizeof(*dev));
}

/* ---------------- ZIP 中央目录定位 ---------------- */

static int artFindCentralDir(int fd, u64 archiveSize, u64 *cdOff, u64 *cdSize, u64 *total)
{
    size_t tailLen;
    u64 tailStart;
    u8 *tail;
    long pos;
    int found = -1;

    if (archiveSize < ZIP_EOCD_SIZE)
        return -1;

    tailLen = (size_t)((archiveSize < (u64)(ZIP_EOCD_SIZE + ZIP_MAX_COMMENT_LEN)) ? archiveSize : (u64)(ZIP_EOCD_SIZE + ZIP_MAX_COMMENT_LEN));
    tailStart = archiveSize - tailLen;
    tail = malloc(tailLen);
    if (!tail)
        return -1;
    if (artReadAt(fd, tailStart, tail, tailLen) < 0) {
        free(tail);
        return -1;
    }

    // 从尾部向前找 EOCD 签名
    for (pos = (long)(tailLen - ZIP_EOCD_SIZE); pos >= 0; pos--) {
        if (artRd32(&tail[pos]) == ZIP_SIG_EOCD) {
            found = 0;
            break;
        }
    }
    if (found < 0) {
        free(tail);
        return -1;
    }

    *total = artRd16(&tail[pos + ZIP_EOCD_OFF_TOTAL]);
    *cdSize = artRd32(&tail[pos + ZIP_EOCD_OFF_CD_SIZE]);
    *cdOff = artRd32(&tail[pos + ZIP_EOCD_OFF_CD_OFF]);

    // ZIP64：EOCD 前紧挨着 ZIP64 EOCD 定位器
    {
        u64 locPos = tailStart + (u64)pos;
        if (locPos >= ZIP_ZIP64_LOC_SIZE) {
            u8 loc[ZIP_ZIP64_LOC_SIZE];
            u8 rec[ZIP_ZIP64_EOCD_SIZE];

            locPos -= ZIP_ZIP64_LOC_SIZE;
            if (artReadAt(fd, locPos, loc, sizeof(loc)) == 0 && artRd32(loc) == ZIP_SIG_ZIP64_LOC) {
                u64 recPos = artRd64(&loc[ZIP_Z64LOC_OFF_EOCD]);
                if (artReadAt(fd, recPos, rec, sizeof(rec)) < 0 || artRd32(rec) != ZIP_SIG_ZIP64_EOCD) {
                    free(tail);
                    return -1;
                }
                *total = artRd64(&rec[ZIP_Z64EOCD_OFF_TOTAL]);
                *cdSize = artRd64(&rec[ZIP_Z64EOCD_OFF_CD_SIZE]);
                *cdOff = artRd64(&rec[ZIP_Z64EOCD_OFF_CD_OFF]);
            }
        }
    }

    free(tail);
    if (*cdOff + *cdSize > archiveSize)
        return -1;
    return 0;
}

// 解析中央目录项的 ZIP64 extra（0x0001），按规范顺序依次覆盖为 0xFFFFFFFF 的字段
static int artApplyZip64Extra(const u8 *extra, size_t extraLen, u64 *uncomp, u64 *comp, u64 *localOff)
{
    size_t p = 0;

    while (p + ZIP_EXTRA_HDR_SIZE <= extraLen) {
        u16 id = artRd16(&extra[p]);
        u16 size = artRd16(&extra[p + 2]);
        const u8 *d = &extra[p + ZIP_EXTRA_HDR_SIZE];
        size_t q = 0;

        if (p + ZIP_EXTRA_HDR_SIZE + size > extraLen)
            return -1;
        if (id == ZIP_EXTRA_ZIP64_ID) {
            if (*uncomp == ZIP_U32_MAX) {
                if (q + sizeof(u64) > size)
                    return -1;
                *uncomp = artRd64(&d[q]);
                q += sizeof(u64);
            }
            if (*comp == ZIP_U32_MAX) {
                if (q + sizeof(u64) > size)
                    return -1;
                *comp = artRd64(&d[q]);
                q += sizeof(u64);
            }
            if (*localOff == ZIP_U32_MAX) {
                if (q + sizeof(u64) > size)
                    return -1;
                *localOff = artRd64(&d[q]);
                q += sizeof(u64);
            }
            return 0;
        }
        p += ZIP_EXTRA_HDR_SIZE + size;
    }
    return 0;
}

/* ---------------- 建立索引 ---------------- */

// 流式读取中央目录，收集记录；返回记录数，失败返回 -1
static int artCollectRecords(int fd, u64 cdOff, u64 cdSize, u64 total, art_build_rec_t *recs, u32 maxRecs)
{
    u8 *buf = malloc(ART_PACK_CHUNK_SIZE);
    u64 readPos = cdOff, cdEnd = cdOff + cdSize, seen = 0;
    size_t fill = 0, pos = 0;
    u32 count = 0;

    if (!buf)
        return -1;
    if (lseek64(fd, (off64_t)cdOff, SEEK_SET) != (off64_t)cdOff) {
        free(buf);
        return -1;
    }

    while (seen < total) {
        size_t need, nameLen, extraLen, cmtLen;
        const u8 *h;

        // 缓冲区内剩余数据不足一个完整中央目录项时，前移并补读
        if (fill - pos < ZIP_CENTRAL_HDR_SIZE ||
            fill - pos < ZIP_CENTRAL_HDR_SIZE + (size_t)artRd16(&buf[pos + ZIP_CEN_OFF_NAME_LEN]) + artRd16(&buf[pos + ZIP_CEN_OFF_EXTRA_LEN]) + artRd16(&buf[pos + ZIP_CEN_OFF_CMT_LEN])) {
            size_t want;
            ssize_t got;

            memmove(buf, &buf[pos], fill - pos);
            fill -= pos;
            pos = 0;
            want = ART_PACK_CHUNK_SIZE - fill;
            if ((u64)want > cdEnd - readPos)
                want = (size_t)(cdEnd - readPos);
            if (want == 0)
                break;
            got = read(fd, &buf[fill], want);
            if (got <= 0)
                break;
            fill += (size_t)got;
            readPos += (u64)got;
            continue;
        }

        h = &buf[pos];
        if (artRd32(h) != ZIP_SIG_CENTRAL)
            break;
        nameLen = artRd16(&h[ZIP_CEN_OFF_NAME_LEN]);
        extraLen = artRd16(&h[ZIP_CEN_OFF_EXTRA_LEN]);
        cmtLen = artRd16(&h[ZIP_CEN_OFF_CMT_LEN]);
        need = ZIP_CENTRAL_HDR_SIZE + nameLen + extraLen + cmtLen;

        if (artRd16(&h[ZIP_CEN_OFF_METHOD]) == ZIP_METHOD_STORED && count < maxRecs) {
            art_build_rec_t *rec = &recs[count];
            u64 comp = artRd32(&h[ZIP_CEN_OFF_COMP]);
            u64 uncomp = artRd32(&h[ZIP_CEN_OFF_UNCOMP]);
            u64 localOff = artRd32(&h[ZIP_CEN_OFF_LOCAL]);

            // 序号超过 31、大小或偏移超出条目字段范围的图，装不进紧凑条目，直接跳过
            if (artParseEntryName((const char *)&h[ZIP_CENTRAL_HDR_SIZE], nameLen, rec) == 0 &&
                rec->suffixIndex <= ART_ENTRY_INDEX_MAX &&
                artApplyZip64Extra(&h[ZIP_CENTRAL_HDR_SIZE + nameLen], extraLen, &uncomp, &comp, &localOff) == 0 &&
                comp > 0 && comp <= ART_PACK_MAX_DATA_SIZE && localOff < ART_ENTRY_OFF_LIMIT) {
                rec->reserved = 0;
                rec->dataSize = (u32)comp;
                rec->localHdrOff = localOff;
                count++;
            }
        }

        pos += need;
        seen++;
    }

    free(buf);
    if (seen != total) {
        LOG("ARTPACK: central directory truncated (%u/%u)\n", (unsigned int)seen, (unsigned int)total);
        return -1;
    }
    return (int)count;
}

// 写 art.idx；成功返回 0
static int artWriteIndex(const char *idxPath, const art_idx_header_t *hdr, const art_idx_game_t *games, const art_idx_entry_t *entries)
{
    size_t gamesBytes = ((size_t)hdr->ps1Count + hdr->ps2Count + 1) * sizeof(art_idx_game_t);
    size_t entriesBytes = (size_t)hdr->entryCount * sizeof(art_idx_entry_t);
    int ok, fd = open(idxPath, O_WRONLY | O_CREAT | O_TRUNC, 0666);

    if (fd < 0)
        return -1;
    ok = write(fd, hdr, sizeof(*hdr)) == (ssize_t)sizeof(*hdr) &&
         write(fd, games, gamesBytes) == (ssize_t)gamesBytes &&
         write(fd, entries, entriesBytes) == (ssize_t)entriesBytes;
    close(fd);
    return ok ? 0 : -1;
}

/*
 * 从 art.zip 建立索引。成功后 dev->games 常驻内存；
 * art.idx 写入成功则释放条目表，失败（只读设备等）则把条目表留在内存中供本次会话使用。
 */
static int artBuildIndex(int zipFd, u64 archiveSize, art_pack_dev_t *dev)
{
    u64 cdOff, cdSize, total;
    art_build_rec_t *recs;
    art_idx_entry_t *entries;
    art_idx_game_t *games;
    art_idx_header_t hdr;
    u32 i, n, unique = 0, gameCount = 0, ps1Count = 0, g = 0;
    u8 curBucket = ART_BUCKET_NONE;
    int count;

    if (artFindCentralDir(zipFd, archiveSize, &cdOff, &cdSize, &total) < 0) {
        LOG("ARTPACK: invalid zip (no central directory)\n");
        return -1;
    }
    if (total == 0 || total > ART_PACK_MAX_ENTRIES) {
        LOG("ARTPACK: unsupported entry count %u\n", (unsigned int)total);
        return -1;
    }

    recs = malloc((size_t)total * sizeof(art_build_rec_t));
    if (!recs)
        return -1;

    count = artCollectRecords(zipFd, cdOff, cdSize, total, recs, (u32)total);
    if (count <= 0) {
        free(recs);
        return -1;
    }
    n = (u32)count;

    qsort(recs, n, sizeof(art_build_rec_t), &artCompareRec);

    // 统计去重后的游戏数；排序后 PS1 在前、PS2 在后
    for (i = 0; i < n; i++) {
        if (i == 0 || recs[i].bucket != recs[i - 1].bucket || strcmp(recs[i].gameId, recs[i - 1].gameId)) {
            gameCount++;
            if (recs[i].bucket == ART_BUCKET_PS1)
                ps1Count++;
        }
    }

    // 多留一条哨兵，用来算最后一个游戏的条目数
    games = calloc((size_t)gameCount + 1, sizeof(art_idx_game_t));
    if (!games) {
        free(recs);
        return -1;
    }

    /*
     * 原地把 32 字节的临时记录压成 8 字节的条目：第 unique 个条目的写入位置
     * 总在第 i(>=unique) 个记录之前，先把记录拷到栈上再写，不会覆盖未读数据。
     */
    entries = (art_idx_entry_t *)recs;
    for (i = 0; i < n; i++) {
        art_build_rec_t rec = recs[i];
        int newGame = (i == 0) || rec.bucket != curBucket || strcmp(rec.gameId, games[g].gameId);

        if (!newGame && unique > 0 && ART_ENTRY_TYPE(entries[unique - 1]) == rec.suffixType && ART_ENTRY_INDEX(entries[unique - 1]) == rec.suffixIndex)
            continue; // 重复条目，保留第一条

        if (newGame) {
            if (i != 0)
                g++;
            memcpy(games[g].gameId, rec.gameId, ART_GAME_ID_LEN);
            games[g].firstEntry = unique;
            curBucket = rec.bucket;
        }

        entries[unique] = ART_ENTRY_MAKE(rec.localHdrOff, rec.dataSize, rec.suffixType, rec.suffixIndex);
        unique++;
    }
    games[gameCount].firstEntry = unique; // 哨兵

    // 收缩到条目实际大小
    {
        void *shrunk = realloc(entries, (size_t)unique * sizeof(art_idx_entry_t));
        if (shrunk)
            entries = (art_idx_entry_t *)shrunk;
    }

    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, ART_IDX_MAGIC, ART_IDX_MAGIC_LEN);
    hdr.version = ART_IDX_VERSION;
    hdr.archiveSize = archiveSize;
    hdr.ps1Count = ps1Count;
    hdr.ps2Count = gameCount - ps1Count;
    hdr.entryCount = unique;

    dev->games = games;
    dev->gameCount = gameCount;
    dev->ps1Count = ps1Count;
    dev->entryCount = unique;
    dev->entryTableOff = ART_IDX_ENTRY_TABLE_OFF(gameCount);

    if (artWriteIndex(dev->idxPath, &hdr, games, entries) == 0) {
        free(entries);
        dev->memEntries = NULL;
        LOG("ARTPACK: index written %s (%u games, %u entries)\n", dev->idxPath, (unsigned int)gameCount, (unsigned int)unique);
    } else {
        dev->memEntries = entries;
        LOG("ARTPACK: cannot write %s, keep index in RAM (%u games, %u entries)\n", dev->idxPath, (unsigned int)gameCount, (unsigned int)unique);
    }
    return 0;
}

/*
 * 从 fd 的 base 偏移处读取索引（头 + 游戏表），成功时填充 dev；头部不匹配返回 -1（需要重建）。不比较时间戳。
 * areaSize 是索引所在区域的字节数：外部 art.idx 为文件大小，要求恰好等于表末尾；
 * 嵌入 zip 的索引为条目的数据区大小（预留区，比实际索引大，多出的部分补零），只要求表末尾不超出。
 * 成功时 dev->entryTableOff 已加上 base，后续按 entryTableOff 读条目表时不需要再区分来源。
 */
static int artLoadIndexAt(int fd, u64 base, u64 areaSize, int embedded, u64 archiveSize, art_pack_dev_t *dev)
{
    art_idx_header_t hdr;
    art_idx_game_t *games;
    size_t gamesBytes;
    u32 gameCount;
    u64 tableEnd;

    if (artReadAt(fd, base, &hdr, sizeof(hdr)) < 0 ||
        memcmp(hdr.magic, ART_IDX_MAGIC, ART_IDX_MAGIC_LEN) || hdr.version != ART_IDX_VERSION ||
        hdr.archiveSize != archiveSize || hdr.ps1Count > ART_PACK_MAX_ENTRIES || hdr.ps2Count > ART_PACK_MAX_ENTRIES ||
        hdr.ps1Count + hdr.ps2Count == 0 || hdr.entryCount > ART_PACK_MAX_ENTRIES)
        return -1;
    gameCount = hdr.ps1Count + hdr.ps2Count;

    tableEnd = (u64)ART_IDX_ENTRY_TABLE_OFF(gameCount) + (u64)hdr.entryCount * sizeof(art_idx_entry_t);
    if (embedded ? (tableEnd > areaSize) : (tableEnd != areaSize))
        return -1;
    // entryTableOff 是 u32，base 加上表偏移不得溢出
    if (base + (u64)ART_IDX_ENTRY_TABLE_OFF(gameCount) > 0xFFFFFFFFu)
        return -1;

    gamesBytes = ((size_t)gameCount + 1) * sizeof(art_idx_game_t);
    games = malloc(gamesBytes);
    if (!games || artReadAt(fd, base + sizeof(art_idx_header_t), games, gamesBytes) < 0 || games[gameCount].firstEntry != hdr.entryCount) {
        free(games);
        return -1;
    }

    dev->games = games;
    dev->gameCount = gameCount;
    dev->ps1Count = hdr.ps1Count;
    dev->entryCount = hdr.entryCount;
    dev->entryTableOff = (u32)(base + ART_IDX_ENTRY_TABLE_OFF(gameCount));
    dev->memEntries = NULL;
    return 0;
}

// 读取外部 art.idx（与图包同名、扩展名为 .idx）；失败返回 -1
static int artLoadIndex(u64 archiveSize, art_pack_dev_t *dev)
{
    off64_t fileSize;
    int ret;
    int fd = open(dev->idxPath, O_RDONLY);

    if (fd < 0)
        return -1;
    fileSize = lseek64(fd, 0, SEEK_END);
    ret = (fileSize < 0) ? -1 : artLoadIndexAt(fd, 0, (u64)fileSize, 0, archiveSize, dev);
    close(fd);
    return ret;
}

/*
 * 读取嵌入 zip 的索引：第一个本地头必须是不压缩的 art.idx 条目，数据区即索引（后面可有补零的预留空间）。
 * 数据区起点按本地头里实际的文件名与 extra 长度计算，与取图时一致。
 * 成功时 dev->idxPath 改为 zip 路径，之后取图与整表载入都按 entryTableOff 从 zip 里读，无需另写一套。
 * 失败返回 -1，dev 保持不变（继续尝试外部索引或重建）。
 */
static int artLoadEmbeddedIndex(int zipFd, u64 archiveSize, art_pack_dev_t *dev)
{
    u8 lh[ZIP_LOCAL_HDR_SIZE + sizeof(ART_PACK_EMBED_IDX_NAME) - 1];
    u32 compSize;
    u64 dataStart;

    if (archiveSize < sizeof(lh) || artReadAt(zipFd, 0, lh, sizeof(lh)) < 0)
        return -1;
    if (artRd32(lh) != ZIP_SIG_LOCAL || artRd16(&lh[ZIP_LOCAL_OFF_METHOD]) != ZIP_METHOD_STORED ||
        artRd16(&lh[ZIP_LOCAL_OFF_NAME_LEN]) != sizeof(ART_PACK_EMBED_IDX_NAME) - 1 ||
        memcmp(&lh[ZIP_LOCAL_HDR_SIZE], ART_PACK_EMBED_IDX_NAME, sizeof(ART_PACK_EMBED_IDX_NAME) - 1))
        return -1;
    compSize = artRd32(&lh[ZIP_LOCAL_OFF_COMP]);
    if (compSize == ZIP_U32_MAX || compSize != artRd32(&lh[ZIP_LOCAL_OFF_UNCOMP]))
        return -1;
    dataStart = (u64)ZIP_LOCAL_HDR_SIZE + artRd16(&lh[ZIP_LOCAL_OFF_NAME_LEN]) + artRd16(&lh[ZIP_LOCAL_OFF_EXTRA_LEN]);
    if (dataStart + compSize > archiveSize)
        return -1;

    if (artLoadIndexAt(zipFd, dataStart, compSize, 1, archiveSize, dev) < 0)
        return -1;
    snprintf(dev->idxPath, sizeof(dev->idxPath), "%s", dev->zipPath);
    return 0;
}

// 在 ART2 目录中查找图包（dirPath 为 ART2 目录路径，末尾带分隔符）：文件名以 .zip 结尾（不区分大小写）即可，主名任意，
// 排除 "." 开头的隐藏文件（含 macOS 的 "._" AppleDouble 假 zip）；取 readdir 遇到的第一个。
// 成功时 zipOut 为图包完整路径，idxNameOut 为对应索引文件名（不含路径）。
// 返回 ART_FIND_PACK_FOUND 表示找到；ART_FIND_PACK_NONE 表示目录存在但没有图包；ART_FIND_PACK_NO_DIR 表示目录打不开（没有 ART2）。
#define ART_FIND_PACK_FOUND  0
#define ART_FIND_PACK_NONE   -1
#define ART_FIND_PACK_NO_DIR -2
static int artFindPackZip(const char *dirPath, char *zipOut, size_t zipOutSize, char *idxNameOut, size_t idxNameOutSize)
{
    struct dirent *ent;
    DIR *dir = opendir(dirPath); // 打开失败即没有 ART2 目录
    size_t extLen = strlen(ART_PACK_ZIP_EXT);
    size_t nameLen, baseLen;
    int found = ART_FIND_PACK_NONE;

    if (!dir)
        return ART_FIND_PACK_NO_DIR;
    while ((ent = readdir(dir)) != NULL) {
        nameLen = strlen(ent->d_name);
        if (nameLen <= extLen)
            continue;
        baseLen = nameLen - extLen;
        if (strcasecmp(ent->d_name + baseLen, ART_PACK_ZIP_EXT))
            continue;
        if (!strncmp(ent->d_name, ART_PACK_HIDDEN_PREFIX, strlen(ART_PACK_HIDDEN_PREFIX)))
            continue;

        snprintf(zipOut, zipOutSize, "%s%s", dirPath, ent->d_name);
        snprintf(idxNameOut, idxNameOutSize, "%.*s%s", (int)baseLen, ent->d_name, ART_PACK_IDX_EXT);
        found = ART_FIND_PACK_FOUND;
        break;
    }
    closedir(dir);
    return found;
}

/* ---------------- PNG 解码（独立实现，行为与 textures.c 一致） ---------------- */

typedef struct
{
    const u8 *data;
    size_t remaining;
} art_png_reader_t;

typedef struct
{
    u8 red;
    u8 green;
    u8 blue;
    u8 alpha;
} art_clut_t;

#define ART_CLUT4_COLORS  16
#define ART_CLUT8_COLORS  256
#define ART_CLUT4_W       8
#define ART_CLUT4_H       2
#define ART_CLUT8_W       16
#define ART_CLUT8_H       16
#define ART_RGBA_BYTES    4
#define ART_RGB_BYTES     3
#define ART_TEX_ALIGN     128

static void artPngRead(png_structp pngPtr, png_bytep data, png_size_t length)
{
    art_png_reader_t *r = (art_png_reader_t *)png_get_io_ptr(pngPtr);

    if (!r || length > r->remaining) {
        png_error(pngPtr, "ARTPACK PNG input exhausted");
        return;
    }
    memcpy(data, r->data, length);
    r->data += length;
    r->remaining -= length;
}

static void artTexFree(GSTEXTURE *tex)
{
    free(tex->Mem);
    tex->Mem = NULL;
    free(tex->Clut);
    tex->Clut = NULL;
}

static void artTexPrepare(GSTEXTURE *tex)
{
    tex->Width = 0;
    tex->Height = 0;
    tex->PSM = GS_PSM_CT24;
    tex->ClutPSM = 0;
    tex->TBW = 0;
    tex->Mem = NULL;
    tex->Clut = NULL;
    tex->Vram = 0;     // VRAM 由纹理管理器分配
    tex->VramClut = 0;
    tex->Filter = GS_FILTER_LINEAR;
    tex->Delayed = 1;  // 只加载到 EE 内存，延迟上传
}

// 调色板转换；8 位调色板需按 GS CSM1 规则交换 8~15 与 16~23 块
static void artFillClut(GSTEXTURE *tex, png_colorp palette, int numPalette, png_bytep trans, int numTrans, int clutColors)
{
    art_clut_t *clut = (art_clut_t *)tex->Clut;
    int i;

    memset(clut, 0, clutColors * sizeof(art_clut_t));
    for (i = 0; i < numPalette; i++) {
        clut[i].red = palette[i].red;
        clut[i].green = palette[i].green;
        clut[i].blue = palette[i].blue;
        clut[i].alpha = (i < numTrans) ? (trans[i] >> 1) : 0x80;
    }
    if (clutColors == ART_CLUT8_COLORS) {
        for (i = 0; i < numPalette; i++) {
            if ((i & 0x18) == 8) {
                art_clut_t tmp = clut[i];
                clut[i] = clut[i + 8];
                clut[i + 8] = tmp;
            }
        }
    }
}

static void artCopyPixels(GSTEXTURE *tex, png_bytep *rows, size_t memSize)
{
    u8 *dst = (u8 *)tex->Mem;
    int x, y;
    size_t i;

    switch (tex->PSM) {
        case GS_PSM_T4:
            for (y = 0; y < tex->Height; y++)
                memcpy(&dst[y * (tex->Width / 2)], rows[y], tex->Width / 2);
            for (i = 0; i < memSize; i++)
                dst[i] = (u8)((dst[i] << 4) | (dst[i] >> 4)); // PNG 高半字节在前，GS 低半字节在前
            break;
        case GS_PSM_T8:
            for (y = 0; y < tex->Height; y++)
                memcpy(&dst[y * tex->Width], rows[y], tex->Width);
            break;
        case GS_PSM_CT24:
            for (y = 0; y < tex->Height; y++)
                for (x = 0; x < tex->Width; x++, dst += ART_RGB_BYTES)
                    memcpy(dst, &rows[y][ART_RGBA_BYTES * x], ART_RGB_BYTES);
            break;
        default: // GS_PSM_CT32，alpha 从 0~255 缩放到 GS 的 0~128
            for (y = 0; y < tex->Height; y++)
                for (x = 0; x < tex->Width; x++, dst += ART_RGBA_BYTES) {
                    memcpy(dst, &rows[y][ART_RGBA_BYTES * x], ART_RGB_BYTES);
                    dst[ART_RGB_BYTES] = rows[y][ART_RGBA_BYTES * x + ART_RGB_BYTES] >> 1;
                }
            break;
    }
}

static int artDecodePng(const u8 *data, size_t size, GSTEXTURE *tex, int dropAlpha, int compactBackground)
{
    art_png_reader_t reader = {data, size};
    png_structp pngPtr;
    png_infop infoPtr = NULL;
    png_bytep *volatile rows = NULL;
    png_bytep volatile allRows = NULL;
    png_bytep volatile rowBuf = NULL;
    volatile int status = ERR_BAD_FILE;
    png_uint_32 w, h;
    int bitDepth, colorType, interlace;
    png_size_t rowBytes;
    size_t memSize;
    int y;
    int directRgb, directRead, streamCompact;

    artTexPrepare(tex);

    pngPtr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!pngPtr)
        return ERR_READ_STRUCT;
    infoPtr = png_create_info_struct(pngPtr);
    if (!infoPtr) {
        png_destroy_read_struct(&pngPtr, NULL, NULL);
        return ERR_INFO_STRUCT;
    }

    if (setjmp(png_jmpbuf(pngPtr))) {
        status = ERR_SET_JMP;
        goto fail;
    }

    png_set_read_fn(pngPtr, &reader, &artPngRead);
    png_set_sig_bytes(pngPtr, 0);
    png_read_info(pngPtr, infoPtr);
    png_get_IHDR(pngPtr, infoPtr, &w, &h, &bitDepth, &colorType, &interlace, NULL, NULL);
    tex->Width = w;
    tex->Height = h;

    if (bitDepth == 16)
        png_set_strip_16(pngPtr);
    if (colorType == PNG_COLOR_TYPE_GRAY || colorType == PNG_COLOR_TYPE_GRAY_ALPHA || bitDepth < 4) {
        png_set_expand(pngPtr);
        if (png_get_valid(pngPtr, infoPtr, PNG_INFO_tRNS))
            png_set_tRNS_to_alpha(pngPtr);
    }
    // 旧解码器不支持灰度图；新解码器转成 RGB 后按真彩处理
    if (colorType == PNG_COLOR_TYPE_GRAY || colorType == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(pngPtr);
    // 直读条件：非隔行扫描，且最终是 CT24（RGB，或忽略 alpha 的 RGBA）或 8 位调色板 T8
    // 此时 libpng 行数据布局与旧路径写入的纹理内存布局一致（像素紧密排列），逐行直接读进纹理，省掉整图行缓冲和拷贝
    // 隔行扫描图必须整图解码，其余格式（CT32、T4、灰度等）继续走旧路径
    directRgb = interlace == PNG_INTERLACE_NONE &&
                (colorType == PNG_COLOR_TYPE_RGB || (colorType == PNG_COLOR_TYPE_RGB_ALPHA && dropAlpha));
    directRead = directRgb || (interlace == PNG_INTERLACE_NONE && colorType == PNG_COLOR_TYPE_PALETTE && bitDepth == 8);
    /* compactBackground 只在 BG 且 Coverflow 时为真。为假时不进入判断，也不算 CT24 大小。 */
    streamCompact = 0;
    if (compactBackground)
        streamCompact = texShouldStreamCompactBackground((int)w, (int)h, colorType, interlace, dropAlpha);
    if (directRgb) {
        // 行缓冲直接是每像素 3 字节，不补 filler；RGBA 在解码时就去掉 alpha
        if (colorType == PNG_COLOR_TYPE_RGB_ALPHA)
            png_set_strip_alpha(pngPtr);
    } else {
        png_set_filler(pngPtr, 0xff, PNG_FILLER_AFTER);
    }
    png_read_update_info(pngPtr, infoPtr);

    switch (png_get_color_type(pngPtr, infoPtr)) {
        case PNG_COLOR_TYPE_RGB_ALPHA:
            // BG/COV 忽略 alpha：行缓冲本来就是每像素 4 字节，按 CT24 只拷前 3 字节
            tex->PSM = dropAlpha ? GS_PSM_CT24 : GS_PSM_CT32;
            break;
        case PNG_COLOR_TYPE_RGB:
            tex->PSM = GS_PSM_CT24;
            break;
        case PNG_COLOR_TYPE_PALETTE: {
            png_colorp palette = NULL;
            png_bytep trans = NULL;
            int numPalette = 0, numTrans = 0, clutColors, clutW, clutH;

            if (!png_get_PLTE(pngPtr, infoPtr, &palette, &numPalette))
                goto fail;
            png_get_tRNS(pngPtr, infoPtr, &trans, &numTrans, NULL);

            if (bitDepth == 4) {
                tex->PSM = GS_PSM_T4;
                clutColors = ART_CLUT4_COLORS;
                clutW = ART_CLUT4_W;
                clutH = ART_CLUT4_H;
            } else if (bitDepth == 8) {
                tex->PSM = GS_PSM_T8;
                clutColors = ART_CLUT8_COLORS;
                clutW = ART_CLUT8_W;
                clutH = ART_CLUT8_H;
            } else {
                status = ERR_BAD_DEPTH;
                goto fail;
            }
            if (numPalette < 1 || numPalette > clutColors) {
                status = ERR_BAD_DEPTH;
                goto fail;
            }

            tex->ClutPSM = GS_PSM_CT32;
            tex->Clut = memalign(ART_TEX_ALIGN, gsKit_texture_size_ee(clutW, clutH, GS_PSM_CT32));
            if (!tex->Clut)
                goto fail;
            artFillClut(tex, palette, numPalette, trans, numTrans, clutColors);
            break;
        }
        default:
            status = ERR_BAD_DEPTH;
            goto fail;
    }

    if (tex->Width == 0 || tex->Height == 0 || tex->Width > ART_PACK_MAX_TEX_DIM || tex->Height > ART_PACK_MAX_TEX_DIM ||
        gsKit_texture_size(tex->Width, tex->Height, tex->PSM) > ART_PACK_MAX_TEX_BYTES) {
        status = ERR_BAD_DIMENSION;
        goto fail;
    }

    rowBytes = png_get_rowbytes(pngPtr, infoPtr);

    if (streamCompact) {
        size_t ct16Size = (size_t)tex->Width * (size_t)tex->Height * sizeof(u16);
        u16 *dst;

        // 流式 CT16S：只分配 w*h*2，用单行缓冲解码并立刻转换
        if (rowBytes == 0 || rowBytes < (png_size_t)tex->Width * 3)
            goto fail;
        tex->PSM = GS_PSM_CT16S;
        tex->Mem = memalign(ART_TEX_ALIGN, ct16Size);
        rowBuf = malloc(rowBytes);
        if (!tex->Mem || !rowBuf)
            goto fail;
        dst = (u16 *)tex->Mem;
        for (y = 0; y < tex->Height; y++) {
            png_read_row(pngPtr, rowBuf, NULL);
            texCompactRgbRowToCt16s(dst + (size_t)y * tex->Width, rowBuf, tex->Width, 3);
        }
        png_read_end(pngPtr, NULL);
        free(rowBuf);
        rowBuf = NULL;
        png_destroy_read_struct(&pngPtr, &infoPtr, NULL);
        return 0; // Mem 所有权交给调用方（texcache）
    }

    memSize = gsKit_texture_size_ee(tex->Width, tex->Height, tex->PSM);
    tex->Mem = memalign(ART_TEX_ALIGN, memSize);

    if (directRead) {
        // 行数据总量不能超过分配的纹理内存，否则说明格式判断有误，直接失败
        // （CT24 的分配大小按每像素 4 字节，比紧密排列的 3 字节行数据大，不能要求严格相等）
        if (rowBytes == 0 || !tex->Mem || (size_t)rowBytes * tex->Height > memSize)
            goto fail;
        for (y = 0; y < tex->Height; y++)
            png_read_row(pngPtr, (png_bytep)tex->Mem + (size_t)y * rowBytes, NULL);
        png_read_end(pngPtr, NULL);
        png_destroy_read_struct(&pngPtr, &infoPtr, NULL);
        return 0; // Mem/Clut 所有权交给调用方（texcache）
    }

    rows = calloc(tex->Height, sizeof(png_bytep));
    allRows = malloc((size_t)rowBytes * tex->Height);
    if (rowBytes == 0 || !tex->Mem || !rows || !allRows)
        goto fail;
    for (y = 0; y < tex->Height; y++)
        rows[y] = allRows + (size_t)y * rowBytes;

    png_read_image(pngPtr, rows);
    artCopyPixels(tex, rows, memSize);
    png_read_end(pngPtr, NULL);

    free(allRows);
    free(rows);
    png_destroy_read_struct(&pngPtr, &infoPtr, NULL);
    return 0; // Mem/Clut 所有权交给调用方（texcache）

fail:
    free(rowBuf);
    free(allRows);
    free(rows);
    artTexFree(tex);
    png_destroy_read_struct(&pngPtr, &infoPtr, NULL);
    return status;
}

/* ---------------- 对外接口 ---------------- */

int artPackDeviceInit(int mode, const char *prefix, const char *sep, int rank)
{
    art_pack_dev_t dev;
    char artDir[ART_PACK_PATH_MAX];
    char idxName[ART_PACK_PATH_MAX];
    off64_t archiveSize;
    int zipFd;
    int findRet;

    // 参数无效时未能探测 ART2，返回 NO_ART2，保证 NO_PACK 只表示 ART2 已确认存在
    if (!artValidMode(mode))
        return ART_PACK_INIT_NO_ART2;

    artPackDeviceEnd(mode);
    if (!prefix || !sep)
        return ART_PACK_INIT_NO_ART2;

    // 图包开关关闭：只用 stat 判断 ART2 是否存在（供散图路径使用），不列目录、不打开图包
    if (!gEnableArtPack) {
        struct stat st;

        snprintf(artDir, sizeof(artDir), "%s%s", prefix, ART_PACK_DIR_NAME);
        if (stat(artDir, &st) == 0 && S_ISDIR(st.st_mode))
            return ART_PACK_INIT_NO_PACK;
        return ART_PACK_INIT_NO_ART2;
    }

    memset(&dev, 0, sizeof(dev));
    // 只在 ART2 目录中查找图包；没有 ART2 或其中没有图包时，图包模式关闭，完全走旧流程
    snprintf(artDir, sizeof(artDir), "%s%s%s", prefix, ART_PACK_DIR_NAME, sep);
    // 此前已调用 artPackDeviceEnd，所有失败返回路径上该设备的图包状态都保持未激活
    findRet = artFindPackZip(artDir, dev.zipPath, sizeof(dev.zipPath), idxName, sizeof(idxName));
    if (findRet == ART_FIND_PACK_NO_DIR)
        return ART_PACK_INIT_NO_ART2;
    if (findRet != ART_FIND_PACK_FOUND)
        return ART_PACK_INIT_NO_PACK;

    // 索引与图包同在 ART2 目录，名字跟随图包名（索引由本模块按同名写出，无需再扫目录）
    snprintf(dev.idxPath, sizeof(dev.idxPath), "%s%s", artDir, idxName);

    zipFd = open(dev.zipPath, O_RDONLY);
    if (zipFd < 0) {
        LOG("ARTPACK: cannot open %s\n", dev.zipPath);
        return ART_PACK_INIT_NO_PACK;
    }
    archiveSize = lseek64(zipFd, 0, SEEK_END);

    // 先试嵌入 zip 的索引，再试外部同名 .idx，最后才从中央目录重建
    if (archiveSize > 0 && (artLoadEmbeddedIndex(zipFd, (u64)archiveSize, &dev) == 0 || artLoadIndex((u64)archiveSize, &dev) == 0)) {
        LOG("ARTPACK: index loaded %s (%u games)\n", dev.idxPath, (unsigned int)dev.gameCount);
    } else if (archiveSize > 0) {
        LOG("ARTPACK: rebuilding index for %s ...\n", dev.zipPath);
        if (artBuildIndex(zipFd, (u64)archiveSize, &dev) < 0) {
            LOG("ARTPACK: index build failed, pack disabled\n");
            close(zipFd);
            artFreeDev(&dev);
            return ART_PACK_INIT_NO_PACK;
        }
    } else {
        close(zipFd);
        return ART_PACK_INIT_NO_PACK;
    }
    close(zipFd);

    dev.active = 1;
    dev.rank = rank;
    pthread_mutex_lock(&artPackMutex);
    // 路径可能变化，先关闭指向旧图包的 fd
    if (artPackBestFdMode == mode)
        artPackCloseBestFd();
    artFreeDev(&artPackDevs[mode]);
    artPackDevs[mode] = dev;
    // 新图包发布后重新选择最快设备
    artPackSelectBest();
    pthread_mutex_unlock(&artPackMutex);
    return ART_PACK_INIT_ACTIVE;
}

void artPackDeviceEnd(int mode)
{
    if (!artValidMode(mode))
        return;
    pthread_mutex_lock(&artPackMutex);
    // 卸载前必须关闭该设备上的 fd
    if (artPackBestFdMode == mode)
        artPackCloseBestFd();
    artFreeDev(&artPackDevs[mode]);
    // 释放后重新选择最快设备（被释放的可能正是当前最快设备）
    artPackSelectBest();
    pthread_mutex_unlock(&artPackMutex);
}

int artPackRandomActive(void)
{
    return gArtRandom && gEnableArtPack && artPackBestMode != ART_PACK_NO_BEST;
}

int artPackIsActive(int mode, int isRelative)
{
    int active;

    // 图包开关关闭时一律走旧流程，不加锁
    if (!gEnableArtPack)
        return 0;

    // 绝对路径（主题自带图片）不属于游戏 ART，始终走旧流程
    if (!isRelative || !artValidMode(mode))
        return 0;
    pthread_mutex_lock(&artPackMutex);
    // 与请求设备无关，只要全局有最快图包即视为可用
    active = (artPackBestMode != ART_PACK_NO_BEST);
    pthread_mutex_unlock(&artPackMutex);
    return active;
}

int artPackLoadImage(int mode, const char *folder, int isRelative, const char *value, const char *suffix, GSTEXTURE *resultTex, int compactBackground)
{
    art_pack_dev_t *dev;
    art_idx_entry_t found = 0;
    art_idx_entry_t *list = NULL;
    const art_idx_entry_t *entries;
    char key[ART_GAME_ID_LEN];
    u8 bucket, sufType, sufIndex;
    u8 *buf = NULL;
    size_t bufLen = 0, dataStart = 0;
    int lo, hi, gi = -1, fd, haveEntry = 0, i, result;
    u32 firstEntry, nextEntry, entryNum;

    if (!artValidMode(mode) || !isRelative || !folder || !value || !resultTex)
        return ERR_BAD_FILE;

    // 只有 PS2 与 PS1(POPS) 从图包取图；APPS 等其它类型在图包模式下无图
    if (!strcmp(folder, ART_FOLDER_NAME))
        bucket = ART_BUCKET_PS2;
    else if (!strcmp(folder, ART_FOLDER_PS1))
        bucket = ART_BUCKET_PS1;
    else
        return ERR_BAD_FILE;

    if (artNormalizeId(value, strlen(value), key) < 0 || artParseRequestSuffix(suffix, &sufType, &sufIndex) < 0)
        return ERR_BAD_FILE;

    pthread_mutex_lock(&artPackMutex);
    BG_PROF_MARK(BG_EV_PACK_LOCK, suffix, value, 0);
    // 统一使用全局最快设备的图包，而不是请求设备自己的图包
    if (artPackBestMode == ART_PACK_NO_BEST) {
        pthread_mutex_unlock(&artPackMutex);
        return ERR_BAD_FILE;
    }
    dev = &artPackDevs[artPackBestMode];
    if (!dev->active) {
        pthread_mutex_unlock(&artPackMutex);
        return ERR_BAD_FILE;
    }

    // 游戏表分 PS1、PS2 两段，各自按 gameId 排序，只在对应的一段里二分查找
    if (bucket == ART_BUCKET_PS1) {
        lo = 0;
        hi = (int)dev->ps1Count - 1;
    } else {
        lo = (int)dev->ps1Count;
        hi = (int)dev->gameCount - 1;
    }
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        const art_idx_game_t *g = &dev->games[mid];
        int cmp = strcmp(g->gameId, key);
        if (cmp == 0) {
            gi = mid;
            break;
        }
        if (cmp < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    if (gi < 0) {
        pthread_mutex_unlock(&artPackMutex);
        return ERR_BAD_FILE;
    }

    firstEntry = dev->games[gi].firstEntry;
    nextEntry = dev->games[gi + 1].firstEntry; // 末尾有哨兵，gi + 1 不会越界
    if (nextEntry < firstEntry || nextEntry > dev->entryCount) {
        pthread_mutex_unlock(&artPackMutex);
        return ERR_BAD_FILE;
    }
    entryNum = nextEntry - firstEntry;

    if (dev->memEntries) {
        entries = &dev->memEntries[firstEntry];
    } else {
        // 该游戏的所有条目在 art.idx 中连续，一次读出
        size_t bytes = (size_t)entryNum * sizeof(art_idx_entry_t);
        list = malloc(bytes);
        fd = list ? open(dev->idxPath, O_RDONLY) : -1;
        if (fd < 0 || artReadAt(fd, (u64)dev->entryTableOff + (u64)firstEntry * sizeof(art_idx_entry_t), list, bytes) < 0) {
            if (fd >= 0)
                close(fd);
            free(list);
            pthread_mutex_unlock(&artPackMutex);
            return ERR_BAD_FILE;
        }
        close(fd);
        entries = list;
    }

    if (sufIndex >= ART_REQ_RANDOM_SCR2) {
        i = artPickRandomEntry(entries, entryNum, sufType, sufIndex, key, bucket);
        BG_PROF_MARK(BG_EV_PICK, suffix, value, i >= 0 ? (int)ART_ENTRY_INDEX(entries[i]) : -1);
        if (i >= 0) {
            found = entries[i];
            haveEntry = 1;
        }
    } else {
        for (i = 0; i < (int)entryNum; i++) {
            if (ART_ENTRY_TYPE(entries[i]) == sufType && ART_ENTRY_INDEX(entries[i]) == sufIndex) {
                found = entries[i];
                haveEntry = 1;
                break;
            }
        }
    }
    free(list);

    if (!haveEntry || ART_ENTRY_SIZE(found) == 0 || ART_ENTRY_SIZE(found) > ART_PACK_MAX_DATA_SIZE) {
        pthread_mutex_unlock(&artPackMutex);
        return ERR_BAD_FILE;
    }

    // 一次读取 本地头 + 文件名/extra 余量 + 数据；extra 过长时再补读一次
    // 使用最快设备常驻的 fd；定位/读取失败时关闭，下次取图重新打开
    fd = artPackEnsureBestFd();
    if (fd != ART_PACK_FD_NONE) {
        size_t want = ZIP_LOCAL_HDR_SIZE + ART_PACK_LOCAL_SLACK + ART_ENTRY_SIZE(found);
        buf = malloc(want);
        if (buf) {
            if (lseek64(fd, (off64_t)ART_ENTRY_OFF(found), SEEK_SET) != (off64_t)ART_ENTRY_OFF(found)) {
                artPackCloseBestFd();
            } else {
                ssize_t got;
                // EE 看不到 IOP FatFs 的 fast seek 是否生效，这里不另记状态。
                // SEEK_DONE 到 READ_DONE 的间隔可以间接反映定位和读盘。
                BG_PROF_MARK(BG_EV_SEEK_DONE, suffix, value, 0);
                got = read(fd, buf, want);
                BG_PROF_MARK(BG_EV_READ_DONE, suffix, value, (int)got);
                if (got < 0) {
                    artPackCloseBestFd();
                } else if (got >= ZIP_LOCAL_HDR_SIZE && artRd32(buf) == ZIP_SIG_LOCAL) {
                    dataStart = ZIP_LOCAL_HDR_SIZE + artRd16(&buf[ZIP_LOCAL_OFF_NAME_LEN]) + artRd16(&buf[ZIP_LOCAL_OFF_EXTRA_LEN]);
                    if ((size_t)got >= dataStart + ART_ENTRY_SIZE(found)) {
                        bufLen = ART_ENTRY_SIZE(found);
                    } else if (dataStart > ZIP_LOCAL_HDR_SIZE + ART_PACK_LOCAL_SLACK) {
                        // 头部超出余量：直接定位到数据处重读
                        if (artReadAt(fd, ART_ENTRY_OFF(found) + dataStart, buf, ART_ENTRY_SIZE(found)) == 0) {
                            dataStart = 0;
                            bufLen = ART_ENTRY_SIZE(found);
                        } else {
                            artPackCloseBestFd();
                        }
                    }
                }
            }
        }
    }
    pthread_mutex_unlock(&artPackMutex);

    if (bufLen == 0) {
        free(buf);
        return ERR_BAD_FILE;
    }

    result = artDecodePng(buf + dataStart, bufLen, resultTex, texSuffixDropsAlpha(suffix), compactBackground);
    BG_PROF_MARK(BG_EV_DECODE_DONE, suffix, value, result);
    free(buf);
    return result;
}
