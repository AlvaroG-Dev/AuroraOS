/* kernel/linker.ld */
/* Script de enlace para el kernel x86_64 */

ENTRY(_start)

KERNEL_VMA = 0xFFFFFFFF80000000;

SECTIONS
{
    . = KERNEL_VMA + 0x1000000;

    .text : AT(ADDR(.text) - KERNEL_VMA)
    {
        *(.text)
        *(.text.*)
    }

    . = ALIGN(4096);
    .rodata : AT(ADDR(.rodata) - KERNEL_VMA)
    {
        *(.rodata)
        *(.rodata.*)
    }

    . = ALIGN(4096);
    .data : AT(ADDR(.data) - KERNEL_VMA)
    {
        *(.data)
        *(.data.*)
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
        *(.eh_frame)
    }
    
    . = ALIGN(4096);
    _kernel_end = .;
}
