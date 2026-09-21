AS      = nasm

CROSS := $(shell which x86_64-elf-gcc 2>/dev/null)
ifeq ($(CROSS),)
    CC = gcc
    CFLAGS = -ffreestanding -O2 -Wall -Wextra -fno-exceptions -fno-stack-protector \
             -nostdlib -nostartfiles -nodefaultlibs -I. -I./lib -mcmodel=small -m64 -fno-pic -fstack-clash-protection
else
    CC = x86_64-elf-gcc
    CFLAGS = -ffreestanding -O2 -Wall -Wextra -fno-exceptions -fno-stack-protector \
             -nostdlib -nostartfiles -nodefaultlibs -I. -I./lib -mcmodel=small -m64 -fstack-clash-protection
endif

LD      = $(CC)
LDFLAGS = -nostdlib -T link.ld -no-pie -m64

LIB_OBJS = crt0.o syscall.o lib/string.o lib/malloc.o lib/file.o lib/ipc.o lib/process.o

APPS = hello calc filetest ipctest pftest waitpid_stress mmap_race mmap_race_worker vma_overlap vma_isolation vma_isolation_worker shell \
       segvtest segvtest_stack segvtest_nx segvtest_mmap_ro segvtest_kernel

all: $(APPS)

lib/string.o: lib/string.c lib/string.h
	$(CC) $(CFLAGS) -c lib/string.c -o lib/string.o

lib/malloc.o: lib/malloc.c lib/malloc.h
	$(CC) $(CFLAGS) -c lib/malloc.c -o lib/malloc.o

lib/file.o: lib/file.c lib/file.h
	$(CC) $(CFLAGS) -c lib/file.c -o lib/file.o

lib/ipc.o: lib/ipc.c lib/ipc.h
	$(CC) $(CFLAGS) -c lib/ipc.c -o lib/ipc.o

lib/process.o: lib/process.c lib/process.h
	$(CC) $(CFLAGS) -c lib/process.c -o lib/process.o

crt0.o: crt0.asm
	$(AS) -f elf64 $< -o $@

syscall.o: syscall.c syscall.h
	$(CC) $(CFLAGS) -c $< -o $@

# ---------------------------------------------------------------------------
# Apps "normales" (terminan con SYS_EXIT)
# ---------------------------------------------------------------------------

hello: apps/hello/main.c $(LIB_OBJS)
	$(CC) $(CFLAGS) -c apps/hello/main.c -o hello.o
	$(LD) -o hello $(LIB_OBJS) hello.o $(LDFLAGS)
	mkdir -p ../sysroot/apps
	cp hello ../sysroot/apps/

calc: apps/calc/main.c $(LIB_OBJS)
	$(CC) $(CFLAGS) -c apps/calc/main.c -o calc.o
	$(LD) -o calc $(LIB_OBJS) calc.o $(LDFLAGS)
	mkdir -p ../sysroot/apps
	cp calc ../sysroot/apps/

filetest: apps/filetest/main.c $(LIB_OBJS)
	$(CC) $(CFLAGS) -c apps/filetest/main.c -o filetest.o
	$(LD) -o filetest $(LIB_OBJS) filetest.o $(LDFLAGS)
	mkdir -p ../sysroot/apps
	cp filetest ../sysroot/apps/

ipctest: apps/ipctest/main.c $(LIB_OBJS)
	$(CC) $(CFLAGS) -c apps/ipctest/main.c -o ipctest.o
	$(LD) -o ipctest $(LIB_OBJS) ipctest.o $(LDFLAGS)
	mkdir -p ../sysroot/apps
	cp ipctest ../sysroot/apps/

pftest: apps/pftest/main.c $(LIB_OBJS)
	$(CC) $(CFLAGS) -c apps/pftest/main.c -o pftest.o
	$(LD) -o pftest $(LIB_OBJS) pftest.o $(LDFLAGS)
	mkdir -p ../sysroot/apps
	cp pftest ../sysroot/apps/

# ---------------------------------------------------------------------------
# Stress de lifecycle: spawn + waitpid repetidos
# ---------------------------------------------------------------------------

mmap_race_worker: apps/mmap_race_worker/main.c $(LIB_OBJS)
	$(CC) $(CFLAGS) -c apps/mmap_race_worker/main.c -o mmap_race_worker.o
	$(LD) -o mmap_race_worker $(LIB_OBJS) mmap_race_worker.o $(LDFLAGS)
	mkdir -p ../sysroot/apps
	cp mmap_race_worker ../sysroot/apps/

mmap_race: apps/mmap_race/main.c $(LIB_OBJS)
	$(CC) $(CFLAGS) -c apps/mmap_race/main.c -o mmap_race.o
	$(LD) -o mmap_race $(LIB_OBJS) mmap_race.o $(LDFLAGS)
	mkdir -p ../sysroot/apps
	cp mmap_race ../sysroot/apps/

vma_overlap: apps/vma_overlap/main.c $(LIB_OBJS)
	$(CC) $(CFLAGS) -c apps/vma_overlap/main.c -o vma_overlap.o
	$(LD) -o vma_overlap $(LIB_OBJS) vma_overlap.o $(LDFLAGS)
	mkdir -p ../sysroot/apps
	cp vma_overlap ../sysroot/apps/

vma_isolation_worker: apps/vma_isolation_worker/main.c $(LIB_OBJS)
	$(CC) $(CFLAGS) -c apps/vma_isolation_worker/main.c -o vma_isolation_worker.o
	$(LD) -o vma_isolation_worker $(LIB_OBJS) vma_isolation_worker.o $(LDFLAGS)
	mkdir -p ../sysroot/apps
	cp vma_isolation_worker ../sysroot/apps/

vma_isolation: apps/vma_isolation/main.c $(LIB_OBJS)
	$(CC) $(CFLAGS) -c apps/vma_isolation/main.c -o vma_isolation.o
	$(LD) -o vma_isolation $(LIB_OBJS) vma_isolation.o $(LDFLAGS)
	mkdir -p ../sysroot/apps
	cp vma_isolation ../sysroot/apps/

waitpid_stress: apps/waitpid_stress/main.c $(LIB_OBJS)
	$(CC) $(CFLAGS) -c apps/waitpid_stress/main.c -o waitpid_stress.o
	$(LD) -o waitpid_stress $(LIB_OBJS) waitpid_stress.o $(LDFLAGS)
	mkdir -p ../sysroot/apps
	cp waitpid_stress ../sysroot/apps/

# ---------------------------------------------------------------------------
# Apps de segfault (deben morir por #PF)
# ---------------------------------------------------------------------------

segvtest: apps/segvtest/main.c $(LIB_OBJS)
	$(CC) $(CFLAGS) -c apps/segvtest/main.c -o segvtest.o
	$(LD) -o segvtest $(LIB_OBJS) segvtest.o $(LDFLAGS)
	mkdir -p ../sysroot/apps
	cp segvtest ../sysroot/apps/

segvtest_stack: apps/segvtest_stack/main.c $(LIB_OBJS)
	$(CC) $(CFLAGS) -c apps/segvtest_stack/main.c -o segvtest_stack.o
	$(LD) -o segvtest_stack $(LIB_OBJS) segvtest_stack.o $(LDFLAGS)
	mkdir -p ../sysroot/apps
	cp segvtest_stack ../sysroot/apps/

segvtest_nx: apps/segvtest_nx/main.c $(LIB_OBJS)
	$(CC) $(CFLAGS) -c apps/segvtest_nx/main.c -o segvtest_nx.o
	$(LD) -o segvtest_nx $(LIB_OBJS) segvtest_nx.o $(LDFLAGS)
	mkdir -p ../sysroot/apps
	cp segvtest_nx ../sysroot/apps/

segvtest_mmap_ro: apps/segvtest_mmap_ro/main.c $(LIB_OBJS)
	$(CC) $(CFLAGS) -c apps/segvtest_mmap_ro/main.c -o segvtest_mmap_ro.o
	$(LD) -o segvtest_mmap_ro $(LIB_OBJS) segvtest_mmap_ro.o $(LDFLAGS)
	mkdir -p ../sysroot/apps
	cp segvtest_mmap_ro ../sysroot/apps/

segvtest_kernel: apps/segvtest_kernel/main.c $(LIB_OBJS)
	$(CC) $(CFLAGS) -c apps/segvtest_kernel/main.c -o segvtest_kernel.o
	$(LD) -o segvtest_kernel $(LIB_OBJS) segvtest_kernel.o $(LDFLAGS)
	mkdir -p ../sysroot/apps
	cp segvtest_kernel ../sysroot/apps/

shell: apps/shell/main.c $(LIB_OBJS)
	$(CC) $(CFLAGS) -c apps/shell/main.c -o shell.o
	$(LD) -o shell $(LIB_OBJS) shell.o $(LDFLAGS)
	mkdir -p ../sysroot/apps
	cp shell ../sysroot/apps/

# ---------------------------------------------------------------------------
# Clean
# ---------------------------------------------------------------------------

clean:
	rm -f *.o lib/*.o
	rm -f hello calc filetest ipctest pftest waitpid_stress mmap_race mmap_race_worker vma_overlap vma_isolation vma_isolation_worker
	rm -f segvtest segvtest_stack segvtest_nx segvtest_mmap_ro segvtest_kernel shell