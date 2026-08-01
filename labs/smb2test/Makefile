EE_BIN = SMB2TEST.ELF
EE_OBJS = main.o ps2dev9.o netman.o smsutils.o smap.o ps2ip.o ps2ips.o smb2test_irx.o

EE_LIBS = -lnetman -lps2ips -lpatches -ldebug -lc -lkernel
EE_CFLAGS = -G0 -O2 -Wall
EE_LDFLAGS = -s

BIN2C = $(PS2SDK)/bin/bin2c

all: $(EE_BIN)

clean:
	$(MAKE) -C iop clean
	rm -f $(EE_BIN) $(EE_OBJS) ps2dev9.c netman.c smsutils.c smap.c ps2ip.c ps2ips.c smb2test_irx.c
	$(MAKE) -C ../../modules/network/SMSUTILS clean

rebuild: clean all

ps2dev9.c: $(PS2SDK)/iop/irx/ps2dev9.irx
	$(BIN2C) $< $@ ps2dev9_irx

netman.c: $(PS2SDK)/iop/irx/netman.irx
	$(BIN2C) $< $@ netman_irx

../../modules/network/SMSUTILS/SMSUTILS.irx:
	$(MAKE) -C ../../modules/network/SMSUTILS

smsutils.c: ../../modules/network/SMSUTILS/SMSUTILS.irx
	$(BIN2C) $< $@ smsutils_irx

smap.c: $(PS2SDK)/iop/irx/smap.irx
	$(BIN2C) $< $@ smap_irx

# 与 OPL 菜单 ETH 相同：netman + smsutils + smap + ps2ip-nm + ps2ips
ps2ip.c: $(PS2SDK)/iop/irx/ps2ip-nm.irx
	$(BIN2C) $< $@ ps2ip_irx

ps2ips.c: $(PS2SDK)/iop/irx/ps2ips.irx
	$(BIN2C) $< $@ ps2ips_irx

iop/smb2test.irx:
	$(MAKE) -C iop

smb2test_irx.c: iop/smb2test.irx
	$(BIN2C) $< $@ smb2test_irx

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
