ENTRY(_start)
       
SECTIONS
{
    . = 0xFFFFFFFF80000000 + 0x1000000;
    .text : AT(ADDR(.text) - 0xFFFFFFFF80000000)
    {
        __text_start = .;
        *(.text)
        *(.text.*)
        __text_end = .;
    }
    . = ALIGN(4096);
    .rodata : AT(ADDR(.rodata) - 0xFFFFFFFF80000000)
    {
        __rodata_start = .;
        *(.rodata)
        *(.rodata.*)
        __rodata_end = .;
    }
    . = ALIGN(8);
    .uaccess_table : AT(ADDR(.uaccess_table) - 0xFFFFFFFF80000000)
    {
        __uaccess_table_start = .;
        KEEP(*(.uaccess_table))
        __uaccess_table_end = .;
    }
    . = ALIGN(8);
    .tests : AT(ADDR(.tests) - 0xFFFFFFFF80000000)
    {
        __tests_start = .;
        KEEP(*(.tests))
        __tests_end = .;
    }
    . = ALIGN(4096);
    .data : AT(ADDR(.data) - 0xFFFFFFFF80000000)
    {
        __data_start = .;
        *(.data)
        *(.data.*)
        __data_end = .;
    }
    . = ALIGN(4096);
    .bss : AT(ADDR(.bss) - 0xFFFFFFFF80000000)
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
ASSERT(_kernel_end <= 0xFFFFFFFF90000000,
       "El kernel solapa con HEAP_VMA: sube HEAP_VMA en kernel/memlayout.h")
