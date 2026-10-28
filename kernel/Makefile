# kernel/Makefile
# Detecta automaticamente el toolchain disponible

AS      = nasm

CROSS := $(shell which x86_64-elf-gcc 2>/dev/null)
ifeq ($(CROSS),)
    CC = gcc
    CFLAGS = -ffreestanding -O2 -Wall -Wextra \
             -fno-exceptions -mno-red-zone \
             -mcmodel=kernel -fno-stack-protector \
             -fno-asynchronous-unwind-tables -fno-pic \
             -nostdlib -nostartfiles -nodefaultlibs -I. \
             -fno-omit-frame-pointer -g -O0 \
             -march=x86-64 -mtune=generic \
             -mno-avx -mno-avx2 -mno-avx512f \
             -Wno-unused-parameter -Wframe-larger-than=2048
else
    CC = x86_64-elf-gcc
    CFLAGS = -ffreestanding -O2 -Wall -Wextra \
             -fno-exceptions -mno-red-zone -mcmodel=kernel \
             -fno-stack-protector -I. \
             -fno-omit-frame-pointer -g -O0 \
             -march=x86-64 -mtune=generic \
             -mno-avx -mno-avx2 -mno-avx512f \
             -Wno-unused-parameter -Wframe-larger-than=2048
endif

LD      = $(CC)
LDFLAGS = -nostdlib -T linker.ld -no-pie -Wl,--build-id=none

OBJS = boot.o isr_stubs.o switch.o syscall.o syscall_entry.o main.o string.o serial.o gdt.o idt.o paging.o pmm.o \
        buddy.o spinlock.o heap.o slab.o sched.o ps2.o rtc.o tarfs.o vfs.o ipc.o bmp.o pci.o cpu.o simd.o boot_console.o\
        process.o elf.o klog.o pf.o panic.o driver.o input.o wait.o tty.o test.o acpi.o apic.o time.o block.o ata_pio.o \
        ata_common.o atapi.o ata_dma.o ahci.o completion.o part.o fat32.o pipe.o pty.o gfx/gfx.o gfx/window.o \
        ipi.o gfx/compositor.o gfx/taskbar.o gfx/font_aa.o gfx/font_manager.o gfx/winsrv.o gfx/gfx_filter.o iso9660.o \
        uaccess.o uaccess_asm.o uaccess_faults.o signal.o procfs.o futex.o swap.o sysctl.o epoll.o signalfd.o unix_socket.o devtmpfs.o evdev.o drm/drm_core.o drm/drm_dumb.o drm/drm_prime.o drm/drm_syncobj.o display_edid.o rseq.o firmware.o drm/drm_sysfs.o drm/drm_linux_scanout.o lkpi_boot.o vt.o sysfs_dyn.o cmdline.o nl_uevent.o timerfd.o ptimer.o pidfd_inotify.o \
        memfd.o sysfs.o tests/pmm_stress.o tests/kernel_tests.o smp_trampoline.o smp_boot.o vdso.o vdso_blob.o tmpfs.o \
		net/skb.o net/netif.o net/checksum.o net/net_tests.o net/loopback.o net/loopback_tests.o net/ip.o net/ip_tests.o \
		net/arp.o net/arp_tests.o net/icmp.o net/icmp_tests.o net/net_proxy.o net/ping.o net/udp.o net/udp_tests.o \
		net/socket.o net/inet_socket.o net/socket_tests.o net/tcp.o net/tcp_tests.o net/dhcp.o acpi_aml.o acpi_pm.o \
		usb/usb_dma.o usb/usb_core.o usb/usb_urb.o usb/xhci/xhci_ring.o usb/xhci/xhci.o usb/xhci/xhci_cmd.o usb/xhci/xhci_port.o \
		usb/xhci/xhci_control.o usb/usb_hid.o usb/usb_storage.o usb/usb_hub.o usb/hid_parser.o usb/usb_uas.o boot_splash.o kthread.o workqueue.o \
		irq_threaded.o ksymtab.o ksymtab_exports.o kur_loader.o sha256.o bus.o aur_loader.o dma_user.o gfx/dock.o gfx/menubar.o gfx/cursor.o gfx/about.o irq_user.o aur_bus.o block_proxy.o input_proxy.o gpu_proxy.o msi_user.o usb/usb_proxy.o kmod_sys.o vmap.o

		 
TARGET  = kernel.elf

all: $(TARGET)

$(TARGET): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

linker.ld: linker.ld.in memlayout.h
	cpp -P -undef -D__LINKER__ -I. -x c $< -o $@

# Dependencias de cabeceras (-MMD genera un .d junto a cada .o): sin esto,
# tocar un .h dejaba objetos viejos con otro layout de struct y el kernel
# arrancaba roto (dock sin pintar, tests fallando...).
DEPFLAGS = -MMD -MP

%.o: %.c
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

# El compositor y las primitivas gráficas son casi todo bucles por píxel: a
# -O0 van varias veces más lentos, y era lo que hacía que la ventana fuera a
# saltos al arrastrarla. Se compilan a -O2 aunque el resto del kernel siga a
# -O0 para depurar (-g y el frame pointer se mantienen).
GFX_CFLAGS = $(CFLAGS) -O2 -fno-strict-aliasing -fno-tree-loop-distribute-patterns
GFX_DEPS = gfx/gfx.h gfx/theme.h gfx/window.h gfx/compositor.h gfx/dock.h \
           gfx/menubar.h gfx/taskbar.h gfx/cursor.h gfx/winsrv.h gfx/about.h \
           simd.h

gfx/%.o: gfx/%.c $(GFX_DEPS)
	$(CC) $(GFX_CFLAGS) $(DEPFLAGS) -c $< -o $@

simd.o: simd.c simd.h
	$(CC) $(GFX_CFLAGS) $(DEPFLAGS) -c $< -o $@

%.o: %.asm
	$(AS) -f elf64 $< -o $@

# ---------------------------------------------------------------------------
# Trampoline SMP: se ensambla a binario plano y se convierte a .o con
# objcopy. Los símbolos se renombran a smp_trampoline_{start,end,size}.
# ---------------------------------------------------------------------------
smp_trampoline.bin: smp_trampoline.asm
	$(AS) -f bin $< -o $@

smp_trampoline.o: smp_trampoline.bin
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
	        --rename-section .data=.smp_trampoline,alloc,load,readonly,data,contents \
	        --redefine-sym _binary_smp_trampoline_bin_start=smp_trampoline_start \
	        --redefine-sym _binary_smp_trampoline_bin_end=smp_trampoline_end \
	        --redefine-sym _binary_smp_trampoline_bin_size=smp_trampoline_size \
	        --add-section .note.GNU-stack=/dev/null \
	        $< $@

# ---------------------------------------------------------------------------
# vDSO: shared object embebido como blob.
#
# Se construye con el toolchain del host (gcc + ld + objcopy) y se
# embebe como blob (mismo truco que smp_trampoline.o). En runtime el
# kernel copia el blob a una pagina y la mapea en cada exec.
#
# El vDSO no tiene GOT/PLT/relocations: solo immediatos. Por eso puede
# vivir a VMA fija sin que el kernel tenga que aplicar parches.
# ---------------------------------------------------------------------------
VDSO_OBJ      = vdso/vdso.o
VDSO_SO       = vdso/vdso.so
VDSO_BLOB_OBJ = vdso_blob.o

$(VDSO_OBJ): vdso/vdso.S
	$(CC) -c -fno-pic -fno-stack-protector -mno-red-zone \
	      -ffreestanding -nostdlib -fno-asynchronous-unwind-tables \
	      $< -o $@

$(VDSO_SO): $(VDSO_OBJ) vdso/vdso.ld vdso/vdso.map
	ld -shared -T vdso/vdso.ld \
	   --version-script=vdso/vdso.map \
	   --hash-style=sysv \
	   --build-id=none -nostdlib \
	   -o $@ $(VDSO_OBJ)

# objcopy del .so ENTERO (no -O binary: queremos conservar el ELF
# header, program headers y secciones dinamicas).
$(VDSO_BLOB_OBJ): $(VDSO_SO)
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
	        --rename-section .data=.vdso_blob,alloc,load,readonly,data,contents \
	        --redefine-sym _binary_vdso_vdso_so_start=vdso_blob_start \
	        --redefine-sym _binary_vdso_vdso_so_end=vdso_blob_end \
	        --redefine-sym _binary_vdso_vdso_so_size=vdso_blob_size \
	        --add-section .note.GNU-stack=/dev/null \
	        $(VDSO_SO) $@


-include $(OBJS:.o=.d)

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(TARGET) smp_trampoline.bin linker.ld \
	      $(VDSO_OBJ) $(VDSO_SO) $(VDSO_BLOB_OBJ)

.PHONY: all clean