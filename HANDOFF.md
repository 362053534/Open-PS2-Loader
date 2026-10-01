# Open-PS2-Loader 工作交接文档

> 本文是后续开发的交接入口。之后每次修改代码、资源、CFG 或构建配置并提交时，必须在同一个提交中同步更新本文的“变更记录”和“当前状态”。

## 1. 仓库与分支

- 仓库：`362053534/Open-PS2-Loader`
- 工作目录：`/home/user/Open-PS2-Loader`
- 固定工作分支：`arena/01a0ba0f-open-ps2-loader`
- 不要切换、创建或推送其它分支。
- 当前远程：`origin/arena/01a0ba0f-open-ps2-loader`
- 本文创建时最新提交：`791c279 fix: enable colors for built-in list theme`
- 当前工作区：干净。

## 2. 当前任务重点

当前主要维护 Coverflow 主题、ICO 光碟图显示，以及两个内置主题的颜色设置。

### Coverflow ICO 当前参数

参数位于 `src/themes.c`：

```c
#define COVERFLOW_ICO_SIZE 128
#define COVERFLOW_ICO_POPUP_GAP 13
```

当前行为：

- ICO 只在 Coverflow 绘制路径中显示。
- ICO 使用独立的 ICO cache，不挤占 Coverflow 封面 cache。
- ICO 固定尺寸为 `128×128`。
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
    y=262

appsMain3:
    y=309
```

### 代码基准

文件：`src/themes.c`

```c
#define COVERFLOW_BASELINE_YOFFSET 169
#define COVERFLOW_APPS_YOFFSET -50
#define COVERFLOW_NONCENTER_YOFFSET -37
#define COVERFLOW_APPS_NONCENTER_YOFFSET -34
```

这些值的意图：

- CFG 以外的公共基准相对之前下移 `8` 像素；
- 内置 CFG 的游戏/APPS Coverflow 元素各上移 `8` 像素进行抵消；
- 因此内置主题的最终位置保持原来的视觉位置；
- 外部 Coverflow 主题如果没有相同的 CFG 补偿，会使用新的代码基准。

APPS 的 `COVERFLOW_APPS_YOFFSET` 保持 `-50`，游戏和 APPS 的共同下移由 `COVERFLOW_BASELINE_YOFFSET` 负责。

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
- `src/gui.c`：UI 颜色配置对话框及内置主题颜色可编辑逻辑。
- `src/menusys.c`：Coverflow 导航、单步移动、L1/R1 翻页动画和当前 item 变化检测。

### 非 Coverflow 内置列表 ART 对比结论

- 普通背景、封面和其它 ART 仍走 `drawGameImage()` → `getGameImageTexture()` → `cacheGetTexture()`；Coverflow 的 quiet cache 入口没有替换这条路径。
- 普通列表不启用 Coverflow 的 hires 图元裁切、倒影和封面预取；普通 `rmDrawPixmap()`/overlay 仍走原有提交路径。
- `cacheLoadImage1()` 的请求冷却保护必须只对普通请求生效。普通请求的 `quiet` 标志为 0，光标切换后仍可丢弃已经从 IO 队列取出的旧请求；Coverflow quiet 请求的 `quiet` 标志为 1，绕过 `cdFramesCount`，避免被普通列表的单封面冷却机制误停载。
- 当前分支内置列表主题的默认背景资源为 `settings_bg`，普通主题继续使用 `Index_*`、`case` 和 `apps_case` 资源；这些资源未改为 Coverflow 专用资源。

## 7. 不要恢复的内容

- 不要恢复 `COVERFLOW_DIAG_BDM_INDEX_ALWAYS` 或其它 BdmIndex 常显诊断逻辑。
- 不要恢复已删除的纯封面诊断函数。
- 不要覆盖普通非 CF 的 `gfx/Index_0.png`～`gfx/Index_4.png`。
- WOPL/Coverflow 专用 BdmIndex 资源必须使用独立文件名。
- 不要把第三方 Coverflow 的 `coverflow_animation_speed` 忽略掉。
- 不要把游戏和 APPS 的非中心垂直偏移合并成一个不可区分的值。
- 不要用 `texLoading > 0` 隐藏或重置当前未变化游戏的 ICO。
- 未完成完整构建和 PS2 实机验证前，不要继续提交未经验证的 hires 裁切方案。
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

### 2026-10-01

- 新增本交接文档。
- 当前最新代码提交：`791c279 fix: enable colors for built-in list theme`。
- 修复两个内置主题共享颜色配置时，非 CF 内置主题颜色控件灰显且无法保存的问题。
- ICO 当前尺寸为 `128×128`，弹出距离为 `13` 像素。
- 当前内置 Coverflow CFG 使用 `main3 y=262`、`appsMain3 y=309`；代码公共基准为 `169`，APPS 专用偏移为 `-50`。
- 对比 `origin/362053534-patch-1` 时发现 `cacheLoadImage1()` 的全局冷却移除会改变普通列表的光标切换行为；已改为仅 Coverflow quiet 请求绕过 `cdFramesCount`，普通请求恢复 worker 侧旧请求丢弃保护。
- 当前环境无法运行 PS2SDK/GSKIT 交叉构建或实机验证；本次仅完成静态代码对比和 `git diff --check`。
