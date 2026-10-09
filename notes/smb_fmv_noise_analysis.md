# SMB 模式 FMV 音频杂音排查记录

针对提交 `6254970`（改进SMB：拔线模拟开关仓，断线后无限重连）。
现象：**部分游戏**播放 FMV 时音频出现明显杂音；回退整包改动后杂音消失；该提交功能重要，需要保留。

---

## 1. 改动清单（相对基线的完整盘点）

与上游基线（ps2homebrew/Open-PS2-Loader `626e5e46`，树差异最小的匹配点）对比，游戏内（In-Game）环境的改动为 21 个文件。其中与 **SMB 读取路径**直接相关的如下：

| # | 改动 | 位置 | 稳态开销 |
|---|------|------|----------|
| ① | 每 30 秒发送一次 `SMB_COM_ECHO` 保活包（新线程，优先级 40） | `device-smb.c` → `smb_Echo()` | 每次 Echo 期间持有 `smb_io_sema`，阻塞游戏读盘一个 RTT |
| ② | socket 增加 `SO_SNDTIMEO`/`SO_RCVTIMEO` = 30 秒 | `smb.c` `OpenTCPSession()`；SMSTCPIP 新增超时实现 | 仅超时触发时生效 |
| ③ | `SO_KEEPALIVE` + `TCP_KEEPALIVE` = 60 秒 | 同上 | 仅线路空闲 60s 后才发探测包 |
| ④ | 后台重连线程（2s 周期）+ 链路监控线程（0.5s 周期）；读取失败→标记断线→后台无限重连→原读取静默重试 | `device-smb.c` | 平时仅睡循环，断线是状态机整体接管 |
| ⑤ | 短读（服务器返回 0 字节 / 撞到文件尾）→ **补零后按成功返回** | `smb.c` `smb_ReadFile()`、`device-smb.c` `DeviceReadSectors()` | 每次短读都把零数据当真数据交给游戏 |
| ⑥ | 越过 PVD 标称容量的读取不再报错/截断，继续读、读不到补零 | `cdvdman.c` `cdvdman_read_sectors()`、`DeviceReadSectorsCompressed()` | 越界读取从"报错"变成"成功+脏数据" |
| ⑦ | `DeviceReady()` 未连接时返回 `SCECdNotReady`（旧：恒定 Complete） | `device-smb.c` | 已连接后无差异 |
| ⑧ | `cdvdfsv` 的 `CD_NCMD_GETTOC` 直接返回成功、不再真的读 TOC | `cdvdfsv/ncmd.c` | 无 |
| ⑨ | `ioops.c` 去掉各文件操作前的 `WaitEventFlag` 等待、新增 `sceCdDiskReady(0)` | `ioops.c` | 极少 |
| ⑩ | EE 侧：`ee_core` IGR 组合键判定改动（Start+Select 单键即可触发）、cdvdman 启动时下发风扇 S 命令、`smbinit` 改为常驻、HDD 模式加载 USB 模块 | `ee_core`/`iopmgr.c`/`smbinit` | 极少 |

其余改动（atad/device-hdd/device-bdm/mcemu/zso 加固等）属于 HDD/BDM/VMC/ZSO 路径，SMB 纯 ISO 场景不经过。

> Note：`吃保底`前提——游戏内并不会加载 netman（system.c 的游戏模块表只有 `smap-ingame` + `ingame_smstcpip` + `smbinit`），
> 因此"拔线监控线程"目前取不到链路状态、实际空转（见 §5-发现2），可排除为杂音来源。

## 2. 根因假设（按嫌疑排序）

### H1：`SMB_COM_ECHO` 心跳包（对应开关 T1）—— 嫌疑最高
- FMV 期间读盘几乎不间断，Echo 只有在读盘间隙抢信号量才能发出（忙时每 2 秒重试一次，直到抢到为止），即必然插进数据流的第一个空隙。
- Echo 一旦发出，`smb_io_sema` 被占满整个往返；服务器（尤其路由器/NAS 上的 Samba、手机 SMB）对 Echo 响应慢或不规范时，游戏读盘被憋住，最坏可被 30 秒 RCVTIMEO 放大；**Echo 失败直接把整个会话标记为断线(`smbConnectionState=2`)**，触发拆链+全量重连（协商→建会话→连共享→重开 ISO→重新读），数秒级停顿。
- 预期症状：**杂音/爆音以约 30 秒左右的周期规律性出现**，服务器越差越明显；杂音时画面可能有轻微顿一下。

### H2：读超时被放大成断线重连（对应开关 T2、T4）—— 嫌疑中高
- 旧代码：读取卡死就永远等待（表现为短暂停顿后恢复）；游戏收到读错误也会自己重试。
- 新代码：任何 >30s 的慢响应或瞬时错误 → 关 socket → 后台重建整个会话 → 原读取才重试。一次瞬时抖动被放大成数秒的数据断流。
- 预期症状：杂音伴随**画面同步卡顿**（不是只有声音问题），在无效率负载的网络上偶发、成簇出现。

### H3：脏数据被当成有效音频播放（对应开关 T5、T6）—— 嫌疑中
- 旧代码：短读/越界 = 读错误，游戏侧按"流结束/重试"处理；新代码：补零 + 返回成功。
- 全零数据块喂给 ADPCM/PCM 解码器 = 白噪声/咔哒声，且游戏完全不知道数据是坏的。
- 触发面：分片镜像（`ul.*` 末尾分片不齐）、重建/魔改镜像（PVD 容量与文件实际大小不符）、ZSO、以及任何越过文件物理末尾的读取（不少游戏的流读取窗口会稍微越界）。
- 预期症状：**与网络质量无关的恒定细碎杂音**，换网线/换服务器不变；换"干净完整"的镜像后消失。

### H4：`GETTOC` 假实现 / ioops 事件标志等待移除 —— 嫌疑低
- GETTOC 不再返回真实 TOC：个别依赖 TOC 判断的游戏行为可能跑偏；ioops 时序变化影响小。
- 预期症状：个别游戏特有、行为怪异（与杂音的关联性弱）。

### H5：EE 侧改动（IGR 组合键、风扇命令、smbinit 常驻）—— 嫌疑低
- 每项成本都是一次性的或每帧几条指令，不足以影响音频流。IGR 改动属于触发条件变化（Start+Select 现在单击两次/长按会重启），与杂音无关。

## 3. 减法测试矩阵（每构建只改一项）

开关定义在 `modules/iopcore/cdvdman/smb_tuning.h`，全部置 1 = 与提交完全一致。
**每个构建只把一项置 0**，问题游戏上回放同一段 FMV 对比。

| 构建 | 开关置 0 | 验证假设 | 杂音消失 ⇒ 结论 | 杂音依旧 |
|------|----------|----------|-----------------|----------|
| B1 | `SMB_FEAT_ECHO_KEEPALIVE` | H1 | 根因在 Echo（信号量占用/失败触发重连） | → B2 |
| B2 | `SMB_FEAT_SOCK_TIMEOUT` | H2 前半 | 超时把瞬时卡顿放大成重连 | → B3 |
| B3 | `SMB_FEAT_RECONNECT_THREADS` | H2 后半（状态机/线程整体，含 Echo） | 根因在后台线程/静默重试机制 | → B4 |
| B4 | `SMB_FEAT_SHORTREAD_ZEROFILL` | H3 前半 | 补零数据当音频是根因 | → B5 |
| B5 | `SMB_FEAT_OOB_READ_TOLERANT` | H3 后半（越界读语义） | 根因在越界读/魔改镜像兼容逻辑 | → B6 |
| B6 | `SMB_FEAT_TCP_KEEPALIVE` | 排除项 | TCP 层保活（理论上无害） | → 第二轮（H4/H5） |

观察要点（帮助分辨假设）：
1. 拿秒表记录杂音出现的**周期**：≈30s 规律出现 → 强烈指向 H1。
2. 杂音时**画面是否同时卡住**：卡 → H2/重连；只有声音花 → H1 轻症或 H3。
3. 换服务器物理链路（有线直连、换 SMB 服务器）**不变** → 指向 H3（镜像相关）。
4. 换一张完整未魔改的镜像测试：消失 → H3/T5/T6 方向。

## 4. 根因确定后的收敛方向（预案）

- H1 成立：删除 Echo（SMB 服务器很少在分钟级回收会话，TCP 层已有 KeepAlive 兜底）；或把 Echo 失败与"标记断线"解耦，失败仅计数不拆链。
- H2 成立：提高 RCVTIMEO 或只对"确定性断线错误"（ECONNRESET 等）进重连，超时/慢响应重试原请求而不拆链。
- H3 成立：区分"读到文件物理 EOF"（允许补零）与"中途短读/镜像尺寸不符"（恢复报错），不给解码器喂零。
- H4/H5：按第二轮手工回退清单逐项还原（见 §6）。

## 5. 排查过程中顺带发现的问题

1. **越界读已修（本 commit）**：`ps2ip_init()` 读 `info.exports[46]`，但 ps2ip 导出表只有 41 项（0..40），取到的是野指针；已改为置 `NULL`。注意 `smb_AbortConnection()` 目前无调用者（死代码），将来启用前必须先在 SMSTCPIP 导出表补上 `lwip_shutdown`。
2. **拔线监控在游戏内空转**：`getModInfo("netman")` 在游戏内失败（netman 只在菜单加载），所以"拔线 0.5s 模拟开关仓"目前实际靠 30s 读超时/TCP 保活发现，需要另找游戏内链路状态来源（如 SMAP 驱动导出或 PHY 轮询）才能真正生效。
3. **上游旧代码的信号量泄漏**：原 `smb_ReadFile()` 读取出错时不 `SignalSema` 直接返回（下一次读取会死锁）。本提交顺带修了它；减法恢复旧语义时保留了信号量释放，避免把人家的 bug 修回去又引入死锁。
4. **说明与实现不一致**：提交说明写"拔线 0.5 秒内模拟开仓"，但游戏内无 netman（发现 2），实际发现时延受 RCVTIMEO 30s / TCP KeepAlive 60s 限制。

## 6. 第二轮手工回退清单（低嫌疑，先不动）

若 B1–B6 全部未能消除杂音，按顺序手工回退以下改动再测：
1. `modules/iopcore/cdvdfsv/ncmd.c`：恢复 `CD_NCMD_GETTOC` 真实读 TOC（当前被整块注释、直接 `*(int *)buf = 1;`）。
2. `modules/iopcore/cdvdman/ioops.c`：把各入口删掉的 `WaitEventFlag(cdvdman_stat.intr_ef, 1, WEF_AND, NULL);` 恢复，并去掉新增的 `sceCdDiskReady(0);`。
3. `ee_core/src/padhook.c`：恢复 IGR 组合键判定（要求 R1+L1+R2+L2 前置）。
4. `modules/iopcore/cdvdman/cdvdman.c` `_start()`：去掉 `FanSpeedChange_2(0x00);`。
5. `modules/network/smbinit/main.c`：恢复 `MODULE_NO_RESIDENT_END`。

## 7. 辅助观测（可选）

若需要把杂音与内部事件对上时间轴：用 `INGAME_DEBUG=1` + UDP TTY 构建，在
`smbReconnectThread` 的状态切换处和 `smb_Echo()` 调用处各加一行 `DPRINTF`，
杂音出现的时刻对照日志即可一眼区分 H1/H2。
