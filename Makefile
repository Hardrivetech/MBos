CC      := gcc
LD      := ld
AS      := nasm

CFLAGS  := -m32 -ffreestanding -fno-pie -fno-stack-protector -Wall -Wextra -O2
ASFLAGS := -f elf32
LDFLAGS := -m elf_i386 -T linker.ld

BUILD_DIR := build
ISO_DIR   := $(BUILD_DIR)/iso

KERNEL_OBJS := \
	$(BUILD_DIR)/boot.o \
	$(BUILD_DIR)/interrupts.o \
	$(BUILD_DIR)/kernel.o

all: $(BUILD_DIR)/mbos.iso

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/boot.o: src/boot.asm | $(BUILD_DIR)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD_DIR)/interrupts.o: src/interrupts.asm | $(BUILD_DIR)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD_DIR)/kernel.o: src/kernel.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel.bin: $(KERNEL_OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJS)

$(ISO_DIR)/boot/grub:
	mkdir -p $(ISO_DIR)/boot/grub

$(ISO_DIR)/boot/kernel.bin: $(BUILD_DIR)/kernel.bin | $(ISO_DIR)/boot/grub
	cp $< $@

$(ISO_DIR)/boot/grub/grub.cfg: grub/grub.cfg | $(ISO_DIR)/boot/grub
	cp $< $@

$(ISO_DIR)/boot/grub/mbos.png: grub/mbos.png | $(ISO_DIR)/boot/grub
	cp $< $@

$(BUILD_DIR)/mbos.iso: $(ISO_DIR)/boot/kernel.bin $(ISO_DIR)/boot/grub/grub.cfg $(ISO_DIR)/boot/grub/mbos.png
	grub-mkrescue -o $@ $(ISO_DIR)

run: $(BUILD_DIR)/mbos.iso
	qemu-system-i386 -cdrom $(BUILD_DIR)/mbos.iso

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all run clean check-env

.PHONY: check-env
check-env:
	@echo "Checking build environment..."
	@echo -n "Checking ld -m elf_i386 support: " ; \
	if ld -m elf_i386 --version >/dev/null 2>&1; then echo "ok"; else echo "missing (use WSL or cross-toolchain)"; fi
	@echo -n "Checking gcc -m32 support: " ; \
	if gcc -m32 -v >/dev/null 2>&1; then echo "ok"; else echo "missing (install gcc-multilib or use WSL)"; fi

.PHONY: docker-build docker-run-build

docker-build:
	docker build -t mbos-builder:latest .

docker-run-build:
	docker run --rm -v "$(PWD)":/work -w /work mbos-builder:latest make
