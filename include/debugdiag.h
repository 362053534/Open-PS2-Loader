#ifndef __DEBUG_DIAG_H
#define __DEBUG_DIAG_H

#include <tamtypes.h>
#include <kernel.h>

#ifdef __DEBUG

// 这些阶段只记录主线程最后到达的位置，不在阶段切换时输出日志，避免改变时序。
enum gui_diag_stage {
    GUI_DIAG_NONE = 0,
    GUI_DIAG_FRAME_BEGIN,
    GUI_DIAG_GUI_LOCK_WAIT,
    GUI_DIAG_GUI_LOCK_HELD,
    GUI_DIAG_RM_START,
    GUI_DIAG_FRAME_ACTIVE,
    GUI_DIAG_GUI_SHOW,
    GUI_DIAG_MENU_LOCK_WAIT,
    GUI_DIAG_MENU_LOCK_HELD,
    GUI_DIAG_MENU_UNLOCK,
    GUI_DIAG_THEME_ELEMENT,
    GUI_DIAG_COVERFLOW,
    GUI_DIAG_COVERFLOW_TEXTURE,
    GUI_DIAG_COVERFLOW_SUBMIT,
    GUI_DIAG_TEX_BIND,
    GUI_DIAG_COVERFLOW_PREFETCH,
    GUI_DIAG_COVERFLOW_ICO,
    GUI_DIAG_ART_FLUSH,
    GUI_DIAG_GREETING,
    GUI_DIAG_OVERLAY,
    GUI_DIAG_DEFERRED,
    GUI_DIAG_GUI_END,
    GUI_DIAG_RM_END,
    GUI_DIAG_HIRES_SYNC,
    GUI_DIAG_HIRES_FLIP,
    GUI_DIAG_QUEUE_EXEC,
    GUI_DIAG_GS_FINISH,
    GUI_DIAG_TEX_MANAGER_NEXT_FRAME,
    GUI_DIAG_GUI_UNLOCK,
    GUI_DIAG_INPUT,
    GUI_DIAG_FRAME_HOOK,
    GUI_DIAG_INTRO,
    GUI_DIAG_MSGBOX,
    GUI_DIAG_COUNT
};

extern volatile u32 gGuiDiagHeartbeat;
extern volatile u32 gGuiDiagFrame;
extern volatile u32 gGuiDiagStageFrame;
extern volatile int gGuiDiagStage;
extern volatile int gGuiDiagStageThread;
extern volatile int gGuiDiagEnabled;
extern const char *volatile gGuiDiagElementName;

// menuRenderElements 在持锁前更新这些快照，主线程停在 menuSemaId 时仍可读到最后光标。
extern volatile int gMenuDiagCurrentId;
extern volatile int gMenuDiagCursor;
extern volatile int gMenuDiagItemCount;
extern volatile int gMenuDiagMenuMode;
extern volatile int gMenuDiagIsCoverflow;
extern volatile char gMenuDiagCurrentText[128];

// Coverflow 的请求/绘制阶段使用无锁整数快照，避免看门狗进入主题锁或缓存锁。
extern volatile int gCoverflowDiagCurrentId;
extern volatile int gCoverflowDiagStartId;
extern volatile int gCoverflowDiagDirection;
extern volatile int gCoverflowDiagSteps;
extern volatile int gCoverflowDiagRenderCount;
extern volatile int gCoverflowDiagRenderIndex;
extern volatile int gCoverflowDiagTexturePhase;
extern volatile int gCoverflowDiagTextureItemId;

#define GUI_DIAG_STAGE(stage)                                                                                              \
    do {                                                                                                                   \
        gGuiDiagStageFrame = gGuiDiagFrame;                                                                                \
        gGuiDiagStageThread = GetThreadId();                                                                               \
        gGuiDiagStage = (stage);                                                                                           \
    } while (0)

const char *guiDiagStageName(int stage);
void guiDiagGetSemaIds(int *queueSemaId, int *guiLockSemaId);
void menuDiagGetSemaIds(int *menuSemaId, int *menuListSemaId);
void rmDiagGetState(int *isHires, int *videoMode, int *width, int *height, int *activeBuffer);
void texDiagGetRequestState(int *loading, int *active, const char **suffix, const char **value, int *itemId);
void texDiagGetMutexState(int *owner, int *ownerSite, int *workerWaitSite, int *otherWaiter, int *otherWaitSite,
                          int *lastUnlockThread, int *lastUnlockSite, unsigned int *lockCount);
void texDiagGetThreadState(int *bgCreated, int *covCreated, int *icoCreated);

#else

#define GUI_DIAG_STAGE(stage) ((void)0)

#endif

#endif
