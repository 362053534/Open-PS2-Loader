int DeviceWritePage(int mc_num, void *buf, u32 page_num);
int DeviceReadPage(int mc_num, void *buf, u32 page_num);
void DeviceShutdown(void);
#ifdef PFS_BDM_DRIVER
int DeviceCheckPages(int mc_num, u32 page_num, u32 count);
#endif
