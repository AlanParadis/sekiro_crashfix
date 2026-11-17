/*
 * sekiro_crashfix - fixes the random "every few minutes" crash of
 * Sekiro: Shadows Die Twice 1.6.0.0 (Steam) on some PCs.
 *
 * Built as dinput8.dll and dropped next to sekiro.exe. The game loads it
 * like the real DirectInput DLL; every DirectInput call is forwarded to the
 * system's dinput8.dll. See docs/TECHNICAL.md for the full analysis.
 *
 * The bug in one paragraph
 * ------------------------
 * Some game code decrements the last 8-byte field of a 0x258-byte object
 * up to ~0.5 s AFTER that object has been destroyed and freed. In the
 * game's heap, those 8 bytes become the "previous block" pointer of the
 * next heap block as soon as the object is freed, so the pointer ends up
 * one byte too low. The next free of the neighbouring block follows that
 * misaligned pointer and corrupts the heap, which crashes the game later
 * at a random place (access violations at sekiro.exe+1AA564F / +1AA566D /
 * +1AA573B, or inside ntdll's critical-section code). If the memory has
 * already been reused, another object gets one of its fields decremented
 * instead (e.g. a lock pointer), which also crashes.
 *
 * What this DLL does
 * ------------------
 *  1. Quarantine (root cause): objects of that size freed in the main heap
 *     are kept, destroyed but untouched, for 10 s before being given back
 *     to the allocator. The late decrement hits dead memory that still
 *     belongs to the dead object, and does no harm.
 *  2. Validation (safety net): every free in the game's allocator is
 *     checked first. A neighbour pointer that is off by a few bytes but
 *     whose aligned neighbour is a consistent free block is repaired;
 *     anything else that would corrupt the heap makes that one free be
 *     skipped (the block leaks).
 *  3. The game's own "block already free" / "pointer outside heap"
 *     checks, which log nothing and carry on in the retail build, now
 *     leave the free function cleanly instead.
 *
 * No byte of sekiro.exe on disk is modified. In memory, three small code
 * locations of the free function are redirected, and only after checking
 * that they contain exactly the expected bytes; on any other version of
 * the game the DLL does nothing but forward DirectInput calls.
 *
 * Built without the C runtime (see build/). License: MIT.
 */

#define SCF_VERSION "1.0.0"

/* Top of the current thread's stack (TEB->StackBase). Overridable so the
 * heap logic can be unit-tested on another OS (see tests/). */
#ifndef STACK_BASE
static unsigned long long STACK_BASE(void) {
    unsigned long long t; __asm__ volatile("movq %%gs:8, %0" : "=r"(t)); return t;
}
#endif

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef long               LONG;
typedef int                BOOL;
typedef void              *HANDLE;
typedef u16                WCHAR;
#define WINAPI __stdcall

typedef struct { u16 y, mo, dow, d, h, mi, s, ms; } SYSTEMTIME;

__declspec(dllimport) u32    WINAPI GetSystemDirectoryW(WCHAR *, u32);
__declspec(dllimport) void * WINAPI LoadLibraryW(const WCHAR *);
__declspec(dllimport) void * WINAPI GetProcAddress(void *, const char *);
__declspec(dllimport) void * WINAPI GetModuleHandleW(const WCHAR *);
__declspec(dllimport) u32    WINAPI GetModuleFileNameW(void *, WCHAR *, u32);
__declspec(dllimport) HANDLE WINAPI CreateFileW(const WCHAR *, u32, u32, void *, u32, u32, HANDLE);
__declspec(dllimport) BOOL   WINAPI WriteFile(HANDLE, const void *, u32, u32 *, void *);
__declspec(dllimport) BOOL   WINAPI CloseHandle(HANDLE);
__declspec(dllimport) void   WINAPI GetLocalTime(SYSTEMTIME *);
__declspec(dllimport) BOOL   WINAPI DisableThreadLibraryCalls(void *);
__declspec(dllimport) BOOL   WINAPI VirtualProtect(void *, u64, u32, u32 *);
__declspec(dllimport) BOOL   WINAPI FlushInstructionCache(HANDLE, const void *, u64);
__declspec(dllimport) HANDLE WINAPI GetCurrentProcess(void);
__declspec(dllimport) u32    WINAPI GetCurrentThreadId(void);
__declspec(dllimport) HANDLE WINAPI CreateThread(void *, u64, u32 (WINAPI *)(void *), void *, u32, u32 *);
__declspec(dllimport) void   WINAPI Sleep(u32);
__declspec(dllimport) u64    WINAPI GetTickCount64(void);
__declspec(dllimport) u64    WINAPI VirtualQuery(const void *, void *, u64);
__declspec(dllimport) void * WINAPI AddVectoredExceptionHandler(u32, LONG (WINAPI *)(void *));

/* ------------------------------------------------------------------ */
/* Addresses (sekiro.exe 1.6.0.0)                                     */
/* ------------------------------------------------------------------ */
#define RVA_FREE       0x1A40DA0u   /* free(heap, ptr)                      */
#define RVA_FREE_CONT  0x1A40DAEu   /* first instruction after our hook     */
#define RVA_SITE_A     0x1A40DD6u   /* game's "block not in use" error path */
#define RVA_SITE_B     0x1A40DFDu   /* game's "pointer outside heap" path   */

static const u8 ORIG_FREE[14] = {0x48,0x85,0xD2, 0x0F,0x84,0xC1,0x01,0x00,0x00,
                                 0x48,0x89,0x74,0x24,0x20};
static const u8 ORIG_FREE_TAIL[5] = {0x57, 0x48,0x83,0xEC,0x20};  /* at +14, never patched */
static const u8 ORIG_A[27] = {
    0x4C,0x8B,0xCA, 0x4C,0x8D,0x05,0xC0,0x3F,0x39,0x01, 0xBA,0x93,0x07,0x00,0x00,
    0x48,0x8D,0x0D,0x04,0x40,0x39,0x01, 0xE8,0xAF,0xBA,0xF9,0xFF};
static const u8 ORIG_B[27] = {
    0x4C,0x8B,0xCF, 0x4C,0x8D,0x05,0x09,0x40,0x39,0x01, 0xBA,0x9A,0x07,0x00,0x00,
    0x48,0x8D,0x0D,0x35,0x40,0x39,0x01, 0xE8,0x88,0xBA,0xF9,0xFF};

/* ------------------------------------------------------------------ */
/* Assembly stubs                                                     */
/* ------------------------------------------------------------------ */
void free_hook(void);
void free_hook_nq(u64 heap, u64 ptr);   /* real free, callable from C */
void err_stub_a(void);
void err_stub_b(void);
u64  g_free_cont;            /* absolute address of RVA_FREE_CONT */

__asm__(
    ".intel_syntax noprefix\n"
    ".text\n"
    /* Replaces: test rdx,rdx / je ret / mov [rsp+20h],rsi
     * Entry: rcx = heap, rdx = ptr, rsp = 8 mod 16 */
    ".globl free_hook\n"
    "free_hook:\n"
    "    test rdx, rdx\n"
    "    jz 1f\n"
    "    mov r8, rsp\n"
    "    push rcx\n"
    "    push rdx\n"
    "    sub rsp, 0x28\n"
    "    call validate_free\n"
    "    add rsp, 0x28\n"
    "    pop rdx\n"
    "    pop rcx\n"
    "    test eax, eax\n"
    "    jz 1f\n"
    "    mov qword ptr [rsp+0x20], rsi\n"
    "    jmp qword ptr [rip + g_free_cont]\n"
    "1:\n"
    "    ret\n"

    /* Same as free_hook but without quarantine; also used as a plain
     * function call from C to really free a quarantined block. */
    ".globl free_hook_nq\n"
    "free_hook_nq:\n"
    "    test rdx, rdx\n"
    "    jz 3f\n"
    "    mov r8, rsp\n"
    "    push rcx\n"
    "    push rdx\n"
    "    sub rsp, 0x28\n"
    "    call validate_free_nq\n"
    "    add rsp, 0x28\n"
    "    pop rdx\n"
    "    pop rcx\n"
    "    test eax, eax\n"
    "    jz 3f\n"
    "    mov qword ptr [rsp+0x20], rsi\n"
    "    jmp qword ptr [rip + g_free_cont]\n"
    "3:\n"
    "    ret\n"

    /* Game's own error branches inside free. State there:
     * rdi = ptr, rsi = heap, rbx/rbp clobbered; saved rbx [rsp+30h],
     * rbp [rsp+38h], rsi [rsp+48h]; then pushed rdi; then return address.
     * Nothing in the heap has been modified yet: just leave the function. */
    ".globl err_stub_a\n"
    "err_stub_a:\n"
    "    mov edx, 1\n"
    "    jmp 2f\n"
    ".globl err_stub_b\n"
    "err_stub_b:\n"
    "    mov edx, 2\n"
    "2:\n"
    "    mov rbp, qword ptr [rsp+0x38]\n"
    "    mov rcx, rdi\n"
    "    mov r8, rsp\n"
    "    mov r9, rsi\n"
    "    sub rsp, 0x20\n"
    "    call on_game_error\n"
    "    add rsp, 0x20\n"
    "    mov rbx, qword ptr [rsp+0x30]\n"
    "    mov rsi, qword ptr [rsp+0x48]\n"
    "    add rsp, 0x20\n"
    "    pop rdi\n"
    "    ret\n"
    ".att_syntax\n");


/* ------------------------------------------------------------------ */
/* Globals                                                            */
/* ------------------------------------------------------------------ */
static u8   *g_base;
static u64   g_xlo[16], g_xhi[16];
static int   g_nx;
static WCHAR g_logpath[600];
static volatile LONG g_loglock, g_lines;
static volatile LONG g_n_repair, g_n_skip, g_n_game, g_n_crash;
static volatile long long g_n_frees, g_log_bytes;

/* Ring buffer of the last frees, dumped in crash reports */
typedef struct { u64 t, ptr, h, prev, nh, ret1, ret2; u32 tid, res; } FreeRec;
#define RING 64
static FreeRec g_ring[RING];
static volatile long long g_ring_i;

/* Heaps seen so far */
static volatile u64 g_heaps[16];


/* ------------------------------------------------------------------ */
/* Tiny log, no CRT                                                   */
/* ------------------------------------------------------------------ */
static int put_str(char *b, int p, const char *s) { while (*s) b[p++] = *s++; return p; }
static int put_dec(char *b, int p, u64 v, int w) {
    char t[24]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n < w) t[n++] = '0';
    while (n) b[p++] = t[--n];
    return p;
}
static int put_hex(char *b, int p, u64 v, int digits) {
    static const char H[] = "0123456789ABCDEF";
    int i; for (i = (digits - 1) * 4; i >= 0; i -= 4) b[p++] = H[(v >> i) & 0xF];
    return p;
}
static int put_time(char *b, int p) {
    SYSTEMTIME t; GetLocalTime(&t);
    p = put_dec(b, p, t.h, 2); b[p++] = ':';
    p = put_dec(b, p, t.mi, 2); b[p++] = ':';
    p = put_dec(b, p, t.s, 2); b[p++] = ' ';
    return p;
}
static void write_log(const char *buf, int len) {
    u32 w; HANDLE h; long i; int got = 0;
    if (__sync_add_and_fetch(&g_log_bytes, len) > 16ll * 1024 * 1024) return;   /* 16 MB cap */
    /* bounded spin: a crash report must never deadlock on our own lock */
    for (i = 0; i < 50000000; i++) if (!__sync_lock_test_and_set(&g_loglock, 1)) { got = 1; break; }
    h = CreateFileW(g_logpath, 0x4 /*FILE_APPEND_DATA*/, 0x3, 0, 4 /*OPEN_ALWAYS*/, 0x80, 0);
    if (h != (HANDLE)(long long)-1) { WriteFile(h, buf, (u32)len, &w, 0); CloseHandle(h); }
    if (got) __sync_lock_release(&g_loglock);
}
static void log_msg(const char *msg) {
    char buf[200]; int p = put_time(buf, 0);
    p = put_str(buf, p, msg); buf[p++] = '\r'; buf[p++] = '\n';
    write_log(buf, p);
}
static void log_bytes(const char *msg, const u8 *q, int n) {
    char buf[200]; int p = put_time(buf, 0), i;
    p = put_str(buf, p, msg);
    for (i = 0; i < n; i++) { buf[p++] = ' '; p = put_hex(buf, p, q[i], 2); }
    buf[p++] = '\r'; buf[p++] = '\n';
    write_log(buf, p);
}
static int log_budget(void) { return __sync_add_and_fetch(&g_lines, 1) <= 2000; }

/* Readable memory check (VirtualQuery), so diagnostics never fault */
static int readable(u64 a, u64 len) {
    u8 mbi[48]; u64 base, size; u32 state, prot;
    if (a < 0x10000 || a >= 0x7FFFFFFF0000ull) return 0;
    if (!VirtualQuery((const void *)a, mbi, sizeof mbi)) return 0;
    base = *(u64 *)(mbi + 0); size = *(u64 *)(mbi + 0x18);
    state = *(u32 *)(mbi + 0x20); prot = *(u32 *)(mbi + 0x24);
    if (state != 0x1000 || (prot & 0x101) || !(prot & 0xEE)) return 0;
    return a + len <= base + size;
}

/* Hex dump, 32 bytes per line */
static void log_hexdump(const char *title, u64 a, u64 len) {
    char buf[300]; int p; u64 off;
    if (!log_budget()) return;
    p = put_str(buf, 0, "    "); p = put_str(buf, p, title);
    p = put_str(buf, p, " @"); p = put_hex(buf, p, a, 12);
    if (!readable(a, len)) { p = put_str(buf, p, " (unreadable)\r\n"); write_log(buf, p); return; }
    buf[p++] = '\r'; buf[p++] = '\n'; write_log(buf, p);
    for (off = 0; off < len; off += 32) {
        u64 k;
        p = put_str(buf, 0, "      +"); p = put_hex(buf, p, off, 3); buf[p++] = ':';
        for (k = 0; k < 32 && off + k < len; k++) {
            if ((k & 7) == 0) buf[p++] = ' ';
            p = put_hex(buf, p, ((const u8 *)a)[off + k], 2);
        }
        buf[p++] = '\r'; buf[p++] = '\n'; write_log(buf, p);
    }
}

/* Scan the stack for values that look like return addresses into sekiro.exe */
static int looks_like_ret(u64 v) {
    const u8 *c; int i, ok = 0;
    for (i = 0; i < g_nx; i++) if (v >= g_xlo[i] + 8 && v < g_xhi[i]) { ok = 1; break; }
    if (!ok) return 0;
    c = (const u8 *)v;
    if (c[-5] == 0xE8) return 1;
    if (c[-6] == 0xFF && c[-5] == 0x15) return 1;
    if (c[-2] == 0xFF && (c[-1] & 0xF8) == 0xD0) return 1;
    if (c[-3] == 0xFF && (c[-2] & 0x38) == 0x10) return 1;
    if (c[-6] == 0xFF && (c[-5] & 0x38) == 0x10) return 1;
    return 0;
}
static int put_callers(char *buf, int p, u64 *sp) {
    u64 top, *q; int n = 0;
    top = STACK_BASE();   /* TEB->StackBase */
    p = put_str(buf, p, "\r\n    callers:");
    for (q = sp; (u64)(q + 1) <= top && q < sp + 200 && n < 10; q++)
        if (looks_like_ret(*q)) {
            p = put_str(buf, p, " +"); p = put_hex(buf, p, *q - (u64)g_base, 7); n++;
        }
    return p;
}

/* ------------------------------------------------------------------ */
/* Heap validation                                                    */
/* ------------------------------------------------------------------ */
#define HEAP_LO(h) (*(u64 *)((h) + 0x28))   /* lowest valid user pointer  */
#define HEAP_HI(h) (*(u64 *)((h) + 0x30))   /* highest valid user pointer */

/* Is blk a 16-byte aligned block header inside this heap? (lo/hi are the
 * heap's user-pointer bounds; passed explicitly because several heaps,
 * each with its own lock, can be freeing at the same time.) */
static int in_heap(u64 blk, u64 lo, u64 hi) {
    return (blk & 0xF) == 0 && blk + 0x10 >= lo && blk <= hi + 0x10 && blk >= 0x10000;
}
static u64 rd(u64 a) { return *(volatile u64 *)a; }

/* Is F a free block whose next pointer is exactly B? */
static int is_free_before(u64 F, u64 B, u64 lo, u64 hi) {
    u64 fh;
    if (!in_heap(F, lo, hi) || F >= B) return 0;
    fh = rd(F + 8);
    return (fh & 1) == 0 && (fh & ~3ull) == B;
}

static void log_event(const char *what, u64 ptr, u64 a, u64 b, u64 *sp, int callers) {
    char buf[600]; int p;
    if (!log_budget()) return;
    p = put_time(buf, 0);
    p = put_str(buf, p, what);
    p = put_str(buf, p, " ptr="); p = put_hex(buf, p, ptr, 12);
    p = put_str(buf, p, " a=");   p = put_hex(buf, p, a, 16);
    p = put_str(buf, p, " b=");   p = put_hex(buf, p, b, 16);
    p = put_str(buf, p, " [repaired="); p = put_dec(buf, p, (u64)g_n_repair, 1);
    p = put_str(buf, p, " skipped=");   p = put_dec(buf, p, (u64)g_n_skip, 1);
    p = put_str(buf, p, "]");
    if (callers) p = put_callers(buf, p, sp);
    buf[p++] = '\r'; buf[p++] = '\n';
    write_log(buf, p);
}

/* Called from free_hook, under the heap lock. Return 1 to let the game
 * free the block, 0 to skip (leak) it. */
static void ring_add(u64 ptr, u64 h, u64 prev, u64 nh, u64 *ret_sp, u32 res) {
    long long i = __sync_fetch_and_add(&g_ring_i, 1);
    FreeRec *r = &g_ring[i & (RING - 1)];
    u64 *q; int k;
    r->t = GetTickCount64(); r->tid = GetCurrentThreadId(); r->res = res;
    r->ptr = ptr; r->h = h; r->prev = prev; r->nh = nh;
    r->ret1 = ret_sp[0]; r->ret2 = 0;
    for (q = ret_sp + 1, k = 0; k < 24; k++, q++) if (looks_like_ret(*q)) { r->ret2 = *q; break; }
}

static void note_heap(u64 heap, u64 lo, u64 hi) {
    int i;
    for (i = 0; i < 16; i++) {
        if (g_heaps[i] == heap) return;
        if (g_heaps[i] == 0 && __sync_bool_compare_and_swap(&g_heaps[i], 0, heap)) {
            char buf[160]; int p = put_time(buf, 0);
            p = put_str(buf, p, "heap seen: obj="); p = put_hex(buf, p, heap, 12);
            p = put_str(buf, p, " range="); p = put_hex(buf, p, lo, 12);
            p = put_str(buf, p, "-"); p = put_hex(buf, p, hi, 12);
            buf[p++] = '\r'; buf[p++] = '\n'; write_log(buf, p);
            return;
        }
    }
}



/* ================================================================== */
/* QUARANTINE (the fix for the root cause)                             */
/* ================================================================== */
/*
 * Objects of the victim class (0x258 bytes, block 0x260, main heap) are
 * not given back to the allocator immediately: they are kept, destroyed
 * but untouched, for Q_MS milliseconds. The late decrement observed after
 * their destruction (0-450 ms in the logs) then lands in dead memory that
 * still belongs to the dead object, instead of the allocator's metadata
 * (crash) or a new object reusing the memory (silent corruption, e.g. the
 * odd lock pointer of the 17:55 crash). After the delay the block is
 * really freed, through the same validation as every other free.
 */
#define RVA_MAIN_HEAP   0x3B182D8u   /* heap object that gets corrupted              */
#define VICTIM_BLOCK    0x260u       /* heap block size of the 0x258-byte object     */
#define Q_MS            10000        /* how long a destroyed object stays quarantined */
#define Q_N             4096         /* quarantine capacity                          */
static u64  g_q_ptr[Q_N], g_q_t[Q_N];
static volatile long long g_q_head, g_q_tail, g_q_in, g_q_out;
static volatile LONG g_q_lock;
static void (*g_free_direct)(u64, u64) = free_hook_nq;

static void q_lock(void)   { while (__sync_lock_test_and_set(&g_q_lock, 1)) { } }
static void q_unlock(void) { __sync_lock_release(&g_q_lock); }

/* Really free quarantined blocks that are old enough (or one, if forced). */
static void q_drain(u64 heap, int force_one) {
    int n;
    for (n = 0; n < 64; n++) {
        u64 ptr = 0, now = GetTickCount64();
        q_lock();
        if (g_q_tail != g_q_head) {
            long long i = g_q_tail & (Q_N - 1);
            if (force_one || now - g_q_t[i] >= Q_MS) { ptr = g_q_ptr[i]; g_q_tail++; }
        }
        q_unlock();
        if (!ptr) return;
        force_one = 0;
        __sync_add_and_fetch(&g_q_out, 1);
        g_free_direct(heap, ptr);
    }
}

static void q_push(u64 heap, u64 ptr) {
    int full;
    q_lock(); full = (g_q_head - g_q_tail) >= Q_N; q_unlock();
    if (full) q_drain(heap, 1);
    q_lock();
    { long long i = g_q_head & (Q_N - 1); g_q_ptr[i] = ptr; g_q_t[i] = GetTickCount64(); g_q_head++; }
    q_unlock();
    __sync_add_and_fetch(&g_q_in, 1);
}

static int validate_core(u64 heap, u64 ptr, u64 *ret_sp, int allow_q);
int validate_free(u64 heap, u64 ptr, u64 *ret_sp)    { return validate_core(heap, ptr, ret_sp, 1); }
int validate_free_nq(u64 heap, u64 ptr, u64 *ret_sp) { return validate_core(heap, ptr, ret_sp, 0); }

static int validate_core(u64 heap, u64 ptr, u64 *ret_sp, int allow_q) {
    u64 B, h = 0, N, nh = 0, P = 0, cand, fixed = 0, lo, hi;
    u64 *sp = ret_sp;
    int d, res = 1;

    __sync_add_and_fetch(&g_n_frees, 1);
    lo = HEAP_LO(heap); hi = HEAP_HI(heap);
    note_heap(heap, lo, hi);

    if ((ptr & 0xF) || ptr < lo || ptr > hi) {
        /* the game's own range check would also catch this; let it handle */
        ring_add(ptr, 0, 0, 0, ret_sp, 9);
        return 1;
    }
    B = ptr - 0x10;
    h = rd(B + 8);

    if ((h & 1) == 0) {                          /* not in use: double free */
        __sync_add_and_fetch(&g_n_skip, 1);
        log_event("SKIP double free", ptr, h, 0, sp, 1);
        ring_add(ptr, h, P, nh, ret_sp, 0);
        return 0;
    }

    if (allow_q && heap == (u64)g_base + RVA_MAIN_HEAP) {
        q_drain(heap, 0);                        /* release objects whose delay is over */
        if ((h & ~3ull) - B == VICTIM_BLOCK) {
            q_push(heap, ptr);
            ring_add(ptr, h, 0, 0, ret_sp, 3);   /* 3 = quarantined */
            return 0;                            /* the game thinks it is freed */
        }
    }

    /* ---- next block ---- */
    N = h & ~3ull;
    if (!in_heap(N, lo, hi) || N <= B) {
        __sync_add_and_fetch(&g_n_skip, 1);
        log_event("SKIP bad next pointer", ptr, h, 0, sp, 1);
        ring_add(ptr, h, P, nh, ret_sp, 0);
        return 0;
    }
    nh = rd(N + 8);
    if ((nh & 2) == 0) {
        /* N thinks we are already free although we are in use: the game
         * would compute "nh - 2" and create a misaligned pointer. */
        __sync_add_and_fetch(&g_n_skip, 1);
        log_event("SKIP next block flags inconsistent", ptr, h, nh, sp, 1);
        ring_add(ptr, h, P, nh, ret_sp, 0);
        return 0;
    }
    if ((nh & 1) == 0) {
        /* N is free and will be merged: its own next must be sane */
        u64 NN = nh & ~3ull;
        if ((nh & 3) != 2 || !in_heap(NN, lo, hi) || NN <= N || rd(NN) != N) {
            __sync_add_and_fetch(&g_n_skip, 1);
            log_event("SKIP corrupt free neighbour (next)", ptr, nh, NN, sp, 0);
            ring_add(ptr, h, P, nh, ret_sp, 0);
            return 0;
        }
    }

    /* ---- previous block (only used when it is free) ---- */
    if ((h & 2) == 0) {
        P = rd(B);
        if (!is_free_before(P, B, lo, hi)) {
            /* Use-after-free damage: P is off by a few bytes. Look for the
             * real free block among the 16-byte aligned addresses nearby. */
            for (d = -2; d <= 2 && !fixed; d++) {
                cand = ((P + 8) & ~0xFull) + (u64)(long long)(d * 16);
                if (is_free_before(cand, B, lo, hi)) fixed = cand;
            }
            if (fixed) {
                LONG nr;
                *(volatile u64 *)B = fixed;
                nr = __sync_add_and_fetch(&g_n_repair, 1);
                log_event("REPAIR prev pointer", ptr, P, fixed, sp, 1);
                if (nr <= 3) log_hexdump("damaged free block (P+0x10..B+8)", fixed + 0x10, B + 8 - (fixed + 0x10));
                res = 2;
            } else {
                __sync_add_and_fetch(&g_n_skip, 1);
                log_event("SKIP corrupt free neighbour (prev)", ptr, P, h, sp, 0);
                ring_add(ptr, h, P, nh, ret_sp, 0);
                return 0;
            }
        }
    }
    ring_add(ptr, h, P, nh, ret_sp, (u32)res);
    return 1;
}

/* Game's own error branches (should now be rare: validate_free runs first) */
void on_game_error(u64 ptr, int reason, u64 *sp, u64 heap) {
    (void)heap;
    __sync_add_and_fetch(&g_n_game, 1);
    log_event(reason == 1 ? "SKIP (game check) block not in use"
                          : "SKIP (game check) pointer outside heap", ptr, 0, 0, sp, 1);
}


/* ------------------------------------------------------------------ */
/* Crash reporter (first-chance, never interferes: always continues    */
/* the normal exception search)                                        */
/* ------------------------------------------------------------------ */
static const char *REGN[16] = {"rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
                               "r8 ","r9 ","r10","r11","r12","r13","r14","r15"};
static volatile u64 g_seen_rip[8];

static int put_addr(char *b, int p, u64 v) {           /* "sekiro+RVA" or raw */
    if (v >= (u64)g_base && v < (u64)g_base + 0x5000000) {
        p = put_str(b, p, "sekiro+"); return put_hex(b, p, v - (u64)g_base, 7);
    }
    return put_hex(b, p, v, 16);
}

static void dump_ring(void) {
    char buf[300]; int p, k; long long end = g_ring_i, i;
    u64 now = GetTickCount64();
    p = put_str(buf, 0, "  last frees (newest last; res 0=skipped 1=ok 2=repaired 3=quarantined 9=out of range):\r\n");
    write_log(buf, p);
    for (i = end - 32; i < end; i++) {
        FreeRec *r;
        if (i < 0) continue;
        r = &g_ring[i & (RING - 1)];
        p = put_str(buf, 0, "    -"); p = put_dec(buf, p, now - r->t, 1); p = put_str(buf, p, "ms tid=");
        p = put_hex(buf, p, r->tid, 4);
        p = put_str(buf, p, " res="); p = put_dec(buf, p, r->res, 1);
        p = put_str(buf, p, " ptr="); p = put_hex(buf, p, r->ptr, 12);
        p = put_str(buf, p, " h=");   p = put_hex(buf, p, r->h, 12);
        p = put_str(buf, p, " prev="); p = put_hex(buf, p, r->prev, 12);
        p = put_str(buf, p, " nh=");  p = put_hex(buf, p, r->nh, 12);
        p = put_str(buf, p, " from "); p = put_addr(buf, p, r->ret1);
        p = put_str(buf, p, " <- ");  p = put_addr(buf, p, r->ret2);
        buf[p++] = '\r'; buf[p++] = '\n'; write_log(buf, p);
        (void)k;
    }
}

static LONG WINAPI crash_veh(void *ep_) {
    u64 *ep = (u64 *)ep_;
    u32 *er = (u32 *)ep[0];
    u8 *ctx = (u8 *)ep[1];
    u32 code = er[0];
    u64 rip, *regs, nparams;
    char buf[700]; int p, i, k;

    if (code != 0xC0000005u && code != 0xC0000374u && code != 0xC000001Du &&
        code != 0xC0000096u && code != 0x80000002u) return 0;
    rip = *(u64 *)(ctx + 0xF8);
    for (i = 0; i < 8; i++) {
        if (g_seen_rip[i] == rip) return 0;              /* already reported */
        if (g_seen_rip[i] == 0 && __sync_bool_compare_and_swap(&g_seen_rip[i], 0, rip)) break;
    }
    if (i == 8) return 0;
    if (__sync_add_and_fetch(&g_n_crash, 1) > 5) return 0;

    regs = (u64 *)(ctx + 0x78);                        /* rax..r15, Windows order */
    p = put_time(buf, 0);
    p = put_str(buf, p, "=== EXCEPTION (first-chance, may still be handled) code=");
    p = put_hex(buf, p, code, 8);
    p = put_str(buf, p, " at "); p = put_addr(buf, p, rip);
    nparams = *(u32 *)((u8 *)er + 0x18);
    if (code == 0xC0000005u && nparams >= 2) {
        u64 *info = (u64 *)((u8 *)er + 0x20);
        p = put_str(buf, p, info[0] == 0 ? " read " : info[0] == 1 ? " write " : " exec ");
        p = put_hex(buf, p, info[1], 16);
    }
    p = put_str(buf, p, " tid="); p = put_hex(buf, p, GetCurrentThreadId(), 4);
    buf[p++] = '\r'; buf[p++] = '\n'; write_log(buf, p);

    /* registers, 4 per line */
    for (i = 0; i < 16; i += 4) {
        p = put_str(buf, 0, "  ");
        for (k = i; k < i + 4; k++) {
            p = put_str(buf, p, REGN[k]); buf[p++] = '='; p = put_hex(buf, p, regs[k], 16); buf[p++] = ' ';
        }
        buf[p++] = '\r'; buf[p++] = '\n'; write_log(buf, p);
    }
    /* code bytes around rip */
    log_hexdump("code at rip-32", rip - 32, 64);
    /* memory around every register that points somewhere readable;
     * for odd values also show the 16-byte aligned neighbourhood */
    for (i = 0; i < 16; i++) {
        u64 v = regs[i], a = (v & ~0xFull) - 0x20;
        if (i == 4) continue;                          /* rsp: stack dumped below */
        if (!readable(a, 0x60)) continue;
        if (v >= (u64)g_base && v < (u64)g_base + 0x5000000) continue;   /* code/data of exe */
        log_hexdump(REGN[i], a, 0x60);
    }
    /* call stack (heuristic scan) */
    p = put_str(buf, 0, "  stack:");
    {
        u64 *q = (u64 *)regs[4], top; int n = 0;
        top = STACK_BASE();
        for (; (u64)(q + 1) <= top && n < 24 && q < (u64 *)regs[4] + 1024; q++)
            if (looks_like_ret(*q)) { p = put_str(buf, p, " "); p = put_addr(buf, p, *q); n++; }
    }
    buf[p++] = '\r'; buf[p++] = '\n'; write_log(buf, p);
    log_hexdump("stack", regs[4], 0x100);

    p = put_str(buf, 0, "  counters: frees="); p = put_dec(buf, p, (u64)g_n_frees, 1);
    p = put_str(buf, p, " repaired="); p = put_dec(buf, p, (u64)g_n_repair, 1);
    p = put_str(buf, p, " skipped="); p = put_dec(buf, p, (u64)g_n_skip, 1);
    p = put_str(buf, p, " game_checks="); p = put_dec(buf, p, (u64)g_n_game, 1);
    buf[p++] = '\r'; buf[p++] = '\n'; write_log(buf, p);
    dump_ring();
    p = put_str(buf, 0, "=== end of report\r\n"); write_log(buf, p);
    return 0;                                          /* EXCEPTION_CONTINUE_SEARCH */
}

static void heartbeat(void) {
    char buf[200]; int p = put_time(buf, 0);
    p = put_str(buf, p, "alive: frees="); p = put_dec(buf, p, (u64)g_n_frees, 1);
    p = put_str(buf, p, " quarantined="); p = put_dec(buf, p, (u64)(g_q_head - g_q_tail), 1);
    p = put_str(buf, p, " q_total="); p = put_dec(buf, p, (u64)g_q_in, 1);
    p = put_str(buf, p, " q_released="); p = put_dec(buf, p, (u64)g_q_out, 1);
    p = put_str(buf, p, " repaired="); p = put_dec(buf, p, (u64)g_n_repair, 1);
    p = put_str(buf, p, " skipped="); p = put_dec(buf, p, (u64)g_n_skip, 1);
    p = put_str(buf, p, " game_checks="); p = put_dec(buf, p, (u64)g_n_game, 1);
    buf[p++] = '\r'; buf[p++] = '\n'; write_log(buf, p);
}


/* ------------------------------------------------------------------ */
/* Patching                                                           */
/* ------------------------------------------------------------------ */
typedef struct { u32 rva; int len; const u8 *orig; u8 patch[32]; const char *name; } Patch;
static Patch g_p[3];

static int eq(const u8 *a, const u8 *b, int n) { int i; for (i = 0; i < n; i++) if (a[i] != b[i]) return 0; return 1; }

static void make_jump(Patch *pt, u32 rva, int len, const u8 *orig, void (*target)(void), const char *name) {
    u64 t = (u64)target; int i;
    pt->rva = rva; pt->len = len; pt->orig = orig; pt->name = name;
    pt->patch[0] = 0x48; pt->patch[1] = 0xB8;            /* mov rax, imm64 */
    for (i = 0; i < 8; i++) pt->patch[2 + i] = (u8)(t >> (8 * i));
    pt->patch[10] = 0xFF; pt->patch[11] = 0xE0;          /* jmp rax        */
    for (i = 12; i < len; i++) pt->patch[i] = 0xCC;
}
static int write_code(u8 *dst, const u8 *src, int n) {
    u32 old, tmp; int i;
    if (!VirtualProtect(dst, (u64)n, 0x40, &old)) return 0;
    for (i = 0; i < n; i++) dst[i] = src[i];
    VirtualProtect(dst, (u64)n, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), dst, (u64)n);
    return 1;
}
/* 2 = already in place, 1 = applied now, 0 = write failed, -1 = unknown bytes */
static int apply(Patch *pt) {
    u8 *s = g_base + pt->rva;
    if (eq(s, pt->patch, pt->len)) return 2;
    if (!eq(s, pt->orig, pt->len)) return -1;
    return write_code(s, pt->patch, pt->len) ? 1 : 0;
}

static void parse_sections(void) {
    const u8 *nt = g_base + *(const u32 *)(g_base + 0x3C);
    u16 nsec = *(const u16 *)(nt + 6), optsz = *(const u16 *)(nt + 0x14), k;
    const u8 *sec = nt + 0x18 + optsz;
    for (k = 0; k < nsec && g_nx < 16; k++, sec += 40) {
        u32 vsize = *(const u32 *)(sec + 8), va = *(const u32 *)(sec + 12);
        if (*(const u32 *)(sec + 36) & 0x20000000u) {
            g_xlo[g_nx] = (u64)g_base + va; g_xhi[g_nx] = (u64)g_base + va + vsize; g_nx++;
        }
    }
}

static u32 WINAPI worker(void *arg) {
    int i, k, warned = 0;
    (void)arg;
    /* The game's code is decrypted after DLLs load: wait for it. */
    for (i = 0; ; i++) {
        if (eq(g_base + RVA_FREE, ORIG_FREE, 14) && eq(g_base + RVA_FREE + 14, ORIG_FREE_TAIL, 5)) break;
        if (i == 1200) {
            log_bytes("ERROR: game code never found, fix NOT applied. Bytes:", g_base + RVA_FREE, 14);
            return 0;
        }
        Sleep(500);
    }
    g_free_cont = (u64)g_base + RVA_FREE_CONT;
    make_jump(&g_p[0], RVA_SITE_A, 27, ORIG_A, err_stub_a, "site A");
    make_jump(&g_p[1], RVA_SITE_B, 27, ORIG_B, err_stub_b, "site B");
    make_jump(&g_p[2], RVA_FREE,   14, ORIG_FREE, free_hook, "free hook");
    for (k = 0; k < 3; k++) {
        int r = apply(&g_p[k]);
        if (r <= 0) {
            char buf[120]; int p = put_time(buf, 0);
            p = put_str(buf, p, "ERROR: could not apply "); p = put_str(buf, p, g_p[k].name);
            buf[p++] = '\r'; buf[p++] = '\n'; write_log(buf, p);
            return 0;
        }
    }
    {
        char buf[120]; int p = put_time(buf, 0);
        p = put_str(buf, p, "sekiro_crashfix " SCF_VERSION " active (applied after ");
        p = put_dec(buf, p, (u64)i / 2, 1);
        p = put_str(buf, p, " s)\r\n");
        write_log(buf, p);
    }
    heartbeat();
    for (i = 1; ; i++) {
        Sleep(5000);
        if (i % 120 == 0) heartbeat();                 /* every 10 minutes */
        for (k = 0; k < 3; k++) {
            int r = apply(&g_p[k]);
            if (r == 1) log_msg("patch was reverted by the game, re-applied");
            if (r < 0 && !warned) { log_msg("WARNING: patched bytes changed by something else"); warned = 1; }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* dinput8 proxy                                                      */
/* ------------------------------------------------------------------ */
static void *g_real;
static void *real_proc(const char *name) {
    if (!g_real) {
        WCHAR path[600]; u32 n = GetSystemDirectoryW(path, 560);
        const WCHAR *f = L"\\dinput8.dll"; int i = 0;
        while (f[i]) { path[n + i] = f[i]; i++; } path[n + i] = 0;
        g_real = LoadLibraryW(path);
        if (!g_real) return 0;
    }
    return GetProcAddress(g_real, name);
}
typedef long (WINAPI *fn5)(void *, u32, const void *, void **, void *);
typedef long (WINAPI *fn3)(const void *, const void *, void **);
typedef long (WINAPI *fn0)(void);
typedef void *(WINAPI *fnp)(void);

__declspec(dllexport) long WINAPI DirectInput8Create(void *h, u32 v, const void *iid, void **out, void *u) {
    fn5 f = (fn5)real_proc("DirectInput8Create"); return f ? f(h, v, iid, out, u) : (long)0x80004005;
}
__declspec(dllexport) long WINAPI DllGetClassObject(const void *a, const void *b, void **c) {
    fn3 f = (fn3)real_proc("DllGetClassObject"); return f ? f(a, b, c) : (long)0x80004005;
}
__declspec(dllexport) long WINAPI DllCanUnloadNow(void) {
    fn0 f = (fn0)real_proc("DllCanUnloadNow"); return f ? f() : 1;
}
__declspec(dllexport) long WINAPI DllRegisterServer(void) {
    fn0 f = (fn0)real_proc("DllRegisterServer"); return f ? f() : (long)0x80004005;
}
__declspec(dllexport) long WINAPI DllUnregisterServer(void) {
    fn0 f = (fn0)real_proc("DllUnregisterServer"); return f ? f() : (long)0x80004005;
}
__declspec(dllexport) void *WINAPI GetdfDIJoystick(void) {
    fnp f = (fnp)real_proc("GetdfDIJoystick"); return f ? f() : 0;
}

BOOL WINAPI DllMain(void *inst, u32 reason, void *res) {
    (void)res;
    if (reason == 1) {
        u32 n, i, slash = 0;
        DisableThreadLibraryCalls(inst);
        g_base = (u8 *)GetModuleHandleW(0);
        n = GetModuleFileNameW(0, g_logpath, 560);
        for (i = 0; i < n; i++) if (g_logpath[i] == '\\') slash = i + 1;
        {
            const WCHAR *f = L"sekiro_crashfix.log"; i = 0;
            while (f[i]) { g_logpath[slash + i] = f[i]; i++; }
            g_logpath[slash + i] = 0;
        }
        log_msg("loading sekiro_crashfix " SCF_VERSION);
        parse_sections();
        AddVectoredExceptionHandler(1, crash_veh);
        CreateThread(0, 0, worker, 0, 0, 0);
    }
    return 1;
}
