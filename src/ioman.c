#include "include/opl.h"
#include "include/ioman.h"
#include "include/debugdiag.h"
#include "include/textures.h"
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

// 独立看门狗不依赖主线程或 IO worker 的调用链，才能报告主线程停在渲染锁/GS 等待的情况。
#define GUI_DIAG_WATCHDOG_STACK_SIZE (16 * 1024)
#define GUI_DIAG_WATCHDOG_INTERVAL_US 20000
#define GUI_DIAG_WATCHDOG_STALL_TICKS 150
static u8 guiDiagWatchdogStack[GUI_DIAG_WATCHDOG_STACK_SIZE] ALIGNED(16);
static ee_thread_t guiDiagWatchdogThreadDef;
static s32 guiDiagWatchdogThreadId = -1;
static volatile int guiDiagWatchdogTerminate;
static volatile int guiDiagWatchdogRunning;
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

#ifdef __DEBUG
static const char *ioDiagWaitTypeName(u32 waitType)
{
    switch (waitType) {
        case 0:
            return "none";
        case 1:
            return "sleep";
        case 2:
            return "sema";
        default:
            return "?";
    }
}

static void ioDiagReportSema(const char *name, int semaId)
{
    ee_sema_t sema;
    memset(&sema, 0, sizeof(sema));
    int ret = (semaId > 0) ? ReferSemaStatus(semaId, &sema) : -1;
    ioDiagPrintfMonitorNoLock("[GUI_WD] sema name=%s id=%d ret=%d count=%d max=%d wait_threads=%d\n",
                              name, semaId, ret, sema.count, sema.max_count, sema.wait_threads);
}

static void guiDiagReportStall(unsigned int stalledTicks)
{
    int ioThreadId = -1;
    int ioEndSemaId = -1;
    int ioPrintfSemaId = -1;
    int activeType = -1;
    unsigned int progress = 0;
    int pending = ioDiagGetPendingNoLock();
    int mainThreadId = gGuiDiagStageThread;
    int guiQueueSemaId = -1;
    int guiLockSemaId = -1;
    int menuSemaId = -1;
    int menuListSemaId = -1;
    int isHires = 0;
    int videoMode = -1;
    int width = 0;
    int height = 0;
    int activeBuffer = -1;
    int texLoading = 0;
    int activeArt = 0;
    int activeArtItem = -1;
    const char *activeSuffix = "-";
    const char *activeValue = "-";
    const char *texStage;
    int texStageThread = -1;
    int mutexOwner = -1;
    int mutexOwnerSite = 0;
    int mutexWorkerWait = 0;
    int mutexOtherWaiter = -1;
    int mutexOtherWaitSite = 0;
    int mutexLastUnlockThread = -1;
    int mutexLastUnlockSite = 0;
    unsigned int mutexLocks = 0;
    int pthreadBG = 0;
    int pthreadCOV = 0;
    int pthreadICO = 0;
    ee_thread_status_t mainStatus;
    ee_thread_status_t ioStatus;

    ioGetDiagState(&ioThreadId, &ioEndSemaId, &ioPrintfSemaId, &activeType, &progress);
    guiDiagGetSemaIds(&guiQueueSemaId, &guiLockSemaId);
    menuDiagGetSemaIds(&menuSemaId, &menuListSemaId);
    rmDiagGetState(&isHires, &videoMode, &width, &height, &activeBuffer);
    texDiagGetRequestState(&texLoading, &activeArt, &activeSuffix, &activeValue, &activeArtItem);
    texDiagGetMutexState(&mutexOwner, &mutexOwnerSite, &mutexWorkerWait, &mutexOtherWaiter,
                         &mutexOtherWaitSite, &mutexLastUnlockThread, &mutexLastUnlockSite, &mutexLocks);
    texDiagGetThreadState(&pthreadBG, &pthreadCOV, &pthreadICO);
    texStage = texGetDiagLastStage(&texStageThread);

    // 先写出阶段和 Coverflow 快照，再调用可能依赖 IOP 的线程状态查询。
    ioDiagPrintfMonitorNoLock("[GUI_WD] stall frame=%u heartbeat=%u stagnant_ticks=%u stage=%d(%s) stage_frame=%u stage_thread=%d element=%s\n",
                              (unsigned int)gGuiDiagFrame, (unsigned int)gGuiDiagHeartbeat, stalledTicks,
                              gGuiDiagStage, guiDiagStageName(gGuiDiagStage), (unsigned int)gGuiDiagStageFrame,
                              gGuiDiagStageThread, gGuiDiagElementName ? (const char *)gGuiDiagElementName : "-");
    ioDiagPrintfMonitorNoLock("[GUI_WD] menu game_id=%d cursor=%d/%d mode=%d coverflow=%d text=%s\n",
                              gMenuDiagCurrentId, gMenuDiagCursor, gMenuDiagItemCount, gMenuDiagMenuMode,
                              gMenuDiagIsCoverflow, (const char *)gMenuDiagCurrentText);
    ioDiagPrintfMonitorNoLock("[CF_DIAG] current=%d start=%d dir=%d steps=%d render=%d center=%d tex_phase=%d tex_item=%d\n",
                              gCoverflowDiagCurrentId, gCoverflowDiagStartId, gCoverflowDiagDirection,
                              gCoverflowDiagSteps, gCoverflowDiagRenderCount, gCoverflowDiagRenderIndex,
                              gCoverflowDiagTexturePhase, gCoverflowDiagTextureItemId);

    memset(&mainStatus, 0, sizeof(mainStatus));
    memset(&ioStatus, 0, sizeof(ioStatus));
    int mainRet = (mainThreadId > 0) ? ReferThreadStatus(mainThreadId, &mainStatus) : -1;
    int ioRet = (ioThreadId > 0) ? ReferThreadStatus(ioThreadId, &ioStatus) : -1;

    ioDiagPrintfMonitorNoLock("[GUI_WD] main id=%d ret=%d status=0x%02x wait_type=%u(%s) wait_id=%u cur_prio=%d\n",
                              mainThreadId, mainRet, mainStatus.status, mainStatus.waitType,
                              ioDiagWaitTypeName(mainStatus.waitType), mainStatus.waitId,
                              mainStatus.current_priority);
    ioDiagPrintfMonitorNoLock("[GUI_WD] io id=%d ret=%d status=0x%02x wait_type=%u(%s) wait_id=%u cur_prio=%d active=%d pending=%d progress=%u worker_stage=%d(%s)\n",
                              ioThreadId, ioRet, ioStatus.status, ioStatus.waitType,
                              ioDiagWaitTypeName(ioStatus.waitType), ioStatus.waitId,
                              ioStatus.current_priority, activeType, pending, progress,
                              gIODiagWorkerStage, ioDiagWorkerStageName(gIODiagWorkerStage));
    ioDiagPrintfMonitorNoLock("[GUI_WD] io_sema=%d gui_queue=%d gui_lock=%d menu=%d menu_list=%d\n",
                              ioEndSemaId, guiQueueSemaId, guiLockSemaId, menuSemaId, menuListSemaId);
    ioDiagReportSema("io_queue", ioEndSemaId);
    ioDiagReportSema("io_printf", ioPrintfSemaId);
    ioDiagReportSema("file_lock", texGetFileLockSemaId());
    ioDiagReportSema("gui_queue", guiQueueSemaId);
    ioDiagReportSema("gui_lock", guiLockSemaId);
    ioDiagReportSema("menu", menuSemaId);
    ioDiagReportSema("menu_list", menuListSemaId);
    ioDiagPrintfMonitorNoLock("[GUI_WD] rm hires=%d vmode=%d size=%dx%d active_buffer=%d\n",
                              isHires, videoMode, width, height, activeBuffer);
    ioDiagPrintfMonitorNoLock("[GUI_WD] tex loading=%d active=%d suffix=%s item=%d value=%s stage=%s stage_thread=%d pthread_bg=%d pthread_cov=%d pthread_ico=%d\n",
                              texLoading, activeArt, activeSuffix ? activeSuffix : "-", activeArtItem,
                              activeValue ? activeValue : "-", texStage ? texStage : "-", texStageThread,
                              pthreadBG, pthreadCOV, pthreadICO);
    ioDiagPrintfMonitorNoLock("[GUI_WD] tex_mutex owner=%d owner_site=%d worker_wait_site=%d other_waiter=%d other_wait_site=%d last_unlock=%d/%d locks=%u\n",
                              mutexOwner, mutexOwnerSite, mutexWorkerWait, mutexOtherWaiter,
                              mutexOtherWaitSite, mutexLastUnlockThread, mutexLastUnlockSite, mutexLocks);
}

static void guiDiagWatchdogThread(void *arg)
{
    unsigned int lastHeartbeat = 0;
    unsigned int stagnantTicks = 0;
    unsigned int lastReport = 0;
    int reported = 0;

    (void)arg;
    guiDiagWatchdogRunning = 1;
    while (!guiDiagWatchdogTerminate) {
        usleep(GUI_DIAG_WATCHDOG_INTERVAL_US);

        if (!gGuiDiagEnabled) {
            lastHeartbeat = gGuiDiagHeartbeat;
            stagnantTicks = 0;
            reported = 0;
            continue;
        }

        unsigned int heartbeat = gGuiDiagHeartbeat;
        if (heartbeat != lastHeartbeat) {
            if (reported)
                ioDiagPrintfMonitorNoLock("[GUI_WD] recovered frame=%u heartbeat=%u\n",
                                          (unsigned int)gGuiDiagFrame, heartbeat);
            lastHeartbeat = heartbeat;
            stagnantTicks = 0;
            lastReport = 0;
            reported = 0;
            continue;
        }

        if (stagnantTicks < 0xFFFFFFFFU)
            stagnantTicks++;
        if (stagnantTicks >= GUI_DIAG_WATCHDOG_STALL_TICKS &&
            (!reported || stagnantTicks - lastReport >= GUI_DIAG_WATCHDOG_STALL_TICKS)) {
            lastReport = stagnantTicks;
            reported = 1;
            guiDiagReportStall(stagnantTicks);
        }
    }

    guiDiagWatchdogRunning = 0;
    ExitDeleteThread();
}
#endif

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

#ifdef __DEBUG
    guiDiagWatchdogTerminate = 0;
    guiDiagWatchdogRunning = 0;
    guiDiagWatchdogThreadDef.attr = 0;
    guiDiagWatchdogThreadDef.stack_size = GUI_DIAG_WATCHDOG_STACK_SIZE;
    guiDiagWatchdogThreadDef.gp_reg = &_gp;
    guiDiagWatchdogThreadDef.func = &guiDiagWatchdogThread;
    guiDiagWatchdogThreadDef.stack = guiDiagWatchdogStack;
    // 比 GUI 主线程略高，主线程忙等而未进入 sleep 时也能产生停顿快照。
    guiDiagWatchdogThreadDef.initial_priority = 30;
    guiDiagWatchdogThreadId = CreateThread(&guiDiagWatchdogThreadDef);
    StartThread(guiDiagWatchdogThreadId, NULL);
#endif
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
#ifdef __DEBUG
    guiDiagWatchdogTerminate = 1;
    while (guiDiagWatchdogRunning)
        usleep(1000);
    guiDiagWatchdogThreadId = -1;
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

int ioDiagGetPendingNoLock(void)
{
    return isIOPending;
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

int ioDiagPrintfMonitorNoLock(const char *format, ...)
{
    // 看门狗线程不能与主线程共享诊断缓冲区，否则停顿快照可能互相覆盖。
    static char diagMonitorBuf[768] ALIGNED(64);
    va_list args;
    va_start(args, format);
    int len = vsnprintf(diagMonitorBuf, sizeof(diagMonitorBuf), format, args);
    va_end(args);

    if (len < 0)
        return len;
    if (len >= (int)sizeof(diagMonitorBuf))
        len = sizeof(diagMonitorBuf) - 1;

#ifdef __EESIO_DEBUG
    sio_putsn(diagMonitorBuf);
#else
    int written = 0;
    while (written < len) {
        int ret = write(STDOUT_FILENO, diagMonitorBuf + written, len - written);
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
