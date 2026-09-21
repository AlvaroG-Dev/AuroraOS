OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);  /* R-X */
    data PT_LOAD FLAGS(6);  /* RW- */
}

SECTIONS
{
    . = 0x400000;

    .text : {
        *(.text*)
    } :text

    .rodata : {
        *(.rodata*)
        *(.rodata.*)
    } :text

    . = ALIGN(0x1000);

    .data : {
        *(.data*)
    } :data

    .bss : {
        *(.bss*)
        *(COMMON)
    } :data

    . = ALIGN(0x1000);
}
