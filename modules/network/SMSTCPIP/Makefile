#Enable to reduce memory usage (in-game use).
INGAME_DRIVER ?= 0

#Enable to enable support for packet input from the interrupt context.
INTERRUPT_CTX_INPKT ?= 1

#Enable to build with the functions that were removed since OPL does not use them.
FULL_LWIP ?= 0

IOP_BIN = SMSTCPIP.irx
IOP_OBJS = ps2ip.o inet.o ip.o ip_addr.o ip_frag.o etharp.o tcp_in.o tcp_out.o \
	tcp.o tcpip.o mem.o api_lib.o api_msg.o sockets.o netif.o udp.o memp.o \
	icmp.o pbuf.o timers.o exports.o imports.o
IOP_LIBS =

IOP_INCS += -Iinclude -I../../iopcore/common
IOP_CFLAGS += -DLWIP_NOASSERT

ifeq ($(INGAME_DRIVER),1)
IOP_CFLAGS += -DINGAME_DRIVER
endif

# sys_arch_mbox_fetch() 的单次装填版本：整个 fetch 只做一次 SetAlarm/CancelAlarm
# 而不是每次唤醒都做。需要配合设置了 SO_RCVTIMEO 的套接字才有效果。
ifeq ($(LWIP_MBOX_SINGLE_ALARM),1)
IOP_CFLAGS += -DLWIP_MBOX_SINGLE_ALARM
endif

ifeq ($(INTERRUPT_CTX_INPKT),1)
IOP_CFLAGS += -DINTERRUPT_CTX_INPKT
endif

ifeq ($(FULL_LWIP),1)
IOP_CFLAGS += -DFULL_LWIP
endif

include $(PS2SDK)/Defs.make
include ../../Rules.bin.make
include $(PS2SDK)/samples/Makefile.iopglobal

ifneq ($(IOP_CC_VERSION),3.2.2)
ifneq ($(IOP_CC_VERSION),3.2.3)
IOP_CFLAGS += -mno-memcpy
# Ignore warnings due to the old version of lwIP being used.
IOP_CFLAGS += -Wno-attributes -Wno-array-bounds -Wno-unused-but-set-variable
endif
endif
