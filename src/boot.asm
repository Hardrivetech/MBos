; MBos Multiboot entry (32-bit)

MBALIGN  equ 1 << 0
MEMINFO  equ 1 << 1
VIDINFO  equ 1 << 2
FLAGS    equ MBALIGN | MEMINFO | VIDINFO
MAGIC    equ 0x1BADB002
CHECKSUM equ -(MAGIC + FLAGS)

VBE_MODE_TYPE   equ 0
VBE_MODE_WIDTH  equ 1024
VBE_MODE_HEIGHT equ 768
VBE_MODE_DEPTH  equ 32

section .multiboot
align 4
    dd MAGIC
    dd FLAGS
    dd CHECKSUM
    ; Keep mode fields at offsets 32..44 for ELF kernels.
    dd 0
    dd 0
    dd 0
    dd 0
    dd 0
    dd VBE_MODE_TYPE
    dd VBE_MODE_WIDTH
    dd VBE_MODE_HEIGHT
    dd VBE_MODE_DEPTH

section .bss
align 16
stack_bottom:
    resb 16384
stack_top:

section .text
global start
extern kernel_main

start:
    cli
    mov esp, stack_top
    push ebx            ; multiboot info pointer
    push eax            ; multiboot magic
    call kernel_main

.hang:
    cli
    hlt
    jmp .hang

section .note.GNU-stack noalloc noexec nowrite progbits
