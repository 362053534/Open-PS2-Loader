IOP_BIN = smb2test.irx
IOP_OBJS = main.o smb2test_smb2.o smb2_read_safe.o imports.o

IOP_INCS += -I. -I.. -Ivendor -I../../../modules/iopcore/cdvdman -I../../../modules/iopcore/common -I../../../modules/network/common -I$(PS2SDK)/ports/include
IOP_CFLAGS += -DSMB_DRIVER -D__CDVDMAN_INTERNAL__ -DSMB2TEST_BUILD -D_IOP -D__PS2__ -include smb2test_internal.h
IOP_LIBS += -L$(PS2SDK)/ports_iop/lib -lsmb2 -lgcc
IOP_LDFLAGS += -Wl,--allow-multiple-definition -Wl,--wrap=smb2_service

vpath %.c vendor

include $(PS2SDK)/Defs.make
include ../../../modules/Rules.bin.make
include $(PS2SDK)/samples/Makefile.iopglobal
