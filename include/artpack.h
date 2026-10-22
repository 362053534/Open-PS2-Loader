#ifndef __ART_PACK_H
#define __ART_PACK_H

#include <gsKit.h>

/*
 * art.zip 图包加载器（第一阶段）。
 *
 * 设备根目录存在 art.zip（不区分大小写）时，该设备进入“图包模式”：
 * PS2/PS1 游戏图片只从图包读取，找不到即视为没有图片，不再回退 ART/ART2 散文件。
 * 没有 art.zip 时图包模块完全不参与，旧流程不变。
 *
 * 设备以 item_list_t::mode（BDM_MODE..HDD_MODE）区分。
 */

// 设备就绪后调用（在 sbDetectArtBuckets 之前），prefix 为设备根前缀
// 返回 1 表示进入图包模式（调用方可跳过 ART2 探测），0 表示走旧流程
int artPackDeviceInit(int mode, const char *prefix);

// 设备清理/关闭时调用，释放该设备的全部图包内存；可重复调用
void artPackDeviceEnd(int mode);

// 该设备是否处于图包模式；isRelative 为 0（主题内绝对路径图片）时始终返回 0
int artPackIsActive(int mode, int isRelative);

// 从图包加载一张图片到 GSTEXTURE，返回值与 itemGetImage 一致：>=0 成功，<0 无图
int artPackLoadImage(int mode, const char *folder, int isRelative, const char *value, const char *suffix, GSTEXTURE *resultTex);

#endif
