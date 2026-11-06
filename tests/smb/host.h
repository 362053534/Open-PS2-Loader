#ifndef SMB_HOST_H
#define SMB_HOST_H

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef struct { u32 lo, hi; } iop_sys_clock_t;
typedef struct { int initial, max, option, attr; } iop_sema_t;
struct irx_export_table { int unused; };
typedef struct { void *exports[43]; } modinfo_t;

#define __CDVDMAN_INTERNAL__
#define SCECdNotReady 0
#define SCECdComplete 2
#define SCECdTrayOpen 1
#define SCECdTrayClose 2
#define SCECdErNO 0
#define SCECdErTRMOPN 7
#define SCECdErREAD 48
#define DPRINTF(...) printf(__VA_ARGS__)

int CreateSema(iop_sema_t *s);
int WaitSema(int s);
int PollSema(int s);
int SignalSema(int s);
int DelayThread(unsigned int usec);
int GetSystemTime(iop_sys_clock_t *clock);
int USec2SysClock(u32 usec, iop_sys_clock_t *clock);
int SetAlarm(iop_sys_clock_t *clock, unsigned int (*callback)(void *), void *arg);
int CancelAlarm(unsigned int (*callback)(void *), void *arg);
int RegisterLibraryEntries(struct irx_export_table *table);
int getModInfo(const char *name, modinfo_t *info);
int sceCdTrayReq(int mode, u32 *count);
void cdvdman_kick_rthread(int from_intr);

#include "oplsmb.h"
#include "cdvd_config.h"
#include "smb.h"
#include "device.h"
int smb_read_thread_on_wake(int busy);

#endif
