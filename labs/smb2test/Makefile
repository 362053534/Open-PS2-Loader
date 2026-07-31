EE_BIN = SMB2TEST.ELF
EE_OBJS = main.o ps2dev9.o smsutils.o smstcpip.o smap.o smb2test_irx.o

EE_LIBS = -lpatches -ldebug -lc -lkernel
EE_CFLAGS = -G0 -O2 -Wall
EE_LDFLAGS = -s

BIN2C = $(PS2SDK)/bin/bin2c

all: $(EE_BIN)

clean:
	$(MAKE) -C iop clean
	rm -f $(EE_BIN) $(EE_OBJS) ps2dev9.c smsutils.c smstcpip.c smap.c smb2test_irx.c

rebuild: clean all

ps2dev9.c: $(PS2SDK)/iop/irx/ps2dev9.irx
	$(BIN2C) $< $@ ps2dev9_irx

../../modules/network/SMSUTILS/SMSUTILS.irx:
	$(MAKE) -C ../../modules/network/SMSUTILS

smsutils.c: ../../modules/network/SMSUTILS/SMSUTILS.irx
	$(BIN2C) $< $@ smsutils_irx

smstcpip.c:
	$(MAKE) FULL_LWIP=1 -C ../../modules/network/SMSTCPIP rebuild
	$(BIN2C) ../../modules/network/SMSTCPIP/SMSTCPIP.irx $@ smstcpip_irx

../../modules/network/smap-ingame/smap.irx:
	$(MAKE) -C ../../modules/network/smap-ingame

smap.c: ../../modules/network/smap-ingame/smap.irx
	$(BIN2C) $< $@ smap_irx

iop/smb2test.irx:
	$(MAKE) -C iop

smb2test_irx.c: iop/smb2test.irx
	$(BIN2C) $< $@ smb2test_irx

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
