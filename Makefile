TARGET := riscv64-unknown-elf
CC = clang
ARCHFLAGS := --target=$(TARGET) -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany
ASFLAGS := $(ARCHFLAGS)
CFLAGS := $(ARCHFLAGS) \
	-ffreestanding -fno-builtin -fno-stack-protector -fno-pic -msmall-data-limit=0 \
	-O2 -Wall -Wextra
LDFLAGS := $(ARCHFLAGS) -nostdlib -fuse-ld=lld -Wl,-T,linker.ld -Wl,--build-id=none

QEMU_DISPLAY ?= $(shell if [ -n "$$DISPLAY" ] || [ -n "$$WAYLAND_DISPLAY" ]; then echo gtk; else echo none; fi)
NOVNC_WEB ?= /usr/share/novnc
NOVNC_LISTEN ?= 0.0.0.0
NOVNC_PORT ?= 6081
VNC_DISPLAY ?= 1
VNC_PORT = $(shell expr 5900 + $(VNC_DISPLAY))

.PHONY: all run novnc clean

all: craftos-riscv64.elf

boot-rv.o: boot.s
	$(CC) $(ASFLAGS) -c $< -o $@

kernel-rv.o: kernel.c
	$(CC) $(CFLAGS) -c $< -o $@

craftos-riscv64.elf: boot-rv.o kernel-rv.o linker.ld
	$(CC) $(LDFLAGS) boot-rv.o kernel-rv.o -o $@

run: craftos-riscv64.elf
	qemu-system-riscv64 -machine virt -m 256M -bios default -kernel $< \
		-global virtio-mmio.force-legacy=false \
		-device virtio-gpu-device,xres=800,yres=600 -device virtio-mouse-device \
		-display $(QEMU_DISPLAY) -serial stdio -monitor none

novnc: craftos-riscv64.elf
	@set -e; \
	test -f "$(NOVNC_WEB)/vnc.html" || { echo "noVNC frontend not found: $(NOVNC_WEB)/vnc.html"; exit 1; }; \
	websockify --web="$(NOVNC_WEB)" "$(NOVNC_LISTEN):$(NOVNC_PORT)" "127.0.0.1:$(VNC_PORT)" & proxy_pid=$$!; \
	sleep 1; \
	if ! kill -0 "$$proxy_pid" 2>/dev/null; then echo "websockify failed to start"; exit 1; fi; \
	qemu-system-riscv64 -machine virt -m 256M -bios default -kernel craftos-riscv64.elf \
		-global virtio-mmio.force-legacy=false \
		-device virtio-gpu-device,xres=800,yres=600 -device virtio-mouse-device \
		-vnc 127.0.0.1:$(VNC_DISPLAY) -serial stdio -monitor none & qemu_pid=$$!; \
	trap 'kill "$$qemu_pid" "$$proxy_pid" 2>/dev/null || true' EXIT INT TERM; \
	printf 'Open http://localhost:$(NOVNC_PORT)/vnc.html?autoconnect=true&resize=scale&path=websockify\n'; \
	wait "$$qemu_pid"

clean:
	rm -f boot-rv.o kernel-rv.o craftos-riscv64.elf
