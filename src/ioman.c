#include "include/opl.h"
#include "include/ioman.h"
#include <kernel.h>
#include <string.h>
#include <malloc.h>
#include <stdio.h>
#include <unistd.h>
#ifdef __EESIO_DEBUG
#include <sio.h>
#endif

#define MAX_IO_REQUESTS 16
#define MAX_IO_HANDLERS 8

extern void *_gp;

static volatile int gIOTerminate = 0;

#define THREAD_STACK_SIZE (96 * 1024)

static u8 thread_stack[THREAD_STACK_SIZE] ALIGNED(16);

struct io_request_t
{
    int type;
    void *data;
    struct io_request_t *next;
};

struct io_handler_t
{
    int type;
    io_request_handler_t handler;
};

/// Circular request queue
static struct io_request_t *gReqList;
static struct io_request_t *gReqEnd;
static int gActiveRequestType = -1;
static void *gActiveRequestData;

static struct io_handler_t gRequestHandlers[MAX_IO_HANDLERS];

static int gHandlerCount;

// id of the processing thread
static s32 gIOThreadId;
// lock for queue end
static s32 gEndSemaId;
// ioPrintf sema id
static s32 gIOPrintfSemaId;

static ee_thread_t gIOThread;
static ee_sema_t gQueueSema;

static volatile int isIOBlocked = 0;
static volatile int isIORunning = 0;
static volatile int isIOPending = 0;
#ifdef __DEBUG
// worker 每取出/完成一个请求就递增；看门狗据此判断 worker 是否仍在前进。
static volatile unsigned int gIODiagProgress = 0;
volatile int gIODiagWorkerStage = IO_WS_NONE;
volatile int gIODiagPrintfCallerStage = IO_WS_NONE;
#endif

// 静态池相关，防止内存碎片化导致死机
static struct io_request_t gRequestPool[MAX_IO_REQUESTS];
static int gRequestPoolInUse[MAX_IO_REQUESTS];
static struct io_request_t *AllocIoRequest(void)
{
    for (int i = 0; i < MAX_IO_REQUESTS; ++i) {
        if (!gRequestPoolInUse[i]) {
            gRequestPoolInUse[i] = 1;
            memset(&gRequestPool[i], 0, sizeof(struct io_request_t));
            return &gRequestPool[i];
        }
    }
    return NULL; // 池已满
}
static void FreeIoRequest(struct io_request_t *req)
{
    int idx = req - gRequestPool;
    if (idx >= 0 && idx < MAX_IO_REQUESTS)
        gRequestPoolInUse[idx] = 0;
}

int ioRegisterHandler(int type, io_request_handler_t handler)
{
    WaitSema(gEndSemaId);

    if (handler == NULL) {
        SignalSema(gEndSemaId);
        return IO_ERR_INVALID_HANDLER;
    }

    if (gHandlerCount >= MAX_IO_HANDLERS) {
        SignalSema(gEndSemaId);
        return IO_ERR_TOO_MANY_HANDLERS;
    }

    int i;

    for (i = 0; i < gHandlerCount; ++i) {
        if (gRequestHandlers[i].type == type) {
            SignalSema(gEndSemaId);
            return IO_ERR_DUPLICIT_HANDLER;
        }
    }

    gRequestHandlers[gHandlerCount].type = type;
    gRequestHandlers[gHandlerCount].handler = handler;
    gHandlerCount++;

    SignalSema(gEndSemaId);

    return IO_OK;
}

static io_request_handler_t ioGetHandler(int type)
{
    int i;

    for (i = 0; i < gHandlerCount; ++i) {
        struct io_handler_t *h = &gRequestHandlers[i];

        if (h->type == type)
            return h->handler;
    }

    return NULL;
}

static void ioProcessRequest(struct io_request_t *req)
{
    if (!req)
        return;

    io_request_handler_t hlr = ioGetHandler(req->type);
    if (hlr)
        hlr(req->data);
}

static void ioWorkerThread(void *arg)
{
    // 轮询式 IO worker：不依赖任何唤醒信号，避免“渲染线程一帧内批量入队、worker 恰处于
    // 清空队列后尚未进入等待”的窗口丢唤醒导致请求永久积压。有请求就处理，队列空则休眠
    // 2ms 再查——空闲轮询对后台 art 加载延迟可忽略，CPU 占用也极低。
    while (!gIOTerminate) {
        // 队列取头节点(整段在队列锁内完成)
        IO_DIAG_STAGE(IO_WS_QUEUE_WAIT);
        WaitSema(gEndSemaId);
        IO_DIAG_STAGE(IO_WS_QUEUE_GOT);
        struct io_request_t *req = gReqList;
        if (req) {
            gReqList = req->next;
            if (!gReqList)
                gReqEnd = NULL;
            gActiveRequestType = req->type;
            gActiveRequestData = req->data;
#ifdef __DEBUG
            gIODiagProgress++;
#endif
        } else {
            gReqEnd = NULL;   // 队列为空时，保险起见设NULL
            isIOPending = 0;
        }
        SignalSema(gEndSemaId);

        if (!req) {
            // 队列为空：短暂休眠后再轮询，避免忙等空转，也不依赖任何唤醒信号。
            IO_DIAG_STAGE(IO_WS_IDLE_SLEEP);
            usleep(2000); // 2ms
            continue;
        }

        IO_DIAG_STAGE(IO_WS_DISPATCH);
        ioProcessRequest(req);
        IO_DIAG_STAGE(IO_WS_RETURNED);

#ifdef __DEBUG
        gIODiagProgress++;
#endif
        IO_DIAG_STAGE(IO_WS_FINISH_WAIT);
        WaitSema(gEndSemaId);
        gActiveRequestType = -1;
        gActiveRequestData = NULL;
        SignalSema(gEndSemaId);
        IO_DIAG_STAGE(IO_WS_FINISH_DONE);
        FreeIoRequest(req);
    }
    WaitSema(gEndSemaId);
    // 提前退出时，清理所有线程，防止内存泄露
    struct io_request_t *req = gReqList;
    gReqList = NULL;
    gReqEnd = NULL;
    while (req) {
        struct io_request_t *next = req->next;
        FreeIoRequest(req);
        req = next;
    }
    isIOPending = 0;
    isIORunning = 0;
    SignalSema(gEndSemaId);
    // 此时信号量一定没人再用，可以销毁
    DeleteSema(gEndSemaId);
    DeleteSema(gIOPrintfSemaId);
    ExitDeleteThread();
}

static void ioSimpleActionHandler(void *data)
{
    io_simpleaction_t action = (io_simpleaction_t)data;

    if (action)
        action();
}

void ioInit(void)
{
    for (int i = 0; i < MAX_IO_REQUESTS; ++i)
        gRequestPoolInUse[i] = 0;

    gIOTerminate = 0;
    gHandlerCount = 0;
    gReqList = NULL;
    gReqEnd = NULL;
    gActiveRequestType = -1;
    gActiveRequestData = NULL;

    gIOThreadId = 0;

    gQueueSema.init_count = 1;
    gQueueSema.max_count = 1;
    gQueueSema.option = 0;

    gEndSemaId = CreateSema(&gQueueSema);
    gIOPrintfSemaId = CreateSema(&gQueueSema);

    // default custom simple action handler
    ioRegisterHandler(IO_CUSTOM_SIMPLEACTION, &ioSimpleActionHandler);

    gIOThread.attr = 0;
    gIOThread.stack_size = THREAD_STACK_SIZE;
    gIOThread.gp_reg = &_gp;
    gIOThread.func = &ioWorkerThread;
    gIOThread.stack = thread_stack;
    gIOThread.initial_priority = 32;

    isIORunning = 1;
    isIOPending = 0;
    gIOThreadId = CreateThread(&gIOThread);
    StartThread(gIOThreadId, NULL);
}

static int ioPutRequestInternal(int type, void *data, int unique)
{
    if (isIOBlocked) {
#ifdef __DEBUG
        LOG("[IO_QUEUE_REJECT] reason=blocked type=%d\n", type);
#endif
        return IO_ERR_IO_BLOCKED;
    }

    // check the type before queueing
    if (!ioGetHandler(type))
        return IO_ERR_INVALID_HANDLER;

    WaitSema(gEndSemaId);
    // ==== 在锁区内检查终止状态 ====
    if (gIOTerminate) {
#ifdef __DEBUG
        LOG("[IO_QUEUE_REJECT] reason=terminating type=%d\n", type);
#endif
        SignalSema(gEndSemaId);
        return IO_ERR_IO_BLOCKED; // 自定义错误码
    }

    if (unique) {
        struct io_request_t *req;

        if ((gActiveRequestType == type) && (gActiveRequestData == data)) {
            SignalSema(gEndSemaId);
            return IO_OK;
        }

        for (req = gReqList; req != NULL; req = req->next) {
            if ((req->type == type) && (req->data == data)) {
                SignalSema(gEndSemaId);
                return IO_OK;
            }
        }
    }

    // We don't have to lock the tip of the queue...
    // If it exists, it won't be touched, if it does not exist, it is not being processed
    struct io_request_t *new_req = AllocIoRequest();
    if (!new_req) {
#ifdef __DEBUG
        LOG("[IO_QUEUE_REJECT] reason=pool_full type=%d\n", type);
#endif
        SignalSema(gEndSemaId);
        return IO_ERR_IO_BLOCKED; // 注意定义该错误码
    }
    isIOPending = 1; // 标记“有队列请求”
    new_req->next = NULL;
    new_req->type = type;
    new_req->data = data;

    if (!gReqList) {
        gReqList = new_req;
        gReqEnd = new_req;
    } else {
        gReqEnd->next = new_req;
        gReqEnd = new_req;
    }

    SignalSema(gEndSemaId);

    // 无需唤醒 worker：worker 采用轮询，会在下一轮(最多 2ms 后)自动发现新入队的请求。

    return IO_OK;
}

int ioPutRequest(int type, void *data)
{
    return ioPutRequestInternal(type, data, 0);
}

int ioPutRequestUnique(int type, void *data)
{
    return ioPutRequestInternal(type, data, 1);
}

int ioRemoveRequestsWithCleanup(int type, io_request_cleanup_t cleanup)
{
    void *removedData[MAX_IO_REQUESTS];

    WaitSema(gEndSemaId);

    int count = 0;
    struct io_request_t *req = gReqList;
    struct io_request_t *last = NULL;

    while (req) {
        if (req->type == type) {
            struct io_request_t *next = req->next;

            if (last)
                last->next = next;

            if (req == gReqList)
                gReqList = next;

            if (req == gReqEnd)
                gReqEnd = last;

            removedData[count++] = req->data;
            FreeIoRequest(req);

            req = next;
        } else {
            last = req;
            req = req->next;
        }
    }

    if (!gReqList && gActiveRequestType < 0)
        isIOPending = 0;

    SignalSema(gEndSemaId);

    // 清理函数可能获取其他锁，必须离开队列锁后再调用，避免锁顺序反转。
    if (cleanup) {
        for (int i = 0; i < count; i++)
            cleanup(removedData[i]);
    }

    return count;
}

int ioRemoveRequests(int type)
{
    return ioRemoveRequestsWithCleanup(type, NULL);
}

void ioEnd(void)
{
#ifdef __DEBUG
    LOG("[IO_END] request terminating=%d pending=%d active=%d\n",
        gIOTerminate, ioHasPendingRequests(), ioGetActiveRequestType());
#endif
    gIOTerminate = 1;
    // 无需唤醒：worker 轮询循环每轮(最多 2ms)都会检查 gIOTerminate 并自行退出。

    // 等待worker线程彻底退出
    unsigned int waitTicks = 0;
    while (isIORunning) {
#ifdef __DEBUG
        if ((++waitTicks % 1000) == 0)
            LOG("[IO_END_WAIT] pending=%d active=%d\n",
                ioHasPendingRequests(), ioGetActiveRequestType());
#endif
        usleep(1000); // 或者YieldCPU(), 可以根据PS2线程API适当替换
    }
#ifdef __DEBUG
    LOG("[IO_END] complete\n");
#endif
}

int ioGetPendingRequestCount(void)
{
    int count = 0;

    WaitSema(gEndSemaId);
    struct io_request_t *req = gReqList;
    while (req) {
        count++;
        req = req->next;
    }

    SignalSema(gEndSemaId);
    return count;
}

int ioGetActiveRequestType(void)
{
    int type;

    WaitSema(gEndSemaId);
    type = gActiveRequestType;
    SignalSema(gEndSemaId);

    return type;
}

int ioIsBlocked(void)
{
    return isIOBlocked;
}

int ioIsTerminating(void)
{
    return gIOTerminate;
}

int ioHasPendingRequests(void)
{
    return isIOPending;
}

#ifdef __EESIO_DEBUG
static char tbuf[2048];
#endif

int ioPrintf(const char *format, ...)
{
#ifdef __DEBUG
    // 只跟踪 worker 自己的 LOG：记录进入前的阶段，返回时恢复。
    int diagIsWorker = (gIOThreadId > 0 && GetThreadId() == gIOThreadId);
    int diagPrevStage = gIODiagWorkerStage;
    if (diagIsWorker) {
        gIODiagPrintfCallerStage = diagPrevStage;
        gIODiagWorkerStage = IO_WS_PRINTF_SEMA_WAIT;
    }
#endif
    if (isIORunning == 1)
        WaitSema(gIOPrintfSemaId);
#ifdef __DEBUG
    if (diagIsWorker)
        gIODiagWorkerStage = IO_WS_PRINTF_WRITE;
#endif

    va_list args;
    va_start(args, format);
#ifdef __EESIO_DEBUG
    int ret = vsnprintf((char *)tbuf, sizeof(tbuf), format, args);
    sio_putsn(tbuf);
#else
    int ret = vprintf(format, args);
#endif
    va_end(args);

    if (isIORunning == 1)
        SignalSema(gIOPrintfSemaId);

#ifdef __DEBUG
    if (diagIsWorker)
        gIODiagWorkerStage = diagPrevStage;
#endif
    return ret;
}

#ifdef __DEBUG
void ioGetDiagState(int *threadId, int *endSemaId, int *printfSemaId, int *activeType, unsigned int *progress)
{
    // 故意不取 gEndSemaId：看门狗必须在 worker 或队列锁异常时仍能读到快照。
    if (threadId)
        *threadId = gIOThreadId;
    if (endSemaId)
        *endSemaId = gEndSemaId;
    if (printfSemaId)
        *printfSemaId = gIOPrintfSemaId;
    if (activeType)
        *activeType = gActiveRequestType;
    if (progress)
        *progress = gIODiagProgress;
}

int ioDiagPrintfNoLock(const char *format, ...)
{
    // 仅主线程看门狗使用，静态缓冲区无需加锁；只格式化整数/字符串，
    // newlib 的字符串 vsnprintf 不会获取 FILE 锁，也不会因此调用 malloc。
    // 必须 16 字节以上对齐：fioWrite() 会把起始地址未对齐的头部（16 - addr%16 字节）
    // 放进 RPC 参数单独发送，其余部分由 IOP 侧从对齐地址取。实机日志中每行只收到
    // 8 字节 "[ART_WD]"，与旧构建里 diagBuf 地址 %16 == 8 完全吻合；对齐后整行一次发送。
    static char diagBuf[512] ALIGNED(64);
    va_list args;
    va_start(args, format);
    int len = vsnprintf(diagBuf, sizeof(diagBuf), format, args);
    va_end(args);

    if (len < 0)
        return len;
    if (len >= (int)sizeof(diagBuf))
        len = sizeof(diagBuf) - 1;

#ifdef __EESIO_DEBUG
    sio_putsn(diagBuf);
#else
    // 直接写 STDOUT_FILENO（libcglue 映射到 tty0:，UDPTTY 同样会收到），
    // 绕过 stdout 的 newlib FILE 递归锁。短写时继续写剩余部分。
    int written = 0;
    while (written < len) {
        int ret = write(STDOUT_FILENO, diagBuf + written, len - written);
        if (ret <= 0)
            break;
        written += ret;
    }
#endif
    return len;
}

const char *ioDiagWorkerStageName(int stage)
{
    switch (stage) {
        case IO_WS_NONE:
            return "none";
        case IO_WS_QUEUE_WAIT:
            return "queue_wait";
        case IO_WS_QUEUE_GOT:
            return "queue_got";
        case IO_WS_IDLE_SLEEP:
            return "idle_sleep";
        case IO_WS_DISPATCH:
            return "dispatch";
        case IO_WS_RETURNED:
            return "returned";
        case IO_WS_FINISH_WAIT:
            return "finish_wait";
        case IO_WS_FINISH_DONE:
            return "finish_done";
        case IO_WS_PRINTF_SEMA_WAIT:
            return "printf_sema_wait";
        case IO_WS_PRINTF_WRITE:
            return "printf_write";
        case IO_WS_ART_BEGIN_LOG:
            return "art_begin_log";
        case IO_WS_ART_GET_IMAGE:
            return "art_get_image";
        case IO_WS_ART_END_LOG:
            return "art_end_log";
        case IO_WS_ART_COMPACT:
            return "art_compact";
        case IO_WS_ART_COUNTERS:
            return "art_counters";
        case IO_WS_ART_MUTEX_WAIT:
            return "art_mutex_wait";
        case IO_WS_ART_MUTEX_HELD:
            return "art_mutex_held";
        case IO_WS_ART_MUTEX_UNLOCKED:
            return "art_mutex_unlocked";
        case IO_WS_ART_STALE_CLEAR:
            return "art_stale_clear";
        case IO_WS_ART_DEC_LOADING:
            return "art_dec_loading";
        case IO_WS_ART_FREE_REQ:
            return "art_free_req";
        case IO_WS_ART_FREE_REQ_DONE:
            return "art_free_req_done";
        case IO_WS_ART_EARLY_EXIT:
            return "art_early_exit";
        default:
            return "?";
    }
}
#endif

int ioBlockOps(int block)
{
    ee_thread_status_t status;
    int ThreadID;

#ifdef __DEBUG
    LOG("[IO_BLOCK] request=%d blocked=%d terminating=%d pending=%d active=%d\n",
        block, isIOBlocked, gIOTerminate, ioHasPendingRequests(), ioGetActiveRequestType());
#endif

    if (block && !isIOBlocked) {
        isIOBlocked = 1;

        ThreadID = GetThreadId();
        ReferThreadStatus(ThreadID, &status);
        ChangeThreadPriority(ThreadID, 90);

        // 等待时每秒记录一次，区分普通清理等待和 worker 永久不返回。
        unsigned int waitTicks = 0;
        while (ioHasPendingRequests()) {
#ifdef __DEBUG
            if ((++waitTicks % 1000) == 0)
                LOG("[IO_BLOCK_WAIT] blocked=%d terminating=%d pending=%d active=%d\n",
                    isIOBlocked, gIOTerminate, ioGetPendingRequestCount(), ioGetActiveRequestType());
#endif
            usleep(1000);
        }

        ChangeThreadPriority(ThreadID, status.current_priority);

        // now all io should be blocked
    } else if (!block && isIOBlocked) {
        isIOBlocked = 0;
    }

#ifdef __DEBUG
    LOG("[IO_BLOCK] complete=%d blocked=%d terminating=%d pending=%d active=%d\n",
        block, isIOBlocked, gIOTerminate, ioHasPendingRequests(), ioGetActiveRequestType());
#endif

    return IO_OK;
}
