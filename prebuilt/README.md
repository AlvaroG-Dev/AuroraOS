# AuroraOS prebuilt software

Precompiled software from other Linux distributions can be staged here without recompiling it.

The intended layout mirrors the target filesystem, for example:

`prebuilt/ubuntu/usr/bin/bash`

The staging system will inspect ELF metadata (`PT_INTERP` and `DT_NEEDED`), resolve shared-library dependencies from AuroraOS's library pool, and install each required library only once in the generated `sysroot`.

A prebuilt binary is only installable when its ABI, dynamic loader, symbol versions, syscalls and other runtime requirements are supported by AuroraOS.
