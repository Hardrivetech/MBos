Build instructions for MBos
=========================

Quick start (WSL recommended on Windows)

- Install prerequisites on Debian/Ubuntu WSL:

```bash
sudo apt update
sudo apt install -y build-essential gcc-multilib nasm grub-pc-bin xorriso
```

- From the repository root:

```bash
make        # builds kernel.bin and mbos.iso
# or to force rebuild:
make clean && make
```

Notes for Windows users

- Building natively on Windows (MSYS/MinGW) often fails because the host
  `ld` may not support `-m elf_i386` emulation. The simplest workaround is
  to use WSL (Windows Subsystem for Linux) and run `make` inside WSL.

- Alternatively, install a cross-toolchain that targets i386-elf and update
  `Makefile` to use `i386-elf-gcc`/`i386-elf-ld` (advanced).

Verifying in QEMU

```bash
wsl qemu-system-i386 -cdrom build/mbos.iso -nographic -serial stdio -m 64M
```

If you prefer file capture for serial output (useful for CI):

```bash
wsl qemu-system-i386 -cdrom build/mbos.iso -nographic -serial file:/mnt/c/Users/<you>/Desktop/MBos/build/qemu_serial.log -m 64M
```

What's next

- For CI-friendly builds, consider adding a cross-toolchain target in
  `Makefile` or a Dockerfile that provides a controlled build environment.

Docker / Reproducible build

This repository includes a `Dockerfile` and a helper script to produce a
reproducible build image. On a machine with Docker installed you can:

```bash
./scripts/build-in-docker.sh
```

Or build the image manually and run the build step:

```bash
docker build -t mbos-builder:latest .
docker run --rm -v "$(pwd)":/work -w /work mbos-builder:latest make
```

CI (GitHub Actions)

A GitHub Actions workflow is included at `.github/workflows/build.yml` which
attempts to build `mbos.iso` on pushes and pull requests and uploads the
artifact for easy download.

