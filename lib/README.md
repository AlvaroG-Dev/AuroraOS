# AuroraOS shared libraries

This directory is the **library source pool** used when assembling the AuroraOS root filesystem.

Libraries are not copied once per executable. A dependency is resolved once and installed into the generated `sysroot` at its runtime path. Multiple programs may therefore share the same library file.

The directory is intentionally separate from `sysroot/lib` and `sysroot/usr/lib`: `sysroot/` is generated build output and must not be treated as the source of libraries.

Future package metadata will live alongside the library pool so that `.deb` imports and other prebuilt packages can declare/provide exact ABI versions.
