; kernel/uaccess_faults.asm
[BITS 64]

section .uaccess_table
align 8

extern raw_copy_from_user_fault
extern raw_copy_from_user_fixup
extern raw_copy_to_user_fault
extern raw_copy_to_user_fixup
extern raw_strncpy_from_user_fault
extern raw_strncpy_from_user_fixup
extern raw_cmpxchg_user_u32_fault
extern raw_cmpxchg_user_u32_fixup

    dq raw_copy_from_user_fault
    dq raw_copy_from_user_fixup
    dq raw_copy_to_user_fault
    dq raw_copy_to_user_fixup
    dq raw_strncpy_from_user_fault
    dq raw_strncpy_from_user_fixup
    dq raw_cmpxchg_user_u32_fault
    dq raw_cmpxchg_user_u32_fixup
    dq 0
    dq 0

section .note.GNU-stack noalloc noexec nowrite progbits