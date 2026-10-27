// lkpi_test — hito de L4 de LinuxKPI (Plan_LinuxKPI.md): ejercita desde el
// espacio de usuario un driver de Linux compilado contra la capa
// (tests/linuxkpi/lkpi_test_dev, /dev/lkpi_test): read/write/llseek, ioctl,
// mmap de memoria DMA y por fallo de pagina (fork, mprotect, zap), poll,
// epoll, fasync (SIGIO), esperas interrumpidas por senales (EINTR y SIGKILL),
// firmware real, dma_buf con /dev/dri/card0 en los dos sentidos y debugfs.
//
// Carga linuxkpi.kur y lkpi_test_dev.kur con finit_module y los descarga al
// final. Como la suite del kernel tambien carga y descarga la capa, espera a
// que termine (su resumen "[TEST] N tests: ..." en /dev/kmsg).
//
// Salida: "LKPI-TEST: ALL PASS (n comprobaciones)" o "LKPI-TEST: FAIL ...".
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../../../../tests/linuxkpi/lkpi_test_dev/lkpi_test_uapi.h"

#define MODDIR "/lib/modules/linuxkpi"

static int g_pass, g_fail, g_skip;

static void check(int ok, const char *fmt, ...)
{
	va_list ap;

	if (ok) {
		g_pass++;
		return;
	}
	g_fail++;
	printf("lkpi_test: FALLO: ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf(" (errno %d: %s)\n", errno, strerror(errno));
	fflush(stdout);
}

static void skip(const char *what)
{
	g_skip++;
	printf("lkpi_test: OMITIDA: %s\n", what);
}

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ------------------------------------------------------- preparativos */

/*
 * /dev/kmsg se lee por indice de linea sobre un anillo que rota: una lectura
 * incremental puede saltarse lineas. Se relee el anillo entero cada segundo.
 */
static int kmsg_has_summary(void)
{
	char line[512];
	int fd = open("/dev/kmsg", O_RDONLY), found = 0;
	ssize_t n;

	if (fd < 0)
		return -1;
	while (!found && (n = read(fd, line, sizeof(line) - 1)) > 0) {
		line[n] = '\0';
		found = strstr(line, "[TEST] ") && strstr(line, " tests: ") &&
			strstr(line, "passed");
	}
	close(fd);
	return found;
}

static void wait_kernel_suite(void)
{
	long t0 = now_ms();
	int r;

	while ((r = kmsg_has_summary()) == 0 && now_ms() - t0 < 900000)
		sleep(1);
	if (r < 0)
		printf("lkpi_test: sin /dev/kmsg, no se espera a la suite\n");
	else if (!r)
		printf("lkpi_test: la suite del kernel no termino en 15 min\n");
}

static int load(const char *path)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC), rc;

	if (fd < 0)
		return -errno;
	rc = syscall(SYS_finit_module, fd, "", 0);
	rc = rc ? -errno : 0;
	close(fd);
	return rc;
}

static int xioctl(int fd, unsigned long req, void *arg)
{
	return ioctl(fd, req, arg) ? -errno : 0;
}

static struct lkpi_test_info info(int fd)
{
	struct lkpi_test_info in;

	memset(&in, 0, sizeof(in));
	check(xioctl(fd, LKPI_IOC_INFO, &in) == 0, "LKPI_IOC_INFO");
	return in;
}

static int pattern_ok(const unsigned char *p, size_t n, unsigned seed)
{
	for (size_t i = 0; i < n; i++)
		if (p[i] != LKPI_TEST_PATTERN(seed, i))
			return 0;
	return 1;
}

/* ----------------------------------------------------------- pruebas */

static void test_basic(int fd)
{
	struct stat st;
	struct lkpi_test_info in = info(fd);
	unsigned char buf[32];

	check(fstat(fd, &st) == 0 && S_ISCHR(st.st_mode) && major(st.st_rdev) == 10,
	      "/dev/lkpi_test es misc (major %u)", major(st.st_rdev));
	check(in.pid == getpid() && in.tgid == getpid(), "current->pid %d/%d (getpid %d)", in.pid,
	      in.tgid, getpid());
	check(!strcmp(in.comm, "lkpi_test"), "current->comm '%s'", in.comm);
	check(in.has_mm == 1 && in.uid == getuid(), "current->mm y uid");

	check(read(fd, buf, 16) == 16 && pattern_ok(buf, 16, 0x11), "read del principio");
	check(lseek(fd, 0, SEEK_CUR) == 16, "el offset avanza");
	check(lseek(fd, 100, SEEK_SET) == 100, "lseek SEEK_SET");
	check(write(fd, "hola", 4) == 4, "write");
	check(pread(fd, buf, 4, 100) == 4 && !memcmp(buf, "hola", 4), "pread de lo escrito");
	check(lseek(fd, 0, SEEK_END) == LKPI_TEST_MEM_SIZE, "lseek SEEK_END = tamano");
	check(read(fd, buf, 8) == 0, "read al final: EOF");
	errno = 0;
	check(lseek(fd, 1, SEEK_END) == -1 && errno == EINVAL, "lseek fuera: EINVAL");
	check(pwrite(fd, "x", 1, LKPI_TEST_MEM_SIZE - 1) == 1, "pwrite al final");
	errno = 0;
	check(ioctl(fd, _IO('L', 99)) == -1 && errno == ENOTTY, "ioctl desconocido: ENOTTY");
}

static void test_mmap(int fd)
{
	struct lkpi_test_info a = info(fd), b;
	size_t fsz = LKPI_TEST_FAULT_PAGES * 4096;
	unsigned char *p, *q;
	pid_t pid;
	int st, ok = 1;

	/* Memoria coherente (dma_mmap_coherent). */
	p = mmap(NULL, LKPI_TEST_DMA_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	check(p != MAP_FAILED, "mmap de la memoria DMA");
	if (p != MAP_FAILED) {
		check(pattern_ok(p, LKPI_TEST_DMA_SIZE, 0x5A), "contenido de la memoria DMA");
		p[5] = 0xEE;
		q = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
		check(q != MAP_FAILED && q[5] == 0xEE, "dos mapeos ven la misma memoria");
		if (q != MAP_FAILED)
			munmap(q, 4096);
		p[5] = LKPI_TEST_PATTERN(0x5A, 5);
		check(munmap(p, LKPI_TEST_DMA_SIZE) == 0, "munmap DMA");
	}
	errno = 0;
	check(mmap(NULL, 2 * LKPI_TEST_DMA_SIZE, PROT_READ, MAP_SHARED, fd, 0) == MAP_FAILED,
	      "mmap mayor que el buffer: error");

	/* Paginas por fallo (vm_operations_struct). */
	a = info(fd);
	p = mmap(NULL, fsz, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		 (off_t)LKPI_TEST_FAULT_PGOFF * 4096);
	check(p != MAP_FAILED, "mmap por fallo de pagina");
	if (p == MAP_FAILED)
		return;
	b = info(fd);
	check(b.faults == a.faults, "sin fallos antes de tocar");
	for (int i = 0; i < LKPI_TEST_FAULT_PAGES; i++)
		ok &= pattern_ok(p + i * 4096, 4096, 0x40 + i);
	check(ok, "contenido de las paginas por fallo");
	b = info(fd);
	check(b.faults == a.faults + LKPI_TEST_FAULT_PAGES, "un fallo por pagina (%u)",
	      b.faults - a.faults);
	check(xioctl(fd, LKPI_IOC_ZAP, NULL) == 0, "LKPI_IOC_ZAP");
	check(pattern_ok(p + 4096, 4096, 0x41), "tras zap se vuelve a pedir la pagina");
	a = info(fd);
	check(a.faults == b.faults + 1, "un fallo nuevo tras el zap");

	/* fork: el hijo hereda el mapeo (vm_ops->open) y lo cierra al salir. */
	a = info(fd);
	pid = fork();
	if (pid == 0)
		_exit(pattern_ok(p, 4096, 0x40) ? 0 : 1);
	check(pid > 0 && waitpid(pid, &st, 0) == pid && WIFEXITED(st) && !WEXITSTATUS(st),
	      "el hijo ve las paginas");
	b = info(fd);
	check(b.vma_opens == a.vma_opens + 1 && b.vma_closes == a.vma_closes + 1,
	      "fork: open %u close %u", b.vma_opens - a.vma_opens, b.vma_closes - a.vma_closes);

	/* mprotect en medio: el VMA se parte en tres (dos open). */
	a = info(fd);
	check(mprotect(p + 4096, 4096, PROT_READ) == 0, "mprotect en medio");
	b = info(fd);
	check(b.vma_opens == a.vma_opens + 2, "mprotect: %u open", b.vma_opens - a.vma_opens);
	check(pattern_ok(p + 4096, 4096, 0x41), "pagina tras mprotect");
	check(munmap(p, fsz) == 0, "munmap");
	a = info(fd);
	check(a.vma_closes == b.vma_closes + 3, "munmap: %u close", a.vma_closes - b.vma_closes);
}

static volatile sig_atomic_t g_sigio, g_alarm;
static void on_sigio(int s) { (void)s; g_sigio++; }
static void on_alarm(int s) { (void)s; g_alarm++; }

static void raise_later(int fd, uint32_t ms)
{
	check(xioctl(fd, LKPI_IOC_RAISE_LATER, &ms) == 0, "LKPI_IOC_RAISE_LATER");
}

static void consume(int fd)
{
	struct lkpi_test_wait w;

	check(xioctl(fd, LKPI_IOC_WAIT, &w) == 0, "LKPI_IOC_WAIT");
}

static void test_events(int fd)
{
	struct pollfd pfd = { .fd = fd, .events = POLLIN | POLLOUT };
	struct epoll_event ev = { .events = EPOLLIN }, out;
	struct sigaction sa;
	struct lkpi_test_wait w;
	long t0;
	int ep, fl, st;
	pid_t pid;

	/* poll */
	check(poll(&pfd, 1, 0) == 1 && pfd.revents == POLLOUT, "poll: sin eventos, solo POLLOUT");
	pfd.events = POLLIN;
	raise_later(fd, 100);
	t0 = now_ms();
	check(poll(&pfd, 1, 3000) == 1 && (pfd.revents & POLLIN), "poll despierta con el evento");
	check(now_ms() - t0 >= 50, "poll espero al temporizador (%ld ms)", now_ms() - t0);
	consume(fd);
	check(poll(&pfd, 1, 0) == 0, "evento consumido");

	/* epoll */
	ep = epoll_create1(EPOLL_CLOEXEC);
	ev.data.fd = fd;
	check(ep >= 0 && epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) == 0, "epoll_ctl");
	check(epoll_wait(ep, &out, 1, 0) == 0, "epoll: nada pendiente");
	raise_later(fd, 50);
	check(epoll_wait(ep, &out, 1, 3000) == 1 && (out.events & EPOLLIN) && out.data.fd == fd,
	      "epoll despierta con el evento");
	consume(fd);
	close(ep);

	/* fasync: SIGIO al dueno */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_sigio;
	sigaction(SIGIO, &sa, NULL);
	fl = fcntl(fd, F_GETFL);
	check(fcntl(fd, F_SETOWN, getpid()) == 0, "F_SETOWN");
	check(fcntl(fd, F_GETOWN) == getpid(), "F_GETOWN");
	check(fcntl(fd, F_SETFL, fl | O_ASYNC) == 0 && (fcntl(fd, F_GETFL) & O_ASYNC),
	      "F_SETFL O_ASYNC");
	g_sigio = 0;
	raise_later(fd, 50);
	t0 = now_ms();
	while (!g_sigio && now_ms() - t0 < 3000)
		usleep(10000);
	check(g_sigio == 1, "SIGIO recibido (%d)", (int)g_sigio);
	consume(fd);
	check(fcntl(fd, F_SETFL, fl) == 0 && !(fcntl(fd, F_GETFL) & O_ASYNC), "quitar O_ASYNC");
	g_sigio = 0;
	raise_later(fd, 30);
	consume(fd);
	usleep(100000);
	check(g_sigio == 0, "sin O_ASYNC no hay SIGIO");

	/* Espera interrumpida: EINTR (signal_pending real) */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_alarm;	/* sin SA_RESTART */
	sigaction(SIGALRM, &sa, NULL);
	g_alarm = 0;
	alarm(1);
	t0 = now_ms();
	errno = 0;
	check(ioctl(fd, LKPI_IOC_WAIT, &w) == -1 && errno == EINTR, "WAIT interrumpido: EINTR");
	check(g_alarm == 1 && now_ms() - t0 >= 500, "lo corto SIGALRM (%ld ms)", now_ms() - t0);

	/* SIGKILL a un hijo bloqueado en el driver (fatal_signal_pending) */
	pid = fork();
	if (pid == 0) {
		int cfd = open(LKPI_TEST_DEV, O_RDWR);

		ioctl(cfd, LKPI_IOC_WAIT, &w);
		_exit(3);
	}
	usleep(300000);
	check(kill(pid, SIGKILL) == 0, "kill SIGKILL");
	alarm(5);
	check(waitpid(pid, &st, 0) == pid && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL,
	      "el hijo bloqueado muere por SIGKILL");
	alarm(0);
}

static void test_firmware(int fd)
{
	struct lkpi_test_fw fw;

	memset(&fw, 0, sizeof(fw));
	strcpy(fw.name, "aurora/selftest.bin");
	check(xioctl(fd, LKPI_IOC_FIRMWARE, &fw) == 0 && fw.result == 0 && fw.size == 10,
	      "firmware de Aurora (%d, %u)", fw.result, fw.size);
	strcpy(fw.name, "lkpi/no-existe.bin");
	check(xioctl(fd, LKPI_IOC_FIRMWARE, &fw) == 0 && fw.result == -ENOENT, "firmware que falta");
	strcpy(fw.name, "i915/kbl_dmc_ver1.bin");
	check(xioctl(fd, LKPI_IOC_FIRMWARE, &fw) == 0, "ioctl de firmware");
	if (fw.result == -ENOENT) {
		skip("linux-firmware no instalado en el initrd");
		return;
	}
	check(fw.result == 0 && fw.size == 8616 && fw.crc32 == 0xc11e0fc6u,
	      "linux-firmware i915/kbl_dmc_ver1.bin (%d, %u, %08x)", fw.result, fw.size, fw.crc32);
}

/* DRM (uapi de Linux, solo lo necesario). */
struct drm_gem_close { uint32_t handle, pad; };
struct drm_prime_handle { uint32_t handle, flags; int32_t fd; };
struct drm_mode_create_dumb { uint32_t height, width, bpp, flags, handle, pitch; uint64_t size; };
struct drm_mode_map_dumb { uint32_t handle, pad; uint64_t offset; };
struct drm_mode_destroy_dumb { uint32_t handle; };
#define DRM_IOCTL_GEM_CLOSE _IOW('d', 0x09, struct drm_gem_close)
#define DRM_IOCTL_PRIME_HANDLE_TO_FD _IOWR('d', 0x2d, struct drm_prime_handle)
#define DRM_IOCTL_PRIME_FD_TO_HANDLE _IOWR('d', 0x2e, struct drm_prime_handle)
#define DRM_IOCTL_MODE_CREATE_DUMB _IOWR('d', 0xB2, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_MAP_DUMB _IOWR('d', 0xB3, struct drm_mode_map_dumb)
#define DRM_IOCTL_MODE_DESTROY_DUMB _IOWR('d', 0xB4, struct drm_mode_destroy_dumb)

static void test_dmabuf(int fd)
{
	struct lkpi_test_dmabuf d;
	struct drm_prime_handle ph;
	size_t size = LKPI_TEST_DMABUF_PAGES * 4096;
	unsigned char *p;
	uint32_t sum = 0;
	int card;

	/* Exportado por el driver de Linux. */
	memset(&d, 0, sizeof(d));
	d.seed = 0x33;
	check(xioctl(fd, LKPI_IOC_DMABUF_EXPORT, &d) == 0 && d.fd >= 0 && d.size == size,
	      "dma_buf_fd (%d, %u)", d.fd, d.size);
	if (d.fd < 0)
		return;
	check(fcntl(d.fd, F_GETFD) & FD_CLOEXEC, "O_CLOEXEC del dma_buf_fd");
	p = mmap(NULL, size, PROT_READ, MAP_SHARED, d.fd, 0);
	check(p != MAP_FAILED && pattern_ok(p, size, 0x33), "mmap del dma_buf exportado");
	if (p != MAP_FAILED)
		munmap(p, size);
	for (size_t i = 0; i < size; i++)
		sum += LKPI_TEST_PATTERN(0x33, i);
	{
		struct lkpi_test_dmabuf back = { .fd = d.fd };

		check(xioctl(fd, LKPI_IOC_DMABUF_IMPORT, &back) == 0 && back.seed == 0x33 &&
		      back.sum == sum, "dma_buf_get del propio fd");
	}
	card = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (card < 0) {
		skip("sin /dev/dri/card0");
		close(d.fd);
		return;
	}
	memset(&ph, 0, sizeof(ph));
	ph.fd = d.fd;
	check(xioctl(card, DRM_IOCTL_PRIME_FD_TO_HANDLE, &ph) == 0 && ph.handle,
	      "PRIME_FD_TO_HANDLE del dma_buf de Linux");
	close(d.fd);	/* el GEM importado mantiene vivas las paginas */
	if (ph.handle) {
		struct drm_mode_map_dumb md = { .handle = ph.handle };
		struct drm_gem_close gc = { .handle = ph.handle };

		if (xioctl(card, DRM_IOCTL_MODE_MAP_DUMB, &md) == 0) {
			p = mmap(NULL, size, PROT_READ, MAP_SHARED, card, md.offset);
			check(p != MAP_FAILED && pattern_ok(p, size, 0x33),
			      "/dev/dri ve las paginas del driver de Linux");
			if (p != MAP_FAILED)
				munmap(p, size);
		}
		check(xioctl(card, DRM_IOCTL_GEM_CLOSE, &gc) == 0, "GEM_CLOSE");
	}

	/* Importado: un dumb buffer de /dev/dri exportado por PRIME. */
	{
		struct drm_mode_create_dumb cd = { .height = 64, .width = 64, .bpp = 32 };
		struct drm_mode_map_dumb md;
		struct drm_mode_destroy_dumb dd;
		struct lkpi_test_dmabuf im;

		check(xioctl(card, DRM_IOCTL_MODE_CREATE_DUMB, &cd) == 0, "CREATE_DUMB");
		memset(&md, 0, sizeof(md));
		md.handle = cd.handle;
		check(xioctl(card, DRM_IOCTL_MODE_MAP_DUMB, &md) == 0, "MAP_DUMB");
		p = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, card, md.offset);
		check(p != MAP_FAILED, "mmap del dumb");
		if (p == MAP_FAILED)
			goto out;
		sum = 0;
		for (size_t i = 0; i < cd.size; i++) {
			p[i] = LKPI_TEST_PATTERN(0x77, i);
			sum += p[i];
		}
		munmap(p, cd.size);
		memset(&ph, 0, sizeof(ph));
		ph.handle = cd.handle;
		ph.flags = O_CLOEXEC;
		check(xioctl(card, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph) == 0 && ph.fd >= 0,
		      "PRIME_HANDLE_TO_FD");
		memset(&im, 0, sizeof(im));
		im.fd = ph.fd;
		check(xioctl(fd, LKPI_IOC_DMABUF_IMPORT, &im) == 0, "dma_buf_get de un fd de PRIME");
		check(im.size == cd.size && im.seed == 0x77 && im.sum == sum && im.nents >= 1,
		      "el driver lee el dumb por vmap (size %u seed %x nents %u)", im.size, im.seed,
		      im.nents);
		close(ph.fd);
		dd.handle = cd.handle;
		check(xioctl(card, DRM_IOCTL_MODE_DESTROY_DUMB, &dd) == 0, "DESTROY_DUMB");
	}
out:
	close(card);
}

static void test_debugfs(void)
{
	char buf[256];
	int fd = open("/sys/kernel/debug/lkpi_test/status", O_RDONLY);
	ssize_t n;

	check(fd >= 0, "abrir debugfs lkpi_test/status");
	if (fd < 0)
		return;
	n = read(fd, buf, sizeof(buf) - 1);
	check(n > 0, "leer debugfs");
	if (n > 0) {
		buf[n] = '\0';
		check(strstr(buf, "opens=") && strstr(buf, "faults="), "contenido de status: %s", buf);
	}
	close(fd);
	fd = open("/sys/kernel/debug/lkpi_test/events", O_RDONLY);
	n = fd >= 0 ? read(fd, buf, sizeof(buf) - 1) : -1;
	check(n > 0 && buf[0] >= '0' && buf[0] <= '9', "debugfs_create_u32");
	if (fd >= 0)
		close(fd);
}

int main(void)
{
	struct lkpi_test_info a, b;
	struct stat st;
	int fd, rc;

	setvbuf(stdout, NULL, _IONBF, 0);
	wait_kernel_suite();
	rc = load(MODDIR "/linuxkpi.kur");
	check(rc == 0 || rc == -EEXIST, "cargar linuxkpi.kur (%d)", rc);
	rc = load(MODDIR "/tests/lkpi_test_dev.kur");
	check(rc == 0, "cargar lkpi_test_dev.kur (%d)", rc);
	fd = open(LKPI_TEST_DEV, O_RDWR | O_CLOEXEC);
	check(fd >= 0, "abrir " LKPI_TEST_DEV);
	if (fd >= 0) {
		test_basic(fd);
		test_mmap(fd);
		test_events(fd);
		test_firmware(fd);
		test_dmabuf(fd);
		test_debugfs();
		a = info(fd);
		close(fd);
		fd = open(LKPI_TEST_DEV, O_RDONLY);
		b = info(fd);
		check(b.releases == a.releases + 1 && b.opens == a.opens + 1, "open/release contados");
		errno = 0;
		check(write(fd, "x", 1) == -1 && errno == EBADF, "write en O_RDONLY: EBADF");
		close(fd);
	}
	check(syscall(SYS_delete_module, "lkpi_test_dev", O_NONBLOCK) == 0,
	      "descargar lkpi_test_dev");
	check(stat(LKPI_TEST_DEV, &st) == -1 && errno == ENOENT, "/dev/lkpi_test desaparece");
	check(syscall(SYS_delete_module, "linuxkpi", O_NONBLOCK) == 0, "descargar linuxkpi");
	if (g_fail)
		printf("LKPI-TEST: FAIL %d de %d (%d omitidas)\n", g_fail, g_pass + g_fail, g_skip);
	else
		printf("LKPI-TEST: ALL PASS (%d comprobaciones, %d omitidas)\n", g_pass, g_skip);
	return g_fail ? 1 : 0;
}
