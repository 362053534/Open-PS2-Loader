/*
 * SMB 模式 FMV 杂音排查——逐项减法(feature subtraction)开关矩阵
 *
 * 用法：每次构建只把其中【一项】从 1 改为 0，烧录后在问题游戏上验证同一段 FMV。
 * 全部置 1 时与提交 6254970（“改进SMB-拔线模拟开关仓，断线后无限重连”）行为完全一致。
 *
 * 依赖关系：
 *   - SMB_FEAT_ECHO_KEEPALIVE 依附于 SMB_FEAT_RECONNECT_THREADS；线程关掉后心跳自然消失。
 *   - SMB_FEAT_SHORTREAD_ZEROFILL 同时作用于 smb.c（ReadFile 报零填充短读）
 *     与 device-smb.c（DeviceReadSectors 对短读的处理）。
 */
#ifndef SMB_TUNING_H
#define SMB_TUNING_H

/* T1: 后台线程每 30 秒发送一次 SMB_COM_ECHO 保活包。
 *     嫌疑：Echo 持有 smb_io_sema 整整一个往返；服务器响应慢/不响应时，
 *     游戏读盘被憋住（最坏 30 秒），Echo 失败还会把整个会话标记为断线并触发全量重连。 */
#define SMB_FEAT_ECHO_KEEPALIVE 1

/* T2: SMB 套接字设置 SO_SNDTIMEO / SO_RCVTIMEO = 30 秒。
 *     嫌疑：旧代码永久阻塞等待；新代码超时即视为断线 -> 拆链重连，
 *     服务器偶发慢响应（磁盘休眠唤醒、网络抖动）会被放大成数秒级读盘停顿。*/
#define SMB_FEAT_SOCK_TIMEOUT 1

/* T3: SMB 套接字设置 SO_KEEPALIVE + TCP_KEEPALIVE = 60 秒（TCP 层保活探测）。*/
#define SMB_FEAT_TCP_KEEPALIVE 1

/* T4: 后台重连线程(2s 周期,优先级40) + 链路监控线程(0.5s 周期) + 读取失败静默等待重试。
 *     置 0：回到旧行为——不开线程，读失败立即向游戏返回读错误。*/
#define SMB_FEAT_RECONNECT_THREADS 1

/* T5: 短读(服务器返回 0 字节/读到文件尾)按“成功+补零”返回。
 *     置 0：恢复旧行为——短读/失败立即作为读错误上报给游戏，不补零。*/
#define SMB_FEAT_SHORTREAD_ZEROFILL 1

/* T6: 读取越过 PVD 标称容量时继续读、读不到补零（兼容 D9 转 D5 类魔改镜像）。
 *     置 0：恢复旧行为——起始 LSN 越界立即报 SCECdErIPI，跨界截断并报 SCECdErEOM。*/
#define SMB_FEAT_OOB_READ_TOLERANT 1

#endif /* SMB_TUNING_H */
