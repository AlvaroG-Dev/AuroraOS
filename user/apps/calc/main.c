// apps/calc/main.c
// Tests: malloc/sbrk, arithmetic, string output via sys_print
// Exits with code 42 as a sentinel for the parent to verify.
#include "../../syscall.h"
#include "../../lib/malloc.h"
#include "../../lib/string.h"

// Simple itoa for unsigned
static void u64_to_str(uint64_t v, char *buf) {
    if (v == 0) { buf[0] = '0'; buf[1] = 0; return; }
    char tmp[24];
    int i = 0;
    while (v) { tmp[i++] = '0' + (int)(v % 10); v /= 10; }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = 0;
}

// Print prefix + number + newline
static void print_num(const char *prefix, uint64_t n) {
    char num_buf[24];
    u64_to_str(n, num_buf);
    sys_print(prefix);
    sys_print(num_buf);
    sys_print("\n");
}

// Fibonacci iterative
static uint64_t fib(int n) {
    if (n <= 1) return (uint64_t)n;
    uint64_t a = 0, b = 1, c = 0;
    for (int i = 2; i <= n; i++) { c = a + b; a = b; b = c; }
    return c;
}

// Sieve of Eratosthenes up to limit (heap allocated)
static int count_primes(int limit) {
    uint8_t *sieve = (uint8_t *)malloc((size_t)(limit + 1));
    if (!sieve) return -1;
    for (int i = 0; i <= limit; i++) sieve[i] = 1;
    sieve[0] = sieve[1] = 0;
    for (int i = 2; (long long)i * i <= limit; i++) {
        if (sieve[i]) {
            for (int j = i * i; j <= limit; j += i)
                sieve[j] = 0;
        }
    }
    int count = 0;
    for (int i = 0; i <= limit; i++) if (sieve[i]) count++;
    free(sieve);
    return count;
}

int main(void) {
    sys_print("[calc] === Aurora OS Calc App ===\n");
    sys_print("[calc] Test 1: Fibonacci\n");

    // Fibonacci test - print first 8 multiples of 3
    for (int i = 0; i < 8; i++) {
        print_num("[calc]   fib = ", fib(i * 3));
    }
    uint64_t fib20 = fib(20);
    print_num("[calc]   fib(20) = ", fib20);
    if (fib20 != 6765) {
        sys_print("[calc] ERROR: fib(20) mismatch!\n");
        sys_exit(1);
    }
    sys_print("[calc]   Fibonacci: OK\n");

    sys_print("[calc] Test 2: Heap / malloc\n");
    // Allocate and fill a dynamic array
    int *arr = (int *)malloc(256 * sizeof(int));
    if (!arr) {
        sys_print("[calc] ERROR: malloc returned NULL\n");
        sys_exit(2);
    }
    for (int i = 0; i < 256; i++) arr[i] = i * i;
    int ok = 1;
    for (int i = 0; i < 256; i++) if (arr[i] != i * i) { ok = 0; break; }
    free(arr);
    sys_print(ok ? "[calc]   malloc/free: OK\n" : "[calc]   malloc/free: FALLO\n");
    if (!ok) sys_exit(3);

    sys_print("[calc] Test 3: Criba de Eratostenes\n");
    int primes = count_primes(1000);
    print_num("[calc]   Primos hasta 1000: ", (uint64_t)primes);
    // There are 168 primes below or equal to 1000 (actually 168 primes <= 1000)
    if (primes != 168) {
        sys_print("[calc] ERROR: conteo de primos incorrecto\n");
        sys_exit(4);
    }
    sys_print("[calc]   Criba: OK\n");

    sys_print("[calc] Test 4: Asignaciones multiples de heap\n");
    void *ptrs[16];
    for (int i = 0; i < 16; i++) {
        ptrs[i] = malloc((size_t)(64 * (i + 1)));
        if (!ptrs[i]) {
            sys_print("[calc] ERROR: malloc multi fallo\n");
            sys_exit(5);
        }
    }
    for (int i = 0; i < 16; i++) free(ptrs[i]);
    sys_print("[calc]   Multi-malloc/free: OK\n");

    sys_print("[calc] Todas las pruebas PASARON. Saliendo con codigo 42.\n");
    sys_exit(42);
    return 0;
}
