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

    /* Read-only/executable sections belong exclusively to the R-X segment. */
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
     * Start the writable segment after the complete R-X image.
     * ALIGN is deliberately relative to the current location: .text can
     * extend beyond 0x401000, so a fixed address there would overlap it.
     */
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
