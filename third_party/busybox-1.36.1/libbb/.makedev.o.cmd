cmd_libbb/makedev.o := x86_64-linux-musl-gcc -Wp,-MD,libbb/.makedev.o.d  -std=gnu99 -Iinclude -Ilibbb  -include include/autoconf.h -D_GNU_SOURCE -DNDEBUG -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 -DBB_VER='"1.36.1"' -malign-data=abi -Wall -Wshadow -Wwrite-strings -Wundef -Wstrict-prototypes -Wunused -Wunused-parameter -Wunused-function -Wunused-value -Wmissing-prototypes -Wmissing-declarations -Wno-format-security -Wdeclaration-after-statement -Wold-style-definition -finline-limit=0 -fno-builtin-strlen -fomit-frame-pointer -ffunction-sections -fdata-sections -fno-guess-branch-probability -funsigned-char -static-libgcc -falign-functions=1 -falign-jumps=1 -falign-labels=1 -falign-loops=1 -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-builtin-printf -Os -fpie    -DKBUILD_BASENAME='"makedev"'  -DKBUILD_MODNAME='"makedev"' -c -o libbb/makedev.o libbb/makedev.c

deps_libbb/makedev.o := \
  libbb/makedev.c \
  /home/alvaro/aurora-os-copy/toolchain/x86_64-linux-musl-cross/x86_64-linux-musl/include/stdc-predef.h \
  include/platform.h \
    $(wildcard include/config/werror.h) \
    $(wildcard include/config/big/endian.h) \
    $(wildcard include/config/little/endian.h) \
    $(wildcard include/config/nommu.h) \
  /home/alvaro/aurora-os-copy/toolchain/x86_64-linux-musl-cross/x86_64-linux-musl/include/limits.h \
  /home/alvaro/aurora-os-copy/toolchain/x86_64-linux-musl-cross/x86_64-linux-musl/include/features.h \
  /home/alvaro/aurora-os-copy/toolchain/x86_64-linux-musl-cross/x86_64-linux-musl/include/bits/alltypes.h \
  /home/alvaro/aurora-os-copy/toolchain/x86_64-linux-musl-cross/x86_64-linux-musl/include/bits/limits.h \
  /home/alvaro/aurora-os-copy/toolchain/x86_64-linux-musl-cross/x86_64-linux-musl/include/byteswap.h \
  /home/alvaro/aurora-os-copy/toolchain/x86_64-linux-musl-cross/x86_64-linux-musl/include/stdint.h \
  /home/alvaro/aurora-os-copy/toolchain/x86_64-linux-musl-cross/x86_64-linux-musl/include/bits/stdint.h \
  /home/alvaro/aurora-os-copy/toolchain/x86_64-linux-musl-cross/x86_64-linux-musl/include/endian.h \
  /home/alvaro/aurora-os-copy/toolchain/x86_64-linux-musl-cross/x86_64-linux-musl/include/stdbool.h \
  /home/alvaro/aurora-os-copy/toolchain/x86_64-linux-musl-cross/x86_64-linux-musl/include/unistd.h \
  /home/alvaro/aurora-os-copy/toolchain/x86_64-linux-musl-cross/x86_64-linux-musl/include/bits/posix.h \
  /home/alvaro/aurora-os-copy/toolchain/x86_64-linux-musl-cross/x86_64-linux-musl/include/sys/sysmacros.h \

libbb/makedev.o: $(deps_libbb/makedev.o)

$(deps_libbb/makedev.o):
