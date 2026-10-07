# Open-PS2-Loader 工作交接文档

> 本文是后续开发的交接入口。之后每次修改代码、资源、CFG 或构建配置并提交时，必须在同一个提交中同步更新本文的“变更记录”和“当前状态”。

## 1. 仓库与分支

- 仓库：`362053534/Open-PS2-Loader`
- 工作目录：`/home/user/Open-PS2-Loader`
- 固定工作分支：`arena/01a0ba0f-open-ps2-loader`
- 不要切换、创建或推送其它分支。
- 当前远程：`origin/arena/01a0ba0f-open-ps2-loader`
- 本文创建时最新提交：`791c279 fix: enable colors for built-in list theme`
- 当前诊断基线提交：`91ccccf5a041eddc1c1e4e81595f0ca4deeff661`；本次在该基线上增加 Coverflow 随机死机的 debug-only 看门狗与阶段快照。
- 当前任务：定位 Coverflow 随机完全死机的最后执行阶段，不改变 Coverflow 功能、渲染后端、纹理缓存行为或正常构建路径。
- 当前工作区：本次源码改动与本文同步提交；未修改图片、CFG、`temp/` 用户资产或 `AGENTS.md`。
- 验证状态：已完成静态 diff 检查。外部 `make iopcore_debug --trace` 已进入源码编译，但首轮在 `src/menusys.c` 发现 `theme_element_t` 没有 `name` 字段；已改为记录 `coverflow`/`theme_element` 分类标签，等待重新跑完整 debug 构建和实机验证。

## 2. 当前任务重点

当前主要维护 Coverflow 主题、ICO 光碟图显示，以及两个内置主题的颜色设置。

### Coverflow ICO 当前参数

参数位于 `src/themes.c`：

```c
#define COVERFLOW_ICO_W 128
#define COVERFLOW_ICO_H 138
#define COVERFLOW_ICO_POPUP_GAP 13
```

当前行为：

- ICO 只在 Coverflow 绘制路径中显示。
- ICO 使用独立的 ICO cache，不挤占 Coverflow 封面 cache。
- ICO 逻辑尺寸为 `128×138`（448 下整数 X_SCALE/Y_SCALE 后约 `128×128`）。
- 正常显示封面时，ICO 从中心封面背后向左弹出。
- ICO 最终右边缘距离中心封面左边缘 `13` 个逻辑像素。
- ICO 底边与中心封面底边对齐。
- ICO 使用线性过滤和倒影。
- 宽屏模式下，ICO 横向宽度、右边缘和弹出间距使用 `rmWideScaleF()` 压缩；高度保持不变，以抵消电视横向拉伸。
- 当前单次弹出动画使用 Coverflow 的动画时长和纯三次缓出算法。

### ART 加载与 ICO 状态规则

最新约定是：

- 当前中心游戏变化时，立即清除旧 ICO 状态。
- Coverflow 移动动画开始时，正常封面模式下立即隐藏旧 ICO。
- 如果当前游戏没有变化，单独出现 `texLoading > 0` 不得隐藏已显示的 ICO，也不得重置 ICO 动画状态。
- `texLoading == 0` 只用于决定新的 ICO 请求何时入队，不能作为当前 ICO 的隐藏条件。
- `coverflowIcoLoaded`、`coverflowIcoPopupActive` 和 `coverflowIcoPopupStartTime` 不得因为背景图单独加载而被清除或重新初始化。

### 关闭封面图时

当前 `c422986` 的行为保留：

- `gEnableArtCOV == 0` 时跳过 Case 和封面实际绘制；
- 保留 ICO 绘制路径；
- 停止 Coverflow 移动动画状态；
- ICO 仍根据当前 item 加载和显示；
- 当前实现没有扩展为“五个 ICO 继承 Coverflow 动画模块”的方案。

## 3. 当前布局参数

### 内置 Coverflow CFG

文件：`misc/conf_theme_coverflow.cfg`

当前 Coverflow 元素位置：

```ini
main3:
    y=260

appsMain3:
    y=310
```

### 代码基准

文件：`src/themes.c`

```c
#define COVERFLOW_BASELINE_YOFFSET 165
#define COVERFLOW_APPS_YOFFSET -60
#define COVERFLOW_APPS_CENTER_YOFFSET 1
```

这些值的意图：

- CFG 以外的公共基准相对原 161 下移 `4` 像素（`COVERFLOW_BASELINE_YOFFSET=165`；曾为下移 8 / 169，2026-10-07 再上移 4）；
- 内置 CFG 的游戏/APPS Coverflow 元素曾各上移 `8` 像素，用以抵消当时代码侧的下移 8；
- 本次只改代码基准、未再改 CFG y，故内置主题封面相对改前视觉位置上移 4 像素；
- 外部 Coverflow 主题如果没有相同的 CFG 补偿，会使用新的代码基准。

APPS 的 `COVERFLOW_APPS_YOFFSET` 为 `-60`，相对原 `-50` 上移 10 像素；游戏和 APPS 的共同下移由 `COVERFLOW_BASELINE_YOFFSET` 负责。`COVERFLOW_APPS_CENTER_YOFFSET` 只作为 APPS 中心封面底边锚点的微调；非中心封面不再使用独立的 Y 偏移。

### Coverflow 数量与横向尺寸

- `coverflow_count` 重新支持第三方主题设置；加载后规范化为 `1/3/5/7/9`：小于等于 1 为 1，中间偶数向上加 1，大于 9 为 9。
- 5 张封面继续使用当前内部基线大小和间距；3 张保持 5 张的非中心封面大小，先反算满屏间距再从每侧间距扣除 60 像素，以留下约 60 像素安全距离；7/9 张保持 5 张间距并反推较小的非中心封面尺寸。
- 3 张目标整排宽度约为 `640 - 2×60`；7/9 张按 `(数量 - 1) × (非中心宽 + 间隙) + 中心宽 = 640` 铺满逻辑屏幕；1 张时没有非中心封面，保持中心封面几何。
- `COVERFLOW_MAX` 为 9，绘制/动画临时数组和 Coverflow cache 槽位均按规范化后的数量工作。

## 4. 倒影实现

文件：`src/renderman.c`

ICO 使用 `rmDrawPixmapReflect()`。

此前倒影被拆成多条横带，并且每条横带的 UV 方向没有形成真正的垂直镜像，导致：

- 阶梯状分层；
- 横带接缝；
- 纹理采样重复、重叠感。

当前 `rmDrawReflectionRows()` 已改为：

- 一个带顶点 alpha 渐变的 quad；
- 倒影顶部采样原图底部；
- 倒影底部采样原图上方四分之一；
- 使用反向 UV 形成真正的垂直镜像。

Case + Cover 的独立 `rmDrawOverlayPixmapReflectFrac()` 路径不要随意与 ICO 的普通 pixmap reflection 路径混合修改。

## 5. 内置主题颜色

两个内置主题共享同一套自定义颜色：

- 背景色；
- UI 文字色；
- 普通文字色；
- 选中文字色。

全局颜色变量位于 `src/opl.c`，包括：

```c
gDefaultBgColor
gDefaultTextColor
gDefaultUITextColor
gDefaultSelTextColor
```

`src/themes.c` 中的 `thmSetColors()` 会把这些颜色应用到当前主题。

颜色设置 UI 位于 `src/gui.c`。两个内置主题的 GUI 索引均应视为可编辑，判断应使用：

```c
themeID < THM_NUM_BUILTIN
```

不要再使用只适用于单个内置主题的：

```c
themeID == 0
temp = !temp
```

当前已修复：

- 内置非 CF 主题颜色按钮显示为灰色的问题；
- 内置非 CF 主题颜色无法保存的问题。

## 6. 相关文件

- `src/themes.c`：Coverflow 布局、ICO cache、ICO 加载条件、ICO 弹出动画、内置主题加载。
- `include/themes.h`：主题结构体、内置主题数量和主题接口。
- `src/renderman.c`：ICO 普通 pixmap 倒影、Coverflow Case+Cover 倒影、宽屏坐标缩放。
- `include/renderman.h`：渲染器接口。
- `src/texcache.c`：ART 异步 cache 和请求队列；ICO 继续复用这里的机制。
- `misc/conf_theme_coverflow.cfg`：内置 Coverflow 主题 CFG，由 Makefile 的 `bin2c` 规则嵌入。
- `src/gui.c`：UI 颜色配置对话框及内置主题颜色可编辑逻辑；debug 构建记录 GUI 帧心跳、输入/菜单/绘制边界。
- `src/menusys.c`：Coverflow 导航、单步移动、L1/R1 翻页动画和当前 item 变化检测；debug 构建快照 `menuSemaId`、光标和当前游戏。
- `src/textures.c`、`include/textures.h`：大型背景（CT24 与带 alpha 的 CT32）的 CT16S 压缩，降低纹理池争用；已有 ART 解码阶段供看门狗读取。
- `include/debugdiag.h`：仅 `__DEBUG` 生效的阶段、Coverflow 上下文和跨模块诊断接口。
- `src/ioman.c`：debug 构建独立 kernel 看门狗线程；停顿后通过独立静态缓冲区直写诊断快照。
- `src/renderman.c`：debug 构建记录 hires sync/flip、普通 queue/finish、TexManager bind/nextFrame 边界。
- `src/texcache.c`：debug 构建提供 texture loading、活动 ART、pthread 创建状态和 `texLoadingMutex` 无锁快照。

### 低分辨率大背景卡顿的根因与处理

- 测试图 `temp/SLPM_552.82_BG.png` 是 `640×480` RGB PNG。CT24 在 gsKit 中按约 `1,228,800` bytes 占用 VRAM。
- NTSC 448i 普通双缓冲的纹理池约为 `1,900,544` bytes；再扣除 plank、case 和当前 Coverflow 封面后，背景会迫使 gsKit TexManager 在每帧反复驱逐/重新上传纹理。这是 VRAM 工作集抖动，不是 PNG worker 与 BG 擦除之间的直接竞态。
- Background cache 统一保持两个槽位，不区分普通列表、Coverflow 或 GS 分辨率；切换游戏时保留上一张完整背景作为 fallback。
- 所有视频模式下，加载成功的大型背景（>512KB 的 CT24，以及带 alpha 的 CT32）在进入 cache 前转换为带抖动的 CT16S；CT32 的 alpha 被忽略，结果全部不透明（原来透明/半透明处透出的是帧清屏黑色，现在显示该像素存储的 RGB）。framebuffer、双缓冲、坐标和普通主题绘制路径不变。
- 静态检查当前默认 `usePthread = 0` 的生命周期：待处理请求由 `ioRemoveRequestsWithCleanup()` 移除并通过 UID 校验，正在执行的请求不在队列中；cache 选择只复用 `qr == 0` 的槽，因此取消请求不会把活动槽交给下一张图。`rmEndFrame()` 先执行 `gsKit_finish()`，随后才允许 `texFree()`/`rmUnloadTexture()` 复用纹理管理器 block，未发现渲染线程过早释放的直接路径。
- 是否压缩 BG 在渲染线程入队时记录到请求中，IO worker 不直接解引用模式切换中的 `gsGlobal`；这也覆盖了默认队列和备用 pthread 加载路径。

### 非 Coverflow 内置列表 ART 对比结论

- 普通背景、封面和其它 ART 仍走 `drawGameImage()` → `getGameImageTexture()` → `cacheGetTexture()`；Coverflow 的 quiet cache 入口没有替换这条路径。
- 普通列表不启用 Coverflow 的 hires 图元裁切、倒影和封面预取；普通 `rmDrawPixmap()`/overlay 仍走原有提交路径。
- `cacheLoadImage1()` 的请求冷却保护必须只对普通请求生效。普通请求的 `quiet` 标志为 0，光标切换后仍可丢弃已经从 IO 队列取出的旧请求；Coverflow quiet 请求的 `quiet` 标志为 1，绕过 `cdFramesCount`，避免被普通列表的单封面冷却机制误停载。
- 当前分支内置列表主题的默认背景资源为 `settings_bg`，普通主题继续使用 `Index_*`、`case` 和 `apps_case` 资源；这些资源未改为 Coverflow 专用资源。

### Coverflow overscan 变换

- `rmSetOverscan()` 通过 `iDisplayWidth/iDisplayHeight` 缩放逻辑坐标，并通过 `fRenderXOff/fRenderYOff` 添加一次显示区域偏移。
- Coverflow 的 `rmBuildCoverTransform()` 必须保持 transform 坐标处于 render offset 之前；Case、封面和倒影各自在提交阶段统一添加一次 offset。
- 不要在 `rmBuildCoverTransform()` 中提前加入 `fRenderXOff/fRenderYOff`，否则 overscan 大于 0 时会重复应用偏移，导致整个 Coverflow 模块向右/下漂移。

### Coverflow 垂直缩放变换

- Coverflow 动画的 `centerFactor` 只决定尺寸、明暗和水平布局补偿；垂直方向不再插值非中心 Y 偏移。
- 中心封面的底边是整个 Coverflow 模块的定位锚点，中心尺寸决定固定的水平中心线；非中心封面 Y 相对偏移始终为 0。
- 每个动画阶段的封面都围绕这条中心线按自身当前高度计算底边，因此非中心与中心封面中点水平对齐，不再反推 `verticalScalePivotY`。Case、封面和倒影仍共享同一浮点绘制变换。

## 7. 不要恢复的内容

- 不要恢复 `COVERFLOW_DIAG_BDM_INDEX_ALWAYS` 或其它 BdmIndex 常显诊断逻辑。
- 不要恢复已删除的纯封面诊断函数。
- 不要覆盖普通非 CF 的 `gfx/Index_0.png`～`gfx/Index_4.png`。
- WOPL/Coverflow 专用 BdmIndex 资源必须使用独立文件名。
- 不要把第三方 Coverflow 的 `coverflow_animation_speed` 忽略掉。
- 不要把游戏和 APPS 的非中心垂直偏移合并成一个不可区分的值。
- 不要用 `texLoading > 0` 隐藏或重置当前未变化游戏的 ICO。
- 未完成完整构建和 PS2 实机验证前，不要继续提交未经验证的 hires 裁切方案。
- 不要把大型 RGB 背景恢复为低分辨率路径中的长期 CT24 常驻纹理，也不要把 Background cache 恢复为 3 个槽；这会重新触发 NTSC 448i 的 VRAM 工作集抖动。
- 不要恢复“带 alpha 的 CT32 背景不压缩”；用户决定 CT32 背景也无条件压缩为不透明 CT16S（不逐像素检查是否全不透明）。
- 不要执行会生成大量无关文件的完整 `make clean release`；当前环境也没有有效的 PS2SDK/GSKIT 交叉工具链。

## 8. 验证限制

当前环境缺少：

- PS2SDK；
- GSKIT；
- `mips64r5900el-ps2-elf-gcc`；
- PS2 实机或有效模拟器验证环境。

每次提交至少执行：

```sh
git diff --check
git status --short
git log -1 --oneline
```

如果无法完成 PS2 构建或实机测试，提交说明中必须明确注明。

## 9. 提交更新规则

每次后续修改必须：

1. 先读取本文，确认当前参数和行为；
2. 修改代码或资源；
3. 在本文同步更新当前状态、参数、已知限制或变更记录；
4. 将代码修改和 `HANDOFF.md` 放在同一个提交中；
5. 执行 `git diff --check`；
6. 提交到固定分支 `arena/01a0ba0f-open-ps2-loader`；
7. 推送到 `origin/arena/01a0ba0f-open-ps2-loader`。

## 10. 变更记录

### 2026-10-07 · 增加 Coverflow 随机死机的 debug-only 定位看门狗

- 用户确认 ART worker 停滞和 720p/1080i 花屏问题已解决；本次只定位 Coverflow 浏览时随机完全死机，不以减少 Coverflow 功能、切换后端或改变正常渲染/缓存行为规避问题。
- `include/debugdiag.h` 定义仅 `__DEBUG` 编译的阶段快照：GUI 帧心跳、GUI 锁等待、`menuSemaId` 等待/持有、主题元素、Coverflow 封面纹理请求/提交/预取/ICO、普通 gsKit queue/finish、hires sync/flip、TexManager bind/nextFrame、输入和 frame hook。
- `src/ioman.c` 在 debug 构建启动独立 kernel 看门狗线程；主线程约 3 秒没有推进帧心跳时，通过独立静态缓冲区和直接 `write()`/SIO 路径输出 `[GUI_WD]`、`[CF_DIAG]` 快照。快照包含主/IO 线程状态、IO worker stage/progress/队列标志、IO printf/file lock、GUI/menu 信号量状态、视频模式、当前游戏 ID/文字/光标、Coverflow 动画起点/目标/步数/当前绘制槽位，以及纹理请求、ART 解码阶段、pthread 和纹理 mutex 状态。
- `src/gui.c`、`src/menusys.c`、`src/themes.c`、`src/renderman.c` 只写无锁 volatile 诊断字段；release 构建不增加看门狗线程、日志、锁或阶段路径。
- 首轮外部 `make iopcore_debug --trace` 已确认 PS2 交叉工具链可用，并成功编译到本次修改的 `src/renderman.c` 后进入 `src/menusys.c`；错误原因是诊断代码误用了不存在的 `theme_element_t.name` 字段，现已改为稳定的元素分类标签。尚未完成重新构建和实机验证；下一步重跑 debug 构建，复现一次死机并收集死机前最后一组 `[GUI_WD]`/`[CF_DIAG]` 日志。

### 2026-10-07 · Coverflow 整排基线再上移 4 像素（BASELINE 169→165）

- 用户要求：游戏与 APPS 封面模块整体再上移 4 像素（基线校准）。
- `src/themes.c`：`COVERFLOW_BASELINE_YOFFSET` 169→165（正值下移；相对原 161 现为再下移 4）。游戏与 APPS 共用该偏移；`COVERFLOW_APPS_YOFFSET` / `COVERFLOW_APPS_CENTER_YOFFSET`、cfg 的 Coverflow `y`、BdmIndex、ItemText、MenuIcon 未改。碟片 ICO 锚定封面底边，随封面一起上移、相对位置不变。
- 未改：`misc/conf_theme_coverflow.cfg`（基线刻意不靠 cfg y；改一处宏即可同时覆盖游戏+APPS）。
- 已提交。

### 2026-10-07 — Coverflow 非中心封面略缩小（scale -36→-40）；CF BdmIndex 下移 6 像素；L1/R1 翻页动画 2.5×→2.0×；ICO 128→138

- 用户要求：非中心封面定稿 scale -40；CF 主题 BdmIndex 再下移至 y=420（相对原 414 下移 6）；L1/R1 整页翻页动画时长由单步的 2.5 倍改为 2.0 倍；碟片 ICO 逻辑边长 128→138，使 448 下整数 Y_SCALE 得满 128 屏线（128→119）。游戏与 APPS 共用同一非中心 scale。
- `src/themes.c`：`COVERFLOW_DEFAULT_NONCENTER_SCALE` -36→-40（5 张基线非中心主图宽 104→100；高按比例：游戏约 159→152.9，APPS 约 111→107.1）。`thmTriggerCoverflowAnimMulti` 内 `gCoverflowActiveAnimSpeed = gCoverflowAnimSpeed * 2.5f` 改为 `* 2.0f`（默认单步 200ms → 翻页 400ms，原为 500ms）；注释同步。`COVERFLOW_ICO_SIZE` 128→138（仍走 `rmDrawPixmapReflect`→整数 `Y_SCALE`：`(138*448)/480=128`；128 时为 119）。
- `misc/conf_theme_coverflow.cfg`：`main6` BdmIndex `y` 414→420。未改列表主题。
- 未改：中心 scale、封面基准、单步动画时长、`COVERFLOW_ICO_POPUP_GAP`、其它 cfg。
- 验证：CI；需实机看非中心封面、BdmIndex、翻页动画速度与中心碟片 ICO 尺寸。

### 2026-10-07 — 非中心 scale 试 -40 后改回 -36；ICO 拆为 128×138

- 用户要求：非中心封面 scale 从 -40 改回 -36（试过后定稿回退）。不改 BdmIndex y=420、翻页 2.0×。碟片 ICO 由正方 `COVERFLOW_ICO_SIZE` 拆为宽 128、高 138，使 448 下屏约 128×128（横向不再随高一起变大）。
- `src/themes.c`：`COVERFLOW_DEFAULT_NONCENTER_SCALE` -40→-36（5 张基线非中心主图宽 100→104）；`COVERFLOW_ICO_SIZE` 改为 `COVERFLOW_ICO_W` 128 + `COVERFLOW_ICO_H` 138（`rmWideScaleF` 仅作用于宽）。未提交，待用户确认。

### 2026-10-07 — 修复 fntRefreshCache / IfPending 误用语言序号当字体槽（改刷 gTheme->fonts[0]）

- 背景：自 f5ecc1a5（2025-07-07）起，`fntRefreshCache()` 用 `fonts[lngGetGuiValue()]` 刷新字模；语言序号与 fntSys 字体槽不是同一索引。内置主题绘制句柄是 `theme->fonts[0]`（通常为槽 0，`fntLoadDefault` 也写槽 0）。`guiLangID != 0`（繁中/日/韩及显式选中的外挂简中等）时刷新落到空槽，翻页/切设备/界面过渡的字模清空变成空操作，42 个 atlas 用尽后新字空白，直到改分辨率/语言/设置触发 `fntUpdateAspectRatio`。4f55b526 的 Coverflow `fntRefreshCacheIfPending()` 复用了同一错误索引，pending 打在绘制字体上却检查语言槽，条件刷新永不触发。本问题早于 Coverflow（列表主题 1080i 缺字排查时引入）。
- 做法：两函数改为刷新/检查 `gTheme->fonts[0]` 对应槽；`gTheme == NULL`、句柄越界或 `!isValid` 时直接返回。仍仅在 720p/1080i（`gVMode == 10 || 11`）生效。抽出 `fntGuiPrimaryFont()`，`#include "include/lang.h"` 改为 `"include/themes.h"`。不刷全部字体、不改 atlas/导航调用点。
- 未改：`fntUpdateAspectRatio`、主题加载、`thmLoadFonts`、cfg。
- 验证：工作箱交叉编译；需在非默认语言 + 720p/1080i 下确认列表翻页与 Coverflow 滑过大量标题后不再缺字。

### 2026-10-06 — 列表主题封面外壳 case.png 改为 8 位调色板（T8 + tRNS），每帧省 195,584 B 显存

- 背景：列表主题游戏封面 `main3` 的 `overlay=case`（`misc/conf_theme_OPL.cfg`）每帧都绑定。`gfx/case.png` 自 7c6cb73a（2025-06-04 校正封面位置）起是 256x256 RGBA，按 CT32 载入占 262,144 B；1080i 列表页（约 1 MB 字体 atlas + 真彩 BG 压缩后 655,360 B）估算会超过 1,982,464 B 纹理池。Coverflow 的 `cf_case.png`、列表主题的 `apps_case.png` 和上游的 `case.png` 本来就是 8 位调色板。
- 做法：只换图，不改代码/cfg。pngquant 2.18（`256 --nofs --speed 1 --strip`）量化为 256 色调色板 + tRNS（177 项），尺寸、几何、外壳内框坐标不变；去掉 sRGB/gAMA 块，与 `apps_case.png` 一致（IHDR/PLTE/tRNS/IDAT/IEND）。`textures.c` 调色板 8 位分支按 T8 + CT32 CLUT 载入（alpha = tRNS>>1，无 tRNS 项为 0x80）。
- 显存：262,144 B → 66,560 B（含 1,024 B CLUT），省 195,584 B（约 1080i 纹理池的 9.9%）。文件 16,022 B → 7,500 B。
- 质量（对原图）：alpha 最大误差 15/255、平均 0.071；换算到 GS 0–128 alpha 最大 8、平均 0.07；完全透明像素（alpha=0）逐像素一致（外壳窗口仍完全透明，ATEST NOTEQUAL 0 行为不变）；预乘 PSNR 53.6 dB（Pillow FASTOCTREE 仅 34.4 dB）；叠在暗/木纹/亮背景上 PSNR 52.9–54.4 dB，套在封面上（列表主题位置）59.5 dB，肉眼无差别。对比图在工作箱 `/workspace/rv/case_compare/`。
- 未改：`apps_case.png`、`cf_case.png`、全部 cfg 与代码、第三方主题。
- 验证：CI 编译检查；需实机确认列表主题封面外壳观感正常。

### 2026-10-06 — 关闭 hires 诊断：HIRES_PASS_DIAG 改为 0（去掉洋红分界线与 [HIRES_ALIAS]/[HIRES_TEX] 日志）

- 用户要求：花屏已在电视上确认消失（64539981 + gsKit v1.3.8 补丁实机日志 0 条 `[HIRES_ALIAS]`），关掉诊断日志和诊断用的洋红分割线。
- 做法：仅把 `src/renderman.c` 的 `#define HIRES_PASS_DIAG 1` 改为 `0`，诊断代码保留在 `#if HIRES_PASS_DIAG` 内备用（需要时改回 1 或编译加 `-DHIRES_PASS_DIAG=1`）。
  - 关闭后不再编译：`hiresDiagDrawPassSeams()`（洋红 2 行横线，release/debug 都有）、`hiresDiagRecordBind()`/`hiresDiagEndFrame()`（`[HIRES_ALIAS]`/`[HIRES_TEX]`，仅调试构建输出）及其约 3.6 KB 静态数组；`rmTexBind()` 只剩直接调用 `gsKit_TexManager_bind`，渲染与纹理行为不变。
- 未改：`__DEBUG` 下的 `[ART_LOAD]`/`[ART_REQ_*]`/`[ART_DIAG]`/`[IO_END]` 与 IO worker 阶段诊断（来自 2026-10-04/05 的 ART 停滞调查，与本次 hires 诊断无关，release 构建本来就没有）、gsKit、cfg、其它代码。
- 验证：工作箱 CI 镜像 chroot 中 release（`make all`）与调试（`DEBUG=1 IOPCORE_DEBUG=1`）构建均通过，renderman.c 无新增警告，ELF 中无 `HIRES_` 字符串；CI 编译检查；实机确认洋红线消失。

### 2026-10-06 — 两套内置主题的 1 像素 alphamask 改为只压暗游戏背景图，不压暗默认背景

- 用户要求：列表主题和 CF 主题的 1 像素 alphamask 只影响游戏背景图，不影响默认背景图 settings_bg。
- 做法：复用 Background 元素已有的 `mask=` 属性（`src/themes.c` `initMutableImage()` 读取 `<元素>_mask`；`drawGameImage()` 仅在 `drewGameArt`（真正取到游戏 BG 美术，含切换时暂显的上一张 BG）时紧接背景画遮罩，回退 settings_bg/plasma 或无选中项时不画）。无 C 代码改动。
  - `misc/conf_theme_OPL.cfg`：`main0` 加 `mask=alphamask`；删除全屏 StaticImage `main1`（alphamask）；原 main2–main9 → main1–main8，appsMain4/5/6 → appsMain3/4/5。
  - `misc/conf_theme_coverflow.cfg`：同样处理；原 main2–main9 → main1–main8，appsMain4/9 → appsMain3/8。结果与 a92b8e71 之前的元素定义一致，仅注释改写（新注释不含 ASCII `:`/`=`）。
  - 必须顺延编号：元素按 main0、main1… 依次读取，遇到第一个缺号就停止（`src/themes.c` `thmLoad()`）；C 代码只引用 `"main0"`。
- 撤销 a92b8e71 中“alphamask 每帧都画、同时压暗 settings_bg”的规则。显示游戏背景图时观感不变：遮罩绘制参数相同（0,0、ALIGN_NONE、全屏宽高、SCALING_NONE、`gDefaultCol`，与原 StaticImage 的 `aligned=0`、`scaled=0`、`DIM_INF` 等价，保留 778af6d5 的宽屏修复），顺序仍是紧接背景、在其它元素之前。兜底 settings_bg 不再压暗（CF 木纹平均灰度约 8.8 → 17.6）。
- `conf_theme_coverflow_sample.cfg` 本来就是 `main0` + `mask=alphamask`，未改；详情页 info 元素、第三方主题不受影响。
- 显存：alphamask 为 1x1 CT32（256 B）；mask 纹理不在元素间共享，游戏/应用列表各一份（最多 512 B），每帧只画其中一个；兜底背景时不再绑定。
- HANDOFF §7：删除 b6966c37 加的“升级 gsKit 需移植补丁/ClutStorageMode/rmOnVSync”常驻规则（§10 中的 gsKit 记录保留）。
- 未改：C 代码、`HIRES_PASS_DIAG=1`、其它资源。
- 验证：CI 编译检查；需实机确认游戏背景图压暗正常、settings_bg 不压暗。

### 2026-10-06 — CI 镜像的 gsKit 改为 v1.3.8 + “本帧已绑定纹理不驱逐”补丁（仅文档，本提交用于触发 CI）

- 来源：gsKit 现在来自 `362053534/gsKit` 分支 `texmanager-1.3.8` @ `db2858a6`（基于 tag v1.3.8，只改 `ee/gs/src/gsTexManager.c`，+54 行），由 `362053534/ps2sdk-ports` 提交 `efcfc707` 的 `build-cmakelibs.sh:104` 固定到完整 SHA：`$FETCH db2858a675f04b2d47fc53cef26283178d0eadf6 https://github.com/362053534/gsKit.git &`。镜像链 ps2sdk-ports → ps2dev → ps2homebrew（`ghcr.io/362053534/ps2homebrew:main`）已自动重建，新镜像的 `libgskit.a` 含新函数 `_blockAllocUnusedThisFrame`。
- 原因：hires（720p/1080i）每帧把整个绘制队列（含 inline 纹理上传）按 pass 重放 2~3 次。gsKit 原 `_blockAlloc()` 按权重驱逐时偏向“本帧已画完”的纹理（权重 = 上帧次数，低于“本帧还要画”的 2 倍），被驱逐的地址在同一帧被新上传覆盖，第 2/3 个 pass 读到错误内容，即 HIRES_PASS_DIAG 记录的 `[HIRES_ALIAS]` 同帧显存别名（中间横带花屏）。
- 补丁行为：分配显存时先检查只腾出“本帧尚未绑定”（`iUseCount == 0`，由 `gsKit_TexManager_nextFrame()` 每帧清零，OPL 在 `rmEndFrame()` 的 hires sync/flip 之后调用，按帧而非按 pass）的块能否凑出连续空间；能则只驱逐这些块（仍按原权重顺序），否则什么都不动、走 gsKit 原逻辑，因此不会比原来更差。一帧所需纹理本身超过纹理池时，仍可能经回退路径出现别名。
- OPL 侧：无代码改动，不复制 gsKit 源码；`HIRES_PASS_DIAG=1` 保持，诊断只读 `tex->Vram`，与补丁无关，预期 `[HIRES_ALIAS]` 大幅减少，需实机日志确认。
- 升级 gsKit 时必须重新核对并移植此补丁（见 §7）；直接换成 gsKit master/v1.5.1 还需 ps2sdk-ports 加 `-DBUILD_EXAMPLES=`（libpng/zlib pkg-config 问题）以及 OPL 的 `ClutStorageMode`/`rmOnVSync` 兼容改动。
- 同时更正上一条（9fefcf35）的措辞：OPL 的 alpha 测试为 `ATST=NOTEQUAL`、`AREF=0`（`src/renderman.c:410-412`），只跳过完全透明像素，并非“恒通过”；结论不变。

### 2026-10-06 — 带 alpha 的 CT32 背景也无条件压缩为 CT16S（全部不透明）

- 用户要求：带 alpha 的 BG 也压缩，不逐像素检查是否全不透明。
- `src/textures.c` `texCompactBackground()`：除 CT24 外也接受 CT32（每像素 4 字节，只读 R/G/B，忽略 A），阈值（>512KB）和 4x4 有序抖动不变，所有像素都写 alpha 位（0x8000）。
  - gsKit 的 TEXA 为 `(TA0=0x00, AEM=0, TA1=0x80)`，A 位为 1 即 alpha 0x80（完全不透明）；`rmDrawQuad()` 对 CT16S 关闭 alpha 混合，整张图完整可见。
  - 原来 CT32 BG 开 alpha 混合；OPL 的 alpha 测试为 `ATST=NOTEQUAL`、`AREF=0`（`src/renderman.c:410-412`），只跳过完全透明（alpha 0）的像素，并非恒通过（2026-10-06 后续提交更正）。BG 是 main0、第一个绘制，下面只有帧清屏黑色（非 hires `gColBlack`，hires 每个 pass 清成黑色），不是 settings_bg。所以：原来半透明处压暗/透明处显示黑色，现在半透明像素变成不透明，全透明像素显示 PNG 中存储的 RGB（通常是黑色，但不保证）。
  - 640x480 RGBA BG（如 SLUS_217.76）：VRAM 1,228,800 B → 655,360 B（省 573,440 B），EE 内存 1,228,800 B → 614,400 B。上次日志中 CF 的 42 次同帧别名有 41 次来自这张未压缩的 RGBA BG。
- `include/textures.h`、`src/texcache.c`：只改注释。
- HANDOFF §6/§7：删掉“RGBA/CT32 背景不压缩”的描述和禁止项，§6 背景压缩说明同步为当前状态（所有视频模式）。
- gsKit TexManager 驱逐修复（不驱逐本帧已绑定的纹理）暂缓：用户将另行审阅直接修改上游 gsKit 的补丁，本次不包含任何 gsKit 相关改动。
- 未改：`HIRES_PASS_DIAG=1`、其它代码与资源。
- 验证：CI 编译检查；需实机确认 RGBA BG 显示是否正常（原透明区域的颜色）。

### 2026-10-06 — settings_bg 恢复旧木纹图（512x256 16 色）；CF 的 alphamask 规则改成与列表主题一致

- 用户要求：
  - 兜底背景 `gfx/settings_bg.png` 换回旧的近黑木纹图（blob `b9cc8513`，1024x512 8 位调色板，28 级灰），按用户指定缩成 512x256、16 色 4 位调色板、无 tRNS；
  - 默认 CF 主题的 1 像素 alphamask 改成和列表主题一样：在 CFG 里配置、持续驻留，作为默认背景图和游戏背景图的遮罩层。
- `gfx/settings_bg.png`：Lanczos 缩到 512x256，再用 Lloyd 算法从原 28 级灰里优化出 16 级灰调色板（6–26），不抖动。对比过 box/Lanczos/最近邻 × 无抖动/有序抖动/FS 抖动，Lanczos 无抖动在全屏双线性拉伸后最接近原图（抖动没有收益）：
  - 平均灰度 17.58（与原图相同），标准差 4.47（原 4.65）；
  - PSNR 48.7 dB（640x448、1280x720、1920x1080 一致），木纹细节相关系数 0.87/0.79/0.61；
  - 缩到一半分辨率后，细纹理对比度低于原图（640x448 下 1.36 vs 1.80）；
  - 叠 50% 遮罩后 PSNR 54.7 dB。
  - VRAM 525,312 B（原 1024x512 T8）→ 65,792 B（512x256 T4，含 256 B CLUT），与上一版中灰图相同。列表主题 `conf_theme_OPL.cfg` 同样用这张图兜底。
- `misc/conf_theme_coverflow.cfg`：照搬列表主题 `conf_theme_OPL.cfg` main1 的做法。
  - 新增 `main1`：StaticImage，`default=alphamask`，`aligned=0`、`scaled=0`、`width/height=DIM_INF`，全屏，每帧都画；
  - 删除 `main0` 的 `mask=alphamask`；
  - 原 main1–main8 顺延为 main2–main9，appsMain3/appsMain8 顺延为 appsMain4/appsMain9。元素按 main0、main1… 顺序读取和绘制，插入只能顺延编号；C 代码只引用 `"main0"`。
  - 绘制顺序：背景 → alphamask → ItemsList → plank → Coverflow → MenuIcon → HintText → BdmIndex → LoadingIcon → ItemText。
  - alphamask 是 1x1 黑色、PNG alpha 128（GS alpha 64 = 50%）。现在无论画的是游戏背景图还是 settings_bg 都会压暗 50%：游戏背景图观感不变（以前也压暗），settings_bg 木纹平均灰度约 17.6 → 8.8。
  - 详情页与列表主题一致：info0 背景无遮罩，info1 为全屏 info 图，未改。
  - C 里的 `<元素>_mask` 机制保留给第三方主题；`conf_theme_coverflow_sample.cfg` 未改（仍用 `mask=alphamask`）。
  - alphamask 每帧只绑定一次，VRAM 压力大时仍可能被驱逐，与列表主题相同，重传只有 256 B。
- 未改：`HIRES_PASS_DIAG=1`、C 代码、`plank.png`、`cover.png`、其它 cfg。对比图：工作箱 `/workspace/woodbg/compare.png`。
- 验证：仅依赖 CI 编译检查，需实机确认木纹兜底背景压暗 50% 后的观感。

### 2026-10-06 — 降低 VRAM 占用：settings_bg 缩小、plank 缩小、真彩 BG 压缩改为全模式

- 原因：HIRES_PASS_DIAG 日志显示 hires（720p/1080i）花屏来自同帧 VRAM 驱逐，剩余事件都与 plank 和真彩 BG 有关（SLPM_552.82 的 640x480 RGB BG 在 hires 下按 CT24 占 1,228,800 B，约为 1080i 纹理池 1,982,464 B 的 62%，导致连续 33 帧整体抖动）。本次只降低显存占用，不改 gsKit 驱逐规则。用户确认三项一起提交。
- `gfx/settings_bg.png`：1024x512 T8（525,312 B）→ 512x256 4 位调色板 T4（65,792 B，含 256 B CLUT），省 459,520 B。取原图每隔一个像素、保留原 5 级灰色调色板（PNG 调色板补齐为 16 项，无 tRNS）。Background 元素恒为 640x480 主题坐标并整图拉伸（themes.c initBasic 用 screenWidth/screenHeight，renderman.c rmSetupQuad uv 取整张纹理，LINEAR 过滤），所以两套内置主题、各分辨率下仍铺满全屏；模拟拉伸后平均灰度 72.69（原 72.70），颗粒标准差 1.9（原 2.2），肉眼无差别。T4 加载路径已被 info.png/screen.png 使用。
- `gfx/plank.png`：1024x256 T8（263,168 B）→ 256x256 T8（66,560 B），省 196,608 B。只缩宽度（考虑 alpha 的 box 缩放），256 行全部保留，高光线和阴影 alpha 不变；像素映射回原 85 项 RGBA 调色板（含 tRNS）。plank 由内置 CF 主题 main2（StaticImage，width=DIM_INF，height=124）整图拉伸；三种分辨率下与原图逐像素误差 ≤2/255。缩高度（1024x128/512x128）会把高光线糊掉（误差 8–18/255），未采用。
- `src/texcache.c` `cacheShouldCompactBackground()`：去掉 `gsGlobal->DoubleBuffering == ON && gsGlobal->PSM == CT24` 条件，所有视频模式的 BG 请求都标记压缩；是否真正转换仍由 `texCompactBackground()` 决定（仅 >512KB 的无 alpha CT24 图，抖动转 CT16S，失败保留原图），在加载线程执行，每张 BG 一次。原条件来自 2d564ed1（只针对 NTSC 448i 低分辨率问题，未写排除 hires 的技术原因）；由于双缓冲恒开、≤704x576 的模式 framebuffer 都是 CT24，原条件实际等于“非 720p/1080i”。720p/1080i 的 framebuffer 本身就是 CT16S，压缩不损失可见色深。640x480 真彩 BG：1,228,800 B → 655,360 B（省 573,440 B），EE 内存净省 307,200 B（转换时临时多占 614,400 B）。列表主题 main0、详情页 info0、CF 延迟 BG/PrevCacheID_BG 都走同一个 BG cache 和入队函数；除 rmDrawQuad 的 alpha 判断（CT16S 与 CT24 同样关 alpha）外没有代码依赖 BG 的 PSM。带 alpha 的 RGBA（CT32）BG 仍不压缩（保持 alpha 语义），仍占 1,228,800 B。
- 合计：settings_bg + plank 常驻占用 788,480 B → 132,352 B，固定省 656,128 B（约 1080i 纹理池的 33%）；有真彩 BG 时在 720p/1080i 再省 573,440 B，SLPM_552.82 场景 BG + plank 由 1,491,968 B（池的 75%）降到 721,920 B（36%）。
- 风险：第三方主题若在 StaticImage 等元素里引用内置 `plank`/`settings_bg` 却不写 width/height（DIM_UNDEF 按纹理原尺寸绘制），显示尺寸会从 1024x256/1024x512 变为 256x256/512x256；Background 元素以及内置/示例 cfg 都写了尺寸，不受影响；自带 plank.png/settings_bg.png 的主题也不受影响。settings_bg 颗粒略柔和。
- 未改：`HIRES_PASS_DIAG=1`、`src/fntsys.c`、`gfx/cover.png`、全部 cfg、gsKit。对比图在工作箱 `/workspace/bgsmall/compare.png`、`/workspace/plank/compare.png`。
- 验证：仅依赖 CI 编译检查，未实机验证；需实机确认背景/plank 观感及 hires 花屏情况。

### 2026-10-06 — 回退三项改动：Coverflow 底层绘制、main0 default、1080i 字体（用户决定）

- 原因：用户实机反馈 bc065c68 的 1080i 字体改动（去掉 2 倍纵向超采样）让文字发虚/变软，决定回退；同时回退 Coverflow 背景相关的两项改动，恢复原行为。直接恢复文件内容，未用 `git revert`，上面的历史记录保留。
- `src/themes.c`：删除 9f7f8307 在 `drawGameImage()` 中加的 `cfBaseLayer` 底层绘制代码，恢复原来的二选一：有 BG 美术画 BG（照旧叠 `mask=alphamask`），否则画默认图，再否则画 plasma。与 f952227f 完全一致（`git diff f952227f -- src/themes.c` 为空）。
- `misc/conf_theme_coverflow.cfg`：`main0` 恢复 `default=settings_bg`，第 18、20 行注释恢复为 9f6387ca 之前的原文（回退到 settings_bg）。与 59ec1f60 完全一致。
- `src/fntsys.c`：恢复 1080i（隔行 FRAME 模式）的 `hs *= 2` 纵向超采样，以及 `fntRenderGlyph()` 中 `oy/2`、`height/2` 的绘制端压缩。与 9f6387ca 完全一致（即撤销 RiptOPL #615 移植）。字体 atlas 显存恢复到原来水平。
- 保留不变：`gfx/cover.png`（8 位调色板 T8，66,560 B）、`gfx/settings_bg.png`（f34ba74e 中灰图，Coverflow 无 BG 美术时再次用它兜底）、`HIRES_PASS_DIAG=1`。
- 验证：仅依赖 CI 编译检查，需实机确认 1080i 字体清晰度与 Coverflow 背景兜底。

### 2026-10-06 — 降低 hires 显存压力：cover.png 改 8 位调色板；1080i 字体去掉 2 倍纵向超采样（对齐 RiptOPL #615）

- 背景：HIRES_PASS_DIAG 日志（1080i，9f7f8307 与 9f6387ca 两次启动）显示花屏来自 gsKit TexManager 在同一帧内驱逐本帧已绘制、只绑定一次的纹理（BG、plank、alphamask、settings_bg），hires 3-pass 重放时第 2/3 pass 读到新纹理。9f6387ca 中占位封面 `cover.png`（256x256 CT24）是头号“覆盖者”（29/76 次），字体 atlas 也占用大量显存。本次只降低显存压力，不改 gsKit 驱逐规则。
- `gfx/cover.png`：原 256x256 RGB（451 色，1,598 B）→ 256x256 8 位调色板 PNG（256 色，2,091 B，无 tRNS），Pillow MAXCOVERAGE 量化、不抖动（试过 Floyd–Steinberg，PSNR 反而更低）；与原图最大通道误差 3/255，PSNR 71.2 dB，肉眼无差别。加载器按 T8 + CT32 CLUT 载入，VRAM 由 262,144 B 降为 66,560 B（省 195,584 B）。CLUT alpha 为 0x80，封面压暗颜色 alpha 也固定 0x80，仍不透明；与现有 T8 游戏封面走同一路径。Coverflow `main3`/`appsMain3` 与列表主题 ItemCover 的 `default=cover` 都用这张图。
- `src/fntsys.c`：移植 RiptOPL 提交 `48ee173b`（PR #682，issue #615），逐行一致：删除 `fntUpdateAspectRatio()` 中隔行 FRAME 模式的 `hs *= 2`，以及 `fntRenderGlyph()` 中 `oy/2`、`height/2` 的绘制端压缩。原因：atlas 用 `GS_FILTER_NEAREST` 采样，2:1 纵向缩小只是丢掉一半字形行；FRAME 模式本来也只能显示一半行数，超采样没有收益。本仓库只有 1080i 是隔行 FRAME 模式（480i/576i 为 FIELD），所以只影响 1080i（列表主题同样受影响），屏上字形几何不变、可能略锐利；字形位图高度减半，每个 96x96 atlas 能放约两倍字形，字体显存约减半（估算）。RiptOPL 对 renderman.c 的注释改动针对本仓库没有的 480i/576i flicker-free 行，未移植。
- 未改：`HIRES_PASS_DIAG=1`、全部 cfg、9f7f8307 的 themes.c 代码、gsKit。
- 验证：仅依赖 CI 编译检查，未实机验证；需在 1080i 实机确认字体与占位封面观感。

### 2026-10-06 — Coverflow 主界面去掉 settings_bg 兜底，无 BG 美术时回退 plasma

- 用户要求：删除 `misc/conf_theme_coverflow.cfg` 中 `main0`（Background）的 `default=settings_bg`（原第 24 行），并同步改第 18、20 行注释（原写“回退到 settings_bg”）。其余 cfg 不动。
- 效果：`main0` 无默认图，`defaultTexture` 为 NULL，9f7f8307 加的 `cfBaseLayer` 恒为假，`drawGameImage()` 走原来的二选一：取到 BG 美术（且背景开关开启）就画 BG，并照旧叠加 `mask=alphamask`（遮罩加载与 default 无关，只在真正画出 BG 时叠加）；否则（无 BG、背景开关关、尚未加载、或无选中项）画 `guiDrawBGPlasma()`。
- 未改：9f7f8307 的 C 代码保留（对 Coverflow 已无作用，但若以后再配 default 仍按“底层+叠加”生效）；`gfx/settings_bg.png`（f34ba74e 中灰图）保留，列表主题 `conf_theme_OPL.cfg` 仍以它兜底；`HIRES_PASS_DIAG=1`、`coverflow_dim_covers=1`、详情页 `info0`、`texcache.c` 均不变。
- 注意：Coverflow 主界面不再加载 `settings_bg`（列表主题仍会用），VRAM 中不再有这 525,312 B；无 BG 时改为 plasma。快速翻页时 BG 与 plasma 会交替出现。
- 验证：仅依赖 CI 编译检查，未实机验证。

### 2026-10-06 — settings_bg 美术换回之前的 background.png（约 85KB 的中灰底图）

- 用户要求：“把settings_bg换回之前约85KB的background.png的美术资源”。用户在候选中选定 blob `f34ba74e`：即 `0fff90c6`（2025-07-09）引入、一直用到 `54cda759`（2026-09-28，被当时的 settings_bg 木纹黑底覆盖）为止的 `gfx/background.png`（88,600 B，中灰，平均 RGB 约 73）。
- 改动：仅用该 blob 原样覆盖 `gfx/settings_bg.png`（`git hash-object` = `f34ba74e…`），文件名/纹理 ID/cfg/Makefile 均不变。旧文件 `b9cc8513` 为 91,321 B（近黑木纹，28 色）。
- 格式：1024x512、8 位调色板（256 色）PNG，加载为 T8 + CT32 CLUT，通过 `texSizeValidate`。VRAM 占用不变：524,288 B 像素 + 1,024 B CLUT = 525,312 B（与旧图相同，只换外观）。
- 影响范围：列表主题 `conf_theme_OPL.cfg` 的 `main0` 同样 `default=settings_bg`，因此列表主题的兜底背景也会变成这张中灰图；Coverflow 按 9f7f8307 的逻辑仍每帧先画它作底层、有 BG 美术时叠加在上。其他（`HIRES_PASS_DIAG=1` 等）不变。
- 验证：仅依赖 CI 编译检查，未实机验证。

### 2026-10-06 — Coverflow 主界面背景改为“settings_bg 常驻底层 + BG 美术叠加”（不再二选一）

- 原因：CF 主界面 `main0`（Background，`pattern=BG`、`default=settings_bg`、`mask=alphamask`）原来是二选一：取到游戏 BG 美术就只画 BG（+遮罩），否则只画 `settings_bg`。快速翻页时 `settings_bg`（1024x512 T8，约 525 KB）与约 329 KB 的 BG 轮流进出 VRAM，配合 hires 3-pass 重放的同帧 VRAM 别名，是中间横带花屏的主要来源之一。
- 改动（用户已批准，仅此一项）：`src/themes.c` `drawGameImage()` 中，当元素为 Background、当前为 Coverflow 主题且配置了默认图时，每帧先画默认图 `settings_bg` 作底层；取到 BG 美术时再在同帧画 BG，并照旧仅在画出 BG 时叠加 `alphamask`；未取到 BG（或背景开关关闭）时只画 `settings_bg`，不再走默认图/plasma 分支。
- 为什么改 C 而不是只改 cfg：RiptOPL（`rebuild/main` 的 `misc/theme_coverflow.cfg`）是 `main0` Background（`pattern=BG`，无 default）+ `main1` StaticImage `incebtion` 全屏常驻；但它的静态层画在 BG 之上（半透明压暗），顺序与本次要求相反。本仓库 Background 元素只能是第一个（`themes.c` 解析处 “Background elem can only be the first one”，否则会自动插入一个 BG 背景到最前），`mask` 也只对 Background 生效，所以 cfg 无法表达“静态底层在下、BG 在上且带遮罩”，只能在 Background 绘制里加最小改动。
- 未改：`misc/conf_theme_coverflow.cfg`、列表主题 `conf_theme_OPL.cfg`（它同样是 `default=settings_bg` 的二选一，本次未动）、`coverflow_dim_covers=1`、封面默认图、`texcache.c` 的 `PrevCacheID_BG`、详情页 `info0`（无 default）、`HIRES_PASS_DIAG` 诊断（仍为 1）。
- 注意：有 BG 时 VRAM 工作集同时包含 `settings_bg` 和 BG（不再轮换）；外观上 BG 为不透明全屏图时与原来一致，BG 带透明区域时会透出 `settings_bg`（原来透出黑色清屏）。
- 验证：仅依赖 CI 编译检查，未实机验证。

### 2026-10-06 — hires 中间横条花屏诊断：pass 分界线 + 同帧显存别名日志（HIRES_PASS_DIAG，默认开启）

- 目的：用户反馈 720p/1080i 快速翻页时花屏总是屏幕中间一条横带（静止正常，1 张封面不花）。gsKit hires 每帧把同一个 draw queue（含 TexManager 队列内纹理上传）按 pass 重放 3 次；怀疑本帧早先绘制、当时未上传而直接用 VRAM 旧内容的纹理，被本帧稍后的新上传覆盖，导致第 2/3 个 pass 读到错误纹理（假设 1），或中间 pass 超时（假设 2）。本次只加诊断，不改任何渲染/缓存行为。
- 开关：`src/renderman.c` 顶部 `HIRES_PASS_DIAG`，本次默认 `1`。诊断结束后改为 `0`（或编译加 `-DHIRES_PASS_DIAG=0`）即完全移除，`rmTexBind()` 退化为直接调用 `gsKit_TexManager_bind`。
- 实验 1（只可视化，release/debug 都有）：`rmEndFrame()` hires 分支在 `gsKit_hires_sync` 之前调用 `hiresDiagDrawPassSeams()`，用 `gsKit_prim_sprite` 按帧缓冲坐标画洋红色 2 行高横线（分界线上一 pass 的最后一行 + 下一 pass 的第一行）。pass 高度按 gsKit `gsKit_hires_init_screen` 同一算法：passCount 限 2..4，CT32/CT24 按 32 行、CT16S 按 64 行对齐，`ceil(Height/passCount)` 向上对齐。720p 分界在第 256/512 行（屏幕高度约 35.6%/71.1%）；1080i（FRAME 模式 Height 减半为 540）分界在缓冲第 192/384 行，即实际画面第 384/768 行；480i/480p/576 hires（2 pass、CT24）各 1 条线。不加 overscan 偏移，不经过 CPU 裁剪；只临时关闭 `PrimAlphaEnable`（仅影响该图元 PRIM.ABE）并恢复，不写 ALPHA/TEST/SCISSOR 等寄存器。非 hires 不画。
- 实验 2（只日志，仅调试构建有输出，例如 CI `OPNPS2LD-DEBUG` 里的 `opl-iopcore_debug-*.elf` / `opl-ingame_debug-*.elf`，UDPTTY 用 ps2client 抓）：renderman.c 中全部 8 处 `gsKit_TexManager_bind` 改为 `rmTexBind()`（返回值、调用顺序不变）。hires 下每次绑定记录纹理指针、尺寸、PSM、`Vram`+本体大小、`VramClut`+CLUT 大小，以及 `gsKit_TexManager_bind` 返回值（非 0 表示本次在队列里插入了上传）。固定 128 项数组，满了停止记录并计 `dropped`，帧末清空。
  - `[HIRES_ALIAS] f=帧号 new#序号=指针 宽x高 psm vram=地址+字节 clut=地址+字节 over old#序号=...`：本帧一次上传的 VRAM 范围覆盖了本帧更早绑定、当时未上传（且之前未在同地址重新上传）的纹理。每帧最多输出 8 行，总数见 `alias=`。
  - `[HIRES_TEX] f=帧号 binds= uploads= bytes= alias= alias_logged= dropped=`：仅当本帧有上传、别名或溢出时输出。
- 判读：
  - 花屏横带的上沿/下沿与洋红线重合 → 与 pass 有关；只有线以下（第 2/3 个 pass）花、线以上同一封面正常 → 支持假设 1；花区边界跟着封面轮廓走、与线无关 → 更像 EE 内存生命周期问题（假设 3）。
  - 花屏那一帧附近出现 `[HIRES_ALIAS]` → 假设 1 基本确认，按 old/new 尺寸可判断被覆盖的是背景、遮罩、木板还是封面；只看到大量 `uploads/bytes` 而无 alias → 偏向假设 2（中间 pass 超时）。
  - 注意 UDP 日志本身耗时，可能略微改变时序；洋红线每帧多 2 个无纹理 sprite，影响可忽略。
- 验证：仅依赖 CI 编译检查，未实机验证。

### 2026-10-06 — 按用户要求重新开启 Coverflow 非中心压暗

- 内置主题 `misc/conf_theme_coverflow.cfg`：`coverflow_dim_covers` 改回 `1`（撤销 e990e8ff 的关闭）。
- 按用户要求恢复翻页时按 `centerFactor` 在 `COVERFLOW_DIM_RGB(0x66)`↔`0x80` 插值压暗。
- 仅改 CFG，源码路径不变。验证：推送并查 CI。

### 2026-10-06 — 关闭 Coverflow 翻页明暗插值（恒定亮度）

- 内置主题 `misc/conf_theme_coverflow.cfg`：`coverflow_dim_covers=0`（原为 1）。
- 效果：翻页动画中封面不再按 `centerFactor` 在 `COVERFLOW_DIM_RGB(0x66)`↔`0x80` 间插值压暗；恒为 `gDefaultCol` 全亮。引擎开关与插值代码保留，第三方主题仍可自行开启。
- 验证：提交后推送并查 CI。

### 2026-10-06 — 按用户要求回退到 92a5b972 源码树

- 新建提交使工作树与 92a5b972（stale ART 仅 worker texFree）一致；**不** force-push / 不改写历史。
- 一并回退的区间 f71f419..4fd3bbac（用户确认的排查结论）：
  - f71f419 hires PrimAlphaEnable 清屏修复、Phase-0 VRAM 减压、统一封面尺寸、封面数夹紧等试验。
  - **结论**：减压/mask/统一缩放均未消除花屏；**hi-res 下 1 张封面无花屏**（多封面仍花）。
- 本文件在 92a5b972 内容之上追加本条说明。

### 2026-10-05 — stale 丢弃改为 worker 仅 texFree，避免 IO 线程 rmUnloadTexture

- 核实：`cacheClearExpiredItem` 仅在 `cacheLoadImage1` 代际失效且 `sameEntry`（`trackGeneration` 的 CF COV/ICO/BG）时调用；入队前主线程已 `cacheClearItem`；`keepResult` 未发布则 qr 一直非 0；`cacheGetTexture`/`quiet` 在 `qr!=0` 时返回 Prev/NULL 而非当前槽（挑 Prev 槽前会清 Prev）。故该结果从未 bind/TexManager。
- 变更：worker 侧改为 `texFree` + 槽位复位（保留 UID 匹配且 `qr!=0` 即清的 7fc85f65 规则），不再 `cacheClearItem`/`rmUnloadTexture`。
- 验证：仅 CI。

### 2026-10-05 — 修补 ART 槽位 qr 泄漏源；收紧 BG D2 空闲判定

- 按用户批准的最小方案（不恢复未批准的 recover）：
  1. `cacheClearExpiredItem`：UID 匹配且 `qr!=0` 即清槽（不再要求 `qr==2`），消除 stale 丢弃后 qr 永久钉住。仍在 IO worker 上调用 `rmUnloadTexture`（与改前相同，未加重）。
  2. 请求取消逻辑保持不变。
  3. `submenuRebuildCache`（仅当已有 cache_id）、`submenuDestroy`、`submenuRemoveItem`：在释放 list 项指针之前 `cacheCancelPendingArtRequests()`；不在重建路径上直接改槽位 qr。
  4. BG D2 让步额外要求 `ioGetActiveRequestType() < 0`；不做 recover。
- 验证：仅 CI。

### 2026-10-05 — Coverflow BG 改回列表式 PrevCacheID_BG（含保护）并删除 CF 专用显示回退

- 按用户决定：删除 `cacheGetCoverflowBgDisplayFallback` / `cacheCoverflowBgNoteDisplayed` / `covBgDisp*` / `bgfb` 及 themes 中 CF 专用回退记账；CF 背景只走 `getCoverflowTexture`（主界面仍 defer 到预取 COV 后入队）。
- quiet BG 路径对齐列表 `PrevCacheID_BG`：ForceRefresh→记 -2 且不保持；当前 -2/缺图→记 -2；命中→记槽并返回；加载中或暂不入队→若 Prev≥0 返回该槽纹理（原始列表风格、不做额外 UID/Mem 校验）；选槽 `count>1` 时保护 `i != PrevCacheID_BG`，候选内仍 preferEmpty 再最旧 lastUsed；若占用的恰是 Prev 槽则先置 Prev=-2 再清空。
- 卡死让步仅 D2：保护下无候选、其它槽均 `qr!=0`、且 `texLoading==0` 且 `!ioHasPendingRequests()` 时，本轮允许占用 Prev 槽，并先 Prev=-2（显示回落默认）。无时间阈值。
- 保留：预取后入队、ForceRefresh 老化、`gEnableArtBG` 关闭→默认背景。
- 验证：仅 CI。实机重点：背景关固定默认；切换游戏保持上一张；A 有图→B 无图→C 加载中为默认。

### 2026-10-05 — Coverflow 主界面 BG 入队改到预取 COV 之后

- 按用户要求：仅调整 CF 主题背景图入队时机到「预取 COV 入队之后、紧接着入队」。
- 主界面（`GUI_SCREEN_MAIN`）：背景元素只显示/回退（`allowRequest=0`），`drawCoverFlow` 在预取 COV 循环之后、ICO 之前调用 `flushDeferredCoverflowBackground()` 入队 BG。
- 详情页等无 Coverflow 元素的画面：仍在背景绘制时立即入队，避免漏请求或打乱 SCR 等顺序。
- 不新增保护/重试/延迟规则；`5921aecf` 的显示回退与记录逻辑不变。
- 验证：仅 CI。实机可看 ART 日志：可见 COV → 预取 COV → BG → ICO。

### 2026-10-05 — Coverflow BG 显示回退改为“上一帧实际画面”

- 问题：`cacheGetCoverflowBgDisplayFallback` 按 `lastUsed` 最高的已加载槽回退；若上一款游戏无 BG（画的是默认），缓存里仍留着更早游戏的外部 BG，切到下一款加载中时会闪出那张旧图，而不是上一帧的默认底。
- 修复：Coverflow `drawGameImage` BG 路径记录上一帧实际显示——外部命中记 `slot+UID`，默认/plasma（含 `-2`、ForceRefresh、无有效槽）记默认标记；经回退路径画出时不更新记录。回退只在记录为有效 `slot+UID` 且槽仍 `UID` 匹配/`qr==0`/`texFound==1`/`Mem` 时返回，否则 NULL→默认。ForceRefresh 将记录重置为默认。
- 保留：当前项 `-2` 不走回退；回退不改 `lastUsed`；`bgfb` 仅在真正画出回退时计数；quiet BG 选槽偏好不变；无槽位保护/不拦请求。
- 验证：仅 CI。实机：A 有外部 BG → B 无 BG（应持续默认）→ C 加载中应仍为默认，不应闪 A。

### 2026-10-05 — 修复 Coverflow BG 显示回退：ForceRefresh 老化 + BG 选槽偏好

- 根因：`cacheGetCoverflowBgDisplayFallback()` 在 `ForceRefreshPrevTexCache!=0` 时直接返回 NULL；该标志在列表重建/切页签/进详情时置 1，本应由 `cacheGetTexture()` 加到 2 再由 `flushBatchRequests` 清 0。Coverflow 主界面只走 quiet 路径，标志永久停在 1，显示回退从未生效（`bgfb` 恒为 0），切换游戏时只能画主题默认 BG。
- 修复 1：`flushBatchRequests` 在标志仍为 1 时老化为 2，下一帧清 0。切换后仍有 1～2 帧不显示其它设备 BG；非 Coverflow 的 `PrevCacheID_*` 重置仍由 `cacheGetTexture` 在标志非 0 时完成，行为不变。
- 修复 2：quiet 选槽对 BG 优先挑不可显示槽（`texFound!=1` 或无 Mem），再按 `lastUsed` 最旧；若只剩上一张已显示 BG 仍可占用（偏好非禁令）。COV/ICO 选槽不变。
- 保留：缺图 `-2`→默认/plasma；显示回退不改 `lastUsed`；`bgfb` 计数。
- 验证：仅 CI；实机看切换游戏时上一张 BG 是否保持，以及 `[ART_DIAG] bgfb=` 在 BG 加载窗口内是否 >0。

### 2026-10-05 — 移除 Coverflow 缺图 2.5 秒定时重试（恢复上游行为）

- 删除 `COVERFLOW_MISSING_RETRY_MS`、`gCovMissingRetryAt`、`gCovRetryMissingThisFrame`、`gCovRetryEvalFrame`、`coverflowUpdateRetryWindow()`、`coverflowRetryMissing()` 及全部调用点（COV/ICO/BG）。
- 恢复上游行为：art 一旦记为缺图（`cache_id=-2`）不再自动重请求，直到游戏列表重建。详情页 BG 显示回退（`fe2ec6f2`）本就跳过 `-2`，不受影响。
- 历史：`31edcf56` 引入 → `d9395854` 回退 → `8e493c66` 未经用户授权再次加回。**此后未经用户明确批准不得再加回该规则。**
- 验证：仅 CI，未实机。

### 2026-10-05 — Coverflow 背景显示回退（不影响槽位选择）

- 在 `318f91e1` 简化后，当前游戏 BG 未就绪时会闪默认/plasma。按用户决定加回**仅显示**回退：`cacheGetCoverflowBgDisplayFallback()` 扫描 BG 缓存中 `qr==0 && texFound==1 && Mem` 的槽，取 `lastUsed` 最大者绘制；不改 `lastUsed`、不保护任何槽、不参与 quiet 选槽。
- 缺图（`cache_id==-2`）仍画默认/plasma，不借用其它游戏背景，避免张冠李戴；与上游列表主题行为一致。强制刷新页签时（`ForceRefreshPrevTexCache`）也不回退。
- `[ART_DIAG]` 增加 `bgfb=` 计数（每 120 帧窗口内显示回退命中次数）。`8839c837` 详情页按键屏蔽不变。
- 验证：仅 CI，未实机。

### 2026-10-05 — Coverflow 主题 BG 改为与 COV 相同的普通请求，移除 BG 专用规则

- 背景：`b49ef627` 加诊断后长时间实机测试未再复现“BG 永久不入队”，概率很低；按用户决定直接简化，去掉 CF 专用的 BG 特殊处理。
- 移除：
  - `fa80d1c7`：背景元素在 CF 下只走 `cacheGetTextureNoRequest()`、真正请求延后到 `drawCoverFlow()` 末尾的 `queueDeferredCoverflowBackground()`（排在 ICO 之后）；quiet 路径选槽时对 `PrevCacheID_BG` 的保护（`protectedId`）。
  - `f9951840` 及后续：`cacheGetBackgroundFallback()`（双槽 BG 回退）、`[ART_DIAG]` 的 `bgfb/bgmiss` 计数。
  - `b49ef627`：全部 BG 诊断（`[BG_EV]/[BG_SLOTS]/[BG_Q]/[BG_DQ]`、`cacheClearItem` 诊断宏）。通用 `[ART_DIAG]`/看门狗保留。
- 现在：CF 主题的背景元素在 `drawGameImage()` 中直接调用 `getCoverflowTexture(...,1)`，与 COV 走同一 quiet 路径（同样的代际/取消/缺图重试规则）；未加载完成或缺图时按原逻辑画主题默认背景或 plasma，不再显示上一张 BG。详情页背景也由背景元素直接请求，不再依赖 `drawCoverFlow()`。
- 缺图重试窗口改为 `coverflowUpdateRetryWindow()` 每帧计算一次（COV/ICO/BG 共用），避免详情页没有 Coverflow 元素时沿用旧标志、每帧重新请求缺失 BG。
- 保留：非 CF 主题的 `cacheGetTexture()`/`PrevCacheID_BG` 原逻辑、BG 2 槽（`59da56b0`）、低分辨率 BG 压缩（`5136b567`，对所有主题生效，非 CF 专用）、`8839c837` 详情页按键屏蔽。
- 验证：仅 CI 编译，未实机验证。

### 2026-10-05 — 主界面 Coverflow BG 停止入队诊断（仅调试构建）

- 背景：`647cf154` 实机日志中最后一次 BG 请求在 68652 行（SLPM_652.66）成功，之后约 2 万帧再无 BG 入队，COV/ICO 正常；`loading=0 queued=0`。推测为 2 个 BG 槽位中一个受 `PrevCacheID_BG` 保护、另一个 `qr` 泄漏（无请求持有却非 0），但现有日志无法证实。
- 新增（全部在 `#ifdef __DEBUG` 内，release 无行为变化；`cacheGetBackgroundFallback()` 仅把条件拆开以便计数，返回值不变）：
  - `[BG_EV]`：BG 槽位每次 `qr` 置位/清零事件，site 取值 `q_alloc/q_nomem/nq_alloc/cancel/cancel_skip/release/release_skip/wk_begin/wk_keep/wk_stale/wk_lost/exp_reserve/exp_skip/clear/pt_clear/destroy`，含 `seq/f/line/c/slot/uid/ref_uid/qr=旧->新/tf/gen/art_gen/item/tid`。事件在关中断下写入 256 项环形缓冲区，由主线程在 `flushBatchRequests()` 每帧用 LOG 输出，不限流；溢出时输出 `[BG_EV] dropped=`。`cacheClearItem` 在调试构建中改为记录调用行号的宏。
  - `[BG_SLOTS]`：每 120 帧（紧跟 `[ART_DIAG]`）输出每个 BG cache 的 `prev`(PrevCacheID_BG)、`force`、`bgforce`(force=1 时 fallback 被跳过次数)、`loading`、`art_gen`，以及每槽 `qr/uid/tf/lu/gen/mem`。
  - `[BG_Q]`：quiet 路径 BG 未入队原因 `missing/loading/found_missing/no_request/no_slot`（no_slot 附槽位快照），同原因同 item 120 帧限流，`supp=` 为抑制次数。
  - `[BG_DQ]`：`queueDeferredCoverflowBackground()` 的 `frame_mismatch/null_ptr`（120 帧限流）与 `call`（item 变化时输出 cid/uid）。
- 判读：`[BG_SLOTS]` 中 `loading=0` 且某槽 `qr!=0` 即为泄漏，按 uid 回查该槽最后一条 `[BG_EV]` 的 site（例如 `q_alloc` 后无 `wk_begin/cancel` 表示请求丢失，`wk_stale` 后无 `exp_reserve/clear` 表示丢弃路径未清）。若无泄漏而 `[BG_Q] reason=no_slot` 持续，则看 `prev` 与各槽 `lu` 判断是保护槽还是本帧已用。
- 验证：仅依赖 CI 编译检查，未实机验证。

### 2026-10-05 — Coverflow 主题详情页禁用翻页与跳首/末项，修复输入锁死

- `647cf154` 实机（新镜像、已修复 SDK）：在详情页按 L1/R1 翻页一次后所有按键无响应，但 `[ART_DIAG]` 仍每 120 帧打印，主循环未卡死。原因：Coverflow 主题下 `menuPrevPage()/menuNextPage()` 会触发 `thmTriggerCoverflowAnimMulti()` 并置 `gCoverflowPageScrollActive=1`，`menuHandleInputInfo()` 每帧先经 `menuTickCoverflowScroll()` 等待动画结束；而 `isAnimating` 只在 `drawCoverFlow()` 中清零，详情页没有 Coverflow 元素，动画标志永远不清，输入（包括返回键）被永久屏蔽。
- 修复：`src/menusys.c` 的 `menuHandleInputInfo()` 在 `gTheme->coverflow` 非空时不再响应 L1/R1（`menuPrevPage/menuNextPage`）与 L2/R2（`menuFirstPage/menuLastPage`）；确认/返回（Cross/Circle）、左右切换游戏等其它详情页按键不变。非 Coverflow 主题保留原有详情页翻页与跳首/末项。
- 未改动画超时、BG 加载逻辑或诊断输出。主界面 BG 停止加载的问题仍待进一步诊断。

### 2026-10-05 — 修正看门狗输出并跟踪 worker 阶段与 texLoadingMutex（仅调试构建）

- `ea621fe7` 实机结果（旧镜像、未修复 SDK）：最后一个请求 `ICO SLAJ_250.30` 已打印 `texend_*` 与 `[ART_REQ_END] result=0`，随后 `[ART_DIAG]` 显示 `done` 已计数、`active_req=0`，之后 `start=0 done=0` 永久停滞、`active=3`。即本次 worker 停在 `diagArtCompleted++` 之后、`ioProcessRequest()` 返回之前（看门狗依据的进度计数未再递增，主线程 `ioGetActiveRequestType()` 仍能取得队列锁）。上一次（23:22 日志）`active_req=1` 且 `done` 少 1，停在 `texEnd()` 内。两次停点不同，但下一步都是 newlib `free()`（这次是 `free(ioReq)` 或 stale 路径的 `cacheClearItem()`），与 malloc 递归锁竞态假设一致；主线程期间仍能反复获取 `texLoadingMutex`，不支持该互斥锁被永久持有。
- 看门狗每行只收到 8 字节 `[ART_WD]`：`ioDiagPrintfNoLock()` 的静态缓冲区在旧构建中地址 `%16 == 8`，`fioWrite()` 把未对齐的头部 `16 - addr%16 = 8` 字节放进 RPC 参数单独发送，其余部分丢失（每次报告 9 行，每行恰为 8 字节，与之吻合）。现改为 `ALIGNED(64)` 缓冲区，并对短写循环续写。
- 新增 `gIODiagWorkerStage`：ioman worker 循环（`queue_wait/queue_got/idle_sleep/dispatch/returned/finish_wait/finish_done`）、worker 内 `ioPrintf`（`printf_sema_wait/printf_write`，并记录调用前阶段 `printf_caller_stage`）、`cacheLoadImage1()` 结束阶段（`art_end_log/art_compact/art_counters/art_mutex_wait/art_mutex_held/art_mutex_unlocked/art_stale_clear/art_dec_loading/art_free_req/art_free_req_done/art_early_exit`）。看门狗输出 `[ART_WD] worker_stage=`。
- 所有 `texLoadingMutex` 加/解锁统一改用 `TEX_LOADING_LOCK()/TEX_LOADING_UNLOCK()`：release 下直接展开为原 `pthread_mutex_lock/unlock`；调试下额外记录持有线程、加锁行号、worker/其它线程正在等待的行号、最近解锁线程与行号。看门狗输出 `[ART_WD] tex_mutex ...`，并按 pthread-embedded `struct pthread_mutex_t_` 布局输出 `[ART_WD] tex_mutex_pte ...`（内部信号量状态）。行号对应本提交的 `src/texcache.c`。
- 验证限制：box 构建环境本次不可用，未能用旧镜像 chroot 编译；改动已人工审阅，未实机验证。

### 2026-10-05 — ART worker newlib 递归锁停滞诊断（仅调试构建）

- 重新分析 `opl-coverflow.log`：停滞请求 `SLPS_254.76 COV` 已完整打印 `read_end`/`close`/`png_done`/`success`，之后再无 `[ART_REQ_BEGIN]` 或 `file_lock_wait`，而 `active_req=1` 永久保持（`diagActiveArt` 只在 `itemGetImage()` 返回后清零）。`success` 之后只剩 `texEnd()` 中的 `free()`/`png_destroy_read_struct()`，此时 ATA 文件已关闭，因此“卡在 ATA 读”的假设被明显削弱。
- 新的首要假设：个人 ps2sdk fork 的 `ee/libcglue/src/lock.c` 缺少上游 `c9a9e2fc`（`__retarget_lock_acquire_recursive` 竞态）。IO worker 优先级 32，主线程 31（更高）；worker 在 `count++` 与 `WaitSema` 之间被主线程抢占后会永久阻塞在 malloc 锁，而主线程走“递归”分支继续正常 malloc，与日志中主线程仍能重载主题、解码 PNG 完全一致。ps2sdk `master` 已 cherry-pick 上游 `93206ac9`、`c9a9e2fc`（本仓库之外，需重建镜像链后再构建 OPL）。
- 本次只增加 `__DEBUG` 诊断，不改变 IO 行为：
  - `texEnd()` 增加 `[ART_LOAD] stage=texend_free_file / texend_png_destroy / texend_done`；`cacheLoadImage1()` 在 `itemGetImage()` 返回后打印 `[ART_REQ_END] ... result=`。
  - `flushBatchRequests()` 每帧调用 `cacheDiagWatchdog()`：IO worker 有活动请求且进度计数约 300 帧（≈5 秒）不变时输出 `[ART_WD]` 快照，之后每 300 帧重复，恢复后输出 `[ART_WD] recovered`。快照包含 worker/主线程 `ReferThreadStatus`、`__lock___malloc_recursive_mutex`、`__lock___sfp_recursive_mutex`、stdout FILE 锁的 `sem_id/owner_thread/count` 及 `ReferSemaStatus`，以及 IO 队列、ioPrintf、文件锁信号量。
  - 看门狗输出走新的 `ioDiagPrintfNoLock()`：静态缓冲区 + `vsnprintf` + `write(STDOUT_FILENO)`（EESIO 构建用 `sio_putsn`），不经过 ioPrintf 信号量、stdout FILE 锁或 malloc 锁。
- 判读：若 `io_thread status=0x04 wait_type=2(sema)` 且 `wait_id` 等于 `lock name=malloc` 的 `sem_id`，同时该锁 `sema_count=0 wait_threads=1`、`count>=1`，即确认 malloc 递归锁停滞；`last_stage` 应为 `texend_*`。
- 验证：已用旧镜像 `ghcr.io/362053534/ps2homebrew:main`（2026-09-27 构建）在本地 chroot 中完成 `make iopcore_debug` 编译链接，`DEBUG=1 EESIO_DEBUG=1` 和 release 下这三个源文件也能编译；尚未实机验证。

### 2026-10-04 — ART/mass1 ATA 永久停滞诊断交接

- 用户提供了包含提交 `8437435` 的 `iopcore_debug` ELF 的完整目标机日志；本次日志已读到最后的 `IOP cmd: RESET`、卸载和 `freepad: DMA Busy`。
- 本次永久停止从约 frame 2997–3117 形成：`active=3` 长期不变，`start=0`、`done=0`；队列中的 ART 请求持续堆积。整个停滞期间 `blocked=0`、`terminating=0`、`qalloc=0`、`qput=0`、`qerr=0`，因此不能归因于 `ioBlockOps()`、`gIOTerminate` 或请求池耗尽，也不能把本次套用成上一阶段的 `qerr=-5` 问题。
- `active_req=1/COV/SLPS_254.76/128` 长期保持，说明诊断视角中的当前 COV 请求没有退出；但现有 `[ART_LOAD]` 使用共享诊断状态且多线程/多路径输出交错，不能仅凭它确认具体卡在该文件的 open、lseek、分配、read 还是 PNG 阶段。
- 设备初始化时存在明确的实际 ATA 异常：`status=0x51 error=0x04`，`set_transfer type=0x40 mode=7 result=-503`，随后 ATA 设备仍以 `UDMA0` 注册为 `mass1`。这必须继续调查，但目前不能把它单独认定为 frame 2997 的直接根因，因为此前有大量 `mass1` read/PNG/success 完成记录。
- 日志完全没有 `[ATA_DIAG]`。已核实当前 Coverflow 的 `mass1:` 路径加载的是 `$(PS2SDK)/iop/irx/ata_bd.irx`，而不是本仓库 `modules/iopcore/cdvdman/atad.c`；后者的诊断代码不会覆盖本次真实运行的 ATA 模块。不得继续在错误的本地 `atad.c` 上追加同类日志并声称已覆盖 BDM ATA。
- 下一步必须在实际 PS2SDK `ata_bd.irx` 构建来源中增加 read/command 边界诊断：逻辑或物理 LBA、扇区数、命令开始/返回、超时、重试、ATA status/error、DMA/中断状态和最终错误码；同时把 EE 侧 ART 日志改为请求 ID + 请求本地路径，消除共享 `diagTexturePath`/active 字段造成的串线。若 `ata_bd.irx` 源码不在本仓库，应在实际 PS2SDK 依赖/构建链中修改或接入明确的 debug 版本，不再修改无关的 CDVDMAN ATAD 路径。
- 本地环境仍缺少 `/samples/Makefile.iopglobal` 及完整 PS2SDK/GSKIT 交叉工具链，不能宣称本地目标构建通过；目标机验证仍待接入正确的 `ata_bd.irx` 诊断后进行。
- 本次更新同时把“每次提交必须同步更新 `HANDOFF.md`”以及 `AGENTS.md` 规则文件的安全提交例外写入 `AGENTS.md`。按现有安全约束，`AGENTS.md` 提交必须单独暂存，不能把 `HANDOFF.md` 或源码/资源混入。

### 2026-10-01

- 新增本交接文档。
- 当前最新代码提交：`791c279 fix: enable colors for built-in list theme`。
- 修复两个内置主题共享颜色配置时，非 CF 内置主题颜色控件灰显且无法保存的问题。
- ICO 当前逻辑尺寸为 `128×138`（448 下约 `128×128`），弹出距离为 `13` 像素。
- 当前内置 Coverflow CFG 使用 `main3 y=260`、`appsMain3 y=310`；代码公共基准为 `169`，APPS 专用偏移为 `-50`。
- 对比 `origin/362053534-patch-1` 时发现 `cacheLoadImage1()` 的全局冷却移除会改变普通列表的光标切换行为；已改为仅 Coverflow quiet 请求绕过 `cdFramesCount`，普通请求恢复 worker 侧旧请求丢弃保护。
- 修复 Coverflow overscan 偏移重复应用：`rmBuildCoverTransform()` 保持未加 render offset 的坐标，Case、封面和倒影各只在提交阶段加一次偏移。
- Coverflow 垂直动画曾改为围绕反推的 `verticalScalePivotY` 缩放并取消独立的 `centerFactor`→Y 位移曲线；该方案已在 2026-10-02 改为中心封面底边锚点与中心线对齐方案。
- 定位 NTSC 448i 大背景卡顿：`640×480` CT24 BG 约占 `1.23 MiB`，与 plank/case/Coverflow 封面共同触发 gsKit VRAM 驱逐和重复上传；不是单纯的 BG 删除竞态。
- 背景 cache 统一使用 2 槽，不再区分普通列表、Coverflow 或 GS 原生分辨率；两槽 fallback 逻辑保持不变。背景仍仅在 CT24 + 双缓冲路径中将大型无 alpha CT24 压缩为带抖动的 CT16S，RGBA/CT32 背景保持原格式，不改 framebuffer 或双缓冲。
- 静态审计确认默认 IO 路径不会复用 `qr` 活动槽，也没有发现渲染线程在 `gsKit_finish()` 前释放纹理的直接路径；BG 压缩条件由入队请求携带，避免 worker 在模式切换时读取 `gsGlobal`。
- 修复 CI 交叉编译发现的类型错误：`GSTEXTURE.Mem` 是 `u32 *`，CT16S 压缩临时缓冲为 `u16 *`；提交时使用显式 `(u32 *)` 转换，保持底层 16-bit 缓冲布局不变。
- 当前环境无法运行 PS2SDK/GSKIT 交叉构建或实机验证；CI 已实际编译到 `src/textures.c`，此前唯一阻塞错误为上述指针类型不匹配。
- 实机验证显示 Coverflow 背景 cache 使用 3 槽也不死机；按后续请求取消主题分支，所有主题 Background cache 统一调整为 2 槽。
- 低分辨率整数绘制链实测观感更不和谐，已撤回 `58e059b` 的整数 Coverflow 绘制改动，恢复原有浮点 Coverflow 绘制旁路。
- 后续低分辨率实测显示单槽仍会死机，已撤回按 Coverflow/分辨率区分单槽的尝试；背景 cache 恢复所有主题和所有分辨率统一使用 2 槽，根因不再归因于 cache 槽位数。

### 2026-10-02

- Coverflow 垂直对齐改为以中心封面底边为模块锚点，中心封面目标尺寸确定统一的水平中心线。
- 删除游戏/APPS 非中心封面的独立 Y 偏移和 `verticalScalePivotY` 反推；动画中每张封面按自身当前高度围绕中心线定位，使非中心与中心封面中点对齐。
- APPS Coverflow 的非 CFG 基线偏移从 `-50` 调整为 `-60`，使 APPS 封面模块整体上移 10 像素；游戏 Coverflow 和 CFG 坐标不变。
- Coverflow 支持第三方主题的 `coverflow_count`，统一规范化为 1/3/5/7/9；3 张保持 5 张基线非中心封面大小，满屏反算后每侧间距扣除 60 像素，7/9 张沿用 5 张间距反推非中心封面大小以覆盖整排逻辑屏幕宽度。
- 已撤回 Overlay 内框偏移补偿和独立安全边界宏；3 张仅通过间距数值调整，不改变动画位置补偿逻辑。
- 背景图 cache 恢复为所有主题、所有 GS 分辨率统一使用双槽；低分辨率实测单槽仍会死机，因此不再把 cache 槽位数作为根因处理。

### 2026-10-03

- 修复 `src/textures.c` 的 PNG 解码失败清理路径：`texReadData()` 现在检查纹理缓冲、行指针、行缓冲和 libpng 解码失败，并在失败时返回错误；`texLoadAll()` 统一释放 `GSTEXTURE.Mem/Clut`、PNG 行缓冲、压缩文件缓冲和 `png_texture_t`。
- 修复索引 PNG 的 CLUT 分配空指针、非法调色板数量和 `pngTexture` 分配失败路径，避免 `memset(NULL, ...)` 与相关泄露。
- 为外部 PNG 增加有界内存读取；截断/损坏文件不再由自定义 libpng read callback 越界读取，而是进入统一的 longjmp 清理路径。
- 当前修复只处理解码失败/分配失败的资源生命周期，不改变 Background cache 槽位、Coverflow 预取、CT24→CT16S 压缩策略或正常成功加载路径。
- 当前环境仍缺少 PS2SDK/GSKIT 交叉编译器、PS2 实机和模拟器，已执行静态差异检查和 `git diff --check`，未完成目标平台构建与实机验证。
- 字体缓存 pending/条件刷新实验曾在本诊断阶段暂时撤销；本次已恢复 `4f55b526` 引入的策略，不新增每帧字体 atlas 刷新。

### 2026-10-04

- 为 Coverflow 的 active COV/ICO/BG 请求统一增加目标代际：光标变化后，仍在当前 Coverflow 可见窗口或预取范围内的请求会被重新标记为当前代；不在这些范围内的请求即使完成解码，也会释放结果并保持 cache 槽位无效，不再进入后续 GS bind。普通列表请求仍保持原有发布规则。该检查不尝试中断正在执行的 SMB/PNG 读取，也不额外保护动画过渡路径。
- 恢复 Coverflow 字体 atlas 的 pending/条件刷新策略：只有缓存耗尽下一条标题的保留余量时才在导航阶段刷新，不在每帧或每个字符串之间 flush。
- Coverflow 背景改为只在背景绘制阶段查询已有纹理，真正的 BG 请求延后到同一帧 ICO 请求之后提交。ICO 关闭或已知不存在时不等待 ICO 状态，BG 仍会正常入队。
- 2026-10-04 BDM 快速翻页排查：默认 `usePthread=0` 的单一 IO worker、活动请求不可由 `ioRemoveRequestsWithCleanup()` 中途取消，以及 BDM `open/read` 异常时可能长期占住 `fileLockId`，均已确认是必须分别处理的生命周期边界；本轮没有用无条件清零 `texLoading` 掩盖它们。
- 本轮加固 `texcache`：active stale 槽位在释放前使用 UID+`qr` 所有权再次确认；普通请求的早退路径也只释放自己拥有的槽位。Coverflow 动画扩展出来的过渡槽位不再刷新 active 代际，只有目标可见窗口和预取范围会续期 COV/ICO/BG 请求。COV/ICO/BG 对“缺图”和暂时性 BDM 读取失败共用的错误码增加低频重试，避免一次设备抖动把缓存永久钉成占位图。
- BDM 侧减少不必要的目录句柄压力：ART2 分桶探测和 `sbReadList()` 的 CD/DVD 存在性检查改为 `stat()`；`bdmUpdateGameList()` 删除重复的 CD/DVD `dopen/dclose` 探测，仅保留 MX4SIO 必需的一次根目录读取。
- 主题切换不再直接销毁可能仍被 worker 使用的旧 cache：旧 theme 会先进入 deferred-retired 列表，只有 `ioHasPendingRequests()==0` 后才回收；因此切主题不会因等待一个异常 BDM `open/read` 而再次锁死主界面，也不会让旧结果写入已释放 cache。JPG 文件发现也纳入 `fileLockId`，避免主题构建与 IO worker 并发访问 BDM/SMB 文件。
- 静态审计仍确认：`texLoading` 正常入队、队列清理和 worker 出口配平；本轮未启用看门狗或强制置零。已核实外部修复已经进入个人仓库目标分支：`362053534/FatFs-PS2OPL` 的 `iop-r0.15` 为 `33bf38c9b4bc`（`FF_FS_LOCK=0` 时 `f_close/f_closedir` 出错也清除 `obj.fs`），`362053534/ps2sdk` 的 `master` 为 `e3049eb40a52`（BDM `fs_close/fs_dclose`）并包含 `52421dd2ac2f`（SMB `smb_close`）。实际构建链也已核对：ps2sdk 的 `download_dependencies.sh` 固定从该 FatFs fork 的 `iop-r0.15` 拉取，`iop/fs/bdmfs_fatfs/Makefile` 以 `source/include` 为头文件目录并把 `ff.o` 等 FatFs 对象编入 IRX。因此本 OPL 分支不再保留三份临时 patch 文件；重新构建 SDK/FatFs 生成的 IRX 才会携带这些修复。本提交仍只负责 EE 侧目录探测压力、cache 所有权和 UAF 防护。
- 当前环境仍无 PS2SDK/GSKIT 交叉编译器、实机或模拟器，因此本轮完成 `git diff --check` 和静态控制流检查，未宣称目标平台构建或实机验证。

### 2026-10-07 — 撤回刷新旧 ART 修复

- 用户实测 `cd5f4fa` 对刷新后闪回上一张 BG 的问题没有效果，已按要求撤回。
- 当前暂不提出新的代码修复；待先确认 `7e2debc9deae5db7c939b86f65783593fc054e03` 引入的回归根因后再处理。
- 本次仅撤回上一修复及其说明，不改变其它 Coverflow、普通列表或诊断提交。

### 2026-10-07 — 将 ForceRefresh 推进限制在实际取图路径

- 按确认方案撤销 `flushBatchRequests()` 中对 `ForceRefreshPrevTexCache == 1` 的公共自动推进，避免普通列表在没有调用 `cacheGetTexture()` 的刷新帧提前清掉保护状态。
- Coverflow 的 `cacheGetTextureQuietInternal()` 仅在 BG 路径检测到 Force 时清除 `PrevCacheID_BG`，并幂等地将 Force 设为 2；帧末沿用原有 `>1` 清零逻辑。使用赋值而非递增，避免同一帧背景绘制和 deferred enqueue 重复推进。
- 本次未改 COV/ICO、普通列表 cache 选择、纹理加载或 fallback 规则。
- 验证：`git diff --check` 通过；当前环境没有 PS2SDK/GSKIT 交叉编译工具链，尚未完成目标平台编译和实机验证。
- 下一步：实机验证普通列表刷新、设备页签切换、Coverflow BG 刷新及详情页切换，确认 Force 状态和上一张 BG fallback 均按预期清理。

### 2026-10-08 — 按分辨率选择 Coverflow 高度宏

- 448i/p、512i/p（包括 Auto 对应的 PAL/NTSC 默认模式）使用低分辨率高度：ICO `128`、游戏 COV `200`、APPS COV `140`。
- 其它分辨率继续使用原高度宏：ICO `138`、游戏 COV `214`、APPS COV `150`。
- 仅替换 `drawCoverFlow()` 选择的高度输入，不改其它布局和尺寸计算。
- 验证：`git diff --check` 通过；当前环境没有 PS2SDK/GSKIT 交叉编译工具链，尚未完成目标平台编译和实机验证。
