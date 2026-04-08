ENTRY(start)

PHDRS
{
  text PT_LOAD FLAGS(5);
  data PT_LOAD FLAGS(6);
}

SECTIONS
{
  . = 1M;

  __kernel_start = .;

  .multiboot :
  {
    *(.multiboot)
  } :text

  .text :
  {
    *(.text*)
  } :text

  .rodata :
  {
    *(.rodata*)
  } :text

  .data :
  {
    *(.data*)
  } :data

  .bss :
  {
    *(COMMON)
    *(.bss*)
  } :data

  __kernel_end = .;
}
