#define OpenTCPSession smb1_OpenTCPSession
#define smb_NegotiateProtocol smb1_NegotiateProtocol
#define smb_SessionSetupAndX smb1_SessionSetupAndX
#define smb_TreeConnectAndX smb1_TreeConnectAndX
#define smb_OpenAndX smb1_OpenAndX
#define smb_Close smb1_Close
#define smb_ReadFile smb1_ReadFile
#define smb_WriteFile smb1_WriteFile
#define smb_ReadCD smb1_ReadCD
#define smb_Echo smb1_Echo
#define smb_CloseAll smb1_CloseAll
#define smb_Disconnect smb1_Disconnect
#define smb_AbortConnection smb1_AbortConnection
#define SMB2TEST_USE_STANDARD_RECV 1

#include "../../../modules/iopcore/cdvdman/smb.c"
