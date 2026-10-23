#ifndef __ART_PACK_H
#define __ART_PACK_H

#include <gsKit.h>

/*
 * ZIP 图包加载器（第一阶段）。
 *
 * 设备 ART2 目录存在文件名含 OPLM_ART 的 .zip（不区分大小写，取 readdir 遇到的第一个）时，该设备启用图包：
 * PS2/PS1 游戏图片先从 ART2 散文件读取，取不到时再回退到图包（PS2 只查图包 PS2/，PS1 只查图包 PS1/）。
 * 没有符合条件的 zip 时图包模块完全不参与，旧流程不变。
 *
 * 设备以 item_list_t::mode（BDM_MODE..HDD_MODE）区分。
 * 多个设备都有图包时，所有设备的图包回退统一使用速度最快（rank 最小）的设备上的图包。
 */

// 图包设备速度等级：数值越小越快；等级相同时取 mode 序号较小者
#define ART_PACK_RANK_APA        0 // 内置 HDD（APA）
#define ART_PACK_RANK_BDM_HDD    1 // BDM ata
#define ART_PACK_RANK_BDM_ILINK  2 // BDM i.LINK
#define ART_PACK_RANK_BDM_MX4SIO 3 // BDM sdc（MX4SIO）
#define ART_PACK_RANK_SMB        4 // SMB 网络
#define ART_PACK_RANK_BDM_USB    5 // BDM USB
#define ART_PACK_RANK_UNKNOWN    6 // 未知类型（最低优先级）

// 设备就绪后调用（ART2 只在这里探测一次），prefix 为设备根前缀，sep 为路径分隔符（SMB 为 "\\"），rank 为 ART_PACK_RANK_*
// 只在 ART2 目录中查找图包，返回值为下面三态之一：
// ART_PACK_INIT_ACTIVE：ART2 存在且图包已启用（调用方走 ART2 散文件，取不到时回退图包）
// ART_PACK_INIT_NO_PACK：ART2 已确认存在但未启用图包（没有图包或图包初始化失败），调用方走 ART2 散文件
// ART_PACK_INIT_NO_ART2：没有 ART2 或未能探测（参数无效），调用方走旧 ART 目录
#define ART_PACK_INIT_ACTIVE  1
#define ART_PACK_INIT_NO_PACK 0
#define ART_PACK_INIT_NO_ART2 -1
int artPackDeviceInit(int mode, const char *prefix, const char *sep, int rank);

// 设备清理/关闭时调用，释放该设备的全部图包内存；可重复调用
void artPackDeviceEnd(int mode);

// 当前是否有可用图包（全局选出的最快设备的图包，与请求设备无关）；isRelative 为 0（主题内绝对路径图片）或 mode 无效时返回 0
int artPackIsActive(int mode, int isRelative);

// 从全局选出的最快设备的图包加载一张图片到 GSTEXTURE（mode 只做有效性检查），返回值与 itemGetImage 一致：>=0 成功，<0 无图
int artPackLoadImage(int mode, const char *folder, int isRelative, const char *value, const char *suffix, GSTEXTURE *resultTex);

// memEntries 来源：写索引失败时的常驻副本，必须保留
#define ART_PACK_ENTRIES_RESIDENT 0
// memEntries 来源：因成为最快设备而从 art.idx 载入，不再是最快设备时可释放
#define ART_PACK_ENTRIES_FROM_IDX 1
// 最快设备条目表懒加载的最大连续失败次数，用尽后改为每次取图按需读 art.idx
#define ART_PACK_ENTRIES_LOAD_RETRY_MAX 3

// [图包调试显示] 临时代码，调试完成后删除
// 没有最快设备时 bestMode 的取值（与 artpack.c 中 ART_PACK_NO_BEST 一致）
#define ART_PACK_STATUS_NO_BEST (-1)
typedef struct
{
    int bestMode;     // 最快设备 mode，无则 ART_PACK_STATUS_NO_BEST
    int bestRank;     // 最快设备速度等级
    int zipFdOpen;    // art.zip 常驻 fd 是否打开
    int entriesInMem; // 条目表是否在内存
    int entriesFrom;  // 条目表来源 ART_PACK_ENTRIES_*
    u32 entryCount;   // 条目总数
    int loadFails;    // 条目表连续载入失败次数
    int loadGaveUp;   // 是否已放弃载入（改为按需读 art.idx）
    u32 idxOpens;     // 按需打开 art.idx 的累计次数
} art_pack_status_t;
// 非阻塞读取图包状态：成功返回 1；锁被占用返回 0 且不修改 *out
int artPackGetStatus(art_pack_status_t *out);

#endif
