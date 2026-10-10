/*
 * SMB 模式 FMV 杂音修复——patch-1（f91c3cf）上的 D1/D2/D3/E1/E2/E4 尝试开关。
 *
 * 第三轮实机结论（详见 notes/smb_fmv_noise_analysis.md）：
 *   D1（数据面无超时）            干净
 *   D2（+Echo 临时设 setsockopt） 概率杂音
 *   E4（Echo 频率放大 60 倍）     杂音"跟之前一样"，没有变频繁
 *   E1（Echo 永不触发）           干净
 *   E2（Echo 不碰 setsockopt）    多次测试干净
 * ⇒ 噪声需要两个条件同时成立：setsockopt(SO_RCVTIMEO/SO_SNDTIMEO) 真的被执行过
 *   + 一次 Echo 往返真的发生。而 E4 没有让杂音变多，说明它不是"每次 Echo 抖一下"，
 *   而是那次 setsockopt 一旦执行就在 ps2ip 的收包路径上留下了持久影响
 *   （后续 netconn_recv 不再走裸 WaitSema）。
 * ⇒ 结论：SMB 套接字上**永远不要**设 SO_RCVTIMEO/SO_SNDTIMEO，一次都不行。
 *   保活探测改用 SMB_FEAT_ECHO_TIMEOUT=2（MSG_DONTWAIT 自己轮询）。
 *
 * 下面的默认值 = 第三轮验证过的 E2 配置，也就是建议合入 patch-1 的最终形态。
 *
 * 背景（在 6254970 那棵老树上用 OBSERVE 成对日志取到的证据）：
 *   * BASE / CURR 两份日志在**数据面上完全一致** —— 读耗时 15.10 vs 15.44ms，
 *     h50/h100/h250/h500 全 0，zero/shrt 全 0，吞吐 1018 vs 1017 KB/s。
 *     ⇒ 6254970 既没让数据变慢，也没让数据变脏。
 *   * 唯一落在"每一次收包"热路径上的差异是 SO_SNDTIMEO/SO_RCVTIMEO 生效后，
 *     每次 netconn_recv() 都要走 sys_arch_sem_wait() 的超时分支
 *     （GetSystemTime + USec2SysClock + SetAlarm + WaitSema + CancelAlarm
 *       + GetSystemTime + SysClock2USec），而不再是裸 WaitSema。
 *   * 把它降到"每次 recv 只装一次 alarm"（C2）仍然有杂音 ⇒ 问题不是装填频率，
 *     而是这条分支存在本身。
 *   * 第二个（较小的）来源在后台线程组里；patch-1 已经把 500ms 的链路监控线程
 *     合并进重连线程、并且 Echo 改成"空闲 120s 才发"，所以那一半在这里已经消失。
 *
 * 本文件只放 3 个开关，供 CI 用 sed 逐项改动，生成 D1/D2/D3 三个构建。
 */
#ifndef SMB_TUNING_H
#define SMB_TUNING_H

/* ---- D1：数据面不带超时 ----------------------------------------------------
 * 置 0 时不再 setsockopt(SO_SNDTIMEO / SO_RCVTIMEO)，
 * conn->recv_timeout == 0 ⇒ 每一次收包退回裸 WaitSema，零额外系统调用。
 * 代价：单次收发卡死时不会被超时打断（失联检测见 SMB_FEAT_ECHO_TIMEOUT）。 */
#define SMB_FEAT_SOCK_TIMEOUT 0

/* 只拆收方向（netconn_recv 是热路径；发方向几乎不触发）。 */
#define SMB_FEAT_RCV_TIMEOUT 1

/* ---- D2：让 Echo 这一次往返有超时上限 --------------------------------------
 *   0 = 不设上限（收包用裸 WaitSema，服务器不回就一直等）
 *   1 = smb_Echo() 前后临时 setsockopt(SO_RCVTIMEO/SO_SNDTIMEO)，用完清回 0。
 *       数据面零开销，但那一次收包会走 lwip 的 sys_arch_sem_wait alarm 分支。
 *   2 = 完全不碰 setsockopt：用 plwip_recv(MSG_DONTWAIT) 自己轮询等回包，
 *       收到就交给正常解析。数据面与 0 完全一致，且超时上限可控。
 *
 * 需要 SMB_FEAT_SOCK_TIMEOUT=0 才有意义（否则超时本来就在套接字上）。 */
#define SMB_FEAT_ECHO_TIMEOUT 2

/* 模式 2 用：等 Echo 回包的上限（毫秒）与轮询间隔（毫秒）。 */
#define SMB_ECHO_TIMEOUT_MS 3000
#define SMB_ECHO_POLL_MS   20

/* ---- D3：重连线程空闲轮询周期（微秒）--------------------------------------
 * patch-1 默认 2000000（2s）。放慢到 5000000（5s）可以把周期性唤醒再降 2.5 倍。
 * SMB_ECHO_IDLE_TICKS 由本文件一并给出，保证"空闲 120s 才 Echo"不变：
 *   2s × 60 tick = 120s      5s × 24 tick = 120s */
#define SMB_RECONNECT_INTERVAL_US 2000000
#define SMB_ECHO_IDLE_TICKS       60

#endif /* SMB_TUNING_H */
