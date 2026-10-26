#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""linuxkpi-scan.py — que API del kernel de Linux consumen los drivers objetivo.

Plan_LinuxKPI.md, L0.3. Analisis lexico (sin compilar Linux):

 1. Indexa los nombres que DEFINEN las cabeceras de Linux (include/,
    arch/x86/include): macros, funciones (prototipo o static inline), tipos
    (struct/union/enum/typedef), constantes de enum y variables extern.
 2. Para cada driver, tokeniza sus .c/.h, quita lo que el propio driver define
    y se queda con los identificadores que vienen de esas cabeceras.
 3. Clasifica cada simbolo por nivel K0..K9 segun la cabecera que lo define
    (tabla LEVEL_RULES) y escribe un informe Markdown y un TSV completo.

Limitaciones (ver el informe): un nombre definido por una macro generadora
(p. ej. DEFINE_GUARD) no se indexa; las ramas #if de todas las configuraciones
cuentan; un identificador local que coincida con un nombre de cabecera cuenta.
El escaner sobreestima un poco; sirve para ordenar y dimensionar, no para
garantizar que un driver enlaza (eso lo dira el cargador en L5+).

Uso:
  scripts/linuxkpi-scan.py --linux DIR [--nvidia DIR] [--label TXT]
                           [--md docs/linuxkpi_api.md] [--tsv docs/linuxkpi_api.tsv]
"""
import argparse, collections, os, re, sys

# --- Drivers objetivo -------------------------------------------------------
# (nombre, raiz: 'linux'|'nvidia', rutas incluidas (la primera que exista de
# cada grupo 'a|b'), subcadenas de ruta excluidas)
DRIVERS = [
    ('bochs', 'linux', ['drivers/gpu/drm/tiny/bochs.c'], []),
    ('simpledrm', 'linux',
     ['drivers/gpu/drm/sysfb/simpledrm.c|drivers/gpu/drm/tiny/simpledrm.c',
      'drivers/gpu/drm/sysfb/drm_sysfb_helper.c|',
      'drivers/gpu/drm/sysfb/drm_sysfb_modeset.c|',
      'drivers/gpu/drm/sysfb/drm_sysfb_helper.h|'], []),
    ('virtio-gpu', 'linux', ['drivers/gpu/drm/virtio'], []),
    ('i915', 'linux', ['drivers/gpu/drm/i915'],
     ['/selftests/', '/gvt/', '/selftest_', 'mock_', '/kunit']),
    ('xe', 'linux', ['drivers/gpu/drm/xe'], ['/tests/', '_kunit', '/kunit']),
    ('amdgpu', 'linux', ['drivers/gpu/drm/amd'],
     ['/amdkfd/', '/asic_reg/', '/kunit', '/tests/']),
    ('nvidia', 'nvidia',
     ['kernel-open/nvidia|nvidia', 'kernel-open/nvidia-modeset|nvidia-modeset',
      'kernel-open/nvidia-drm|nvidia-drm', 'kernel-open/common|common'], []),
]

# --- Cabecera -> nivel -------------------------------------------------------
# Se prueba en orden; la ruta es relativa a include/ (o arch/x86/include/),
# sin "uapi/" (uapi/linux/x.h cuenta como linux/x.h). Ver Plan_LinuxKPI.md §3.
K9DRM = 'K9-drm'
LEVEL_RULES = [
    # K9 DRM/GPU (con la opcion A, el shim de drm_* propio)
    (r'^drm/ttm/', 'K9-ttm'),
    (r'^drm/gpu_scheduler', 'K9-sched'),
    (r'^drm/drm_(buddy|exec|gpuvm)', 'K9-ttm'),
    (r'^drm/drm_syncobj', 'K7'),
    (r'^drm/(intel|amd)', 'DRV'),        # cabeceras compartidas de un driver
    (r'^linux/(virtio_gpu|kfd_ioctl|kfd_sysfs|amd-pmf-io|vfio)$', 'DRV'),
    (r'^linux/hdmi$', K9DRM),
    (r'^drm/', K9DRM),
    (r'^(linux/)?(trace|tracepoint|ftrace|perf_event|kasan|kcsan|kmsan|kmemleak|'
     r'kcov|static_key|jump_label|static_call|mmu_notifier|hmm|migrate|devfreq|'
     r'swiotlb|cgroup|memcontrol|stackdepot|stacktrace|fault-inject|'
     r'pci-p2pdma|cc_platform|mem_encrypt|kunit|kprobes|tracepoint-defs|'
     r'bpf|btf|psp|vmw_vmci|hyperv|kvm|vfio|mdev|perf|security|lsm)', 'K9'),
    (r'^(trace|kunit|kvm|xen|rdma|net|scsi|target|ras|rv)/', 'K9'),
    (r'^linux/(debugobjects|ref_tracker|relay|oom|sys_soc|tpm|pnp|'
     r'parport|agp|thunderbolt|host1x|pinctrl|mpi|logic_pio|raid/|'
     r'regset|ahci|arch_topology|genl_|zconf|aio)', 'K9'),
    # K7 fences
    (r'^linux/(dma-fence|dma-resv|sync_file|seqno-fence)', 'K7'),
    # K1 memoria
    (r'^linux/(dma-buf|dma-heap)', 'K1-dma'),
    (r'^linux/(dma-|scatterlist|iommu|intel-iommu|iova|swiotlb)', 'K1-dma'),
    (r'^asm/(dma|iommu)', 'K1-dma'),
    (r'^linux/(slab|mm|mm_types|mm_types_task|gfp|gfp_types|vmalloc|highmem|'
     r'highmem-internal|io|io-mapping|iosys-map|ioport|shmem_fs|pagemap|'
     r'pagevec|page[-_]|pfn|memremap|uaccess|swap|mman|mmzone|pgtable|'
     r'mmap_lock|sched/mm|kmemleak|memory|numa|nodemask|huge_mm|folio|'
     r'page_ref|set_memory|vmstat|shrinker|percpu-refcount|mempool|'
     r'memblock|memory_hotplug|secretmem|pgalloc|hugetlb|rmap|writeback|'
     r'ksm|userfaultfd|mm_inline|mempolicy|zswap|balloon|fs_dax|dax)', 'K1'),
    (r'^asm/(io|page|pgtable|set_memory|uaccess|memtype|pat|cacheflush|'
     r'tlbflush|mman|e820|fixmap|highmem|vmalloc|ioremap)', 'K1'),
    (r'^asm-generic/(io|page|pgtable|uaccess|memory_model|getorder|'
     r'cacheflush|tlb|set_memory)', 'K1'),
    # K2 sincronizacion y tiempo
    (r'^linux/(spinlock|mutex|ww_mutex|rwsem|rwlock|semaphore|completion|wait|'
     r'swait|seqlock|rcu|srcu|percpu|timer|hrtimer|workqueue|kthread|sched|'
     r'delay|refcount|kref|lockdep|irq_work|smp|cpumask|preempt|local_lock|'
     r'stop_machine|osq_lock|cpu|cpuhotplug|rcuwait|notifier|lockref|'
     r'freezer|cleanup|sched/|wait_bit|llist|atomic|cmpxchg|bit_spinlock|'
     r'interrupt|hardirq|softirq|irqflags|rtmutex|mutex_types|'
     r'spinlock_types|seqlock_types|timerqueue|alarmtimer|posix-timers|'
     r'tick|clocksource|clockchips|kernel_stat|context_tracking)', 'K2'),
    (r'^asm/(smp|spinlock|qspinlock|qrwlock|preempt|percpu|processor|'
     r'current|irqflags|timer|tsc|delay|msr|cpufeature|cpufeatures|'
     r'special_insns|hardirq|irq)', 'K2'),
    (r'^asm-generic/(qspinlock|qrwlock|percpu|preempt|delay|irq)', 'K2'),
    # K3 estructuras
    (r'^linux/(list|rculist|hlist|rbtree|interval_tree|idr|xarray|'
     r'radix-tree|kfifo|sort|bsearch|string|hash|hashtable|crc|siphash|'
     r'list_sort|circ_buf|maple_tree|generic-radix-tree|rhashtable|'
     r'prime_numbers|min_heap|objpool|union_find|plist|btree|lru_cache|'
     r'linear_range|parser|glob|seq_buf|ctype|uuid|base64|zlib|lz4|'
     r'crc32|crc16|crc8|crc-|textsearch|ascii85|bitrev|reciprocal_div|'
     r'stringhash|xxhash|jhash|math|gcd|lcm|rational|polynomial|'
     r'fixp-arith|int_log|sizes|units|wordpart|log2|minmax|overflow|'
     r'string_helpers|string_choices|kstrtox|sprintf|array_size|'
     r'fortify-string|errname|average|win_minmax|ewma)', 'K3'),
    (r'^asm/(string|word-at-a-time)', 'K3'),
    (r'^crypto/', 'K3'),
    # K4 modelo de dispositivos
    (r'^linux/(device|kobject|sysfs|kernfs|module|moduleparam|export|'
     r'platform_device|of|fwnode|property|mod_devicetable|kdev_t|'
     r'auxiliary_bus|devres|klist|kobj_map|driver|dev_printk|init|'
     r'kmod|bus|class|kallsyms|livepatch|cdev|dmapool)', 'K4'),
    # K5 PCI/IRQ/energia (y el bus virtio, que usa virtio-gpu)
    (r'^linux/virtio', 'K5'),
    (r'^linux/(pci|msi|irq|pm|pm_runtime|pm_qos|pm_wakeup|suspend|vgaarb|'
     r'vga_switcheroo|irqdomain|irqreturn|irqdesc|irqchip|cpufreq|'
     r'cpuidle|reboot|pcieport|aer|aperture|energy_model|'
     r'pm_domain|powercap|syscore_ops|hibernation)', 'K5'),
    (r'^asm/(pci|msi|irq_vectors|io_apic|apic|hw_irq|intel-family|'
     r'cpu_device_id|iosf_mbi|intel_pmc|x86_init|hypervisor|'
     r'reboot|e820/)', 'K5'),
    # K6 firmware, i2c, gpio, dmi, acpi, backlight...
    (r'^(acpi|video|sound|media|soc|misc|clocksource|memory|keys|'
     r'dt-bindings|pcmcia|ufs)/', 'K6'),
    (r'^linux/(firmware|i2c|gpio|dmi|acpi|backlight|leds|thermal|hwmon|'
     r'debugfs|component|mfd/|regulator|power_supply|input|extcon|'
     r'platform_data|mei|fb|clk|reset|efi|vga|screen_info|sysfb|'
     r'console|vt|tty|kd|fbcon|apple-gmux|iosys|seq_file|'
     r'pwm|watchdog|nvmem|eeprom|mtd|spi|phy|usb|hid|edac|cec|'
     r'apple_bl|platform_profile|wmi|mux|interconnect|ipmi|'
     r'auxvec|crash_dump|kexec|sysrq|kmsg_dump|kgdb|nmi|panic|'
     r'printk|ratelimit|dynamic_debug|kern_levels)', 'K6'),
    (r'^asm/(acpi|efi|intel_gmch|vga|video|mce|intel_ds|setup|'
     r'bootparam|boot|realmode|amd_nb|amd|intel)', 'K6'),
    # K8 entorno de usuario / VFS
    (r'^linux/(fs|file|fdtable|poll|eventpoll|anon_inodes|sched/signal|'
     r'sched/task|signal|cred|capability|pid|nsproxy|mount|pseudo_fs|'
     r'uio|ioctl|compat|proc_fs|sysctl|namei|fs_parser|file_ref|'
     r'eventfd|task_work|uidgid|user_namespace|audit|fsnotify|dcache|'
     r'ns_common|pid_namespace|ptrace|binfmts|personality|syscalls|'
     r'uaccess|stat|statfs|path|fs_context|fs_struct|blkdev|bio|'
     r'xattr|splice|pipe_fs_i|random|utsname|time_namespace|pidfd|'
     r'restart_block|thread_info|uprobes|vfs|magic|cpuset|resource|'
     r'filelock|seccomp|userfault|kcmp|elf|errseq|fanotify|inotify|'
     r'mount|quota|shm|ipc|msg|sem|mqueue|socket|net|skbuff|'
     r'in|netdevice|etherdevice|if_)', 'K8'),
    (r'^asm/(thread_info|syscall|ptrace|signal|elf|compat|ia32)', 'K8'),
    # K0 base C (lo demas de linux/, asm/ y asm-generic/)
    (r'^linux/(types|kernel|compiler|stddef|limits|bits|bitops|bitfield|'
     r'bitmap|build_bug|bug|err|errno|stringify|const|align|swab|'
     r'byteorder|unaligned|kconfig|ktime|jiffies|time|timekeeping|'
     r'container_of|args|typecheck|util_macros|instrumented|stdarg|'
     r'nospec|find|count_zeros|hex|kstrtox|cache|linkage|init|panic|'
     r'random|prefetch|poison|kern_levels|instruction_pointer|'
     r'irqflags|stddef|static_assert|hrtimer_types|timex|time64|'
     r'build-salt|vermagic|elfnote|compiler_types|compiler_attributes|'
     r'objtool|sizes|unroll|array_size|log2|ilog2|bitrev|crash_core)',
     'K0'),
    (r'^(asm|asm-generic|vdso)/', 'K0'),
    (r'^linux/(async|range|topology)', 'K2'),
    (r'^linux/(pagewalk|backing-dev)', 'K1'),
    (r'^linux/', 'K?'),
]
# Sustituciones por nombre (sobre la regla de la cabecera).
NAME_RULES = [
    (r'^devm_(k[mzc]?alloc|kfree|kstrdup|krealloc|kmalloc_array|kcalloc|'
     r'add_action|kmemdup)', 'K1'),
    (r'^(dev|pr|netdev)_(err|warn|info|dbg|notice|crit|alert|emerg|'
     r'printk|level|err_probe|WARN|warn_once|err_once|info_once|'
     r'dbg_ratelimited|err_ratelimited|warn_ratelimited|info_ratelimited|'
     r'notice_ratelimited|cont|devel)', 'K0'),
    (r'^(printk|snprintf|scnprintf|vsnprintf|vscnprintf|sprintf|kasprintf|'
     r'kvasprintf|sscanf|vsprintf|WARN|WARN_ON|WARN_ON_ONCE|WARN_ONCE|'
     r'BUG|BUG_ON|BUILD_BUG_ON|BUILD_BUG_ON_MSG|BUILD_BUG_ON_ZERO|'
     r'static_assert|dump_stack|panic|might_sleep|might_fault)$', 'K0'),
    (r'^(copy_(to|from)_user|get_user|put_user|clear_user|access_ok|'
     r'u64_to_user_ptr|strncpy_from_user|memdup_user|vmemdup_user|'
     r'memdup_user_nul|check_zeroed_user|copy_struct_from_user)', 'K1'),
    (r'^(request_irq|request_threaded_irq|free_irq|devm_request_irq|'
     r'devm_request_threaded_irq|enable_irq|disable_irq|synchronize_irq|'
     r'disable_irq_nosync|irq_set_affinity)', 'K5'),
    (r'^(kvfree|kvfree_sensitive|kfree_const)$', 'K1'),
    (r'^(tasklet_|local_bh_|in_interrupt|in_irq|in_softirq|in_task|'
     r'in_atomic)', 'K2'),
]
LEVEL_ORDER = ['K0', 'K1', 'K1-dma', 'K2', 'K3', 'K4', 'K5', 'K6', 'K7', 'K8',
               'K9', K9DRM, 'K9-ttm', 'K9-sched', 'K?']
LEVEL_TITLE = {
    'K0': 'Base C', 'K1': 'Memoria (sin DMA)', 'K1-dma': 'Memoria: DMA, dma-buf, IOMMU',
    'K2': 'Sincronizacion, tiempo, trabajo diferido', 'K3': 'Estructuras y utilidades',
    'K4': 'Modelo de dispositivos', 'K5': 'PCI, IRQ, energia',
    'K6': 'Firmware, I2C, ACPI, backlight, debugfs...', 'K7': 'Fences/sync',
    'K8': 'Entorno de usuario / VFS', 'K9': 'Miscelaneo (stubs: trace, kasan, hmm...)',
    K9DRM: 'DRM core y helpers (shim propio con la opcion A)',
    'K9-ttm': 'TTM, drm_buddy, drm_exec, gpuvm', 'K9-sched': 'drm/gpu_scheduler',
    'K?': 'Sin clasificar',
}

# --- Lexico ------------------------------------------------------------------
RE_COMMENT = re.compile(r'/\*.*?\*/|//[^\n]*', re.S)
RE_STRCHR = re.compile(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'')
RE_IDENT = re.compile(r'\b[A-Za-z_][A-Za-z0-9_]*\b')
RE_DEFINE = re.compile(r'^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)(\()?', re.M)
C_KEYWORDS = set('''auto break case char const continue default do double else enum
extern float for goto if inline int long register restrict return short signed
sizeof static struct switch typedef union unsigned void volatile while _Bool
_Static_assert typeof __typeof__ __attribute__ asm __asm__ __inline__ __inline
__volatile__ __restrict__ __builtin_expect __builtin_constant_p _Generic
__extension__ __alignof__ alignof __int128 bool true false NULL __func__
__FUNCTION__ __LINE__ __FILE__ __VA_ARGS__ __VA_OPT__ defined include
'''.split())


SHORT_OK = {'HZ'}   # nombres de 1-2 letras que si son API (el resto es ruido)


def read(path):
    try:
        with open(path, 'rb') as f:
            return f.read().decode('latin1')
    except OSError:
        return ''


def strip_code(text):
    """Quita comentarios, cadenas y continuaciones de linea."""
    text = text.replace('\\\n', ' ')
    text = RE_COMMENT.sub(lambda m: ' ' if m.group(0).startswith('//') else
                          '\n' * m.group(0).count('\n') or ' ', text)
    return RE_STRCHR.sub('""', text)


def split_pp(text):
    """(codigo sin lineas de preprocesador, lista de lineas de preprocesador)"""
    code, pp = [], []
    for line in text.split('\n'):
        (pp if line.lstrip().startswith('#') else code).append(line)
    return '\n'.join(code), pp


def match_paren(s, i):
    """indice tras el ')' que cierra el '(' en s[i]"""
    d = 0
    n = len(s)
    while i < n:
        c = s[i]
        if c == '(':
            d += 1
        elif c == ')':
            d -= 1
            if d == 0:
                return i + 1
        i += 1
    return n


RE_TAG = re.compile(r'\b(struct|union|enum)\s+([A-Za-z_]\w*)\s*\{')
RE_CALLISH = re.compile(r'([A-Za-z_]\w*)\s*\(')


def index_definitions(text):
    """Nombres definidos en un fichero C: {nombre: tipo}."""
    out = {}
    text = strip_code(text)
    for m in RE_DEFINE.finditer(text):
        out.setdefault(m.group(1), 'macro')
    code, _ = split_pp(text)
    # Recorre el nivel superior: sentencias acabadas en ';' o funciones '{..}'.
    depth = 0
    start = 0          # inicio de la sentencia actual
    head = None        # texto antes del '{' de nivel superior
    body_end = 0       # tras el '}' de un struct/enum/inicializador
    n = len(code)
    i = 0
    pdepth = 0         # ';' y '{' dentro de parentesis (argumentos de macros)
    while i < n:
        c = code[i]
        if c == '(':
            pdepth += 1
        elif c == ')':
            pdepth = max(pdepth - 1, 0)
        elif pdepth:
            pass
        elif c == '{':
            if depth == 0:
                head = code[start:i]
                for mm in RE_TAG.finditer(code[start:i + 1]):
                    out['%s:%s' % mm.groups()] = 'type'
                if re.search(r'\benum\b[^;{}()]*$', head):
                    j = i
                    d = 0
                    while j < n:
                        if code[j] == '{':
                            d += 1
                        elif code[j] == '}':
                            d -= 1
                            if d == 0:
                                break
                        j += 1
                    for part in code[i + 1:j].split(','):
                        mm = re.match(r'\s*([A-Za-z_]\w*)', part)
                        if mm:
                            out.setdefault(mm.group(1), 'enumconst')
            depth += 1
        elif c == '}':
            depth = max(depth - 1, 0)
            if depth == 0 and head is not None:
                if '(' in head and '=' not in head and \
                        not re.search(r'\b(struct|union|enum)\b[^()]*$', head):
                    name = func_name(head)
                    if name:
                        kind = ('inline' if re.search(
                            r'\binline\b|__always_inline', head) else 'func')
                        out.setdefault(name, kind)
                    start = i + 1
                    head = None
                else:
                    body_end = i + 1
        elif c == ';' and depth == 0:
            stmt = (head + ' ' + code[body_end:i]) if head is not None \
                else code[start:i]
            classify_stmt(stmt, out)
            start = i + 1
            head = None
        i += 1
    return out


FN_ATTRS = {'typeof', '__typeof__', '__aligned', '__printf', '__scanf',
            '__section', '__alloc_size', '__realloc_size', '__malloc',
            '__must_hold', '__acquires', '__releases', '__cond_acquires',
            '__counted_by', '__nocfi', '__diagnose_as', '__assume_aligned',
            'EXPORT_SYMBOL', 'EXPORT_SYMBOL_GPL', '__attribute', '__attribute__'}


def func_name(head):
    """Nombre de la funcion en 'tipo attrs nombre(args) attrs'."""
    cands = []
    for m in RE_CALLISH.finditer(head):
        nm = m.group(1)
        if nm in C_KEYWORDS or nm in FN_ATTRS:
            continue
        if paren_depth(head, m.start()) != 0:
            continue
        cands.append(nm)
    for nm in cands:
        if not nm.startswith('__') or nm is cands[-1]:
            return nm
    return cands[0] if cands else None


def paren_depth(s, pos):
    return s.count('(', 0, pos) - s.count(')', 0, pos)


def classify_stmt(stmt, out):
    s = stmt.strip()
    if not s:
        return
    for mm in RE_TAG.finditer(s):
        out['%s:%s' % mm.groups()] = 'type'
    if s.startswith('typedef'):
        m = re.search(r'\(\s*\*\s*([A-Za-z_]\w*)\s*\)', s)
        if m:
            out.setdefault(m.group(1), 'type')
            return
        if '(' in s:   # typedef de tipo funcion: typedef int nombre(args);
            nm = func_name(s[7:])
            if nm:
                out.setdefault(nm, 'type')
            return
        ids = RE_IDENT.findall(re.sub(r'\[[^\]]*\]|\b(struct|union|enum)\s+\w+',
                                      ' ', s))
        if len(ids) >= 2:
            out.setdefault(ids[-1], 'type')
        return
    m = re.match(r'^(struct|union|enum)\s+(\w+)$', s)
    if m:
        out.setdefault('%s:%s' % m.groups(), 'fwd')   # declaracion adelantada
        return
    s = re.sub(r'\b(struct|union|enum)\s+\w+', ' T ', s)
    t = re.sub(r'=.*$', '', s, flags=re.S)
    if '(' in t:
        # puntero a funcion global: tipo (*nombre)(args)
        m = re.search(r'\(\s*\*\s*([A-Za-z_]\w*)\s*\)\s*\(', t)
        if m and paren_depth(t, m.start()) == 0:
            out.setdefault(m.group(1), 'var')
            return
        name = func_name(t)
        # algo delante del nombre (tipo): prototipo; si no, es una macro
        # invocada en el nivel superior (module_init(x); DECLARE_*(...);)
        if name and t[:t.find(name)].strip():
            out.setdefault(name, 'func')
        return
    t = re.sub(r'\[[^\]]*\]', '', t)
    ids = [x for x in RE_IDENT.findall(t) if x not in C_KEYWORDS]
    if len(RE_IDENT.findall(t)) >= 2 and ids:
        out.setdefault(ids[-1], 'var')


def usage_tokens(text):
    """Identificadores que usa un fichero (codigo y cuerpos de #define/#if)."""
    text = strip_code(text)
    cnt = collections.Counter()
    for line in text.split('\n'):
        ls = line.lstrip()
        if ls.startswith('#'):
            m = re.match(r'#\s*(\w+)', ls)
            d = m.group(1) if m else ''
            if d in ('include', 'pragma', 'error', 'warning', 'undef', 'line'):
                continue
            if d == 'define':
                m2 = re.match(r'#\s*define\s+\w+(\([^)]*\))?', ls)
                ls = ls[m2.end():] if m2 else ''
        for m in RE_TAGUSE.finditer(ls):
            if m.group(1):
                cnt['%s:%s' % (m.group(1), m.group(2))] += 1
                continue
            t = m.group(3)
            if t not in C_KEYWORDS and not t.startswith('CONFIG_'):
                cnt[t] += 1
    return cnt


RE_TAGUSE = re.compile(r'\b(?:(struct|union|enum)\s+([A-Za-z_]\w*)|'
                       r'([A-Za-z_][A-Za-z0-9_]*))\b')


# --- Indice de cabeceras -----------------------------------------------------
def header_key(rel):
    """include/linux/x.h -> linux/x ; uapi/linux/x.h -> linux/x"""
    k = rel[:-2] if rel.endswith('.h') else rel
    if k.startswith('uapi/'):
        k = k[5:]
    return k


def build_header_index(linux):
    idx = {}          # nombre -> (cabecera, tipo)
    roots = [(os.path.join(linux, 'include'), ''),
             (os.path.join(linux, 'arch/x86/include'), '')]
    prio = lambda k: (0 if k.startswith('linux/') else 1 if k.startswith('drm/')
                      else 2 if k.startswith('asm/') else 3)
    nfiles = 0
    for root, _ in roots:
        for dp, dns, fns in os.walk(root):
            dns.sort()
            for fn in sorted(fns):
                if not fn.endswith('.h'):
                    continue
                path = os.path.join(dp, fn)
                rel = os.path.relpath(path, root)
                if rel.startswith('generated/'):
                    continue
                key = header_key(rel)
                nfiles += 1
                for nm, kind in index_definitions(read(path)).items():
                    old = idx.get(nm)
                    if old is None or (old[1] == 'fwd' and kind != 'fwd') or (
                            (old[1] == 'fwd') == (kind == 'fwd') and
                            prio(key) < prio(old[0])):
                        idx[nm] = (key, kind)
    return idx, nfiles


def level_for(name, header):
    for rx, lv in NAME_RULES:
        if re.match(rx, name):
            return lv
    for rx, lv in LEVEL_RULES:
        if re.match(rx, header):
            return lv
    return 'K?'


# --- Drivers -----------------------------------------------------------------
def driver_files(root, specs, excludes):
    files = []
    for spec in specs:
        for alt in spec.split('|'):
            if not alt:
                continue
            p = os.path.join(root, alt)
            if os.path.isfile(p):
                files.append(p)
                break
            if os.path.isdir(p):
                for dp, dns, fns in os.walk(p):
                    dns.sort()
                    for fn in sorted(fns):
                        if fn.endswith(('.c', '.h')):
                            files.append(os.path.join(dp, fn))
                break
    return [f for f in files if not any(e in f for e in excludes)]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--linux', required=True)
    ap.add_argument('--nvidia')
    ap.add_argument('--label', default='')
    ap.add_argument('--md', default='docs/linuxkpi_api.md')
    ap.add_argument('--tsv', default='docs/linuxkpi_api.tsv')
    ap.add_argument('--top', type=int, default=40)
    a = ap.parse_args()

    print('indexando cabeceras de %s ...' % a.linux, file=sys.stderr)
    hidx, nhdr = build_header_index(a.linux)
    print('  %d cabeceras, %d nombres' % (nhdr, len(hidx)), file=sys.stderr)

    names = [d[0] for d in DRIVERS]
    use = collections.defaultdict(lambda: collections.Counter())  # sym -> drv -> n
    drv_stats = {}
    for name, rootk, specs, excl in DRIVERS:
        root = a.linux if rootk == 'linux' else a.nvidia
        if not root:
            continue
        files = driver_files(root, specs, excl)
        if not files:
            print('  %s: sin ficheros (ruta?)' % name, file=sys.stderr)
            continue
        own = set()
        toks = collections.Counter()
        lines = 0
        for f in files:
            t = read(f)
            lines += t.count('\n')
            own.update(index_definitions(t).keys())
            toks.update(usage_tokens(t))
        ext = {s: n for s, n in toks.items() if s in hidx and s not in own
               and (len(s) > 2 or s in SHORT_OK)}
        for s, n in ext.items():
            use[s][name] = n
        drv_stats[name] = (len(files), lines, len(ext))
        print('  %s: %d ficheros, %d lineas, %d simbolos externos'
              % (name, len(files), lines, len(ext)), file=sys.stderr)

    rows = []
    for s, per in use.items():
        hdr, kind = hidx[s]
        if kind == 'fwd':
            kind = 'type'
        if level_for(s, hdr) == 'DRV':
            continue  # cabecera propia del driver (viene con sus fuentes)
        rows.append((s.replace(':', ' '), level_for(s, hdr), kind, hdr, sum(per.values()),
                     len(per), per))
    rows.sort(key=lambda r: (LEVEL_ORDER.index(r[1]) if r[1] in LEVEL_ORDER
                             else 99, -r[5], -r[4], r[0]))
    present = [n for n in names if n in drv_stats]

    with open(a.tsv, 'w') as f:
        f.write('# linuxkpi-scan.py %s\n' % a.label)
        f.write('simbolo\tnivel\ttipo\tcabecera\tusos\tn_drivers\t%s\n'
                % '\t'.join(present))
        for s, lv, kind, hdr, tot, nd, per in rows:
            f.write('%s\t%s\t%s\t%s\t%d\t%d\t%s\n' % (
                s, lv, kind, hdr, tot, nd, '\t'.join(str(per.get(n, 0))
                                                     for n in present)))
    write_md(a, rows, present, drv_stats, nhdr, len(hidx))
    print('escrito %s y %s' % (a.md, a.tsv), file=sys.stderr)


KIND_COLS = ['func', 'inline', 'macro', 'type', 'enumconst', 'var']


def write_md(a, rows, present, drv_stats, nhdr, nidx):
    by_lv = collections.defaultdict(list)
    for r in rows:
        by_lv[r[1]].append(r)
    L = []
    w = L.append
    w('# API de Linux que consumen los drivers objetivo (LinuxKPI, L0.3)\n')
    w('Generado por `scripts/linuxkpi-scan.py` (no editar a mano; la tabla '
      'completa esta en `%s`).\n' % os.path.basename(a.tsv))
    if a.label:
        w('**Fuentes analizadas:** %s\n' % a.label)
    w(('Indice: %d cabeceras de Linux, %d nombres definidos. ' % (nhdr, nidx)) +
      'Un simbolo cuenta '
      'si un fichero del driver lo usa, lo define una cabecera de Linux y no lo '
      'define el propio driver. **Analisis lexico**: incluye todas las ramas '
      '`#if`, macros e *inlines* (que la capa tambien tiene que dar) y algo de '
      'ruido; no ve nombres creados por macros generadoras.\n')
    w('## Drivers\n')
    w('| Driver | Ficheros | Lineas | Simbolos externos distintos |')
    w('|---|---:|---:|---:|')
    for n in present:
        nf, nl, ne = drv_stats[n]
        w('| `%s` | %d | %d | %d |' % (n, nf, nl, ne))
    w('')
    w('## Simbolos distintos por nivel y driver\n')
    w('| Nivel | ' + ' | '.join('`%s`' % n for n in present) +
      ' | Union | de ellos funciones (no inline) |')
    w('|---|' + '---:|' * (len(present) + 2))
    for lv in LEVEL_ORDER:
        rs = by_lv.get(lv, [])
        if not rs:
            continue
        cells = [str(sum(1 for r in rs if n in r[6])) for n in present]
        nfunc = sum(1 for r in rs if r[2] == 'func')
        w('| %s %s | %s | **%d** | %d |' % (lv, LEVEL_TITLE.get(lv, ''),
                                           ' | '.join(cells), len(rs), nfunc))
    tot = [str(sum(1 for r in rows if n in r[6])) for n in present]
    w('| **Total** | %s | **%d** | %d |\n' % (' | '.join(tot), len(rows),
                                             sum(1 for r in rows if r[2] == 'func')))
    w('Union acumulada siguiendo el orden de trabajo (cuanto anade cada driver '
      'a lo que ya necesitan los anteriores):\n')
    w('| Driver | Simbolos nuevos | Acumulado | de ellos K9-drm |')
    w('|---|---:|---:|---:|')
    seen = set()
    for n in present:
        new = [r for r in rows if n in r[6] and r[0] not in seen]
        seen.update(r[0] for r in new)
        w('| `%s` | %d | %d | %d |' % (n, len(new), len(seen),
                                       sum(1 for r in new if r[1] == K9DRM)))
    w('')
    w('## Por tipo de definicion\n')
    w('| Nivel | ' + ' | '.join(KIND_COLS) + ' |')
    w('|---|' + '---:|' * len(KIND_COLS))
    for lv in LEVEL_ORDER:
        rs = by_lv.get(lv, [])
        if rs:
            w('| %s | %s |' % (lv, ' | '.join(
                str(sum(1 for r in rs if r[2] == k)) for k in KIND_COLS)))
    w('')
    w('## Estimacion heuristica de tamano de la capa\n')
    w('Formula (orientativa, no medida): funcion 15 lineas, inline 6, tipo 8, '
      'macro 2, variable 2, constante de enum 1. Con la **opcion A** de DRM, '
      'K9-drm/ttm/sched es el *shim* propio sobre `kernel/drm/`; los «stubs» '
      'de K9 cuentan igual aunque muchos sean triviales.\n')
    W = {'func': 15, 'inline': 6, 'type': 8, 'macro': 2, 'var': 2, 'enumconst': 1}
    qemu = [n for n in ('bochs', 'simpledrm', 'virtio-gpu') if n in present]
    w('| Nivel | QEMU (%s) | Todos | kLOC QEMU | kLOC todos |'
      % ', '.join(qemu))
    w('|---|---:|---:|---:|---:|')
    tq = ta = 0
    for lv in LEVEL_ORDER:
        rs = by_lv.get(lv, [])
        if not rs:
            continue
        rq = [r for r in rs if any(n in r[6] for n in qemu)]
        lq = sum(W.get(r[2], 2) for r in rq)
        la = sum(W.get(r[2], 2) for r in rs)
        tq += lq
        ta += la
        w('| %s | %d | %d | %.1f | %.1f |' % (lv, len(rq), len(rs),
                                              lq / 1000, la / 1000))
    w('| **Total** | | | **%.1f** | **%.1f** |\n' % (tq / 1000, ta / 1000))
    w('## Lista por nivel (los %d mas usados; ordenados por numero de drivers '
      'y usos)\n' % a.top)
    w('Columnas por driver: numero de usos. `-` = no lo usa.\n')
    short = {'virtio-gpu': 'virtio', 'simpledrm': 'simple'}
    for lv in LEVEL_ORDER:
        rs = by_lv.get(lv, [])
        if not rs:
            continue
        w('### %s — %s (%d simbolos)\n' % (lv, LEVEL_TITLE.get(lv, ''), len(rs)))
        w('| Simbolo | Tipo | Cabecera | ' +
          ' | '.join(short.get(n, n) for n in present) + ' |')
        w('|---|---|---|' + '---:|' * len(present))
        for s, _, kind, hdr, _, _, per in rs[:a.top]:
            w('| `%s` | %s | `%s` | %s |' % (s, kind, hdr, ' | '.join(
                str(per[n]) if n in per else '-' for n in present)))
        w('')
    with open(a.md, 'w') as f:
        f.write('\n'.join(L) + '\n')


if __name__ == '__main__':
    main()
