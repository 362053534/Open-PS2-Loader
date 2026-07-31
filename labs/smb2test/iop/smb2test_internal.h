#ifndef SMB2TEST_INTERNAL_H
#define SMB2TEST_INTERNAL_H

#include <errno.h>
#include <ioman.h>
#include <loadcore.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sysclib.h>
#include <sysmem.h>
#include <tamtypes.h>
#include <thbase.h>
#include <thsemap.h>

#include "cdvd_config.h"
#include "cdvdman_opl.h"
#include "oplsmb.h"

extern struct cdvdman_settings_smb cdvdman_settings;

int smb_NegotiateProtocol(char *SMBServerIP, int SMBServerPort, char *Username, char *Password, u32 *capabilities, OplSmbPwHashFunc_t hash_callback);
int smb_OpenAndX(char *filename, u8 *FID, int Write);
int smb_Close(int FID);
int smb_ReadFile(u16 FID, u32 offsetlow, u32 offsethigh, void *readbuf, int nbytes);
int smb_Disconnect(void);

#endif
