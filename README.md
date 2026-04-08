# MBos - ASM + C Hobby Kernel Starter

MBos is a tiny hobby kernel skeleton built with NASM and freestanding C.

## What this starter does

- Boots with GRUB using a Multiboot header
- Jumps from ASM to C (`kernel_main`)
- Prints text to VGA text mode (`0xB8000`)
- Loads a flat GDT (code/data segments)
- Remaps PIC and installs IDT entries for ISRs/IRQs
- Handles keyboard IRQ1 and prints keypresses
- Configures PIT timer IRQ0 for periodic ticks
- Parses the Multiboot memory map provided by GRUB
- Builds a simple physical page-frame allocator from usable Multiboot regions
- Builds initial 32-bit identity paging from allocator-provided page tables
- Builds a simple kernel heap on top of the frame allocator and paging layer
- Adds a higher-half virtual alias for the kernel while retaining low identity mappings for bring-up
- Moves the VGA terminal subsystem onto a higher-half virtual alias after paging is enabled
- Moves shell runtime buffers onto the higher-half heap after heap initialization
- Adds safe identity-map inspection and trimming after boot
- Adds user-space split groundwork with safe user-page test mappings
- Adds user-mode execution groundwork (ring3 GDT segments and prepared user test context)
- Adds a minimal `int 0x80` user-to-kernel trap path with syscall counters
- Adds a first syscall dispatch path (`eax` syscall number, `ebx` arg0)
- Adds simulated per-process syscall contexts with process-slot switching
- Adds a tiny round-robin scheduler that can rotate process slots on timer ticks
- Adds a cooperative task abstraction (`runnable`/`stopped`) over process slots
- Adds per-task saved register snapshots captured from timer and syscall interrupts
- Adds an opt-in experimental frame-restore switch path that can resume another task snapshot on scheduler handoff
- Adds per-process prepared user contexts (separate code/stack virtual ranges per PID)
- Adds physical page ownership metadata (allocator, paging structures, kernel heap, user task/test)
- Adds ownership-aware unmap reclamation for empty page-table pages
- Adds blocked-to-ready wakeups with syscall-driven task sleep timers
- Adds explicit wait-queue reasons (`sleep`, `manual`) for blocked tasks
- Adds first GUI groundwork: VBE framebuffer detection, mapping, and `guitest` scene rendering
- Adds first GUI input bridge: keyboard IRQ keypresses can signal event channels
- Includes a tiny shell with `help`, `clear`, `about`, `uptime`, `ticks`, `procinfo`, `procset <id>`, `tasklist`, `taskstats`, `waitq`, `taskspawn <id>`, `taskteardown <id>`, `tasksleep <ticks>`, `taskwait <ticks>`, `taskwake <id>`, `eventwait <ch>`, `eventsig <ch>`, `inputstatus`, `inputlog`, `inputbridgeon`, `inputbridgeoff`, `inputch <ch>`, `pageown <id>`, `taskrun <id>`, `taskstop <id>`, `snapinfo [id]`, `snapnow`, `snapseed [id]`, `snaprestore [id]`, `ctxswon`, `ctxswoff`, `ctxswstat`, `sched`, `schedon`, `schedoff`, `schedq <ticks>`, `guistatus`, `guitest`, `pwd`, `cd <path>`, `syscalls`, `syscallinfo`, `syscalldefs`, `userevents`, `meminfo`, `memmap`, `allocstat`, `allocpage`, `idmap`, `trimid [mb]`, `uspace`, `mapusertest`, `lookuserlast`, `usertestprep`, `userctx`, `userrun`, `pgstat`, `pgdir`, `pgmaptest`, `pglooklast`, `virtlayout`, `kheap`, and `kmalloc <bytes>`
- Scrolls terminal output upward when reaching the last VGA row
- Supports left/right cursor movement and in-line editing
- Supports shell history recall with Up/Down arrow keys
- Builds a bootable ISO and runs in QEMU

## Project Layout

- `src/boot.asm` - Multiboot header, stack setup, jump into C
- `src/interrupts.asm` - GDT/IDT load helpers and ISR/IRQ stubs
- `src/kernel.c` - Minimal kernel entry and VGA output
- `linker.ld` - Kernel link layout
- `grub/grub.cfg` - GRUB menu config
- `Makefile` - Build, ISO, and run commands

## Prerequisites

On Linux/WSL install:

```bash
sudo apt update
sudo apt install -y build-essential nasm grub-pc-bin xorriso qemu-system-x86
```

## Build

```bash
make
```

This creates:

- `build/kernel.bin`
- `build/mbos.iso`

## Run

```bash
make run
```

You should see MBos text in the emulator.
Type in the emulator window to test keyboard IRQ handling.
Use `help` at the `MBos>` prompt to view commands.
Use `uptime` to show time since boot.
Use `ticks` to show raw PIT tick count.
Use `procinfo` to show the active simulated process slot and PID.
Use `procset 2` to switch syscall tracking to a different simulated process.
Use `tasklist` to view lifecycle state and user-context readiness for all process slots.
Use `taskstats` to view lifecycle state counts (`new`, `ready`, `running`, `blocked`, `exited`).
Use `waitq` to inspect blocked-task queue membership (`sleep` vs `manual`).
Use `guistatus` to inspect framebuffer detection/mapping state.
Use `guitest` to draw a simple desktop-style test scene.
Use syscall ID `5` from user code to sleep a task for `ebx` PIT ticks.
Use `taskspawn 3` to prepare PID 3 user context and seed a runnable snapshot.
Use `taskteardown 3` to unmap PID 3 user pages, clear snapshot/context state, and stop the slot.
Use `tasksleep 20` to trigger syscall 5 and block the current task for 20 ticks.
Use `taskwait 20` to place the current task in the manual wait queue with a 20-tick timeout.
Use `taskwake 3` to manually wake a blocked task back to ready state.
Use `eventwait 1` to block current task on event channel 1.
Use `eventsig 1` to wake all tasks waiting on event channel 1.
Use `inputstatus` to inspect keyboard input bridge counters and current channel.
Use `inputlog` to inspect recent keyboard events bridged into event signaling.
Use `inputbridgeon` / `inputbridgeoff` to enable or disable keyboard-to-event signaling.
Use `inputch 1` to route keyboard events to event channel 1.
Use `pageown 3` to inspect ownership class of PID 3 code/stack physical pages.
Use `taskrun 3` or `taskstop 3` to mark a process slot runnable or stopped.
Use `snapinfo` to inspect the latest saved register snapshot for the current process.
Use `snapinfo 2` to inspect a specific process slot snapshot.
Use `snapnow` to trigger an immediate syscall trap snapshot capture.
Use `snapseed` or `snapseed 2` to seed a process snapshot from the prepared `userctx` state.
Use `snaprestore` or `snaprestore 2` to copy a valid saved user-mode snapshot back into the prepared `userrun` context.
Use `ctxswon` / `ctxswoff` to enable or disable experimental frame-restore task switching.
Use `ctxswstat` to inspect restore-mode counters (successful restores and rejects).
Use `sched` to inspect scheduler state.
Use `schedon` / `schedoff` to enable or disable timer-based slot rotation.
Use `schedq 50` to set scheduler quantum in timer ticks.
Use `syscalls` to show how many `int 0x80` traps have been handled.
Use `syscallinfo` to inspect the last syscall number, first argument, and return value.
Use `syscalldefs` to list currently implemented syscall IDs (including event wait/signal IDs 6 and 7).
Use `userevents` to inspect user exit-event counters and last code.
Use `meminfo` to view Multiboot memory totals.
Use `memmap` to print detected RAM regions.
Use `allocstat` to inspect allocator state, including recycled-page pool and ownership counters.
Use `allocpage` to reserve one 4 KiB physical page.
Use `idmap` to inspect the current low identity-mapped range.
Use `trimid` or `trimid 4` to reduce the low identity map after boot.
Use `uspace` to inspect kernel and user virtual space boundaries.
Use `mapusertest` to map one user-accessible test page.
Use `lookuserlast` to inspect the most recent user test mapping.
Use `usertestprep` to allocate and map a ring3 test code+stack context for the current PID.
Use `userctx` to inspect prepared ring3 selector and entry state for the current PID.
Use `userrun` to enter the current PID prepared ring3 test context (non-returning debug path).
Use `pgstat` to inspect paging status.
Use `pgdir` to dump active page-directory entries.
Use `pgmaptest` to map one new test page in a dedicated virtual range.
Use `pglooklast` to inspect the most recently test-mapped virtual page.
Use `virtlayout` to inspect the kernel alias, VGA alias, shell buffers, heap base, and paging test ranges.
Use `kheap` to inspect kernel heap state.
Use `kmalloc 64` to allocate a test block from the kernel heap.
Use `fsls` to list all files in RAMFS.
Use `fscat <name>` to print file contents.
Use `fswrite <name> <text>` to create or overwrite a file.
Use `fsrm <name>` to delete a file.
Use `pwd` to print the current MBFSv1 working directory and `cd <path>` or bare `cd` to change it or return to `/`.
Use `vls` to list the current MBFS working directory (if mounted) and RAMFS from one command.
Use `vcat <path>`, `vwrite <path> <text>`, and `vrm <path>` to access the active virtual filesystem surface.
Use `diskfmt` to format the detected ATA disk as MBFSv1.
Use `diskmount` / `diskumount` to mount or cleanly unmount MBFSv1.
Use `diskls` or `diskls <dir>` to list the current MBFSv1 working directory or a resolved nested directory.
Use `diskmkdir <path>` to create MBFSv1 directories recursively, so paths like `apps/tools/bin` create any missing parent directories automatically.
Use `diskcat <path>`, `diskwrite <path> <text>`, and `diskrm <path>` for MBFSv1 file operations.
MBFSv1 paths now support multiple directory segments, absolute `/...` paths, and relative paths with `.` and `..`, so commands like `cd apps`, `diskmkdir tools/bin`, `diskwrite ./tools/bin/readme hello`, and `diskls ..` work as expected.
Use `fstest` to run smoke tests on file operations.
Use `fdstat` to print the current process FD table.
Use `appfmt <name>` to detect and display the executable format of a file.
Use `appload <name>` to load an app (validates format, initializes memory, displays entry point and stack state).
Use `apprun <name>` to load an app and enter ring3 execution (non-returning).
Use Left/Right arrows to move within the current command line.
Use Up/Down arrows to browse recent commands.

For framebuffer GUI testing choose the GRUB entry `MBos (GUI Test)`.

## Clean

```bash
make clean
```

## Executable Formats

MBos supports multiple executable formats for user-mode applications:

### 1. ELF32 (Linux i386 Static Binaries)
- **Detection:** Magic bytes `0x7F`, `'E'`, `'L'`, `'F'`
- **Loader:** Real ELF32 header parsing with PT_LOAD segment mapping
- **Features:** Zero-fill BSS, proper entry point evaluation
- **Limitations:** Static linking only (no dynamic linking yet)
- **Example:** Compile C → i386 ELF32 with `gcc -m32 -static`

### 2. MBAPP v1 (MBos Application Format)
MBAPP is MBos's native custom executable format for user-mode code and data bundles.

**Header Layout (20 bytes total):**
```c
struct mbapp_header {
    uint32_t magic;       /* 0x50504142 ('MBAP') */
    uint32_t version;     /* 0x00000001 (v1) */
    uint32_t flags;       /* 0x00000000 (reserved, must be 0) */
    uint32_t entry_off;   /* Offset into payload where execution starts */
    uint32_t image_size;  /* Bytes of payload data after this header */
};
```

**File Structure:**
```
[20 bytes: mbapp_header]
[image_size bytes: code/data payload]
```

**Validation Rules:**
- `magic` must equal `0x50504142`
- `version` must equal `1`
- `flags` must equal `0` (for future expansion)
- `entry_off` must be less than `image_size`
- `image_size` must be greater than 0 and fit in one 4 KiB page
- Total file size must equal `sizeof(header) + image_size`

**Execution Model:**
- Payload is loaded into user-space code VA (typically 0x40000000)
- Execution begins at offset `entry_off` within the payload
- Initial stack is constructed with argc/argv/envp (i386 ABI compatible)
- Process heap is initialized (brk syscall available)
- Time syscall (getticks) available for benchmarking
- File I/O syscalls (open/read/write/close/seek/size) access RAMFS

**Boot Example:** `TEST.mbapp`
A test MBAPP v1 app is seeded at boot that prints `MBAP!` via putchar syscalls, then exits.

**Packer Example (Pseudo-code):**
```c
// To create an MBAPP v1 file:
struct mbapp_header hdr = {
    .magic = 0x50504142,
    .version = 1,
    .flags = 0,
    .entry_off = 0,        // or offset to main code
    .image_size = payload_bytes
};
fwrite(&hdr, sizeof(hdr), 1, mbapp_file);
fwrite(payload, payload_bytes, 1, mbapp_file);
```

**Using in MBos:**
- `fsls` - List boot files (see `TEST.mbapp`, `TEST.exe`)
- `appfmt TEST.mbapp` - Detect format (shows `mbapp`)
- `appfmt TEST.exe` - Detect format (shows `pe32`)
- `appload TEST.exe` - Load and validate Windows app
- `apprun TEST.exe` - Load + enter ring3 execution

### 3. RAW (Unformatted Binary)
- **Detection:** Any file that is not ELF32 or MBAPP v1
- **Behavior:** Loaded directly into code VA, executed from offset 0
- **Use:** Quick bytecode blobs without headers

### 4. PE32 (Windows i386 Binaries)
- **Detection:** Magic bytes `'M'`, `'Z'` (MZ header)
- **Loader:** Real PE32 header parsing with section mapping
- **Features:** Section zero-fill, imageBase relocation to user-space VA
- **Limitations:** No dynamic linking, no import table resolution (stub only)
- **Status:** Fully implemented and boot-tested with TEST.exe
- **Example:** Compile C → i386 PE32 with MSVC or MinGW

**Boot Example:** `TEST.exe`
A test PE32 app is seeded at boot with one .text section that prints `PE32!` via putchar syscalls, then exits.

## Syscall Interface

User mode apps interact with the kernel via `int 0x80` where `eax` contains the syscall ID and `ebx` the first argument.

**Process Control (IDs 1-7):**
- `1` - magic (returns 0x42)
- `2` - putchar (write ebx to terminal)
- `3` - yield (reschedule)
- `4` - exit (with return code in ebx)
- `5` - sleep (for ebx ticks)
- `6` - eventwait (block on event channel ebx)
- `7` - eventsignal (wake channel ebx)

**File I/O (IDs 8-13):**
- `8` - open(name_ptr, flags) → fd
- `9` - read(fd, buf_ptr, max_bytes) → bytes_read
- `10` - write(fd, buf_ptr, bytes) → bytes_written
- `11` - close(fd) → status
- `12` - seek(fd, offset) → new_offset
- `13` - size(fd) → file_size

**Memory & Time (IDs 14-15):**
- `14` - brk(addr) - Query (addr=0) or set heap break
- `15` - getticks() - Return current timer ticks

## Next steps

- Move toward paging and memory management
- Add saved register snapshots per task slot for true context restore experimentation
- Add PS/2 mouse packet decode and bridge mouse movement/clicks into event channels
- Implement MBAPP packer toolchain (header generation utilities)
- Add Windows PE32 loader and Win32 ABI compatibility layer
- Add dynamic linking and shared object support
