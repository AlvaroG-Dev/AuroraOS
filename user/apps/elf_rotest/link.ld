OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
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

    .eh_frame : {
        *(.eh_frame*)
    } :text

    .gcc_except_table : {
        *(.gcc_except_table*)
    } :text

    . = ALIGN(0x1000);

    .data : {
        *(.data*)
        *(.data.rel.ro*)
    } :data

    .bss : {
        *(.bss*)
        *(COMMON)
    } :data

    .init_array : {
        *(.init_array*)
    } :data

    .fini_array : {
        *(.fini_array*)
    } :data

    . = ALIGN(0x1000);
}
