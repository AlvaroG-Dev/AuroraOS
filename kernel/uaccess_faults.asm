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

    dq raw_copy_from_user_fault
    dq raw_copy_from_user_fixup
    dq raw_copy_to_user_fault
    dq raw_copy_to_user_fixup
    dq raw_strncpy_from_user_fault
    dq raw_strncpy_from_user_fixup
    dq 0
    dq 0