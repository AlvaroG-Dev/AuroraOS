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
# AuroraOS uses 4 KiB pages. Keep PT_LOAD segments page-aligned and do not
# emit a linker-generated build-id note into the loadable image.
LDFLAGS = -nostdlib -T link.ld -no-pie -m64 -Wl,-z,max-page-size=0x1000,-z,common-page-size=0x1000,--build-id=none

LIB_OBJS = crt0.o syscall.o lib/string.o lib/malloc.o lib/file.o lib/ipc.o lib/process.o

APPS = hello calc filetest ipctest pftest waitpid_stress mmap_race \
       mmap_race_worker vma_overlap vma_isolation vma_isolation_worker \
       munmap_partial elf_validation elf_rotest elf_permtest sbrk_shrink_test \
       sbrk_shrink calloc_overflow shell ls cat app_mkdir app_rm app_cp app_pwd \
       app_mv \
       segvtest segvtest_stack segvtest_nx segvtest_mmap_ro segvtest_kernel \
       win_blit_fault

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

munmap_partial: apps/munmap_partial/main.c $(LIB_OBJS)
	$(CC) $(CFLAGS) -c apps/munmap_partial/main.c -o munmap_partial.o
	$(LD) -o munmap_partial $(LIB_OBJS) munmap_partial.o $(LDFLAGS)
	mkdir -p ../sysroot/apps
	cp munmap_partial ../sysroot/apps/

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

elf_validation: apps/elf_validation/main.c $(LIB_OBJS)
	$(CC) $(CFLAGS) -c apps/elf_validation/main.c -o elf_validation.o
	$(LD) -o elf_validation $(LIB_OBJS) elf_validation.o $(LDFLAGS)
	mkdir -p ../sysroot/apps
	cp elf_validation ../sysroot/apps/

sbrk_shrink_test: apps/sbrk_shrink_test/main.c $(LIB_OBJS)
	$(CC) $(CFLAGS) -c apps/sbrk_shrink_test/main.c -o sbrk_shrink_test.o
	$(LD) -o sbrk_shrink_test $(LIB_OBJS) sbrk_shrink_test.o $(LDFLAGS)
	mkdir -p ../sysroot/apps
	cp sbrk_shrink_test ../sysroot/apps/

sbrk_shrink: apps/sbrk_shrink/main.c $(LIB_OBJS)
	$(CC) $(CFLAGS) -c apps/sbrk_shrink/main.c -o sbrk_shrink.o
	$(LD) -o sbrk_shrink $(LIB_OBJS) sbrk_shrink.o $(LDFLAGS)
	mkdir -p ../sysroot/apps
	cp sbrk_shrink ../sysroot/apps/

elf_rotest: apps/elf_rotest/main.c apps/elf_rotest/link.ld $(LIB_OBJS)
	$(CC) $(CFLAGS) -c apps/elf_rotest/main.c -o elf_rotest.o
	$(LD) -o elf_rotest $(LIB_OBJS) elf_rotest.o -T apps/elf_rotest/link.ld $(LDFLAGS)
	mkdir -p ../sysroot/apps
	cp elf_rotest ../sysroot/apps/

elf_permtest: apps/elf_permtest/main.c $(LIB_OBJS)
	$(CC) $(CFLAGS) -c apps/elf_permtest/main.c -o elf_permtest.o
	$(LD) -o elf_permtest $(LIB_OBJS) elf_permtest.o $(LDFLAGS)
	mkdir -p ../sysroot/apps
	cp elf_permtest ../sysroot/apps/

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

calloc_overflow: apps/calloc_overflow/main.c $(LIB_OBJS)
	$(CC) $(CFLAGS) -c apps/calloc_overflow/main.c -o calloc_overflow.o
	$(LD) -o calloc_overflow $(LIB_OBJS) calloc_overflow.o $(LDFLAGS)
	mkdir -p ../sysroot/apps
	cp calloc_overflow ../sysroot/apps/

shell: apps/shell/main.c $(LIB_OBJS)
	$(CC) $(CFLAGS) -c apps/shell/main.c -o shell.o
	$(LD) -o shell $(LIB_OBJS) shell.o $(LDFLAGS)
	mkdir -p ../sysroot/apps
	cp shell ../sysroot/apps/

win_blit_fault: apps/win_blit_fault/main.c $(LIB_OBJS)
	$(CC) $(CFLAGS) -c apps/win_blit_fault/main.c -o win_blit_fault.o
	$(LD) -o win_blit_fault $(LIB_OBJS) win_blit_fault.o $(LDFLAGS)
	mkdir -p ../sysroot/apps
	cp win_blit_fault ../sysroot/apps/

ls: apps/ls/main.c $(LIB_OBJS)
	$(CC) $(CFLAGS) -c apps/ls/main.c -o ls.o
	$(LD) -o ls $(LIB_OBJS) ls.o $(LDFLAGS)
	mkdir -p ../sysroot/apps
	cp ls ../sysroot/apps/

cat: apps/cat/main.c $(LIB_OBJS)
	$(CC) $(CFLAGS) -c apps/cat/main.c -o cat.o
	$(LD) -o cat $(LIB_OBJS) cat.o $(LDFLAGS)
	mkdir -p ../sysroot/apps
	cp cat ../sysroot/apps/
	
app_mkdir: apps/mkdir/main.c $(LIB_OBJS)
	$(CC) $(CFLAGS) -c apps/mkdir/main.c -o mkdir.o
	$(LD) -o mkdir $(LIB_OBJS) mkdir.o $(LDFLAGS)
	mkdir -p ../sysroot/apps
	cp mkdir ../sysroot/apps/

app_rm: apps/rm/main.c $(LIB_OBJS)
	$(CC) $(CFLAGS) -c apps/rm/main.c -o rm.o
	$(LD) -o rm $(LIB_OBJS) rm.o $(LDFLAGS)
	mkdir -p ../sysroot/apps
	cp rm ../sysroot/apps/

app_cp: apps/cp/main.c $(LIB_OBJS)
	$(CC) $(CFLAGS) -c apps/cp/main.c -o cp.o
	$(LD) -o cp $(LIB_OBJS) cp.o $(LDFLAGS)
	mkdir -p ../sysroot/apps
	cp cp ../sysroot/apps/

app_pwd: apps/pwd/main.c $(LIB_OBJS)
	$(CC) $(CFLAGS) -c apps/pwd/main.c -o pwd.o
	$(LD) -o pwd $(LIB_OBJS) pwd.o $(LDFLAGS)
	mkdir -p ../sysroot/apps
	cp pwd ../sysroot/apps/

app_mv: apps/mv/main.c $(LIB_OBJS)
	$(CC) $(CFLAGS) -c apps/mv/main.c -o mv.o
	$(LD) -o mv $(LIB_OBJS) mv.o $(LDFLAGS)
	mkdir -p ../sysroot/apps
	cp mv ../sysroot/apps/

clean:
	rm -f *.o lib/*.o
	rm -f hello calc filetest ipctest waitpid_stress mmap_race mmap_race_worker vma_overlap vma_isolation vma_isolation_worker elf_validation elf_rotest elf_permtest sbrk_shrink_test sbrk_shrink munmap_partial calloc_overflow
	rm -f segvtest segvtest_stack segvtest_nx segvtest_mmap_ro segvtest_kernel win_blit_fault shell ls cat
	rm -f mkdir rm cp pwd mv