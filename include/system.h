#ifndef __SYSTEM_H
#define __SYSTEM_H

#include "include/mcemu.h"

#define SYS_LOAD_MC_MODULES   0x01
#define SYS_LOAD_USB_MODULES  0x02
#define SYS_LOAD_ISOFS_MODULE 0x04

unsigned int USBA_crc32(const char *string);
int sysGetDiscID(char *discID);
void sysInitDev9(void);
int sysShutdownDev9(void);
// Suppress DDIOC_OFF while deinit releases the last DEV9 reference.
void sysDeferDev9Shutdown(void);
// Power off DEV9 directly without running its registered shutdown callbacks.
void sysForceShutdownDev9(void);
void sysReset(int modload_mask);
void sysExecExit(void);
void sysPowerOff(void);
#ifdef __DECI2_DEBUG
int sysInitDECI2(void);
#endif

void sysLaunchLoaderElf(const char *filename, const char *mode_str, int size_cdvdman_irx, void **cdvdman_irx, int size_mcemu_irx, void **mcemu_irx, int EnablePS2Logo, unsigned int compatflags);

int sysExecElf(const char *path);
int sysLoadModuleBuffer(void *buffer, int size, int argc, char *argv);
int sysCheckMC(void);
int sysCheckVMC(const char *prefix, const char *sep, char *name, int createSize, vmc_superblock_t *vmc_superblock);

#endif
