# Project RICO build.
#
#   make            build everything into build/
#   make host       host daemon (build/rico_hostd)
#   make guest      guest binaries (build/rico_guest, rico_init: static; rico_test)
#   make test       unit/stress tests incl. ThreadSanitizer (no VM needed)
#   make integration  end-to-end test across QEMU/KVM (needs /boot kernel, qemu)
#   make clean

CC      ?= cc
BUILD   := build
INC     := -Isrc -Ihost -Iguest
WARN    := -Wall -Wextra -Wpedantic -Wshadow
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 $(WARN) $(INC)
LDLIBS  := -lpthread
# Guest binaries are fully static so they drop into a bare initramfs / any
# guest image with no libc or loader dependencies.
STATIC  := -static

# liburing (optional): found via pkg-config, or point LIBURING_INC / LIBURING_LIB at it.
# Without it everything builds except the io_uring backend (rico_hostd --backing).
ifneq ($(LIBURING_INC),)
  URING_CFLAGS := -isystem $(LIBURING_INC) -DRICO_HAVE_IOURING
  URING_LIBS   := $(if $(LIBURING_LIB),-L$(LIBURING_LIB) -l:liburing.so.2,-luring)
else ifeq ($(shell pkg-config --exists liburing && echo y),y)
  URING_CFLAGS := $(subst -I,-isystem ,$(shell pkg-config --cflags liburing)) -DRICO_HAVE_IOURING
  URING_LIBS   := $(shell pkg-config --libs liburing)
endif
ifneq ($(URING_CFLAGS),)
  URING_SRC := host/rico_iouring.c
endif

HDRS := $(wildcard src/*.h host/*.h guest/*.h)

.PHONY: all host guest test tsan iouring_test integration clean
all: host guest

host: $(BUILD)/rico_hostd
guest: $(BUILD)/rico_guest $(BUILD)/rico_init $(BUILD)/rico_test
tsan: $(BUILD)/rico_test_tsan
ifneq ($(URING_CFLAGS),)
iouring_test: $(BUILD)/rico_iouring_test
endif

$(BUILD):
	@mkdir -p $@

$(BUILD)/rico_hostd: host/rico_hostd.c host/rico_host.c $(URING_SRC) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(URING_CFLAGS) -o $@ host/rico_hostd.c host/rico_host.c $(URING_SRC) $(LDLIBS) $(URING_LIBS)

$(BUILD)/rico_guest: guest/rico_guest.c $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(STATIC) -o $@ $<

$(BUILD)/rico_init: guest/rico_init.c | $(BUILD)
	$(CC) $(CFLAGS) $(STATIC) -o $@ $<

$(BUILD)/rico_test: guest/rico_test.c host/rico_host.c $(URING_SRC) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(URING_CFLAGS) -o $@ guest/rico_test.c host/rico_host.c $(URING_SRC) $(LDLIBS) $(URING_LIBS)

$(BUILD)/rico_test_tsan: guest/rico_test.c host/rico_host.c $(URING_SRC) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(URING_CFLAGS) -fsanitize=thread -o $@ guest/rico_test.c host/rico_host.c $(URING_SRC) $(LDLIBS) $(URING_LIBS)

# io_uring backend unit tests, under ASan+UBSan (teardown with I/O in flight)
$(BUILD)/rico_iouring_test: host/rico_iouring_test.c host/rico_iouring.c $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(URING_CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer -o $@ host/rico_iouring_test.c host/rico_iouring.c $(URING_LIBS)

# Loaded stress (threads + separate processes), ping-pong, TSan, and the
# standalone daemon+guest pair over a shared file.
N ?= 20000000
test: all tsan
	$(BUILD)/rico_test $(N)
	$(BUILD)/rico_test --procs $(N)
	$(BUILD)/rico_test --pingpong 200000
	$(BUILD)/rico_test_tsan 2000000
	$(BUILD)/rico_test_tsan --pingpong 200000
ifneq ($(URING_CFLAGS),)
	$(MAKE) --no-print-directory $(BUILD)/rico_iouring_test
	$(BUILD)/rico_iouring_test
	$(BUILD)/rico_test --iouring 5000000
	$(BUILD)/rico_test --iouring --pingpong 200000
	$(BUILD)/rico_test --iouring --procs 2000000
	$(BUILD)/rico_test --iouring --sqpoll 2000000
	$(BUILD)/rico_test_tsan --iouring 1000000
endif
	./scripts/run_local.sh $(N)

integration: all
	./scripts/run_integration_test.sh

clean:
	rm -rf $(BUILD)
