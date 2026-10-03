IOP_BIN = cdvdfsv.irx
IOP_OBJS = cdvdfsv.o searchfile.o ncmd.o scmd.o imports.o exports.o

IOP_INCS += -I../common

ifeq ($(DEBUG),1)
IOP_CFLAGS += -DDEBUG
endif

include $(PS2SDK)/Defs.make
include ../../Rules.bin.make
include $(PS2SDK)/samples/Makefile.iopglobal

# FSV_MERGE_SMALLREAD=N：每笔设备读凑成 N 扇区（≥8），把小额读并进相邻读（见 ncmd.c 顶部说明）。
# 例：make FSV_MERGE_SMALLREAD=8
ifneq ($(FSV_MERGE_SMALLREAD),)
IOP_CFLAGS += -DFSV_MERGE_SMALLREAD=$(FSV_MERGE_SMALLREAD)
# 它是命令行变量，make 不会因取值变化而重编译；直接删掉 ncmd.o，免得切 N 时拿到旧二进制。
$(shell rm -f ncmd.o obj/ncmd.o obj.*/ncmd.o)
endif
