#!/bin/sh
# Compila aurora-desktop.exe (PE de 64 bits, consola, sin CRT) con clang + lld.
set -e
cd "$(dirname "$0")"
t=$(mktemp -d)
printf 'LIBRARY user32.dll\nEXPORTS\nGetDesktopWindow\nGetPropW\nWaitForInputIdle\n' > $t/user32.def
printf 'LIBRARY kernel32.dll\nEXPORTS\nSleep\nGetTickCount\nGetStdHandle\nWriteFile\nExitProcess\nCreateProcessW\nWaitForSingleObject\n' > $t/kernel32.def
llvm-dlltool -m i386:x86-64 -d $t/user32.def -l $t/libuser32.a
llvm-dlltool -m i386:x86-64 -d $t/kernel32.def -l $t/libkernel32.a
clang --target=x86_64-w64-windows-gnu -O2 -ffreestanding -fno-stack-protector -nostdlib \
    -fuse-ld=lld -Wl,--subsystem,console -Wl,-e,start -L$t -o aurora-desktop.exe \
    aurora-desktop.c -luser32 -lkernel32
rm -rf $t
echo "aurora-desktop.exe: $(wc -c < aurora-desktop.exe) bytes"
