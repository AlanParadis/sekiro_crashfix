/*
 * Unit tests for the heap validation and quarantine logic of
 * sekiro_crashfix, run on a simulated copy of the game's heap layout.
 * Builds and runs on Linux/WSL/macOS with clang:   ./tests/run_tests.sh
 *
 * Block layout reproduced here (see docs/TECHNICAL.md):
 *   [B+0] previous block pointer (only meaningful while the previous block is free)
 *   [B+8] next block pointer | bit0 (in use) | bit1 (previous in use)
 */
#define __declspec(x)
#define __stdcall
#define STACK_BASE() 0ull
#include "../src/sekiro_crashfix.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Windows API stubs ---- */
static u64 fake_now = 1000;
u32 GetSystemDirectoryW(WCHAR *a, u32 b) { return 0; }
void *LoadLibraryW(const WCHAR *a) { return 0; }
void *GetProcAddress(void *a, const char *b) { return 0; }
void *GetModuleHandleW(const WCHAR *a) { return 0; }
u32 GetModuleFileNameW(void *a, WCHAR *b, u32 c) { return 0; }
HANDLE CreateFileW(const WCHAR *a, u32 b, u32 c, void *d, u32 e, u32 f, HANDLE g) { return (HANDLE)1; }
BOOL WriteFile(HANDLE h, const void *b, u32 n, u32 *w, void *o) { fwrite(b, 1, n, stdout); return 1; }
BOOL CloseHandle(HANDLE h) { return 1; }
void GetLocalTime(SYSTEMTIME *t) { memset(t, 0, sizeof *t); }
BOOL DisableThreadLibraryCalls(void *a) { return 1; }
BOOL VirtualProtect(void *a, u64 b, u32 c, u32 *d) { return 1; }
BOOL FlushInstructionCache(HANDLE a, const void *b, u64 c) { return 1; }
HANDLE GetCurrentProcess(void) { return 0; }
u32 GetCurrentThreadId(void) { return 1; }
HANDLE CreateThread(void *a, u64 b, u32 (*c)(void *), void *d, u32 e, u32 *f) { return 0; }
void Sleep(u32 a) {}
u64 GetTickCount64(void) { return fake_now; }
u64 VirtualQuery(const void *a, void *b, u64 c) { return 0; }
void *AddVectoredExceptionHandler(u32 a, LONG (*f)(void *)) { return 0; }
char __ImageBase;

/* ---- helpers ---- */
static int failures;
#define CHECK(cond, what) do { \
    if (cond) printf("  ok    %s\n", what); \
    else { printf("  FAIL  %s\n", what); failures++; } } while (0)

static u64 W(u64 a, u64 v) { *(u64 *)a = v; return v; }
static u64 R64(u64 a) { return *(u64 *)a; }
static u64 fake_stack[64];
static u64 heap_obj[16];        /* fake heap object: +0x28 lo, +0x30 hi */
static u64 mem;

static int real_frees; static u64 last_real;
static void stub_real_free(u64 heap, u64 ptr) {
    validate_free_nq(heap, ptr, fake_stack);
    real_frees++; last_real = ptr;
}

static void new_heap(int as_main_heap) {
    memset((void *)mem, 0, 0x4000);
    memset(heap_obj, 0, sizeof heap_obj);
    heap_obj[5] = mem + 0x10;           /* lo */
    heap_obj[6] = mem + 0x3000;         /* hi */
    /* make this fake heap the "main heap" (quarantine on) or not */
    g_base = as_main_heap ? (u8 *)((u64)heap_obj - RVA_MAIN_HEAP) : (u8 *)0x10000000;
    g_free_direct = stub_real_free;
    real_frees = 0; last_real = 0;
}

int main(void) {
    u64 X, P, B, N, E;
    setvbuf(stdout, 0, _IONBF, 0);
    mem = (u64)aligned_alloc(16, 0x4000);

    printf("validation (non-quarantined heap)\n");
    new_heap(0);
    /* X used | P free | B used | N used | end */
    X = mem; P = mem + 0x100; B = mem + 0x360; N = mem + 0x500; E = mem + 0x600;
    W(X + 8, P | 3); W(P + 8, B | 2); W(B, P); W(B + 8, N | 1); W(N + 8, E | 3); W(E + 8, (E + 0x100) | 3);

    CHECK(validate_free((u64)heap_obj, B + 0x10, fake_stack) == 1 && R64(B) == P,
          "consistent heap: free allowed, nothing changed");

    W(B, P - 1);    /* exactly the corruption seen in the crash dumps */
    CHECK(validate_free((u64)heap_obj, B + 0x10, fake_stack) == 1 && R64(B) == P,
          "prev pointer = P-1: repaired to P, free allowed");

    W(B, 0x1234567);
    CHECK(validate_free((u64)heap_obj, B + 0x10, fake_stack) == 0,
          "garbage prev pointer: free skipped");
    W(B, P);

    W(B + 8, N | 0);
    CHECK(validate_free((u64)heap_obj, B + 0x10, fake_stack) == 0,
          "block already free (double free): free skipped");
    W(B + 8, N | 1);

    W(N + 8, E | 1);
    CHECK(validate_free((u64)heap_obj, B + 0x10, fake_stack) == 0,
          "next block thinks we are free: free skipped");
    W(N + 8, E | 3);

    W(B + 8, N | 3); W(B, 0xdeadbeef);
    CHECK(validate_free((u64)heap_obj, B + 0x10, fake_stack) == 1,
          "previous block in use: stale prev field ignored, free allowed");

    printf("quarantine (main heap)\n");
    new_heap(1);
    /* X used | V used (0x260 = victim size) | Y used | end */
    {
        u64 V = mem + 0x100, Y = V + VICTIM_BLOCK; E = Y + 0x200; X = mem;
        W(X + 8, V | 3); W(V + 8, Y | 3); W(Y + 8, E | 3); W(E + 8, (E + 0x100) | 3);
        fake_now = 1000;

        CHECK(validate_free((u64)heap_obj, V + 0x10, fake_stack) == 0 && R64(V + 8) == (Y | 3),
              "0x258-byte object: quarantined, its block left untouched");

        W(Y, R64(Y) - 1);   /* the game's late decrement: lands in the dead object */
        CHECK(validate_free((u64)heap_obj, X + 0x10, fake_stack) == 1 && real_frees == 0,
              "other block freed right after: quarantined object kept");

        fake_now += Q_MS - 1;
        CHECK(validate_free((u64)heap_obj, Y + 0x10, fake_stack) == 1 && real_frees == 0,
              "just before the delay: still kept");

        fake_now += 2;
        CHECK(validate_free((u64)heap_obj, Y + 0x10, fake_stack) == 1 && real_frees == 1 && last_real == V + 0x10,
              "after the delay: really freed on the next free");
    }

    printf("%s\n", failures ? "SOME TESTS FAILED" : "all tests passed");
    return failures != 0;
}
