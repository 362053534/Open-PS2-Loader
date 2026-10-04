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
