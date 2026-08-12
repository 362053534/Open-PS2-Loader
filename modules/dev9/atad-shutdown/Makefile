IOP_BIN  = atad_shutdown.irx
IOP_OBJS = main.o imports.o

clean::
	rm -f atad_shutdown.notiopmod.elf atad_shutdown.notiopmod.stripped.elf

include $(PS2SDK)/Defs.make
include ../../Rules.bin.make
include $(PS2SDK)/samples/Makefile.iopglobal
