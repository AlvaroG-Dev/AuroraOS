/* kernel/vdso/vdso.ld
 * vDSO linker script. El PT_LOAD empieza en offset 0 incluyendo
 * ELF header + program headers (FILEHDR PHDRS). Es la convencion
 * de los vDSOs de Linux y lo que glibc/ld.so esperan.
 *
 * Sin GOT, sin PLT, sin relocations.
 */

PHDRS
{
    text    PT_LOAD    FILEHDR PHDRS FLAGS(5);   /* PF_R | PF_X */
    dynamic PT_DYNAMIC FLAGS(4);                 /* PF_R */
}

SECTIONS
{
    . = SIZEOF_HEADERS;

    .text : {
        *(.text)
        *(.text.*)
    } :text

    .rodata : {
        *(.rodata)
        *(.rodata.*)
    } :text

    .dynsym         : { *(.dynsym) }         :text
    .dynstr         : { *(.dynstr) }         :text
    .hash           : { *(.hash) }           :text
    .gnu.version    : { *(.gnu.version) }    :text
    .gnu.version_d  : { *(.gnu.version_d) }  :text

    .dynamic : {
        *(.dynamic)
    } :text :dynamic

    /DISCARD/ : {
        *(.comment)
        *(.note)
        *(.note.*)
        *(.eh_frame)
        *(.gnu.hash)
        *(.rela.*)
        *(.rel.*)
    }
}