/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Fiber and thread stacks commit what Windows commits and grow through their
 * guard page (patch 0604). Assetto Corsa EVO creates about 2,750 fibers with
 * the default sizes of its exe header, 1 MiB reserved and 4 KiB committed; on
 * the console every committed page is direct memory, and Wine committed each
 * fiber's whole stack. This creates as many fibers (the count is argv[1]),
 * with this exe's 1 MiB and 4 KiB, and reports what their stacks commit; then
 * on a new fiber and a new thread it sends itself a WM_GETTEXT with a
 * 200 KB buffer, which Wine passes to the window procedure under the stack
 * pointer, written from its low end past the guard page, and recurses about
 * 600 KiB deep, which must grow the stack through the guard page; last it
 * overflows a fiber and a thread, which must still raise
 * STATUS_STACK_OVERFLOW. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#define DEPTH 600          /* frames of about 1 KiB */
#define GROWTH (512 * 1024)

/* The head of a fiber, as Windows and Wine's kernelbase lay it out. */
struct fiber_head
{
    void *param, *except, *stack_base, *stack_limit, *stack_allocation;
};

#define TEXT_CHARS 100000  /* a WM_GETTEXT buffer of 200 KB */

struct growth
{
    SIZE_T before, after, limit_moved;
    int guard_below, result, callback;
};

static void *main_fiber, *recover_sp, *overflow_resume;
static volatile LONG overflows;
static volatile LONG stop;  /* never set: the recursion of forever() is not provably endless */
static DWORD overflow_thread;
static struct growth fiber_growth, thread_growth;

/* What [low, high) commits, less its guard pages, and its guard pages. */
static SIZE_T committed(const char *low, const char *high, SIZE_T *guard)
{
    MEMORY_BASIC_INFORMATION mbi;
    SIZE_T total = 0;

    if (guard) *guard = 0;
    while (low < high && VirtualQuery(low, &mbi, sizeof(mbi)))
    {
        SIZE_T size = min(mbi.RegionSize, (SIZE_T)(high - low));

        if (mbi.State == MEM_COMMIT && (mbi.Protect & PAGE_GUARD)) { if (guard) *guard += size; }
        else if (mbi.State == MEM_COMMIT) total += size;
        low += size;
    }
    return total;
}

/* What the current stack commits, from its allocation base to its top. */
static SIZE_T own_stack(void)
{
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    MEMORY_BASIC_INFORMATION mbi;

    VirtualQuery(tib->StackLimit, &mbi, sizeof(mbi));
    return committed(mbi.AllocationBase, tib->StackBase, NULL);
}

static __attribute__((noinline)) int recurse(int depth)
{
    volatile char frame[1000];

    frame[0] = (char)depth;
    frame[sizeof(frame) - 1] = 1;
    if (!depth) return frame[0];
    return recurse(depth - 1) + frame[sizeof(frame) - 1];
}

static __attribute__((noinline)) int forever(int depth)
{
    volatile char frame[1000];

    frame[0] = (char)depth;
    if (stop) return 0;
    return forever(depth + 1) + frame[0];
}

static LRESULT CALLBACK text_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_GETTEXT && wparam > 5)
    {
        lstrcpyW((WCHAR *)lparam, L"fiber");
        return 5;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

/* A window procedure's WM_GETTEXT buffer, on the current stack. */
static int large_callback(void)
{
    static WCHAR text[TEXT_CHARS];
    HWND hwnd = CreateWindowExW(0, L"pw_fiber_stacks", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, NULL, NULL);
    LRESULT length;

    if (!hwnd) return 0;
    text[0] = 0;
    length = SendMessageW(hwnd, WM_GETTEXT, TEXT_CHARS, (LPARAM)text);
    DestroyWindow(hwnd);
    return length == 5 && !lstrcmpW(text, L"fiber");
}

/* Runs a large callback and recurses on the current stack, and records how
 * it grew. */
static void grow(struct growth *g)
{
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    char *limit = tib->StackLimit;
    MEMORY_BASIC_INFORMATION mbi;

    g->before = own_stack();
    g->callback = large_callback();
    g->result = recurse(DEPTH);
    g->after = own_stack();
    g->limit_moved = limit - (char *)tib->StackLimit;
    g->guard_below = VirtualQuery((char *)tib->StackLimit - 1, &mbi, sizeof(mbi)) &&
                     mbi.State == MEM_COMMIT && (mbi.Protect & PAGE_GUARD);
}

static void print_growth(const char *what, const struct growth *g)
{
    printf("%s before=%llu after=%llu limit_moved=%llu guard_below=%d callback=%d\n", what,
           (unsigned long long)g->before, (unsigned long long)g->after, (unsigned long long)g->limit_moved,
           g->guard_below, g->callback);
}

static void WINAPI grow_fiber(void *param)
{
    grow(param);
    SwitchToFiber(main_fiber);
}

static DWORD WINAPI grow_thread(void *param)
{
    grow(param);
    return 0;
}

/* Where an overflowed stack resumes, on its top page. */
static void overflow_fiber_done(void) { SwitchToFiber(main_fiber); }
static void overflow_thread_done(void) { ExitThread(0); }

static LONG CALLBACK overflow_handler(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode != STATUS_STACK_OVERFLOW) return EXCEPTION_CONTINUE_SEARCH;
    if (GetCurrentThreadId() != overflow_thread) return EXCEPTION_CONTINUE_SEARCH;
    InterlockedIncrement(&overflows);
#ifdef _WIN64
    ep->ContextRecord->Rsp = (DWORD64)recover_sp;
    ep->ContextRecord->Rip = (DWORD64)overflow_resume;
#else
    ep->ContextRecord->Esp = (DWORD)recover_sp;
    ep->ContextRecord->Eip = (DWORD)overflow_resume;
#endif
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void overflow_here(void *resume)
{
    char *top = ((NT_TIB *)NtCurrentTeb())->StackBase;

    /* A call's return address just pushed, 2 KiB under the top. */
    recover_sp = (void *)((((UINT_PTR)top - 0x800) & ~(UINT_PTR)15) - sizeof(void *));
    overflow_resume = resume;
    forever(0);
}

static void WINAPI overflow_fiber(void *param)
{
    (void)param;
    overflow_here((void *)overflow_fiber_done);
}

static DWORD WINAPI overflow_in_thread(void *param)
{
    (void)param;
    overflow_thread = GetCurrentThreadId();
    overflow_here((void *)overflow_thread_done);
    return 1;
}

int main(int argc, char **argv)
{
    int count = argc > 1 ? atoi(argv[1]) : 3000, created = 0, i, fiber_overflow, thread_overflow;
    void **fibers = calloc(count, sizeof(*fibers));
    SIZE_T total = 0, guard_total = 0, reserve = 0, guard;
    WNDCLASSW cls = { 0 };
    HANDLE thread;
    void *fiber;
    BOOL ok;

    cls.lpfnWndProc = text_proc;
    cls.lpszClassName = L"pw_fiber_stacks";
    main_fiber = ConvertThreadToFiber(NULL);
    if (!fibers || !main_fiber || !RegisterClassW(&cls)) { printf("fiber-stacks verdict=fail setup\n"); return 1; }

    for (i = 0; i < count; i++)
    {
        struct fiber_head *head;

        if (!(fibers[i] = CreateFiberEx(0, 0, 0, grow_fiber, NULL))) break;
        head = fibers[i];
        total += committed(head->stack_allocation, head->stack_base, &guard);
        guard_total += guard;
        reserve += (char *)head->stack_base - (char *)head->stack_allocation;
        created++;
    }
    printf("fibers=%d reserve_per_fiber=%llu committed=%llu committed_per_fiber=%llu guard_per_fiber=%llu\n",
           created, (unsigned long long)(created ? reserve / created : 0), (unsigned long long)total,
           (unsigned long long)(created ? total / created : 0), (unsigned long long)(created ? guard_total / created : 0));
    for (i = 0; i < created; i++) DeleteFiber(fibers[i]);

    fiber = CreateFiberEx(0, 0, 0, grow_fiber, &fiber_growth);
    SwitchToFiber(fiber);
    DeleteFiber(fiber);
    print_growth("fiber-growth", &fiber_growth);

    thread = CreateThread(NULL, 0, grow_thread, &thread_growth, 0, NULL);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    print_growth("thread-growth", &thread_growth);

    AddVectoredExceptionHandler(1, overflow_handler);
    overflow_thread = GetCurrentThreadId();
    fiber = CreateFiberEx(0, 0, 0, overflow_fiber, NULL);
    SwitchToFiber(fiber);
    DeleteFiber(fiber);
    fiber_overflow = overflows;
    thread = CreateThread(NULL, 0, overflow_in_thread, NULL, 0, NULL);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    thread_overflow = overflows - fiber_overflow;
    printf("overflow fiber=%d thread=%d\n", fiber_overflow, thread_overflow);

    ok = created == count && fiber_overflow == 1 && thread_overflow == 1;
    ok = ok && fiber_growth.result == DEPTH && thread_growth.result == DEPTH;
    ok = ok && fiber_growth.after >= GROWTH && thread_growth.after >= GROWTH;
    ok = ok && fiber_growth.guard_below && thread_growth.guard_below;
    ok = ok && fiber_growth.callback && thread_growth.callback;
    printf("fiber-stacks verdict=%s\n", ok ? "pass" : "fail");
    return ok ? 0 : 1;
}
