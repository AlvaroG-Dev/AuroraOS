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

    /* All read-only allocatable sections belong to the R-X segment. */
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

    /*
     * Start the writable segment at an explicit 4 KiB boundary.
     * Do not rely on orphan-section placement between .rodata and .data:
     * an orphan such as .eh_frame can otherwise become the first section
     * of the RW PT_LOAD and make both segments start at 0x400000.
     */
    . = 0x401000;

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
