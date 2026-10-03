CC ?= cc
SANITIZERS ?= address,undefined
BIN = ../../tmp/rcuya_regression/test
IOP_PATCH = ../../modules/iopcore/patches/rcuyapatch
EE_CORE = ../../ee_core
TEST_CFLAGS = -std=gnu99 -g -O1 -Wall -Wextra -Werror -Wno-pointer-to-int-cast -fno-omit-frame-pointer

ifneq ($(SANITIZERS),)
TEST_CFLAGS += -fsanitize=$(SANITIZERS)
TEST_LDFLAGS += -fsanitize=$(SANITIZERS)
endif

.PHONY: all test clean
all: test

test: $(BIN)
	$(BIN)
	python3 check_integration.py

$(BIN): test.c $(IOP_PATCH)/main.c $(IOP_PATCH)/ioplib.c $(IOP_PATCH)/ioplib.h $(EE_CORE)/src/rc_uya.c $(EE_CORE)/include/rc_uya.h $(EE_CORE)/include/util.h $(wildcard include/*.h)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TEST_CFLAGS) -Iinclude -I$(EE_CORE)/include test.c $(IOP_PATCH)/ioplib.c $(EE_CORE)/src/rc_uya.c -o $@ $(LDFLAGS) $(TEST_LDFLAGS)

clean:
	rm -f $(BIN)
