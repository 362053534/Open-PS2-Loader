#ifndef __IOMAN_H
#define __IOMAN_H

// Input output manager
// asynchronous io handling thread with worker queue

#define IO_OK                       0
#define IO_ERR_UNKNOWN_REQUEST_TYPE -1
#define IO_ERR_TOO_MANY_HANDLERS    -2
#define IO_ERR_DUPLICIT_HANDLER     -3
#define IO_ERR_INVALID_HANDLER      -4
#define IO_ERR_IO_BLOCKED           -5

typedef void (*io_request_handler_t)(void *request);
typedef void (*io_request_cleanup_t)(void *request);

typedef void (*io_simpleaction_t)(void);

/** initializes the io worker thread */
void ioInit(void);

/** deinitializes the io worker thread */
void ioEnd(void);

/** registers a handler for a certain request type */
int ioRegisterHandler(int type, io_request_handler_t handler);

/** schedules a new request into the pending request list
 * @note The data are not freed! */
int ioPutRequest(int type, void *data);

/** schedules a request unless an identical type/data request is already pending */
int ioPutRequestUnique(int type, void *data);

/** removes all requests of a given type from the queue
 * @param type the type of the requests to remove
 * @return the count of the requests removed */
int ioRemoveRequests(int type);

/** 删除指定类型的待处理请求，并在队列锁外清理请求数据。 */
int ioRemoveRequestsWithCleanup(int type, io_request_cleanup_t cleanup);

/** returns the count of pending requests */
int ioGetPendingRequestCount(void);

/** returns the type currently being processed by the IO worker, or -1 if idle */
int ioGetActiveRequestType(void);

/** 返回 IO 队列是否被 ioBlockOps() 阻塞 */
int ioIsBlocked(void);

/** 返回 IO worker 是否已经收到终止请求 */
int ioIsTerminating(void);

/** returns nonzero if there are any pending io requests */
int ioHasPendingRequests(void);

/** returns nonzero if the io thread is running */
int ioIsRunning(void);

/** Helper thread safe printf */
int ioPrintf(const char *format, ...);

#ifdef __DEBUG
/** 诊断专用：不加任何锁读取 IO worker 状态，供主线程看门狗在 worker 卡死时使用。 */
void ioGetDiagState(int *threadId, int *endSemaId, int *printfSemaId, int *activeType, unsigned int *progress);

/** 诊断专用输出：静态缓冲区 + 直接 write()/sio，不经过 ioPrintf 信号量、
 * newlib stdout FILE 锁和 malloc 锁，避免看门狗被同一把锁拖死。只能在主线程调用。 */
int ioDiagPrintfNoLock(const char *format, ...);

// 诊断专用：IO worker 当前所处阶段。worker 无锁写入，主线程看门狗无锁读取，
// 即使 worker 永久阻塞也能知道它停在哪一步。
enum {
    IO_WS_NONE = 0,
    IO_WS_QUEUE_WAIT = 1,    // 取请求前 WaitSema(gEndSemaId)
    IO_WS_QUEUE_GOT = 2,     // 已取到请求（或队列为空）
    IO_WS_IDLE_SLEEP = 3,    // 队列为空，usleep 轮询
    IO_WS_DISPATCH = 4,      // 调用 ioProcessRequest 之前
    IO_WS_RETURNED = 5,      // ioProcessRequest 已返回
    IO_WS_FINISH_WAIT = 6,   // 完成后 WaitSema(gEndSemaId)
    IO_WS_FINISH_DONE = 7,   // 已清 active，准备 FreeIoRequest
    IO_WS_PRINTF_SEMA_WAIT = 20, // worker 内 ioPrintf：等待 ioPrintf 信号量
    IO_WS_PRINTF_WRITE = 21,     // worker 内 ioPrintf：vprintf（newlib stdout 锁）
    IO_WS_ART_BEGIN_LOG = 40,
    IO_WS_ART_GET_IMAGE = 41,
    IO_WS_ART_END_LOG = 42,
    IO_WS_ART_COMPACT = 43,
    IO_WS_ART_COUNTERS = 44,
    IO_WS_ART_MUTEX_WAIT = 45,     // 发布结果前等待 texLoadingMutex
    IO_WS_ART_MUTEX_HELD = 46,
    IO_WS_ART_MUTEX_UNLOCKED = 47,
    IO_WS_ART_STALE_CLEAR = 48,    // cacheClearExpiredItem（含 free 纹理）
    IO_WS_ART_DEC_LOADING = 49,    // cacheDecreaseLoading（texLoadingMutex）
    IO_WS_ART_FREE_REQ = 50,       // free(ioReq)（newlib malloc 锁）
    IO_WS_ART_FREE_REQ_DONE = 51,
    IO_WS_ART_EARLY_EXIT = 52,     // 早退路径（释放槽位/计数/free）
};
extern volatile int gIODiagWorkerStage;
// worker 进入 ioPrintf 时，记录调用前的阶段，便于判断是哪条 LOG 卡住。
extern volatile int gIODiagPrintfCallerStage;
#define IO_DIAG_STAGE(s) (gIODiagWorkerStage = (s))
const char *ioDiagWorkerStageName(int stage);
#else
#define IO_DIAG_STAGE(s) ((void)0)
#endif

/** Helper function. Will flush the io operation list
 (wait for all io ops requested to end) and then
 issue a blocking flag that will mean no io
 operation will get in.
 @param block If nonzero, will issue blocking, otherwise it will unblock
*/
int ioBlockOps(int block);

#ifdef __DEBUG
#define PREINIT_LOG(...) printf(__VA_ARGS__)
#define LOG(...)         ioPrintf(__VA_ARGS__)
#else
#define PREINIT_LOG(...)
#define LOG(...)
#endif

#endif
