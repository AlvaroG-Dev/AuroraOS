; Guest de la mini-VM (F1). nasm -f bin hola.asm -o hola.bin
; aurora-vm --plano hola.bin   (carga en 0x1000) imprime "Hola desde la VM".
; Es el mismo codigo que lleva incorporado `aurora-vm --autotest`.
bits 16
org 0x1000
    mov si, msg
l:  lodsb
    test al, al
    jz done
    mov dx, 0x3F8
    out dx, al
    jmp l
done:
    hlt
msg: db "Hola desde la VM", 10, 0
