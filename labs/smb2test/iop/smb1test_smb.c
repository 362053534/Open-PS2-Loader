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

typedef struct
{
    u16 TotalParamCount;
    u16 TotalDataCount;
    u16 MaxParamCount;
    u16 MaxDataCount;
    u8 MaxSetupCount;
    u8 Reserved;
    u16 Flags;
    u32 Timeout;
    u16 Reserved2;
    u16 ParamCount;
    u16 ParamOffset;
    u16 DataCount;
    u16 DataOffset;
    u8 SetupCount;
    u8 Reserved3;
} __attribute__((packed)) smb1test_transaction_request_t;

typedef struct
{
    u16 TotalParamCount;
    u16 TotalDataCount;
    u16 Reserved;
    u16 ParamCount;
    u16 ParamOffset;
    u16 ParamDisplacement;
    u16 DataCount;
    u16 DataOffset;
    u16 DataDisplacement;
    u8 SetupCount;
    u8 Reserved2;
} __attribute__((packed)) smb1test_transaction_response_t;

typedef struct
{
    u16 SearchAttributes;
    u16 SearchCount;
    u16 Flags;
    u16 LevelOfInterest;
    u32 StorageType;
    char SearchPattern[];
} __attribute__((packed)) smb1test_find_first_param_t;

typedef struct
{
    u16 SearchID;
    u16 SearchCount;
    u16 LevelOfInterest;
    u32 ResumeKey;
    u16 Flags;
    char SearchPattern[];
} __attribute__((packed)) smb1test_find_next_param_t;

typedef struct
{
    SMBHeader_t smbH;
    u8 smbWordcount;
    smb1test_transaction_request_t smbTrans;
    u16 SubCommand;
    u16 ByteCount;
    u8 ByteField[];
} __attribute__((packed)) smb1test_find_request_t;

typedef struct
{
    u16 SearchID;
    u16 SearchCount;
    u16 EndOfSearch;
    u16 EAErrorOffset;
    u16 LastNameOffset;
} __attribute__((packed)) smb1test_find_response_param_t;

typedef struct
{
    u32 NextEntryOffset;
    u32 FileIndex;
    s64 Created;
    s64 LastAccess;
    s64 LastWrite;
    s64 Change;
    u64 EndOfFile;
    u64 AllocationSize;
    u32 FileAttributes;
    u32 FileNameLen;
    u32 EAListLength;
    u16 ShortFileNameLen;
    u8 ShortFileName[24];
    char FileName[];
} __attribute__((packed)) smb1test_find_response_data_t;

typedef struct
{
    SMBHeader_t smbH;
    u8 smbWordcount;
    smb1test_transaction_response_t smbTrans;
    u16 ByteCount;
    u8 ByteField[];
} __attribute__((packed)) smb1test_find_response_t;

static void smb1testGetFileName(char *out, unsigned int outSize, const char *in, unsigned int inBytes)
{
    unsigned int offset = 0;

    if (server_specs.Capabilities & SERVER_CAP_UNICODE) {
        while (inBytes >= 2 && offset + 1 < outSize) {
            u16 wc = (u8)in[0] | ((u8)in[1] << 8);

            in += 2;
            inBytes -= 2;
            if (!wc)
                break;
            if (wc < 0x80) {
                out[offset++] = wc;
            } else if (wc < 0x800 && offset + 2 < outSize) {
                out[offset++] = 0xC0 | (wc >> 6);
                out[offset++] = 0x80 | (wc & 0x3F);
            } else if (offset + 3 < outSize) {
                out[offset++] = 0xE0 | (wc >> 12);
                out[offset++] = 0x80 | ((wc >> 6) & 0x3F);
                out[offset++] = 0x80 | (wc & 0x3F);
            }
        }
    } else {
        while (offset < inBytes && offset + 1 < outSize && in[offset]) {
            out[offset] = in[offset];
            offset++;
        }
    }
    out[offset] = '\0';
}

int smb1_FindFirstISO(const char *directory, char *path, unsigned int pathSize)
{
    smb1test_find_request_t *request = (smb1test_find_request_t *)&SMB_buf.smb.u8buff[0];
    smb1test_find_response_t *response = (smb1test_find_response_t *)&SMB_buf.smb.u8buff[0];
    smb1test_find_response_param_t *responseParam;
    smb1test_find_response_data_t *responseData;
    char pattern[192];
    char name[256];
    int command = TRANS2_FIND_FIRST2;
    int endOfSearch = 0;
    int searchID = 0;
    int pathLength;
    int offset;
    int result;
    int i;

    pathLength = strlen(directory);
    if (pathLength + 4 > sizeof(pattern))
        return -EINVAL;
    pattern[0] = '\\';
    memcpy(&pattern[1], directory, pathLength);
    pattern[pathLength + 1] = '\\';
    pattern[pathLength + 2] = '*';
    pattern[pathLength + 3] = '\0';
    for (i = 0; pattern[i]; i++) {
        if (pattern[i] == '/')
            pattern[i] = '\\';
    }

    while (!endOfSearch) {
        ZERO_PKT_ALIGNED(request, sizeof(*request));
        request->smbH.Magic = SMB_MAGIC;
        request->smbH.Cmd = SMB_COM_TRANSACTION2;
        request->smbH.Flags = SMB_FLAGS_CANONICAL_PATHNAMES;
        request->smbH.Flags2 = SMB_FLAGS2_KNOWS_LONG_NAMES | SMB_FLAGS2_32BIT_STATUS;
        if (server_specs.Capabilities & SERVER_CAP_UNICODE)
            request->smbH.Flags2 |= SMB_FLAGS2_UNICODE_STRING;
        request->smbH.UID = UID;
        request->smbH.TID = TID;
        request->smbWordcount = 15;
        request->smbTrans.SetupCount = 1;
        request->SubCommand = command;
        request->smbTrans.ParamOffset = (sizeof(*request) + 3) & ~3;
        request->smbTrans.MaxParamCount = 256;
        request->smbTrans.MaxDataCount = MAX_SMB_BUF - request->smbTrans.MaxParamCount;
        memset(request + 1, 0, request->smbTrans.ParamOffset - sizeof(*request));

        if (command == TRANS2_FIND_FIRST2) {
            smb1test_find_first_param_t *param = (smb1test_find_first_param_t *)&SMB_buf.smb.u8buff[request->smbTrans.ParamOffset];

            param->SearchAttributes = ATTR_READONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_DIRECTORY | ATTR_ARCHIVE;
            param->SearchCount = 1;
            param->Flags = CLOSE_SEARCH_IF_EOS | RESUME_SEARCH;
            param->LevelOfInterest = SMB_FIND_FILE_BOTH_DIRECTORY_INFO;
            param->StorageType = 0;
            pathLength = setStringField(param->SearchPattern, pattern);
            request->smbTrans.TotalParamCount = request->smbTrans.ParamCount = sizeof(*param) + pathLength;
        } else {
            smb1test_find_next_param_t *param = (smb1test_find_next_param_t *)&SMB_buf.smb.u8buff[request->smbTrans.ParamOffset];

            param->SearchID = searchID;
            param->SearchCount = 1;
            param->LevelOfInterest = SMB_FIND_FILE_BOTH_DIRECTORY_INFO;
            param->ResumeKey = 0;
            param->Flags = CLOSE_SEARCH_IF_EOS | RESUME_SEARCH | CONTINUE_SEARCH;
            pathLength = setStringField(param->SearchPattern, "");
            request->smbTrans.TotalParamCount = request->smbTrans.ParamCount = sizeof(*param) + pathLength;
        }

        request->ByteCount = 3 + request->smbTrans.TotalParamCount;
        offset = request->smbTrans.ParamOffset + request->smbTrans.TotalParamCount;
        nb_SetSessionMessage(offset);
        result = GetSMBServerReply(0, NULL, 0);
        if (result <= 0 || response->smbH.Magic != SMB_MAGIC)
            return -EIO;
        if ((response->smbH.Eclass | (response->smbH.Ecode << 16)) != STATUS_SUCCESS)
            return -EIO;

        if (command == TRANS2_FIND_FIRST2) {
            responseParam = (smb1test_find_response_param_t *)&SMB_buf.smb.u8buff[response->smbTrans.ParamOffset];
            searchID = responseParam->SearchID;
        } else {
            responseParam = (smb1test_find_response_param_t *)&SMB_buf.smb.u8buff[response->smbTrans.ParamOffset - 2];
        }
        if (!responseParam->SearchCount)
            return -ENOENT;

        endOfSearch = responseParam->EndOfSearch;
        responseData = (smb1test_find_response_data_t *)&SMB_buf.smb.u8buff[response->smbTrans.DataOffset];
        smb1testGetFileName(name, sizeof(name), responseData->FileName, responseData->FileNameLen);
        pathLength = strlen(name);
        if (!(responseData->FileAttributes & EXT_ATTR_DIRECTORY) && pathLength >= 4 && name[pathLength - 4] == '.' &&
            (name[pathLength - 3] == 'i' || name[pathLength - 3] == 'I') &&
            (name[pathLength - 2] == 's' || name[pathLength - 2] == 'S') &&
            (name[pathLength - 1] == 'o' || name[pathLength - 1] == 'O')) {
            i = strlen(directory);
            if (i + 1 + pathLength + 1 > pathSize)
                return -EINVAL;
            memcpy(path, directory, i);
            path[i] = '/';
            memcpy(&path[i + 1], name, pathLength + 1);
            return 0;
        }
        command = TRANS2_FIND_NEXT2;
    }

    return -ENOENT;
}
