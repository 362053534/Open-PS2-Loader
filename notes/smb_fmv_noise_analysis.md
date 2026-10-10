# SMB 模式 FMV 音频杂音排查记录（v2，基线已修正）

针对提交 `6254970`（改进SMB：拔线模拟开关仓，断线后无限重连）。
现象：**部分游戏**播放 FMV 时音频出现明显杂音；回退该提交（即回到其父提交状态）后杂音消失；该提交功能重要，需要保留。

> **基线修正说明**
> v1 曾误用上游官方 OPL（`626e5e46`）做树差异匹配——因为当时本地仓库里 `smb-speeddown-test`
> 只剩被压扁的单条根提交，看不到真实历史。现已通过 `git fetch origin edf39ddc…` 取回真实的
> 父提交。正确基线为 **`edf39ddc` → `6254970`**（用户实机验证：`edf39ddc` 状态无杂音）。
> 该提交的精确改动面只有 **7 个文件，+291/-52 行**。v1 中关于 u32 LSN、cdvdman 越界读、
> GETTOC、ioops、ee_core IGR/风扇、atad/zso/mcemu 等的指控**全部撤销**——那些改动早于
> `edf39ddc` 即已存在，与本回归无关。

---

## 1. 精确改动清单（edf39ddc → 6254970）

| # | 改动 | 位置 | 稳定运行时的作用 |
|---|------|------|----------|
| ① | 新线程每 ≈30 秒发送一次 `SMB_COM_ECHO` 保活包；Echo 失败(`<0`)立即判 `smbConnectionState=2`，触发全量重连 | `device-smb.c`、`smb.c`、`smb.h` | 每次 Echo 持有 `smb_io_sema` 一个完整 RTT，游戏读盘被阻塞；失败代价是整个会话拆链重建 |
| ② | socket 新增 `SO_SNDTIMEO`/`SO_RCVTIMEO` = 30 秒（配套给 SMSTCPIP 的 `api_lib.c`/`api.h`/`sockets.c` 实现了收发超时；此前这两个 setsockopt 是空操作） | `smb.c` + `SMSTCPIP×3` | 旧代码会永久阻塞等待慢响应；新代码 30 秒无数据即判读取失败，进入 ③ 的重连流程 |
| ③ | 后台重连线程（2 秒周期）+ 链路监控线程（0.5 秒周期）（优先级均为 40）；读取失败不再回报给游戏，改为标记断线→后台无限重连→原读取 100ms 轮询等待并静默重试；`DeviceReady()` 未连接时返回 NotReady | `device-smb.c` | 任何瞬时错误被放大为“拆链+协商+建会话+连共享+重开 ISO+重读”的数秒级断流 |
| ④ | `SO_KEEPALIVE` + `TCP_KEEPALIVE` = 60 秒（TCP 层保活探测） | `smb.c` | 仅线路空闲 60s 后才发探测包；FMV 期间链路繁忙，理论无害，做排除性验证 |
| ⑤ | 错误传播强化：`NegotiateProtocol/SessionSetup/TreeConnect/OpenAndX` 传输失败改为返回错误（原为无限重试或带病继续）；`ReadAndX` 分片接收失败判错；`OpenTCPSession` 连接失败改为返回给上层重试；`smbinit` 改为常驻；新增 `smb_AbortConnection`（目前无调用者） | `smb.c`、`smbinit/main.c` | 集中在启动期/断线期，稳定播放 FMV 时不参与 |

**已在 `edf39ddc` 逐字确认存在、与本回归无关的行为**（v1 曾误列；此处留档）：
- `smb_ReadFile` 短读补零语义、`DeviceReadSectors` 对短读的补零——`edf39ddc` 中已存在；
- cdvdman.c 的 PVD 边界/EOM 语义、u32 LSN、DeviceReadSectorsCompressed/ZSO 全部改动——本提交未触及；
- cdvdfsv GETTOC 假实现、ioops.c、ee_core（padhook IGR、iopmgr）、atad/device-hdd/device-bdm/mcemu、zso.c——本提交未触及。

> Note：游戏内不会加载 netman（system.c 的游戏模块表只有 `smap-ingame`+`ingame_smstcpip`+`smbinit`），
> 链路监控线程取不到链路状态、实际空转——所以"拔线 0.5 秒模拟开关仓"目前只能靠协议层/TCP 层失败来发现。

## 2. 根因假设（按嫌疑排序，修正版）

### H1：`SMB_COM_ECHO` 心跳——嫌疑最高（唯一新增的、稳态下周期性的主动网络行为）
- FMV 期间读盘几乎不间断，`smb_Echo()` 只有抢到信号量的间隙才发出（抢不到就每 2 秒重试），即必然插进数据流的第一个空隙。
- Echo 发出后 `smb_io_sema` 被占满整个 RTT；服务器（路由器/NAS 上的 Samba、旧 Windows 家庭共享等）对 Echo 响应慢或实现不规范时，游戏读盘被憋住，最坏可被 ② 的 30 秒超时放大。
- **更严重的是失败分支**：Echo 返回 -1 直接把会话标为断线（`smbConnectionState=2`），后台线程随后关 socket、重新协商、重建会话、重开 ISO 全部句柄——数秒级断流，只为一次不必要的保活。
- 预期症状：**杂音/爆音以约 30 秒左右的周期规律性出现**；服务器越不标准越明显。

### H2：30 秒收发超时 × 静默重连——嫌疑高（新故障模式的引入）
- 旧代码对慢响应会原地等待到数据到来，本次改为 30 秒判死：服务器硬盘休眠唤醒、WiFi 桥/电力猫抖动、路由器 SMB 卡顿一旦超过 30 秒，读请求失败 → 标记断线 → 全量重连。
- 一次瞬时卡顿被放大成数秒数据断流；重连成功后原读取才重试。
- 预期症状：杂音伴随**画面同步卡顿**（不只是声音）；出现时机与服务器/网络负载相关，成簇偶发。

### H3：TCP 层 `SO_KEEPALIVE`（60 秒）——嫌疑低，排除性验证
- 只在连接空闲 60 秒后才由 lwIP 发送探测包，FMV 期间链路持续繁忙不会触发；标准 lwIP 实现下无害。
- 预期症状：无对应症状，置 0 应当无变化（用于排除）。

### H4：后台线程/重连状态机本身（不依赖 Echo 与超时的部分）——对照验证
- 两份多余的线程唤醒（2s/0.5s，优先级 40）本身成本可忽略；但状态机在“读取失败后无限等待+重试”的行为改变了错误恢复路径。
- 若 B1（关 Echo）、B2（关超时）均无效而 B4（整拆线程，读失败立即报错）有效，则说明问题在状态机/线程交互本身。

## 3. 减法测试矩阵（每构建只改一项，共 4 个）

开关统一定义在 `modules/iopcore/cdvdman/smb_tuning.h`（全 1 = 与提交完全一致）。
CI 工作流 `.github/workflows/smb-bisect.yml` 自动产出，合并产物名 `OPNPS2LD-SMB-Bisect`。

| 构建 | 开关置 0 | 验证假设 | 杂音消失 ⇒ 结论 | 杂音依旧 |
|------|----------|----------|-----------------|----------|
| B1 | `SMB_FEAT_ECHO_KEEPALIVE` | H1 | 根因 = Echo（信号量占用 / 失败触发重连） | → B2 |
| B2 | `SMB_FEAT_SOCK_TIMEOUT` | H2（超时半） | 30s 超时把瞬时卡顿放大成断线重连 | → B3 |
| B3 | `SMB_FEAT_TCP_KEEPALIVE` | H3 | TCP 层保活（排除项） | → B4 |
| B4 | `SMB_FEAT_RECONNECT_THREADS` | H1+H2+H4 全体（读失败回到立即报错） | 若不依赖 Echo/超时也好转，根因在状态机/线程 | → 复查测试方法 |

观察要点：
1. 拿秒表对杂音**周期**：≈30 秒规律出现 → 强烈指向 H1。
2. 杂音时**画面是否同步卡住**：卡 → H2/重连；只有声音花 → H1 轻症。
3. 换服务器/换有线直连后**无变化** → 更像 H1 的协议层问题而非线路问题。
4. 对照组：当前标准版（行为等同 6254970）必须能复现杂音，再判定“某构建修好了”。

## 4. 根因确定后的收敛方向（预案）

## 4.0 第一轮实机结果(用户反馈)

| 构建 | 结果 |
|------|------|
| B1（关 Echo）| **无效**：杂音无变化 → H1（Echo 心跳）**排除** |
| B2（关 30s 收发超时）| **部分有效**：杂音起始时机**延后**，但出现音画不同步 → H2 相关**疑似核心**，但并非全部 |
| B3（关 TCP KeepAlive）| **无效** → H3 **排除** |
| B4（拆后台线程/状态机整体）| **无效** → H4（线程/重连状态机本身）**排除** |

逻辑推演：
1. Echo、线程、状态机、TCP KeepAlive 全部排除后，唯一有效的变量是 **socket 超时一项本身**。
2. 但 30 秒超时只有在“单次 recv/send 真的卡满 30 秒”时才会触发——若超时真的频繁触发，杂音应表现为数秒级的硬停顿；而 B4（超时仍在、 threads 已拆）与完整版无异，说明超时触发的次数有限、且失败后的恢复路径（重连 vs 直接报错）并不改变表现。
3. B2 把超时去掉后读取改为“无限等待”，杂音延后 + 音画错位 ⇒ 说明**读取迟缓/停顿本身客观存在**，超时只是把这个底层问题的表现形态改变了（把“慢”变成“失败+重连”）。其余的提交改动点都无法解释这种稳态差异，需要**数据取证**而不是继续盲猜。

### 下一步：诊断构建取证（DIAG）

CI 产物 **`OPNPS2LD-SMB-Diag`**：功能全开（= 完整提交行为）+ `SMB_DIAG_LOG=1`，
SMB 路径关键事件带毫秒时间戳输出：

- `RD slow/done/fail`：每次读取的耗时（≥250ms 或发生过重试时）、失败类别与重试次数
- `RD wait-reconnect`：读取被憋住等待后台重连的时刻
- `RECONNECT start/ok/fail`、`STATE=2`、`ECHO res=`、`LINK down/up`
- `TCP connect`、`NegotiateProt`、`REPLY/RAX send/recv fail`（含在读位置的 LSN 偏移）

抓取方法：
1. 在 SMB 服务器所在电脑上运行：`ncat -l -u -p 18194`（Windows，Nmap Ncat）或 `nc -ul 18194 > opl_smbd.log`（Linux/macOS）。也可以直接用 ps2link/ps2client 体系任何能收 UDP 18194 的接收器——收到的就是纯文本行。
2. **必须在电脑防火墙放行入站 UDP 18194**（最容易漏的一步）：
   - Windows（管理员 CMD 一次即可）：
     `netsh advfirewall firewall add rule name="OPL-UDP-18194" protocol=UDP dir=in localport=18194 action=allow`
   - Linux（用 ufw 时）：`sudo ufw allow 18194/udp`
3. v2 版 DIAG 构建起，每条 `SMBD` 日志会**同时走两条通道**，任一可达即可：
   - `255.255.255.255:18194` 广播（udptty 原生通道，收不到则多为防火墙/无线 AP 广播隔离）；
   - **单播到 SMB 服务器 IP:18194**（cdvdman 内建的诊断镜像，沿 SMB 数据同一条网络路径走，几乎必达——只要你的抓包程序和 SMB 服务器在同一台电脑上）。
   若抓包电脑**不是** SMB 服务器（例如 SMB 跑在路由器/NAS 上），请把抓包程序放到与 PS2 同一广播域的电脑上抓广播通道；或告诉我抓包电脑的 IP，我可以出固定目的 IP 的构建。
4. v3 版 DIAG 构建起，额外解除了 fork 在 `src/opl.c` main() 里注释掉的 `LOG_ENABLE()`（官方上游此处是启用的）：
   v2 之前 UI 阶段的 udptty/ps2link 服务端完全不加载，任何"主机工具连接 PS2 收日志"的尝试都注定失败；
   v3 恢复后，UI 阶段即可在 18194 收到日志，ps2client 也能连上 OPL 内置的 PS2LINK 服务端。
   进游戏阶段不依赖该行（ee_core 会自己加载 udptty-ingame），因此不影响 B1–B4 的结论。
5. 用 DIAG 版 OPL 启动问题游戏，播到能复现杂音的 FMV，录下完整日志。
6. 对照杂音出现时刻的日志行：
   - 看到 `RD done ... ms=30000` 紧跟 `RD fail`+`RECONNECT` ⇒ 超时触发坐实（服务器/网络真卡 ≥30s）；
   - 看到大量数百~数千 ms 的 `RD done ms=xxx`（无失败）⇒ 服务器/链路本身慢，杂音是欠载；
   - 看到 `ECHO res=-1` 与杂音对时 ⇒ Echo 方向仍有嫌疑（虽已排除，可复查）；
   - 若 IOP 日志平静而杂音依旧 ⇒ 问题不在 SMB 数据面，转向游戏侧/缓存层查。

### 第一份 DIAG 日志（v3 构建）判读结果（2026-10-09）

用户回传的一段游戏内日志（t≈92s 起，约 62 秒窗口）：

- `SMBD` 行仅 3 条，全部是 `ECHO res=1`（92102/124120/154143，间隔约 30s，正常心跳）；
  **无任何 `RD done/fail/wait-reconnect`、`RECONNECT`、`LINK` 行** ⇒ 该窗口内 SMB 传输完全健康：
  每次读盘 <250ms、无失败、无重试、无重连。
- 日志中大量的 `WARNING: WaitSema KE_CAN_NOT_WAIT` 与 `padman: *** VBLANK OVERLAP ***`
  **不是 OPL 的输出**：前者是 **debug 版 IOP Realtime Kernel** 的线程管理器告警（出处见 TCRF
  "PlayStation 2/Development Text"：`WARNING: WaitSema KE_CAN_NOT_WAIT` 是该内核调试串口的固定文案），
  后者是游戏自带 padman 的调试打印。两者在基线版本同样存在，属于环境噪音而非根因；
  但 WaitSema 洪泛集中在读目标缓冲 `0x11d680/0x125680` 与 `0x134ec0/0x13a340` 的窗口
  （推测是游戏音频流线程的缓冲），可作为"游戏流播放受压窗口"的参考标记。
- 重要推论：基线 `edf39ddc` 的 recv 是**永久阻塞**的——如果 FMV 期间真的发生 ≥30s 的服务器停顿，
  基线也会整段卡住并欠载杂音。用户确认基线无杂音 ⇒ **要么不存在 ≥30s 停顿**（那 30s RCVTIMEO
  永不触发，B2 的"部分有效"可能是间歇性杂音单次实机的巧合），**要么停顿被游戏缓冲吸收、而 6254970
  的超时拆链+重连（额外 2–5s）捅破了缓冲**。v4 的 `RD done ms=` 与 `RECONNECT` 行可区分这两条。
- 该窗口**不一定包含杂音时刻**——v3 的 `sceCdRead` 等打印没有时间戳，无法对齐读间隔空洞。

### v4 诊断构建会"进游戏卡死"的原因（2026-10-09 修订）

用户实机反馈：**v2（第一个 Diag 版）本身没问题**（当时是抓包电脑连错了网络），
而后续版本进游戏会卡死。逐条核对 v3/v4 相对 v2 的增量后，两个嫌疑点已全部撤除：

1. **v3 的 `LOG_ENABLE()` 恢复（头号嫌疑，已撤）**：该行会在 UI 阶段加载
   `udptty + ioptrap + ps2link` 三个模块（`src/debug.c: debugSetActive()`）。
   v2 没有它、实机正常；加上它之后才出现进游戏卡死。已把该 CI 步骤删除，
   诊断构建回到 v2 的加载集合。
2. **v4 的"逐调用时间戳打印"（已撤）**：v4 给 `sceCdStatus` / `sceCdGetError` /
   `sceCdRead` / `cdvdman_read` / `cdvdman_cb_event` 全部加了时间戳打印。
   但 `sceCdStatus` 是游戏**紧循环轮询**的（v3 日志里可见成片 `sceCdStatus 10`），
   而 cdvdman 的每条 `printf` 都要 `WaitSema(tty_sema)` + 一次阻塞式 UDP `sendto`
   （`modules/debug/udptty-ingame/udptty.c: tty_write/udp_send`）——
   每秒数千次 UDP 发送足以把 IOP 拖死。已全部还原为 v2 的普通 `DPRINTF`。

**结论：诊断日志必须"事件驱动、稀疏输出"，不能逐读全量打印。**

### v5：成对诊断构建（BASE 基线 vs CURR 当前）+ 读节奏观测点

按用户建议改为"同一套观测点、两棵树分别构建、日志直接逐行做差"：

- **`tools/smb-diag/instrument.py`**：树无关的注入脚本，可分别作用在
  基线 `edf39ddc`（无杂音）与当前 HEAD（有杂音）上。它只依赖两棵树中
  **完全相同的代码形状**（`sceCdRead` 入口 / `cdvdman_read` / `DeviceReadSectors`
  的短读补零 / `smb_ReadAndX` 的返回长度 / `imports.lst`）。
  实测注入后 `ncmd.c`、`diag_stamp.h` 在两棵树中**逐字节相同** ⇒ 字段与触发条件一致。
- **只在 `__IOPCORE_DEBUG` 下展开**，release 构建编译结果与未注入时完全一致。
- CI job `build-diag-pair` 产出两个产物：
  - **`OPNPS2LD-SMB-Diag-BASE`** = 基线 `edf39ddc` + 观测点（**无杂音**对照组）
  - **`OPNPS2LD-SMB-Diag-CURR`** = 当前 HEAD（= 6254970 行为）+ 观测点 + `SMB_DIAG_LOG=1`

注入的日志行（全部带毫秒时间戳，走 udptty UDP 18194）：

| 行 | 触发条件 | 含义 |
|---|---|---|
| `CDVD N ms=… n=<累计读次数> gaps=<累计空洞数>` | 每 256 次读 | 存活心跳 + **读速率**（两版可直接比速率） |
| `CDVD GAP ms=… gap=<距上次读 ms> lsn=… n=…` | 相邻读间隔 ≥ 50ms | **读节奏空洞**：数据流断流 |
| `CDVD SLOW ms=… dur=<读耗时 ms> lsn=… sec=… buf=…` | 单次读 ≥ 100ms | 单次读被拖慢（含目标缓冲地址） |
| `CDVD ZERO ms=… lsn=… got=… want=… buf=…` | 短读 → 尾部被静默填零 | **解码侧听到杂音/花屏的直接证据** |
| `CDVD SHORT ms=… off=… want=… got=…` | 服务器回包数据少于请求 | `ZERO` 的上游原因 |

（CURR 额外保留 SMB 内部事件：`SMBD ECHO res=… ms=…`、`SMBD RD done … ms=…`、
`SMBD RECONNECT …`、`SMBD HB n=… fail=… max=…ms avg=…ms s100/s250/s1k=…`。）

### ⚠ 重要发现：带 `__IOPCORE_DEBUG` 的诊断构建本身就是杂音源（BASE 版也有杂音）

用户实机反馈：**BASE 版（基线 `edf39ddc`）同样出现杂音**——而基线在 release 构建下是确认无杂音的。
排查结论：**不是版本弄错，是诊断构建的打印开销改变了时序**。

- `DPRINTF` 只在 `__IOPCORE_DEBUG` 下展开，而 `INGAME_DEBUG=1` 会通过
  `CDVDMAN_DEBUG_FLAGS = IOPCORE_DEBUG=1` 把它打开（Makefile:182-197）。
- 于是**每次** `sceCdRead` / `cdvdman_read` / `cdvdman_cb_event` / `sceCdGetError` /
  `sceCdStatus` 都会 `printf`；`sceCdStatus`/`sceCdGetError` 是游戏紧循环轮询的。
- 每条 `printf` 的代价是 `WaitSema(tty_sema)` + 一次**阻塞式** UDP `sendto`
  （`modules/debug/udptty-ingame/udptty.c: tty_write → udp_send`，广播 255.255.255.255）。
- FMV 下每秒成百上千次读 × 每条数个打印 = **每秒上千次阻塞式 UDP 发送**，
  足以把音频流拖到欠载 ⇒ 杂音。**BASE 与 CURR 都会中招**，A/B 因此失真。

修法：把"观测"与"逐读打印"解耦——cdvdman 编译为**不带** `__IOPCORE_DEBUG`
（逐读 `DPRINTF` 全空，时序≈release），只靠新增的 `-DDIAG_OBSERVE=1` 打开
`instrument.py` 注入的稀疏观测点（`tools/smb-diag/instrument.py` 的守卫已改为
`#if defined(__IOPCORE_DEBUG) || defined(DIAG_OBSERVE)`，并给 cdvdman 的 Makefile
加了 `DIAG_OBSERVE` 开关）。

### v6：四联对照构建（CI job `build-compare`）

| 产物 | 树 | 构建方式 | 用途 |
|---|---|---|---|
| `OPNPS2LD-SMB-Cmp-BASE-RELEASE` | `edf39ddc` | `make clean release`，零调试 | **环境基准**：确认"你现在这套网络/服务器下，基线是否仍然无杂音" |
| `OPNPS2LD-SMB-Cmp-CURR-RELEASE` | 当前 HEAD | `make clean release`，零调试 | 已知有杂音的对照（同 run 的现成参照） |
| `OPNPS2LD-SMB-Cmp-BASE-QUIET` | `edf39ddc` | 调试版但 cdvdman **不带** `__IOPCORE_DEBUG`，仅稀疏观测 | 无杂音对照组（抓日志） |
| `OPNPS2LD-SMB-Cmp-CURR-QUIET` | 当前 HEAD | 同上 + `SMB_DIAG_LOG=1` | 有杂音实验组（抓日志） |

ELF 文件名里带**树的 7 位 SHA**（BASE 应为 `edf39ddc`），下载时可自证版本没弄错。

**建议测试顺序**：
1. `BASE-RELEASE` —— 若**有杂音** ⇒ 环境（网络/服务器/线材）自上次验证后已变化，
   先修环境，否则任何代码层面的 A/B 都不可信。
2. `CURR-RELEASE` —— 应当有杂音（确认实验仍可复现）。
3. 1、2 成立后再用 `BASE-QUIET` / `CURR-QUIET` 各录一份日志做逐行对比。

**对比方法**：同一个游戏、同一段 FMV，分别用 BASE / CURR 各录一份日志：
- 若 CURR 出现 `GAP`/`SLOW`/`ZERO`/`SHORT` 而 BASE 没有 ⇒ 差异就在这几行的时刻与规模上；
- 若两者都平静但 CURR 有杂音 ⇒ 问题不在读盘数据面，转向解码/缓冲侧；
- 若 BASE 的 `CDVD N` 读速率明显高于 CURR ⇒ 6254970 引入了整体性吞吐下降。

后续预案（视取证结果选择）：提高/移除超时、超时后只重试当前请求而不拆链、
把重连恢复路径改回“先报错给游戏”、或针对服务器慢响应做读缓冲优化。

### v6 实机结果（2026-10-10）：`BASE-RELEASE` 无杂音，`BASE-QUIET` **有杂音**

日志原文：`notes/logs/base-quiet-siren.log`（Siren，stereo，约 69 秒）。

**日志内容本身是干净的** —— 整段没有一条 `CDVD SLOW`（没有单次读 ≥100ms）、
没有一条 `CDVD ZERO`（没有短读补零）、没有一条 `CDVD SHORT`（服务器没有短回）。
也就是说：在这份构建里，SMB 数据面没有丢数据、没有明显卡顿。

**但日志也证明了“QUIET 版并不 quiet”**，量化如下：

| 指标 | 数值 |
|---|---|
| `CDVD GAP` 行数 | 336 条 / 69.15s ≈ **4.9 行/秒** |
| `CDVD N` 行数 | 8 条 |
| `WARNING: WaitSema KE_CAN_NOT_WAIT` | **46 条** |
| `padman: *** VBLANK OVERLAP ***` | 1 条 |
| gap 分布 | 50–59ms: 214，60–99: 86，100–249: 9，250–499: 22，≥500: 5（中位 59ms） |
| 稳态读节奏 | 256 次读 / 7.26s ⇒ **28.4ms/次 ≈ 35 次/秒**（16 扇区 = 32KB/次 ⇒ ~1.15MB/s） |

两个直接结论：

1. **`GAP` 阈值 50ms 定得太低。** 游戏 FMV 的自然请求间隔中位数就是 ~59ms，
   于是 18% 的请求都会打一行 —— “稀疏”实际是 5 行/秒。这个数字本身不至于压垮
   IOP，但说明阈值没有区分“异常”和“常态”，日志的信噪比很差。**已废弃逐事件 GAP
   打印，改为按窗口聚合**（见 v7）。

2. **`WARNING: WaitSema KE_CAN_NOT_WAIT` 不是我们的代码打出来的。**
   它是 IOP 内核调试串口的固定文案（当 `WaitSema` 在不可等待的上下文被调用时）。
   链路是：`udptty-ingame` 的 `tty_write()` = `WaitSema(tty_sema)` + 阻塞式
   `lwip_sendto(255.255.255.255:18194)`；一旦有模块在**中断上下文**调用 printf，
   `WaitSema` 立刻返回 `KE_CAN_NOT_WAIT`，内核再 kprintf 一条告警 —— 而
   `udptty-ingame` 是**带 `-DKPRTTY`** 编译的（`modules/debug/udptty-ingame/Makefile:5`），
   它 `KprintfSet()` 把 IOP 内核的 kprintf 也接到 UDP 上，并由一个 **priority-8 线程**
   （`KPRTTY_Thread`）搬运。日志里 `padman: *** VBLANK OVERLAP ***` 就是这类
   中断上下文 printf 的样本。所以“装了 udptty”等于把整个 IOP 内核的调试输出也
   变成了高优先级线程里的阻塞式网络发送。

### ⚠ 更重要：`INGAME_DEBUG` 会把游戏内的 TCP/IP 栈整套换掉

Makefile 第 194–198 行，`INGAME_DEBUG=1` 分支里有一句
`SMSTCPIP_INGAME_CFLAGS =` —— 把 release 时的 `INGAME_DRIVER=1` **清空**了。
而 `SMSTCPIP_INGAME_CFLAGS` 正是用来编 `modules/network/SMSTCPIP/SMSTCPIP.irx`
（内嵌为 `ingame_smstcpip`，游戏里跑的那套栈）的（Makefile:629）。

两份配置的实际差异（`modules/network/SMSTCPIP/include/lwipopts.h`）：

| 宏 | release（`INGAME_DRIVER=1`） | QUIET（未定义 `INGAME_DRIVER`） |
|---|---|---|
| `PBUF_POOL_SIZE` | 8 | **25** |
| `TCP_WND` | 10240 | **32768** |
| `MEM_SIZE` | 0x400 | `TCP_SND_BUF*2` |
| `MEMP_NUM_TCPIP_MSG` | 15 | **40** |
| `MEMP_NUM_TCP_PCB` / `_LISTEN` | 1 / 1 | 2 / 2 |
| `ARP_TABLE_SIZE` | 2 | 3 |
| `TCP_QUEUE_OOSEQ` | 关 | 开 |
| `LWIP_UDP` | **0** | 1（udptty 需要） |
| `CHECKSUM_CHECK_IP/UDP/TCP/ICMP` | **0**（靠 SMAP 硬件校验） | **1**（IOP 上软件算） |

也就是说 QUIET 构建根本不是“release + 打印”，而是**换了一套 TCP/IP 栈**：
接收窗口大 3.2 倍、pbuf 池大 3 倍、还要在 37MHz 的 IOP 上逐包做软件校验和。
`BASE-QUIET` 出现杂音完全可以由这一项单独解释，与 6254970 无关。

**推论（待验证，但很值得优先验证）**：杂音可能是“在途数据太多 / IOP 来不及收包”
这一类**窗口与突发**问题，而不是某个具体 bug。若属实，则 6254970 里真正要盯的
就不是 Echo / 超时 / 线程，而是它改到的
`SMSTCPIP/api_lib.c`、`SMSTCPIP/sockets.c`、`SMSTCPIP/include/lwip/api.h`
（把 `SO_SNDTIMEO/SO_RCVTIMEO` 从空操作变成真正实现）—— 那三处会改变 recv 侧
何时把窗口交还给对端、以及短读/超时的语义。

### v7：`OBSERVE` 剖面（release 树 + 直发 UDP + 聚合统计）

针对上面两条，观测方式重做：

* **整机保持 release**：`make clean release CDVDMAN_DEBUG_FLAGS="DIAG_OBSERVE=1"
  SMSTCPIP_INGAME_CFLAGS="INGAME_DRIVER=1 INGAME_DRIVER_UDP=1"`。
  不加载 `udptty-ingame` / `ioptrap` / `ps2link`，EE 侧无 `__DEBUG` 打印；
  游戏内 SMSTCPIP 仍是 `INGAME_DRIVER=1`，**只额外打开 `LWIP_UDP` 与一个 netconn**
  （`lwipopts.h` 新增 `INGAME_DRIVER_UDP` 分支：UDP 1、NETCONN 2、MEM_SIZE 0x800；
  TCP 侧的 PBUF_POOL / TCP_WND / 校验和与 release 逐字节一致）。
* **日志不走 printf/tty**：新增 `modules/iopcore/cdvdman/diag_net.c`（由
  `instrument.py` 生成），直接调 ps2ip 导出表的 `lwip_sendto`，**单播**到 SMB 服务器
  IP:18194，不 WaitSema、不广播、不经过 KPRTTY。
* **不逐事件打印**：每 500ms 或 64 次读聚合发**一包**，稳态 2 包/秒、约 110 字节：

  ```
  CDVDS w=<窗口号> ms=<结束时刻> win=<窗口时长ms> n=<读次数> sec=<扇区数> \
        dur=<平均读耗时>/<最大读耗时> h50/h100/h250/h500=<各档计数，互斥> \
        gap=<窗口内最大读间隔> zero=<短读补零次数> shrt=<服务器短回次数>
  CDVDZ  ms=<now> lsn=<lsn> got=<实际> want=<请求>    短读补零（立即上报，每窗口≤4 条）
  CDVDSH ms=<now> off=<偏移> want=<请求> got=<实际>   服务器短回（同上）
  ```

  抓包方式不变：`ncat -l -u -p 18194`（防火墙放行 UDP 18194）。

新增产物（run 里与 v6 的四联并列）：

| 产物 | 树 | 用途 |
|---|---|---|
| `OPNPS2LD-SMB-Cmp-BASE-OBSERVE` | `edf39ddc` | **先看它是否无杂音**；有杂音则说明连“直发 UDP + 聚合”都嫌重，需再退一步到计数不上报 |
| `OPNPS2LD-SMB-Cmp-CURR-OBSERVE` | 当前 HEAD | 与上面逐窗口做差：`dur` 均值/最大值、`h100/h250/h500`、`gap` 最大值、`zero`/`shrt` 是否为 0 |

**判据**：
- `BASE-OBSERVE` 无杂音、`CURR-OBSERVE` 有杂音 ⇒ 回归坐实在 6254970；
  再看两份 `CDVDS` 的 `dur` 分档与 `gap` 差在哪。
- 若 `CURR-OBSERVE` 的 `zero`/`shrt` 出现非 0 ⇒ 数据面真的被填了 0，
  杂音就是解码器收到了静音/垃圾数据，直接盯 `SO_RCVTIMEO` 那三处改动。
- 若两份 `CDVDS` 都平静但 CURR 仍杂音 ⇒ 不在 SMB 数据面，转解码/缓冲侧。

### v7 实机结果（2026-10-10）：两份 OBSERVE 日志**在数据面上完全一致**

日志原文：`notes/logs/observe-base.log`（无杂音）、`notes/logs/observe-curr.log`（有杂音）。
同一段 FMV，各约 70 秒，~123 个统计窗口。稳态窗口（每次 ≥8 读、gap<200ms）的统计：

| 指标 | BASE-OBSERVE（无杂音） | CURR-OBSERVE（有杂音） |
|---|---|---|
| `dur` 均值 | 15.10 ms | 15.44 ms |
| `dur` 最大值（窗口内）均值 | 20.61 ms | 21.03 ms |
| `dur` 史上最大 | 30 ms | 40 ms |
| `h50/h100/h250/h500` | 全 0 | 全 0 |
| `gap` 均值 / 最大 | 62.6 / 142 ms | 61.3 / 147 ms |
| 每窗口读次数 | 18.4 | 18.5 |
| 吞吐 | 1018 KB/s | 1017 KB/s |
| `zero`（短读补零） | 0 | 0 |
| `shrt`（服务器短回） | 0 | 0 |

**结论：SMB 读数据面不是回归所在。**
没有一次读超过 50ms（连 `h50` 都是 0）、没有补零、没有短回、没有重连停顿
（重连会让读卡在 `DelayThread(100ms)` 的等待循环里，`dur` 必然炸到秒级）、
吞吐与请求节奏逐项相同。也就是说 6254970 **既没有让数据变慢，也没有让数据变脏**。

（两份日志都有窗口号缺号 —— BASE 缺 119/120/156/165/180/200/204，
CURR 缺 79/99/104/108/117/172/179 —— 那是抓包侧 UDP 丢包，与 PS2 无关。）

### v8：既然不在数据面，嫌疑收敛到“每一次收包都在多做的事”

既然延迟/吞吐/正确性都一样，问题只能是**开销的形态**：不是“某一次很慢”，
而是“每一次都多花一点”。重读 `edf39ddc → 6254970` 的 diff，唯一落在
**每一次 SMB 收包热路径**上的变更是 T2（`SO_SNDTIMEO/SO_RCVTIMEO = 30s`）：

* 旧代码：`setsockopt(SO_RCVTIMEO)` 是空操作，`conn->recv_timeout == 0`，
  `sys_arch_mbox_fetch()` 走 `sys_arch_sem_wait(sem, 0)` → 一次裸 `WaitSema`。
* 新代码：`conn->recv_timeout = 30000`，于是 **每一次** `netconn_recv()` 都落到
  `modules/network/SMSTCPIP/ps2ip.c` 的 `sys_arch_sem_wait()` 超时分支：

  ```
  GetSystemTime() → USec2SysClock() → SetAlarm() → WaitSema() → CancelAlarm()
                  → GetSystemTime() → SysClock2USec()
  ```

  即 **6 个额外系统调用 + 2 次 alarm 队列操作**，而不是 1 次 `WaitSema`。
* 频率：`smb_ReadFile()` 每次 32KB 读约 3 次 `netconn_recv`（netbios 头 / SMB 头 /
  数据体），FMV 稳态 36 次读/秒 ⇒ **~110 次/秒**，每次都可能命中空邮箱而走上面那条路。
* 更糟的是 `sys_arch_mbox_fetch()` 的 while 循环**每唤醒一次就重来一遍**，
  所以一次 `netconn_recv` 里可能装填/撤销好几轮 alarm。

这与实机结果对得上：
* **B2（`SMB_FEAT_SOCK_TIMEOUT=0`）部分有效** —— 正好就是关掉这一条路。
* **B1/B3/B4 无效** —— Echo（30s 一次，且用 `PollSema` 抢不到就跳过）、
  TCP keepalive（60s 一次）、重连线程（大部分时间在 `DelayThread`）都不在热路径上。
* **OBSERVE 日志里 `dur` 只涨了 0.34ms** —— 说明代价是“细水长流”而不是“卡顿”，
  这也解释了为什么 B2 只是“部分”有效（还有别的细水长流项，见下）。

#### 新的减法矩阵（CI job `build-smb-bisect` 的 C 系列）

| 产物 | 改动 | 验证什么 |
|---|---|---|
| `C1-TMO0+THREADS0` | `SOCK_TIMEOUT=0` **且** `RECONNECT_THREADS=0` | B2 单独只是部分有效，B4 单独无效 —— 两个一起关是否彻底干净？ |
| `C2-SINGLE-ALARM` | `SMSTCPIP_INGAME_CFLAGS="INGAME_DRIVER=1 LWIP_MBOX_SINGLE_ALARM=1"` | **保留 30s 超时语义**，只把 `sys_arch_mbox_fetch()` 改成整段 fetch 装填一次 alarm（新增 `mbox_fetch_timed()`）。若干净 ⇒ 元凶就是 alarm 装填频率，而且这个改法能保住 6254970 的全部功能 |
| `C3-NO-RCVTMO` | `SMB_FEAT_RCV_TIMEOUT=0`（只留 `SO_SNDTIMEO`） | 确认是收方向（热路径）而不是发方向 |
| `C4-SMBINIT-UNLOAD` | `modules/network/smbinit/main.c` 改回 `MODULE_NO_RESIDENT_END` | diff 里另一处无条件改动：smbinit 从“协商完就卸载”变成“常驻 IOP”。与任何开关都无关，单独验一下 |

**判据**：C2 干净而 C1 只是“更干净” ⇒ 直接采用 `LWIP_MBOX_SINGLE_ALARM` 作为正式修复；
C1 干净而 C2 仍有杂音 ⇒ 必须砍掉/改造收包超时本身（改成只在重连路径用带超时的 recv，
数据面继续用无限等待）；C4 干净 ⇒ smbinit 常驻也有份，单独处理。

### v8 实机结果（2026-10-10）：只有 `C1` 干净，其余全有杂音

| 构建 | 内容 | 结果 |
|---|---|---|
| `C1`（`SOCK_TIMEOUT=0` + `RECONNECT_THREADS=0`） | 关掉超时 **且** 关掉后台线程 | **干净，无杂音** |
| `C2`（`LWIP_MBOX_SINGLE_ALARM=1`） | 保留 30s 超时，只是把 alarm 从"每唤醒一次"降到"每次 recv 一次" | 有杂音 |
| `C3`（`SMB_FEAT_RCV_TIMEOUT=0`，只留 `SO_SNDTIMEO`） | 关掉收方向超时 | 有杂音 |
| `C4`（`smbinit` 改回 `MODULE_NO_RESIDENT_END`） | 只改常驻 | 有杂音 |
| （此前）`B2` = `SOCK_TIMEOUT=0` 单独 | | 部分有效（杂音减轻/延后，伴音画不同步） |
| （此前）`B4` = `RECONNECT_THREADS=0` 单独 | | 完全无效 |

把三份结果放在一起推：

```
关超时(Y)  关线程(X)   结果
  否         否        满杂音
  是         否        部分杂音     ← Y 是主因
  否         是        满杂音       ← X 单独被 Y 完全掩盖
  是         是        干净         ← X 是小但独立的第二因
```

所以是**两个相互独立的来源，Y 主 X 次**：
* **Y（主因）**：`SO_SNDTIMEO/SO_RCVTIMEO` 生效后，每一次 `netconn_recv()` 都要
  走 `sys_arch_sem_wait()` 的超时分支。C2 把 alarm 频率降了一个数量级仍然有杂音，
  说明**不是"装填次数太多"，而是"只要有"就不行** —— 那 6 个系统调用 + 2 次 alarm
  操作本身就已经越过了这台机器的余量。
* **X（次因）**：在 `SMB_FEAT_RECONNECT_THREADS` 这一组里面。
  注意 `src/system.c` 里**完全没有 netman** —— 游戏内不加载 netman，
  所以 `pNetManGetGlobalNetIFLinkState == NULL`，链路监控线程其实只是
  每 500ms 醒一次、什么也不做。也就是说 X 不是"读 PHY 太贵"，
  而更像是**多两个后台线程周期性唤醒本身**造成的调度扰动
  （两个线程优先级 40；cdvdman 读线程是 0x0f=15，比它们高，所以它们抢不到读线程，
  但会抢占优先级低于 40 的游戏侧线程）。

### v9：D 系列 —— 拆开 X，并给出"既保功能又无杂音"的修复候选

`modules/iopcore/cdvdman/smb_tuning.h` 新增可调项：

| 宏 | 默认 | 说明 |
|---|---|---|
| `SMB_FEAT_LINK_MONITOR` | 1 | 链路监控线程是否创建 |
| `SMB_LINK_MONITOR_MS` | 500 | 链路监控轮询周期 |
| `SMB_RECONNECT_POLL_MS` | 2000 | 重连线程轮询周期（Echo 间隔已从"15 次轮询"解耦成 `SMB_ECHO_INTERVAL_MS`） |
| `SMB_ECHO_INTERVAL_MS` | 30000 | Echo 保活间隔 |
| `SMB_THREAD_PRIORITY` | 40 | 两个后台线程优先级 |
| `SMB_FEAT_ECHO_TIMEOUT` | 0 | **只在 `smb_Echo()` 这一次往返前后临时装上 30s 超时**，装完立刻清回 0 |

新增构建：

| 产物 | 内容 | 验证什么 |
|---|---|---|
| `D1-TMO0+NOLINKMON` | `SOCK_TIMEOUT=0` + `LINK_MONITOR=0` | X 是不是那个 500ms 唤醒的链路监控线程？ |
| `D2-TMO0+SLOWPOLL` | `SOCK_TIMEOUT=0` + 链路 500→3000ms、重连 2000→10000ms | 不删功能、只把唤醒放慢 6×/5× 是否就够了？ |
| `D3-FIX-CANDIDATE` | D2 + `SMB_FEAT_ECHO_TIMEOUT=1` | **正式修复候选**：数据面零开销（裸 WaitSema），但服务器失联依旧能被 30s 的 Echo 发现并触发重连；拔线模拟开关仓也还在 |

**判据**
- D1 干净、D2 也干净 ⇒ X = 周期性唤醒，D3 就是可以合入的版本。
- D1 干净、D2 仍有杂音 ⇒ 必须彻底去掉链路监控线程（或者把它改成事件驱动，
  比如只在读失败时才去查链路，而不是定时轮询）。
- D1 仍有杂音 ⇒ X 不在链路监控线程，而在重连线程（2s 唤醒）或
  `DeviceReadSectors` 的重试循环 / `DeviceReady()` 的状态依赖上，需要再拆一轮。

## 4. 根因确定后的收敛方向（预案）

- H1 成立：删除 Echo 心跳（SMB 服务器极少在分钟级回收会话，TCP 层 KeepAlive 足够兜底）；或把 Echo 失败与“判定断线”解耦（失败仅计数，连续多次才拆链），并把 Echo 移到确认空闲的更保守策略。
- H2 成立：提高 RCVTIMEO（或恢复不设超时），仅对确定性断线错误（连接复位/发送失败）进重连；超时/慢响应改为原请求重试而不拆链。
- H4 成立：把“读取失败→静默重试”改为“先向游戏报错、由后台完成重连后再恢复”，游戏侧只需一次常规重读。
- 完成修复后重新打开 `smb_tuning.h` 中被保留的全部功能，仅剔除确认的根因。

## 5. 排查过程中顺带发现的问题

1. **越界读（已修）**：`ps2ip_init()` 读 `info.exports[46]`，但 ps2ip 导出表只有 41 项（0..40），取到野指针；已在开关矩阵提交中置 `NULL`。`smb_AbortConnection()` 目前无调用者；将来启用前需先在 SMSTCPIP 的 `exports.tab` 补上 `lwip_shutdown`。
2. **拔线模拟开关仓在游戏内当前不可达**：游戏内无 netman（见 §1 注），物理拔线现在实际依赖 RCVTIMEO 30s / TCP KeepAlive 60s 发现——与提交说明中“0.5 秒内模拟开仓”的预期不符，功能落地前需要游戏内的链路状态来源（如 SMAP 驱动导出或 PHY 轮询）。
3. **回声与重连共用全局 `SMB_buf`**：Echo/读/重连都复用同一个收发缓冲区，靠 `smb_io_sema` 串行化；将来改动时注意任何绕过信号量的直接 socket 调用都会破坏这个前提。

## 6. 辅助观测（可选）

用 `ingame_debug` + UDP TTY 构建，在 `smbReconnectThread` 的状态切换处与 `smb_Echo()` 调用处加
`DPRINTF`，把杂音时刻与内部事件对齐到同一时间轴，可直接区分 H1/H2。

---

# 第二阶段：把修复落到 `362053534-patch-1`（f91c3cf）

> 用户说明：`arena/aac22127-open-ps2-loader` 只是"最早引入问题的时间节点"，
> 用来定位；**正式修复要在 patch-1 的最新提交上进行**。
> 本会话只能推 `arena/…`，所以做法是把这边的树整体换成 `f91c3cf`，
> 在其上做 D1/D2/D3，产物从这边出，改动再由用户合回 patch-1。

## patch-1 与 6254970 那棵老树的差异（影响排查结论）

| 项 | 6254970 老树 | patch-1 (f91c3cf) |
|---|---|---|
| 后台线程 | **2 个**：重连线程(2s) + 链路监控线程(500ms) | **1 个**：重连线程(2s)，链路监控已合并进去 |
| 链路状态来源 | `netman` 导出 14（**游戏内根本不加载 netman ⇒ 恒为 NULL**） | 新增 `pSmapGetLinkStatus`（smap-ingame 导出），链路检测真的能用 |
| Echo 触发 | 每 30s，不管忙不忙 | **空闲 120s 才发**（`SMB_ECHO_IDLE_TICKS=60` × 2s），且连续失败 2 次才判定断线 |
| `SO_SNDTIMEO/SO_RCVTIMEO = 30000` | 有（**主因 Y**） | **仍然有**（`smb.c:138-140`），主因 Y 依旧存在 |
| `netconn_recv` 超时分支 | 有 | 有（`api_lib.c:436/482`） |

⇒ **第二个（较小的）来源 X 在 patch-1 上已经消失**（500ms 的链路监控线程没了）。
所以 patch-1 上大概率只剩 Y 一个来源，D1 一次就应该能干净；
D2/D3 是在"确认干净"之后把功能补回来 / 再压一层。

## D1 / D2 / D3（在 patch-1 上）

新增 `modules/iopcore/cdvdman/smb_tuning.h`（patch-1 此前没有这个文件），
3 个开关 + 2 个可覆盖常量：

| 宏 | 默认 | 说明 |
|---|---|---|
| `SMB_FEAT_SOCK_TIMEOUT` | 1 | 置 0 ⇒ 不再 `setsockopt(SO_SNDTIMEO/SO_RCVTIMEO)`，`conn->recv_timeout == 0`，每次收包退回**裸 `WaitSema`** |
| `SMB_FEAT_RCV_TIMEOUT` | 1 | 只拆收方向（`netconn_recv` 才是热路径） |
| `SMB_FEAT_ECHO_TIMEOUT` | 0 | 置 1 ⇒ 30s 超时**只在 `smb_Echo()` 这一次往返前后临时装上**，用完清回 0。数据面零开销，失联检测保住 |
| `SMB_RECONNECT_INTERVAL_US` | 2000000 | 重连线程轮询周期（`device-smb.c` 里改成 `#ifndef` 可覆盖） |
| `SMB_ECHO_IDLE_TICKS` | 60 | Echo 空闲阈值；配合上面保证"空闲 120s 才 Echo"（5s × 24 tick = 120s） |

| 构建 | defines | 含义 |
|---|---|---|
| `D1-NO-SOCKTMO` | `SMB_FEAT_SOCK_TIMEOUT=0` | 最小改动：数据面零开销。失联检测暂时没有 |
| `D2-NO-SOCKTMO+ECHOTMO` | `+ SMB_FEAT_ECHO_TIMEOUT=1` | **功能完整的修复候选**：数据面零开销 + Echo 仍能发现失联并触发重连 |
| `D3-D2+SLOWPOLL5s` | `+ SMB_RECONNECT_INTERVAL_US=5000000 SMB_ECHO_IDLE_TICKS=24` | 再压一层周期性唤醒（2s→5s），Echo 空闲时长仍为 120s |

参照：`CURR-RELEASE`（patch-1 原样，应当有杂音）、`BASE-RELEASE`（edf39ddc 老基线，应当无杂音）。

**判据**
- D1 干净 ⇒ Y 在 patch-1 上仍是唯一主因 → D2 就是可以直接合的版本。
- D1 干净、D2 有杂音 ⇒ `smb_Echo()` 那一次带超时的往返会污染后续（Echo 之间隔 120s，
  理论上影响很小，但如果真有，就把超时改成只装在 `SO_SNDTIMEO` 上、收方向不装）。
- D1 仍有杂音 ⇒ patch-1 上还有别的来源，需要重新做一轮 OBSERVE 成对日志
  （`tools/smb-diag/instrument.py` 的注入锚点需要按 patch-1 的代码形状更新）。

---

# 第三轮（v11）：D1 干净 / D2 概率杂音 / D3 待确认

## 实机结果

| 构建 | 结果 |
|---|---|
| `D1-NO-SOCKTMO` | 无杂音 |
| `D2-NO-SOCKTMO+ECHOTMO` | **概率性杂音，集中在动画最后几秒** |
| `D3-D2+SLOWPOLL5s` | 暂未遇到杂音（测试次数可能不够） |

## 由此得到的判断

D2 与 D1 的**唯一**代码差别就是 `smb_Echo()` 前后那两组
`setsockopt(SO_SNDTIMEO/SO_RCVTIMEO)`。而 D3 与 D2 的 Echo 行为**完全相同**
（`SMB_ECHO_IDLE_TICKS × SMB_RECONNECT_INTERVAL_US` 都是 120s 空闲才发），
所以 D3 与 D2 只差"线程每 2s 还是每 5s 醒一次" —— 而 D1（2s 唤醒 + 无 Echo 超时）
是干净的，说明单纯的 2s 唤醒本身不产生杂音。

⇒ **D3"干净"极可能只是样本不够**，不应据此认为 5s 轮询解决了问题。

## v11 的 3 个新构建：用放大/对照代替碰运气

不再靠多跑几次去抓小概率事件，而是改变 Echo 的触发频率让信号变大：

| 构建 | defines | 目的 |
|---|---|---|
| `E4-ECHO-AMPLIFY-2s` | `SMB_ECHO_TIMEOUT=1 SMB_ECHO_IDLE_TICKS=1` | **放大器**：空闲 2s 就发一次 Echo（原为 120s，频率 ×60）。若 Echo 是元凶，这份应该明显更吵，一两次播放就能听出来 |
| `E1-ECHO-NEVER` | `SMB_ECHO_TIMEOUT=1 SMB_ECHO_IDLE_TICKS=3600` | **反向对照**：要空闲 2 小时才发 Echo，实测期间一次都不会发。若仍干净 ⇒ 杂音确实需要 Echo 真的发生 |
| `E2-NO-SOCKTMO+ECHO-POLL-NB` | `SMB_FEAT_ECHO_TIMEOUT=2` | **修法候选**：完全不碰 `setsockopt`，改用 `plwip_recv(MSG_PEEK 不可用，所以直接收, MSG_DONTWAIT)` 轮询等回包，上限 `SMB_ECHO_TIMEOUT_MS`。数据面与 D1 完全一致，失联检测保留 |

`E2` 的实现要点（`smb.c`）：
* 新增 `RecvDataEx(sock, buf, size, flags, timeout_ms)`：
  `flags == 0` 时是原来的阻塞收包；`flags == MSG_DONTWAIT` 时自己
  `DelayThread(20ms)` 轮询，超时返回 0。原来的 `RecvData()` 退化成它的一个包装。
* 新增 `GetSMBServerReplyTimed()`：`GetSMBServerReply(0, NULL, 0)` 的非阻塞版，
  只给 `smb_Echo()` 用；数据面继续走原来的阻塞版本。
* `SMB_FEAT_ECHO_TIMEOUT`：0 = 不设上限 / 1 = setsockopt / 2 = 轮询（新）。
* 此 lwip 不支持 `MSG_PEEK`，所以轮询是真的把字节收进 `SMB_buf`；
  超时后残留的半包在 `smb_Disconnect()` 关 socket 时一并丢弃。

**判据**
- E4 明显更吵 + E1 干净 ⇒ Echo 确认是元凶 → **E2 就是最终修复**。
- E4 干净 ⇒ Echo 不是元凶，D2 的杂音另有来源（需要重新上 OBSERVE 日志）。
- E2 干净 ⇒ 可直接合入 patch-1。
- E2 仍吵 ⇒ "Echo 这个动作本身"会干扰（而不是它身上的 setsockopt），
  那就要改成"Echo 期间挂起读线程"或干脆去掉空闲 Echo、只靠链路状态 + 读失败判定断线。

---

# 第三轮实机结果 + 结论

| 构建 | Echo 触发间隔 | Echo 是否设 setsockopt | 结果 |
|---|---|---|---|
| `D1-NO-SOCKTMO` | 120s | 否（模式 0，且数据面本来就没超时） | 干净 |
| `D2-NO-SOCKTMO+ECHOTMO` | 120s | **是**（模式 1） | **概率杂音** |
| `E4-ECHO-AMPLIFY-2s` | **2s**（×60） | **是**（模式 1） | **杂音"跟之前一样"，没有变多** |
| `E1-ECHO-NEVER` | 2h（实测不发） | 是（但从未执行） | 干净 |
| `E2-NO-SOCKTMO+ECHO-POLL-NB` | 120s | **否**（模式 2，`MSG_DONTWAIT` 轮询） | **多次测试干净** |

## 关键推论：不是"每次 Echo 抖一下"

E4 把 Echo 频率放大了 60 倍，杂音**没有变多、也没有变明显**。所以：

> 杂音 ∝ Echo 的次数 —— **不成立**。

把 5 个结果交叉起来看，杂音需要**两个条件同时成立**：

1. `setsockopt(SO_RCVTIMEO / SO_SNDTIMEO)` 真的被执行过；**且**
2. 一次 Echo 往返真的发生过。

只有 1 没有 2（E1：代码在但从没触发）→ 干净。
只有 2 没有 1（E2：Echo 照发但不碰 setsockopt）→ 干净。
两个都有（D2、E4）→ 杂音，而且**跟 Echo 次数无关**。

最自然的解释：**那次 setsockopt 一旦执行，就在 ps2ip 的收包路径上留下了持久影响** ——
之后 `netconn_recv()` 不再走裸 `WaitSema`，即使把值改回 0 也没完全恢复原状。
也就是说这是一次性的"污染"，不是每 Echo 一次的抖动，所以放大 Echo 频率不会让它变严重。
（E4 之所以和 D2 听不出区别，是因为实际测试里 OPL 菜单/载入阶段早就有超过 120s 的空闲，
D2 在进游戏之前就已经把那次 setsockopt 执行过了。）

**一个零成本的验证办法**：用 D2 那个 ELF，**开机后不做任何停留立刻进游戏播 FMV**。
若这时的 D2 是干净的、而在 OPL 菜单里停留 3 分钟以上再进游戏就有杂音，
就说明"一次 setsockopt 就污染整个会话"成立。

## 结论与建议合入 patch-1 的形态

**SMB 套接字上永远不要设 `SO_RCVTIMEO` / `SO_SNDTIMEO`，一次都不行。**
保活探测需要超时上限时，用 `SMB_FEAT_ECHO_TIMEOUT=2`（`MSG_DONTWAIT` 自己轮询）。

`modules/iopcore/cdvdman/smb_tuning.h` 的默认值已改成第三轮验证过的 E2 配置：

```c
#define SMB_FEAT_SOCK_TIMEOUT  0   /* 不设 SO_SNDTIMEO / SO_RCVTIMEO */
#define SMB_FEAT_ECHO_TIMEOUT  2   /* Echo 用 MSG_DONTWAIT 轮询，上限 3s */
#define SMB_ECHO_TIMEOUT_MS    3000
#define SMB_ECHO_POLL_MS       20
```

⇒ 默认值下编译出的 ELF 与用户实测多次无杂音的 **E2 是同一份代码配置**
（CI 里 E2 的 sed 现在就是空操作）。

## 这样改的取舍

* 数据面回到"收发没有超时兜底"= edf39ddc 之前的老行为（OPL 这么跑了很多年）。
  代价：理论上存在"服务器收下请求但永远不回"时读线程会一直等。
* 但这不是失去保护：**控制面（Echo）仍然有 3s 上限**，服务器失联照样能被发现并触发重连；
  拔网线则由 `pSmapGetLinkStatus()` 链路状态 + 收发报错判定。
* `TCP_NODELAY` / `SO_KEEPALIVE` / `TCP_KEEPALIVE` 不受影响，三个构建里都一样，
  而 D1/E1/E2 都干净 ⇒ 它们无责。

---

# 第四轮（v12）：用户质疑"测试流程里根本凑不出 120 秒空闲"

## 质疑本身是成立的

核对过代码，用户说得对：

* `smb_Echo()` **只有一个调用点**（`device-smb.c:146`），只由空闲计数器触发。
* `smbIdleTicks` 在 `DeviceReadSectors()` 里每次 `smb_ReadCD` 后被清零（365/374 行）。
* 所有游戏读盘都走 `DeviceReadSectors`（纯 ISO 时 `DeviceReadSectorsPtr` 直接指向它；
  `DeviceReadSectorsCached` 只在 ZSO 路径上用）。
* `SMB_ECHO_IDLE_TICKS` 只在这两处判断里用到。

⇒ 如果整个流程里**一次都没有**凑够 120 秒连续不读盘，那 D2 与 E1 的行为应该逐字节相同，
不可能一个吵一个干净。**所以"某个地方确实存在 ≥2 分钟连续不读盘的窗口"和"我这套解释错了"
二者必居其一。**

## 容易踩的坑：按手柄 ≠ 读盘

"空闲"的定义是**没有任何 `sceCdRead`**，不是"没有操作"。以下界面在很多游戏里
是整段常驻内存的，完全不读盘：

* 制作人 / 发行商 logo 画面（动辄十几到几十秒）
* 游戏主菜单、存档选择、难度/选项设置
* 标题画面 / attract 演示循环
* 暂停菜单

只要其中任意一处连续停够 2 分钟，Echo 就会发出去。而项目里"概率性出现"
恰好对应"每次测试停留时长不一样"。

## 决定性的验证（新增 E6 / E7）

不再依赖测试流程，直接**保证**那次 Echo 一定执行过：

| 构建 | defines | 含义 |
|---|---|---|
| `E6-FORCED-ECHO+SETSOCKOPT` | `ECHO_TIMEOUT=1 ECHO_FORCE=1` | 启动后立刻强制发一次 Echo（走 setsockopt 那条路） |
| `E7-FORCED-ECHO+POLL-NB` | `ECHO_TIMEOUT=2 ECHO_FORCE=1` | 同样强制发一次 Echo，但走无 setsockopt 的轮询路 |

`SMB_FEAT_ECHO_FORCE` 的实现（`device-smb.c`）：在重连线程里，只要还没成功执行过一次 Echo，
就把 `smbIdleTicks` 顶到阈值；`smb_Echo()` 返回 0 表示"被跳过"（读线程正占着 `smb_io_sema`），
所以会一直重试到它真的跑起来为止。这样 Echo 会在游戏启动后头几秒内必然发生，
**远早于任何 FMV**，从而彻底摆脱"到底有没有 120 秒空闲"这个问题。

**判据**
- 直奔 FMV 时 **E6 吵、E7 干净** ⇒ "那次 setsockopt 一旦执行就留下持久影响"完全坐实；
  同时也说明 D2 里它确实发生过（只是没意识到停在哪）。E2/E7 一系就是最终修复。
- 直奔 FMV 时 **E6 也干净** ⇒ D2 的杂音与 Echo 无关，前三轮的结论要推翻重来
  （D2/E4 与 E1 的差异只剩测试方差，需要重新设计对照）。
- 两个都吵 ⇒ 是"Echo 这个动作本身"在干扰，与 setsockopt 无关；
  那就得改成去掉空闲 Echo、只靠链路状态 + 读失败判定断线。

## 零成本补充验证（用已有的 D2）

同一个 D2 ELF，两种进游戏方式对比：

1. 开机后**不做任何停留**直奔 FMV → 预测干净
2. 在任意菜单停 3 分钟以上再进同一段 FMV → 预测有杂音

成立即说明"120 秒空闲窗口"确实存在，只是没被注意到。

---

# 最终结论（用户 2026-10 定案）

## 范围收敛

用户实测**官方老版本也有同样的问题** ⇒ 那种"随机、偶发、测很多次才碰上"的杂音
是**网络抖动的固有底噪**，不是任何提交引入的回归，**不在修复范围内**。

要修的只是**百分百必现的那一部分**：patch-1 原样必现，去掉数据面的 socket 超时后
掉回与官方老版本相同的"偶发"水平（D1 / D2 / E1 / E2 四组构建都验证了这一点）。

## 根因（百分百必现的那部分）

`OpenTCPSession()` 里这两行：

```c
opt = SMB_IO_TIMEOUT;                                    /* 30000 */
plwip_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, ...);
plwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, ...);
```

一旦生效，`conn->recv_timeout != 0`，ps2ip 的**每一次** `netconn_recv()` 都会走
`sys_arch_mbox_fetch()` 的 alarm 分支（GetSystemTime + USec2SysClock + SetAlarm
+ WaitSema + CancelAlarm + GetSystemTime + SysClock2USec），而不再是裸 `WaitSema`。
edf39ddc 及更早的 OPL 从来没有这两行。

## 修复

去掉这两行，同时保证保活探测仍有界（否则服务器失联时 `smb_Echo()` 会卡死在 recv 上）：

* `RecvDataEx(sock, buf, size, flags, timeout_ms)`：`flags == 0` 是原来的阻塞收包；
  `flags == MSG_DONTWAIT` 时自己 `DelayThread(20ms)` 轮询，超时返回 0。
  原 `RecvData()` 退化为它的包装，**数据面行为一字未改**。
* `GetSMBServerReplyTimed()`：`GetSMBServerReply(0, NULL, 0)` 的非阻塞版，
  **只给 `smb_Echo()` 用**，上限 `SMB_ECHO_TIMEOUT_MS` = 3s。
* 该 lwip 不支持 `MSG_PEEK`，轮询是真的把字节收进 `SMB_buf`；
  超时残留的半包由随后的 `smb_Disconnect()` 关 socket 时一并丢弃。
* `SO_KEEPALIVE` / `TCP_KEEPALIVE` / `TCP_NODELAY` 保持不变。

⇒ 相对于 `362053534-patch-1` 的 `f91c3cf`，**只改 `modules/iopcore/cdvdman/smb.c`
一个文件（+82 / -12）**，补丁见仓库根目录 `smb-fmv-noise-fix.patch`。

## 走过的弯路（留档，别再重来）

1. 前三轮一直在"Echo 那次 setsockopt 污染了后续数据面"上打转。E1（Echo 永不触发）
   多测几次仍然有杂音，该模型被证伪。教训：**概率性症状下，几轮"干净"的观测没有统计意义**。
2. 因此 D1 / E2 / D3 早先报的"干净"都要打折看待。
3. 追查过程中另外发现两件 patch-1 相对 edf39ddc 的差异，**未验证、也未纳入本次修复**：
   * `SO_KEEPALIVE` + `TCP_KEEPALIVE=60000`（6254970 引入）
   * patch-1 新增的 SMB 专用异步读完成流水线（`cdvdman_smb_start_read` /
     `cdvdman_promote_pending` / `cdread_outstand` / stream generation），
     **6254970 也没有**，只存在于 patch-1。用户最初对 B2 的描述是
     "杂音未根除、延后、伴音画不同步"，音画不同步是时序症状 —— 如果将来要继续压
     偶发杂音，这两处（尤其后者）是下一步的入口。
