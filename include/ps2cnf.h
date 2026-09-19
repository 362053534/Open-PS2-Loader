#define CNF_PATH_LEN_MAX 64
#define CNF_LEN_MAX      1024

int ps2cnfGetBootFile(const char *path, char *bootfile);
/* Parse the BOOT2 value out of an already-loaded SYSTEM.CNF image.
 * The buffer must be NUL-terminated within 'size' bytes.
 * 'bootfile' must have room for at least CNF_PATH_LEN_MAX + 1 bytes. */
int ps2cnfGetBootFileFromBuffer(const char *system_cnf, int size, char *bootfile);
