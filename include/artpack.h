#ifndef __ART_PACK_H
#define __ART_PACK_H

#include <gsKit.h>

/*
 * ZIP 图包加载器（第一阶段）。
 *
 * 设备 ART2 目录存在任意命名的 .zip（扩展名不区分大小写，排除 "." 开头的隐藏文件，取 readdir 遇到的第一个）时，该设备启用图包：
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

// 启动时所有设备的图包探测完后调用一次（IO 线程），把最快设备的条目表载入内存；失败时取图回退为按需读 art.idx
void artPackPreloadBestEntries(void);

// 设备清理/关闭时调用，释放该设备的全部图包内存；可重复调用
void artPackDeviceEnd(int mode);

// 当前是否有可用图包（全局选出的最快设备的图包，与请求设备无关）；isRelative 为 0（主题内绝对路径图片）或 mode 无效时返回 0
int artPackIsActive(int mode, int isRelative);

// 随机取图是否正在生效：随机开关打开、图包总开关打开且有可用图包
int artPackRandomActive(void);

// 取走最近一次随机 BG 选图时数出的张数并清空；没有则返回 -1。BG 加载完成后由加载线程调用。
int artPackTakeBgCount(void);

// 从全局选出的最快设备的图包加载一张图片到 GSTEXTURE（mode 只做有效性检查），返回值与 itemGetImage 一致：>=0 成功，<0 无图
// compactBackground 为 1 时，超 512K 的非隔行真彩 PNG 在解码阶段直接写成 CT16S
int artPackLoadImage(int mode, const char *folder, int isRelative, const char *value, const char *suffix, GSTEXTURE *resultTex, int compactBackground);

#endif
