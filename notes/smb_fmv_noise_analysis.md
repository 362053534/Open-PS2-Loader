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

**对比方法**：同一个游戏、同一段 FMV，分别用 BASE / CURR 各录一份日志：
- 若 CURR 出现 `GAP`/`SLOW`/`ZERO`/`SHORT` 而 BASE 没有 ⇒ 差异就在这几行的时刻与规模上；
- 若两者都平静但 CURR 有杂音 ⇒ 问题不在读盘数据面，转向解码/缓冲侧；
- 若 BASE 的 `CDVD N` 读速率明显高于 CURR ⇒ 6254970 引入了整体性吞吐下降。

后续预案（视取证结果选择）：提高/移除超时、超时后只重试当前请求而不拆链、
把重连恢复路径改回“先报错给游戏”、或针对服务器慢响应做读缓冲优化。

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
