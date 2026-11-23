#ifndef __UTIL_H
#define __UTIL_H

int getmcID(void);
int getFileSize(int fd);
void checkMCFolder(void);
int installPopstarterDrivers(int mode, int bdmDeviceType);
int buildPopstarterSMBConfigs(char *ipconfig, int ipconfigCapacity, int *ipconfigSize,
                              char *smbconfig, int smbconfigCapacity, int *smbconfigSize);
int openFile(char *path, int mode);
void *readFile(char *path, int align, int *size);
int listDir(char *path, const char *separator, int maxElem,
            int (*readEntry)(int index, const char *path, const char *separator, const char *name, unsigned char d_type));

typedef struct
{
    int fd;
    int mode;
    char *buffer;
    unsigned int size;
    unsigned int available;
    char *lastPtr;
    short allocResult;
} file_buffer_t;

file_buffer_t *openFileBufferBuffer(short allocResult, const void *buffer, unsigned int size);
file_buffer_t *openFileBuffer(char *fpath, int mode, short allocResult, unsigned int size);
int readFileBuffer(file_buffer_t *readContext, char **outBuf);
void writeFileBuffer(file_buffer_t *fileBuffer, char *inBuf, int size);
void closeFileBuffer(file_buffer_t *fileBuffer);

int max(int a, int b);
int min(int a, int b);
int fromHex(char digit);
char toHex(int digit);

enum CONSOLE_REGIONS {
    CONSOLE_REGION_INVALID = -1,
    CONSOLE_REGION_JAPAN = 0,
    CONSOLE_REGION_USA, // USA and HK/SG.
    CONSOLE_REGION_EUROPE,
    CONSOLE_REGION_CHINA,

    CONSOLE_REGION_COUNT
};

int InitConsoleRegionData(void);
const char *GetSystemDataPath(void);
char GetSystemFolderLetter(void);
int sysDeleteFolder(const char *folder);

int CheckPS2Logo(int fd, u32 lba);

void delay(int count);

/* T1 调试探针：实机看不到 LOG 输出，改为先存内存、启动完成后再落盘到 mass0 根目录的 txt。 */
void oplProbeLog(const char *fmt, ...);
void oplProbeFlush(const char *tag);
void oplProbeMark(const char *label, const char *path);

#endif
