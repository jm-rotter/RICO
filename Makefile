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

HDRS := $(wildcard src/*.h host/*.h guest/*.h)

.PHONY: all host guest test tsan integration clean
all: host guest

host: $(BUILD)/rico_hostd
guest: $(BUILD)/rico_guest $(BUILD)/rico_init $(BUILD)/rico_test
tsan: $(BUILD)/rico_test_tsan

$(BUILD):
	@mkdir -p $@

$(BUILD)/rico_hostd: host/rico_hostd.c host/rico_host.c $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ host/rico_hostd.c host/rico_host.c $(LDLIBS)

$(BUILD)/rico_guest: guest/rico_guest.c $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(STATIC) -o $@ $<

$(BUILD)/rico_init: guest/rico_init.c | $(BUILD)
	$(CC) $(CFLAGS) $(STATIC) -o $@ $<

$(BUILD)/rico_test: guest/rico_test.c host/rico_host.c $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ guest/rico_test.c host/rico_host.c $(LDLIBS)

$(BUILD)/rico_test_tsan: guest/rico_test.c host/rico_host.c $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=thread -o $@ guest/rico_test.c host/rico_host.c $(LDLIBS)

# Loaded stress (threads + separate processes), ping-pong, TSan, and the
# standalone daemon+guest pair over a shared file.
N ?= 20000000
test: all tsan
	$(BUILD)/rico_test $(N)
	$(BUILD)/rico_test --procs $(N)
	$(BUILD)/rico_test --pingpong 200000
	$(BUILD)/rico_test_tsan 2000000
	$(BUILD)/rico_test_tsan --pingpong 200000
	./scripts/run_local.sh $(N)

integration: all
	./scripts/run_integration_test.sh

clean:
	rm -rf $(BUILD)
