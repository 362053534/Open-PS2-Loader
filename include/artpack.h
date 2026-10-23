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
 */

// 设备就绪后调用（ART2 只在这里探测一次），prefix 为设备根前缀，sep 为路径分隔符（SMB 为 "\\"）
// 只在 ART2 目录中查找图包，返回值为下面三态之一：
// ART_PACK_INIT_ACTIVE：ART2 存在且图包已启用（调用方走 ART2 散文件，取不到时回退图包）
// ART_PACK_INIT_NO_PACK：ART2 已确认存在但未启用图包（没有图包或图包初始化失败），调用方走 ART2 散文件
// ART_PACK_INIT_NO_ART2：没有 ART2 或未能探测（参数无效），调用方走旧 ART 目录
#define ART_PACK_INIT_ACTIVE  1
#define ART_PACK_INIT_NO_PACK 0
#define ART_PACK_INIT_NO_ART2 -1
int artPackDeviceInit(int mode, const char *prefix, const char *sep);

// 设备清理/关闭时调用，释放该设备的全部图包内存；可重复调用
void artPackDeviceEnd(int mode);

// 该设备是否已启用图包；isRelative 为 0（主题内绝对路径图片）时始终返回 0
int artPackIsActive(int mode, int isRelative);

// 从图包加载一张图片到 GSTEXTURE，返回值与 itemGetImage 一致：>=0 成功，<0 无图
int artPackLoadImage(int mode, const char *folder, int isRelative, const char *value, const char *suffix, GSTEXTURE *resultTex);

#endif
