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

static int isIOBlocked = 0;
static volatile int isIORunning = 0;
static volatile int isIOPending = 0;

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
    // 【卡死根治·最终方案：轮询，不依赖任何唤醒信号】
    // 背景：本 fork 先后用过 SleepThread()/WakeupThread() 与计数信号量两种“事件唤醒”
    // 方案，实机日志均出现同一死结——队列里有请求(ioQueuedART>0)、worker 却空闲
    // (ioActive=-1)且永不取，说明唤醒在“渲染线程一帧内批量入队、worker 恰处于清空
    // 队列后尚未进入等待”的窗口被丢失。为彻底杜绝，worker 改为主动轮询队列：有请求
    // 就处理，队列空则短暂休眠 2ms 再查。丢唤醒在此结构下不可能发生。2ms 空闲轮询对
    // 后台 art 加载的延迟可忽略，CPU 占用也极低。
    while (!gIOTerminate) {
        // 队列取头节点(整段在队列锁内完成)
        WaitSema(gEndSemaId);
        struct io_request_t *req = gReqList;
        if (req) {
            gReqList = req->next;
            if (!gReqList)
                gReqEnd = NULL;
            gActiveRequestType = req->type;
            gActiveRequestData = req->data;
        } else {
            gReqEnd = NULL;   // 队列为空时，保险起见设NULL
            isIOPending = 0;
        }
        SignalSema(gEndSemaId);

        if (!req) {
            // 队列为空：短暂休眠后再轮询，避免忙等空转，也不依赖任何唤醒信号。
            usleep(2000); // 2ms
            continue;
        }

        ioProcessRequest(req);

        WaitSema(gEndSemaId);
        gActiveRequestType = -1;
        gActiveRequestData = NULL;
        SignalSema(gEndSemaId);
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
    if (isIOBlocked)
        return IO_ERR_IO_BLOCKED;

    // check the type before queueing
    if (!ioGetHandler(type))
        return IO_ERR_INVALID_HANDLER;

    WaitSema(gEndSemaId);
    // ==== 在锁区内检查终止状态 ====
    if (gIOTerminate) {
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
    gIOTerminate = 1;
    // 无需唤醒：worker 轮询循环每轮(最多 2ms)都会检查 gIOTerminate 并自行退出。

    // 等待worker线程彻底退出
    while (isIORunning)
        usleep(1000); // 或者YieldCPU(), 可以根据PS2线程API适当替换
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

int ioHasPendingRequests(void)
{
    return isIOPending;
}

// 【诊断用】返回指定类型仍排在队列(gReqList)里、尚未开始处理的请求数量。
// 注意：不含“正在处理中”的那一个(见 ioGetActiveRequestType)。
int ioGetPendingRequestCountByType(int type)
{
    int count = 0;

    WaitSema(gEndSemaId);
    struct io_request_t *req = gReqList;
    while (req) {
        if (req->type == type)
            count++;
        req = req->next;
    }
    SignalSema(gEndSemaId);
    return count;
}

// 【诊断用】返回 io worker 线程“当前正在处理”的请求类型；空闲时为 -1。
// 若该值长时间恒等于某类型(如 IO_CACHE_LOAD_ART)且不变，说明 worker 卡在该请求的
// 处理函数里(例如 itemGetImage 的 SMB open/read 迟迟不返回)，后续请求全部积压。
int ioGetActiveRequestType(void)
{
    return gActiveRequestType;
}

#ifdef __EESIO_DEBUG
static char tbuf[2048];
#endif

int ioPrintf(const char *format, ...)
{
    if (isIORunning == 1)
        WaitSema(gIOPrintfSemaId);

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

    return ret;
}

int ioBlockOps(int block)
{
    ee_thread_status_t status;
    int ThreadID;

    if (block && !isIOBlocked) {
        isIOBlocked = 1;

        ThreadID = GetThreadId();
        ReferThreadStatus(ThreadID, &status);
        ChangeThreadPriority(ThreadID, 90);

        // wait for all io to finish
        while (ioHasPendingRequests())
            usleep(1000);

        ChangeThreadPriority(ThreadID, status.current_priority);

        // now all io should be blocked
    } else if (!block && isIOBlocked) {
        isIOBlocked = 0;
    }

    return IO_OK;
}
