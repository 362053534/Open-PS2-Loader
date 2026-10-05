# Open-PS2-Loader 工作交接文档

> 本文是后续开发的交接入口。之后每次修改代码、资源、CFG 或构建配置并提交时，必须在同一个提交中同步更新本文的“变更记录”和“当前状态”。

## 1. 仓库与分支

- 仓库：`362053534/Open-PS2-Loader`
- 工作目录：`/home/user/Open-PS2-Loader`
- 固定工作分支：`arena/01a0ba0f-open-ps2-loader`
- 不要切换、创建或推送其它分支。
- 当前远程：`origin/arena/01a0ba0f-open-ps2-loader`
- 本文创建时最新提交：`791c279 fix: enable colors for built-in list theme`
- 当前诊断基线提交：`8437435 debug: trace ART and ATA read stalls`；其后新增 newlib 锁停滞看门狗诊断（见 2026-10-05 变更记录）
- 当前任务：只完善 ART/mass1 永久停止加载的根因诊断，不改变 IO 行为、不通过更换后端绕过问题。
- 当前工作区：本次交接更新涉及 `HANDOFF.md` 和 `AGENTS.md`；未修改源码、图片或 `temp/` 用户资产。

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
#define COVERFLOW_APPS_YOFFSET -60
#define COVERFLOW_APPS_CENTER_YOFFSET 1
```

这些值的意图：

- CFG 以外的公共基准相对之前下移 `8` 像素；
- 内置 CFG 的游戏/APPS Coverflow 元素各上移 `8` 像素进行抵消；
- 因此内置主题的最终位置保持原来的视觉位置；
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
- `src/gui.c`：UI 颜色配置对话框及内置主题颜色可编辑逻辑。
- `src/menusys.c`：Coverflow 导航、单步移动、L1/R1 翻页动画和当前 item 变化检测。
- `src/textures.c`、`include/textures.h`：大型无 alpha 背景的 CT24→CT16S 压缩，降低低分辨率纹理池争用。

### 低分辨率大背景卡顿的根因与处理

- 测试图 `temp/SLPM_552.82_BG.png` 是 `640×480` RGB PNG。CT24 在 gsKit 中按约 `1,228,800` bytes 占用 VRAM。
- NTSC 448i 普通双缓冲的纹理池约为 `1,900,544` bytes；再扣除 plank、case 和当前 Coverflow 封面后，背景会迫使 gsKit TexManager 在每帧反复驱逐/重新上传纹理。这是 VRAM 工作集抖动，不是 PNG worker 与 BG 擦除之间的直接竞态。
- Background cache 统一保持两个槽位，不区分普通列表、Coverflow 或 GS 分辨率；切换游戏时保留上一张完整背景作为 fallback。
- 仅在当前显示为 CT24 且启用双缓冲的低分辨率路径中，将加载成功的大型 RGB/CT24 背景在进入 cache 前转换为带抖动的 CT16S；RGBA/带 alpha 的 CT32 背景不转换，保持 alpha 语义。framebuffer、双缓冲、坐标和普通主题绘制路径不变。
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
- 不要对 CT32/RGBA 背景强制使用 CT16S；当前压缩只针对无 alpha 的 CT24 背景。
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
- ICO 当前尺寸为 `128×128`，弹出距离为 `13` 像素。
- 当前内置 Coverflow CFG 使用 `main3 y=262`、`appsMain3 y=309`；代码公共基准为 `169`，APPS 专用偏移为 `-50`。
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
