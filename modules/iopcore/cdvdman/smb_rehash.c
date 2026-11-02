/*
  Copyright 2009, jimmikaelkael
  Licenced under Academic Free License version 3.0
  Review OpenUsbLd README & LICENSE files for further details.
*/

#include <tamtypes.h>
#include <sysclib.h>

#include "oplsmb.h"
#include "smb.h"
#include "../../network/smbinit/des.h"

/*
 * 挑战应答。和 smbinit 里的 LM_Response 同一套切法：16 字节散列补 5 个 0，再切成三段 7 字节密钥。
 * 重连时不能再进 smbinit，所以这份留在 cdvdman。
 */
static unsigned char *LM_Response(const unsigned char *hash, unsigned char *chal, unsigned char *cipher)
{
    unsigned char padded[21];
    unsigned char key[7];
    int i;

    memcpy(&padded[0], hash, 16);
    memset(&padded[16], 0, 5);

    for (i = 0; i < 3; i++) {
        memcpy(key, &padded[i * 7], 7);
        DES(key, chal, &cipher[i * 8]);
    }

    return cipher;
}

void smb_HashPasswordCached(server_specs_t *ss)
{
    u8 LMresponse[24];
    u8 NTLMresponse[24];

    /* HashedFlag 为 0 才是加密口令。明文是 -1，口令本身已经由协商重新拷进来了。 */
    if (ss->HashedFlag == 0 && strlen(ss->Password) > 0) {
        if (ss->PasswordType == SERVER_USE_ENCRYPTED_PASSWORD) {
            ss->PasswordLen = 24;
            LM_Response(ss->LMhash, ss->EncryptionKey, LMresponse);
            LM_Response(ss->NTLMhash, ss->EncryptionKey, NTLMresponse);
            memcpy(&ss->Password[0], LMresponse, ss->PasswordLen);
            memcpy(&ss->Password[24], NTLMresponse, ss->PasswordLen);
        } else if (ss->PasswordType == SERVER_USE_PLAINTEXT_PASSWORD) {
            ss->PasswordLen = strlen(ss->Password);
            if (ss->PasswordLen > 14)
                ss->PasswordLen = 14;
            else if (ss->PasswordLen == 0)
                ss->PasswordLen = 1;
        }
    }

    ss->HashedFlag = 1;
}
