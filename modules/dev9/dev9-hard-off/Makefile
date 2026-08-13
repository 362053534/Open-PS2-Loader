IOP_BIN  = dev9_hard_off.irx
IOP_OBJS = main.o imports.o
.DEFAULT_GOAL := all

.PHONY: clean-extra
clean: clean-extra
clean-extra:
	rm -f dev9_hard_off.notiopmod.elf dev9_hard_off.notiopmod.stripped.elf

include $(PS2SDK)/Defs.make
include ../../Rules.bin.make
include $(PS2SDK)/samples/Makefile.iopglobal
