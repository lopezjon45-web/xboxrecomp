/*
 * frontend_lifted.c -- xbox_frontend.h for statically recompiled titles.
 *
 * Guest code is a set of lifted C functions found by address (recomp_lookup), arguments go on the simulated stack
 * addressed by g_esp, results come back in g_eax/g_edx. Listed in lifted-only.txt: a direct build defines these
 * itself.
 */
#include "kernel.h"
#include "xbox_memory_layout.h"
#include "xbox/xbox_frontend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_seh_ebp;
extern ptrdiff_t g_xbox_mem_offset;

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
void xbox_worker_stack_free(int slot);
void xbox_PinToGuestCore(void);
void guest_cpu_join(void);
void guest_cpu_part(void);

#define LIFTED_MEM32(addr) (*(volatile uint32_t *)((uintptr_t)(addr) + g_xbox_mem_offset))

/* A kernel export can be the target too: Halo's XAPI hands NtReadFile one of the synthetic thunk VAs as its APC. */
static recomp_func_t lifted_find(uint32_t va)
{
    recomp_func_t fn = recomp_lookup(va);
    if (!fn) fn = recomp_lookup_manual(va);
    if (!fn) fn = recomp_lookup_kernel(va);
    return fn;
}

static void lifted_push(uint32_t value)
{
    g_esp -= 4;
    LIFTED_MEM32(g_esp) = value;
}

/* The routine runs with the stack laid out as a call would leave it: arguments, then a return address of 0 that the
 * lifted `ret` consumes. */
static void lifted_run(recomp_func_t fn, unsigned argc, const uint32_t *argv, XboxCallConv conv)
{
    unsigned first = 0;

    if (conv == XBOX_CALL_FASTCALL || conv == XBOX_CALL_THISCALL) {
        if (argc > 0) g_ecx = argv[first++];
        if (conv == XBOX_CALL_FASTCALL && argc > 1) g_edx = argv[first++];
    }
    for (unsigned i = argc; i-- > first;)
        lifted_push(argv[i]);
    lifted_push(0);
    fn();
}

uint64_t xbox_frontend_guest_call(uint32_t va, unsigned argc, const uint32_t *argv, XboxCallConv conv)
{
    recomp_func_t fn = lifted_find(va);
    int slot = -1;
    uint32_t saved;
    uint64_t result;

    if (!fn) {
        fprintf(stderr, "  [KERNEL] guest code 0x%08X is not in the dispatch\n", va);
        fflush(stderr);
        return 0;
    }
    /* A host thread that never ran guest code (a device's interrupt thread) borrows a worker slice for the call. */
    if (!g_esp) {
        slot = xbox_worker_stack_alloc();
        if (slot < 0) {
            fprintf(stderr, "  [KERNEL] no worker stack to call guest code 0x%08X\n", va);
            fflush(stderr);
            return 0;
        }
        g_esp = XBOX_WORKER_STACK_TOP(slot);
    }
    saved = g_esp;
    lifted_run(fn, argc, argv, conv);
    g_esp = saved;
    result = ((uint64_t)g_edx << 32) | g_eax;
    if (slot >= 0) {
        xbox_worker_stack_free(slot);
        g_esp = 0;
    }
    return result;
}

/* ── Threads ──────────────────────────────────────────────────────────────────────────────────────────────────── */

/* Set on threads spawned here; PsTerminateSystemThread ends only those. */
static RECOMP_TLS int g_is_spawned_thread = 0;

/* This thread's simulated stack, so both exits can give it back: the common exit is ExitThread from
 * PsTerminateSystemThread, which never returns to lifted_thread_main. */
static RECOMP_TLS uint32_t g_thread_stack_top = 0;

struct lifted_thread_start {
    recomp_func_t fn;
    uint32_t ctx1, ctx2, stack_top;
};

static void lifted_run_thread_inline(recomp_func_t fn, uint32_t ctx1, uint32_t ctx2)
{
    uint32_t args[2] = { ctx1, ctx2 };
    uint32_t saved = g_esp;

    g_seh_ebp = g_esp - 12;
    lifted_run(fn, 2, args, XBOX_CALL_STDCALL);
    g_esp = saved;
}

static DWORD WINAPI lifted_thread_main(LPVOID param)
{
    struct lifted_thread_start *s = (struct lifted_thread_start *)param;
    recomp_func_t fn = s->fn;
    uint32_t ctx1 = s->ctx1, ctx2 = s->ctx2;

    xbox_PinToGuestCore();
#if !defined(_WIN32)
    guest_turn_note_start((const void *)fn, ctx1);
#endif

    /* Own register set (RECOMP_TLS), own simulated stack and own TIB. The TIB carries the SEH chain head and, through
     * fs:[4], the CRT's per-thread data; sharing one put two threads inside _lock() each holding the lock the other
     * wanted. */
    g_is_spawned_thread = 1;
    g_esp = s->stack_top;
    g_thread_stack_top = s->stack_top;
    {
        uint32_t tib = xbox_AllocThreadTib();
        if (tib)
            g_fs_base = tib;
        else
            fprintf(stderr, "  [KERNEL] worker thread has no TIB of its own;"
                            " it shares the main thread's\n");
    }
    free(s);

    guest_cpu_join();
    lifted_run_thread_inline(fn, ctx1, ctx2);
    guest_cpu_part();

    fprintf(stderr, "  [KERNEL] worker thread returned (eax=0x%08X)\n", g_eax);
    fflush(stderr);
    xbox_FreeThreadStack(g_thread_stack_top);
    g_thread_stack_top = 0;
    return 0;
}

static HANDLE lifted_spawn(recomp_func_t fn, uint32_t ctx1, uint32_t ctx2, uint32_t stack_top)
{
    struct lifted_thread_start *s = malloc(sizeof(*s));
    HANDLE th;

    if (!s) return NULL;
    s->fn = fn; s->ctx1 = ctx1; s->ctx2 = ctx2; s->stack_top = stack_top;

    th = CreateThread(NULL, 0, lifted_thread_main, s, 0, NULL);
    if (!th) free(s);
    /* Recorded so a host-tick-driven title's watchdog can sample it (kernel_thread.c). */
    else xbox_set_game_thread(th);
    return th;
}

/* Two ways a title expects its first PsCreateSystemThreadEx to behave.
 *
 * INLINE (default): the first call IS the game starting -- run the routine inline, inheriting register state, and it
 * drives its own main loop forever. Halo and Crimson Skies need this.
 *
 * SPAWN: the title's entry spawns an init thread and RETURNS, expecting the host to drive the per-frame tick
 * afterwards (Burnout 3 is tick-driven), so every call spawns a real thread. See
 * docs/technical/burnout3-reunification.md. */
static int g_thread_mode = XBOX_THREAD_MODE_INLINE;
static int g_thread_call_count = 0;
void xbox_SetThreadMode(int mode) { g_thread_mode = mode; }

void *xbox_frontend_spawn_thread(uint32_t entry, unsigned argc, const uint32_t *argv, uint32_t stack_size,
                                 int priority, int suspended, uint32_t *status)
{
    int is_first_call = (g_thread_mode == XBOX_THREAD_MODE_INLINE) && (g_thread_call_count == 0);
    recomp_func_t fn = lifted_find(entry);
    uint32_t ctx1 = argc > 0 ? argv[0] : 0, ctx2 = argc > 1 ? argv[1] : 0;
    const char *inline_workers;
    uint32_t stack_top;
    HANDLE th;

    (void)stack_size; (void)priority; (void)suspended;
    g_thread_call_count++;
    *status = 0;

    if (!fn) {
        fprintf(stderr, "  [KERNEL] PsCreateSystemThread: start routine 0x%08X not found in dispatch!\n", entry);
        return NULL;
    }
    if (is_first_call) {
        uint32_t saved = g_esp;
        lifted_run(fn, argc, argv, XBOX_CALL_STDCALL);
        g_esp = saved;
        fprintf(stderr, "  [KERNEL] PsCreateSystemThread: main thread returned (g_eax=0x%08X)\n", g_eax);
        fflush(stderr);
        return NULL;
    }
    /* RECOMP_WORKERS=inline runs a worker routine on the calling thread instead of spawning one. Not a mode to ship
     * a title in -- a worker that waits for requests never returns -- but it separates a concurrency bug from
     * everything else in one run. */
    inline_workers = getenv("RECOMP_WORKERS");
    if (inline_workers && !strcmp(inline_workers, "inline")) {
        fprintf(stderr, "  [KERNEL] RECOMP_WORKERS=inline: running worker 0x%08X on this thread\n", entry);
        fflush(stderr);
        lifted_run_thread_inline(fn, ctx1, ctx2);
        return NULL;
    }
    stack_top = xbox_AllocThreadStack();
    if (!stack_top) {
        fprintf(stderr, "  [KERNEL] PsCreateSystemThread: out of thread stacks, running worker 0x%08X inline\n",
                entry);
        fflush(stderr);
        lifted_run_thread_inline(fn, ctx1, ctx2);
        return NULL;
    }
    th = lifted_spawn(fn, ctx1, ctx2, stack_top);
    fprintf(stderr, "  [KERNEL] PsCreateSystemThread: spawned worker 0x%08X (ctx=0x%08X, stack top 0x%08X)\n",
            entry, ctx1, stack_top);
    fflush(stderr);
    return th;
}

/* The main thread returns: it is the host's thread, and unwinding back to main() is how the process shuts down
 * cleanly. A worker that returned instead would fall off the end of its start routine into whatever bytes follow. */
void xbox_frontend_exit_thread(uint32_t exit_status)
{
    if (!g_is_spawned_thread)
        return;
    xbox_FreeThreadStack(g_thread_stack_top);
    g_thread_stack_top = 0;
    ExitThread(exit_status);
}

uint32_t xbox_frontend_kpcr(void)
{
    return g_fs_base;
}

uint32_t xbox_frontend_kernel_data_base(void)
{
    return XBOX_KERNEL_DATA_BASE;
}

/* Page zero is deliberately unmapped; the rest of RAM (or the larger map) and the contiguous window are backed. */
int xbox_frontend_va_mapped(uint32_t va, uint32_t bytes)
{
    uint64_t end = (uint64_t)va + bytes;
    uint64_t mapped = g_xbox_map_size ? g_xbox_map_size : g_xbox_total_ram;

    if (va < 0x1000)
        return 0;
    if (end <= mapped)
        return 1;
    return va >= XBOX_CONTIG_BASE && end <= (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE;
}

void xbox_frontend_return_to_hub(uint32_t reason)
{
    xbox_HalReturnToFirmware(reason);
    ExitProcess(0);
}

int xbox_frontend_nv2a_attached(void)
{
    return 1;
}

int xbox_Nv2aIrqPending(void)
{
    return 0;
}

const XboxDiscVolume *xbox_frontend_disc(void)
{
    return NULL;
}
