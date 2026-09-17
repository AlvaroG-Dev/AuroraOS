OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS
{
    all PT_LOAD FLAGS(7);   /* RWX - lectura, escritura, ejecución */
}

SECTIONS
{
    . = 0x400000;

    .text : {
        *(.text*)
    } :all

    .rodata : {
        *(.rodata*)
        *(.rodata.*)          /* Captura .rodata.str1.8, etc. */
    } :all

    .data : {
        *(.data*)
    } :all

    .bss : {
        *(.bss*)
        *(COMMON)
    } :all

    /* Extiende el segmento hasta el límite de página */
    . = ALIGN(0x1000);
}