#ifndef __ART_PACK_H
#define __ART_PACK_H

#include <gsKit.h>

/*
 * ZIP 图包加载器（第一阶段）。
 *
 * 设备 ART2 目录存在文件名含 OPLM_ART 的 .zip（不区分大小写，取 readdir 遇到的第一个）时，该设备进入“图包模式”：
 * PS2/PS1 游戏图片只从图包读取，找不到即视为没有图片，不再回退 ART/ART2 散文件。
 * 没有符合条件的 zip 时图包模块完全不参与，旧流程不变。
 *
 * 设备以 item_list_t::mode（BDM_MODE..HDD_MODE）区分。
 */

// 设备就绪后调用（在 sbDetectArtBuckets 之前），prefix 为设备根前缀，sep 为路径分隔符（SMB 为 "\\"）
// 只在 ART2 目录中查找图包，返回值为下面三态之一：
// ART_PACK_INIT_ACTIVE：进入图包模式（调用方跳过 ART2 分桶探测）
// ART_PACK_INIT_NO_PACK：ART2 存在但没有图包或图包初始化失败，走旧流程（需要 ART2 分桶探测）
// ART_PACK_INIT_NO_ART2：ART2 目录不存在（调用方无需 ART2 分桶探测，分桶清零即可）
#define ART_PACK_INIT_ACTIVE  1
#define ART_PACK_INIT_NO_PACK 0
#define ART_PACK_INIT_NO_ART2 -1
int artPackDeviceInit(int mode, const char *prefix, const char *sep);

// 设备清理/关闭时调用，释放该设备的全部图包内存；可重复调用
void artPackDeviceEnd(int mode);

// 该设备是否处于图包模式；isRelative 为 0（主题内绝对路径图片）时始终返回 0
int artPackIsActive(int mode, int isRelative);

// 从图包加载一张图片到 GSTEXTURE，返回值与 itemGetImage 一致：>=0 成功，<0 无图
int artPackLoadImage(int mode, const char *folder, int isRelative, const char *value, const char *suffix, GSTEXTURE *resultTex);

#endif
