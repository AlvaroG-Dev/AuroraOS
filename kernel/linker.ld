/* kernel/linker.ld */
/* Script de enlace para el kernel x86_64 */

ENTRY(_start)

KERNEL_VMA = 0xFFFFFFFF80000000;

SECTIONS
{
    . = KERNEL_VMA + 0x1000000;

    .text : AT(ADDR(.text) - KERNEL_VMA)
    {
        __text_start = .;
        *(.text)
        *(.text.*)
        __text_end = .;
    }

    . = ALIGN(4096);
    .rodata : AT(ADDR(.rodata) - KERNEL_VMA)
    {
        __rodata_start = .;
        *(.rodata)
        *(.rodata.*)
        __rodata_end = .;
    }

    /* ------------------------------------------------------------------
     * Tabla de fixups para uaccess. Pares (fault_rip, fixup_rip) de
     * 16 bytes cada uno, terminados por (0, 0).
     * ------------------------------------------------------------------ */
    . = ALIGN(8);
    .uaccess_table : AT(ADDR(.uaccess_table) - KERNEL_VMA)
    {
        __uaccess_table_start = .;
        KEEP(*(.uaccess_table))
        __uaccess_table_end = .;
    }

    /* ------------------------------------------------------------------
     * Sección de tests. Los símbolos __tests_start / __tests_end
     * delimitan el array.
     * ------------------------------------------------------------------ */
    . = ALIGN(8);
    .tests : AT(ADDR(.tests) - KERNEL_VMA)
    {
        __tests_start = .;
        KEEP(*(.tests))
        __tests_end = .;
    }

    . = ALIGN(4096);
    .data : AT(ADDR(.data) - KERNEL_VMA)
    {
        __data_start = .;
        *(.data)
        *(.data.*)
        __data_end = .;
    }

    . = ALIGN(4096);
    .bss : AT(ADDR(.bss) - KERNEL_VMA)
    {
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        __bss_end = .;
    }

    /DISCARD/ :
    {
        *(.comment)
        *(.note)
        *(.note.*)
        *(.eh_frame)
    }

    . = ALIGN(4096);
    _kernel_end = .;
}