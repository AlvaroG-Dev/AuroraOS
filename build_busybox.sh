#!/bin/bash

export PATH=$HOME/aurora-os-copy/toolchain/x86_64-linux-musl-cross/bin:$PATH

cd third_party/busybox-1.36.1

make clean
make ARCH=x86_64
make install
file _install/bin/busybox
