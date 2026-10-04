/* SPDX-License-Identifier: MIT
 * Bounded ordinary Win32 event/semaphore semantic matrix.
 * No forced thread termination, fault injection or unbounded waits.
 */
#define _WIN32_WINNT 0x0601
#include <windows.h>

static HANDLE report_file = INVALID_HANDLE_VALUE;
static unsigned checks, failures, cases;

struct line { char data[512]; unsigned used; };
static void text(struct line *line, const char *value)
{
    while (*value && line->used < sizeof(line->data) - 1)
        line->data[line->used++] = *value++;
}
static void hex(struct line *line, DWORD value)
{
    static const char digits[] = "0123456789abcdef";
    text(line, "0x");
    for (unsigned i = 0; i < 8; ++i)
        if (line->used < sizeof(line->data) - 1)
            line->data[line->used++] = digits[(value >> (28 - 4 * i)) & 15];
}
static void write_line(struct line *line)
{
    DWORD written;
    text(line, "\n");
    if (report_file != INVALID_HANDLE_VALUE)
        (void)WriteFile(report_file, line->data, line->used, &written, NULL);
    (void)WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line->data, line->used, &written, NULL);
}
static void begin_case(const char *name)
{
    struct line line = {{0}, 0};
    ++cases;
    text(&line, "PW_SYNC_ORDINARY case="); text(&line, name); write_line(&line);
}
static void check(const char *name, DWORD actual, DWORD expected)
{
    struct line line = {{0}, 0};
    ++checks;
    if (actual != expected) ++failures;
    text(&line, "PW_SYNC_ORDINARY check="); text(&line, name);
    text(&line, " actual="); hex(&line, actual);
    text(&line, " expected="); hex(&line, expected);
    text(&line, actual == expected ? " result=PASS" : " result=FAIL");
    write_line(&line);
}
static BOOL valid(const char *name, HANDLE handle)
{
    BOOL ok = handle && handle != INVALID_HANDLE_VALUE;
    check(name, ok, TRUE);
    return ok;
}
static void close_handle(HANDLE handle)
{
    if (handle && handle != INVALID_HANDLE_VALUE) check("close_handle", CloseHandle(handle), TRUE);
}
typedef LONG (NTAPI *EventOp)(HANDLE, LONG *);
typedef LONG (NTAPI *ReleaseSem)(HANDLE, ULONG, ULONG *);
typedef LONG (NTAPI *CreateEventNative)(HANDLE *, ACCESS_MASK, void *, ULONG, BOOLEAN);
typedef LONG (NTAPI *CreateSemNative)(HANDLE *, ACCESS_MASK, void *, LONG, LONG);
typedef LONG (NTAPI *QueryObject)(HANDLE, ULONG, void *, ULONG, ULONG *);
static EventOp nt_set, nt_reset;
static ReleaseSem nt_release;
static CreateEventNative nt_create_event;
static CreateSemNative nt_create_sem;
static QueryObject nt_query_event, nt_query_sem;
#define ACCESS_DENIED_STATUS 0xc0000022u
#define TYPE_MISMATCH_STATUS 0xc0000024u
#define SEMAPHORE_LIMIT_STATUS 0xc0000047u

static void events(BOOL manual)
{
    begin_case(manual ? "manual_event_previous_state" : "auto_event_previous_state");
    HANDLE event = CreateEventW(NULL, manual, FALSE, NULL);
    if (!valid("create_event", event)) return;
    LONG previous = 99;
    check("empty_event", WaitForSingleObject(event, 0), WAIT_TIMEOUT);
    check("native_set", (DWORD)nt_set(event, &previous), 0); check("set_previous_zero", previous, 0);
    check("native_set_again", (DWORD)nt_set(event, &previous), 0); check("set_previous_one", previous, 1);
    check("native_reset", (DWORD)nt_reset(event, &previous), 0); check("reset_previous_one", previous, 1);
    check("native_reset_again", (DWORD)nt_reset(event, &previous), 0); check("reset_previous_zero", previous, 0);
    for (unsigned i = 0; i < 8; ++i)
    {
        check("cycle_set", SetEvent(event), TRUE);
        check("cycle_wait", WaitForSingleObject(event, 0), WAIT_OBJECT_0);
        check("cycle_second_wait", WaitForSingleObject(event, 0), manual ? WAIT_OBJECT_0 : WAIT_TIMEOUT);
        check("cycle_reset", ResetEvent(event), TRUE);
        check("cycle_reset_empty", WaitForSingleObject(event, 0), WAIT_TIMEOUT);
    }
    close_handle(event);
}
static void semaphores(void)
{
    begin_case("semaphore_count_maximum_previous");
    HANDLE sem = CreateSemaphoreW(NULL, 0, 3, NULL);
    if (!valid("create_semaphore", sem)) return;
    ULONG previous = 99;
    check("empty_semaphore", WaitForSingleObject(sem, 0), WAIT_TIMEOUT);
    check("release_two", (DWORD)nt_release(sem, 2, &previous), 0); check("release_previous_zero", previous, 0);
    check("release_one", (DWORD)nt_release(sem, 1, &previous), 0); check("release_previous_two", previous, 2);
    previous = 99;
    check("maximum_violation", (DWORD)nt_release(sem, 1, &previous), SEMAPHORE_LIMIT_STATUS);
    check("maximum_failure_output", previous, 99);
    for (unsigned i = 0; i < 3; ++i) check("consume_count", WaitForSingleObject(sem, 0), WAIT_OBJECT_0);
    check("consumed_empty", WaitForSingleObject(sem, 0), WAIT_TIMEOUT);
    previous = 99;
    check("wrong_object_type", (DWORD)nt_set(sem, (LONG *)&previous), TYPE_MISMATCH_STATUS);
    check("wrong_type_output", previous, 99);
    for (unsigned i = 0; i < 8; ++i)
    {
        check("cycle_release", (DWORD)nt_release(sem, 1, &previous), 0);
        check("cycle_previous", previous, 0);
        check("cycle_consume", WaitForSingleObject(sem, 0), WAIT_OBJECT_0);
    }
    close_handle(sem);
}
static void access_checks(void)
{
    begin_case("independent_wait_modify_permissions");
    for (unsigned semaphore = 0; semaphore < 2; ++semaphore)
    {
        HANDLE wait_only = NULL, modify_only = NULL;
        LONG status = semaphore ? nt_create_sem(&wait_only, SYNCHRONIZE, NULL, 1, 2) :
                                  nt_create_event(&wait_only, SYNCHRONIZE, NULL, 1, TRUE);
        check("create_wait_only", (DWORD)status, 0);
        status = semaphore ? nt_create_sem(&modify_only, SEMAPHORE_MODIFY_STATE, NULL, 0, 2) :
                             nt_create_event(&modify_only, EVENT_MODIFY_STATE, NULL, 1, FALSE);
        check("create_modify_only", (DWORD)status, 0);
        if (valid("wait_only_handle", wait_only))
        {
            ULONG previous = 99;
            check("wait_permission", WaitForSingleObject(wait_only, 0), WAIT_OBJECT_0);
            status = semaphore ? nt_release(wait_only, 1, &previous) : nt_set(wait_only, (LONG *)&previous);
            check("modify_access_denied", (DWORD)status, ACCESS_DENIED_STATUS);
            check("access_failure_output", previous, 99);
            check("denied_modify_kept_empty", WaitForSingleObject(wait_only, 0), WAIT_TIMEOUT);
        }
        if (valid("modify_only_handle", modify_only))
        {
            ULONG previous = 99;
            status = semaphore ? nt_release(modify_only, 1, &previous) : nt_set(modify_only, (LONG *)&previous);
            check("modify_permission", (DWORD)status, 0); check("modify_previous", previous, 0);
            SetLastError(0);
            check("wait_access_denied", WaitForSingleObject(modify_only, 0), WAIT_FAILED);
            check("wait_access_error", GetLastError(), ERROR_ACCESS_DENIED);
        }
        close_handle(wait_only); close_handle(modify_only);
    }
}
static void query_pulse(void)
{
    begin_case("ordinary_query_and_pulse_transfer");
    HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL), sem = CreateSemaphoreW(NULL, 0, 3, NULL);
    struct { LONG type, state; } event_info = {99, 99};
    struct { LONG current, maximum; } sem_info = {99, 99};
    ULONG length = 0, previous;
    if (valid("query_event", event))
    {
        check("query_hot_set", (DWORD)nt_set(event, NULL), 0);
        check("query_event_status", (DWORD)nt_query_event(event, 0, &event_info, sizeof(event_info), &length), 0);
        check("query_manual_type", event_info.type, 0); check("query_signaled", event_info.state, 1);
        check("pulse_without_waiters", PulseEvent(event), TRUE);
        check("pulse_restores_empty", WaitForSingleObject(event, 0), WAIT_TIMEOUT);
    }
    if (valid("query_semaphore", sem))
    {
        check("query_hot_release", (DWORD)nt_release(sem, 2, &previous), 0);
        check("query_semaphore_status", (DWORD)nt_query_sem(sem, 0, &sem_info, sizeof(sem_info), &length), 0);
        check("query_current_two", sem_info.current, 2); check("query_maximum_three", sem_info.maximum, 3);
        check("query_after_consume", WaitForSingleObject(sem, 0), WAIT_OBJECT_0);
        check("query_semaphore_again", (DWORD)nt_query_sem(sem, 0, &sem_info, sizeof(sem_info), &length), 0);
        check("query_current_one", sem_info.current, 1);
    }
    close_handle(event); close_handle(sem);
}
static void multiwait(void)
{
    begin_case("ordinary_multiwait_partial_and_complete");
    HANDLE handles[2] = {CreateEventW(NULL, FALSE, TRUE, NULL), CreateSemaphoreW(NULL, 0, 1, NULL)};
    if (valid("multi_event", handles[0]) && valid("multi_semaphore", handles[1]))
    {
        check("warm_event_wait", WaitForSingleObject(handles[0], 0), WAIT_OBJECT_0);
        check("rearm_event", SetEvent(handles[0]), TRUE);
        check("partial_wait_all", WaitForMultipleObjects(2, handles, TRUE, 0), WAIT_TIMEOUT);
        check("partial_kept_event", WaitForSingleObject(handles[0], 0), WAIT_OBJECT_0);
        check("rearm_both_event", SetEvent(handles[0]), TRUE);
        check("rearm_both_semaphore", ReleaseSemaphore(handles[1], 1, NULL), TRUE);
        check("complete_wait_all", WaitForMultipleObjects(2, handles, TRUE, 0), WAIT_OBJECT_0);
        check("multi_event_consumed", WaitForSingleObject(handles[0], 0), WAIT_TIMEOUT);
        check("multi_semaphore_consumed", WaitForSingleObject(handles[1], 0), WAIT_TIMEOUT);
        check("wait_any_rearm", ReleaseSemaphore(handles[1], 1, NULL), TRUE);
        check("wait_any_index", WaitForMultipleObjects(2, handles, FALSE, 0), WAIT_OBJECT_0 + 1);
    }
    close_handle(handles[0]); close_handle(handles[1]);
}
static void signal_alertable(void)
{
    begin_case("ordinary_signal_wait_and_alertable");
    HANDLE target = CreateEventW(NULL, TRUE, TRUE, NULL), event = CreateEventW(NULL, FALSE, FALSE, NULL);
    HANDLE sem = CreateSemaphoreW(NULL, 0, 1, NULL);
    if (valid("signal_target", target) && valid("signal_event", event) && valid("signal_sem", sem))
    {
        check("signal_event_then_wait", SignalObjectAndWait(event, target, 0, FALSE), WAIT_OBJECT_0);
        check("signal_event_consumed", WaitForSingleObject(event, 0), WAIT_OBJECT_0);
        check("signal_semaphore_then_wait", SignalObjectAndWait(sem, target, 0, FALSE), WAIT_OBJECT_0);
        check("signal_semaphore_consumed", WaitForSingleObject(sem, 0), WAIT_OBJECT_0);
        check("alertable_rearm", SetEvent(event), TRUE);
        check("alertable_event_wait", WaitForSingleObjectEx(event, 0, TRUE), WAIT_OBJECT_0);
        check("alertable_event_consumed", WaitForSingleObject(event, 0), WAIT_TIMEOUT);
        check("alertable_sem_rearm", ReleaseSemaphore(sem, 1, NULL), TRUE);
        check("alertable_semaphore_wait", WaitForSingleObjectEx(sem, 0, TRUE), WAIT_OBJECT_0);
        check("alertable_semaphore_consumed", WaitForSingleObject(sem, 0), WAIT_TIMEOUT);
    }
    close_handle(target); close_handle(event); close_handle(sem);
}
static void named_inherited(void)
{
    begin_case("named_and_inheritable_objects");
    SECURITY_ATTRIBUTES attributes = {sizeof(attributes), NULL, TRUE};
    HANDLE event = CreateEventW(NULL, FALSE, FALSE, L"pw_sync_ordinary_event");
    HANDLE alias = OpenEventW(EVENT_ALL_ACCESS, FALSE, L"pw_sync_ordinary_event");
    HANDLE sem = CreateSemaphoreW(&attributes, 0, 1, NULL);
    if (valid("named_event", event) && valid("named_alias", alias))
    {
        check("named_set", SetEvent(event), TRUE); check("named_alias_wait", WaitForSingleObject(alias, 0), WAIT_OBJECT_0);
        check("named_shared_state", WaitForSingleObject(event, 0), WAIT_TIMEOUT);
    }
    if (valid("inheritable_semaphore", sem))
    {
        check("inheritable_release", ReleaseSemaphore(sem, 1, NULL), TRUE);
        check("inheritable_wait", WaitForSingleObject(sem, 0), WAIT_OBJECT_0);
    }
    close_handle(event); close_handle(alias); close_handle(sem);
}
static void aliases_reuse(void)
{
    begin_case("warm_alias_close_source_and_handle_reuse");
    HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL), alias = NULL;
    if (valid("alias_original", event))
    {
        check("alias_warm_set", SetEvent(event), TRUE); check("alias_warm_wait", WaitForSingleObject(event, 0), WAIT_OBJECT_0);
        check("create_alias", DuplicateHandle(GetCurrentProcess(), event, GetCurrentProcess(), &alias, 0, FALSE,
                                               DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE), TRUE);
        /* CLOSE_SOURCE ends the original handle's legal lifetime. */
        event = NULL;
        if (valid("alias_after_close_source", alias))
        {
            check("alias_set", SetEvent(alias), TRUE); check("alias_wait", WaitForSingleObject(alias, 0), WAIT_OBJECT_0);
            check("alias_consumed", WaitForSingleObject(alias, 0), WAIT_TIMEOUT);
        }
    }
    close_handle(event); close_handle(alias);
    for (unsigned i = 0; i < 8; ++i)
    {
        HANDLE next = i & 1 ? CreateSemaphoreW(NULL, 1, 1, NULL) : CreateEventW(NULL, FALSE, TRUE, NULL);
        if (valid("recreated_object", next)) check("recreated_wait", WaitForSingleObject(next, 0), WAIT_OBJECT_0);
        close_handle(next);
    }
}
struct handoff { HANDLE target, ready; };
static struct handoff handoffs[3][2];
static DWORD WINAPI waiting_worker(void *argument)
{
    struct handoff *handoff = argument;
    if (!SetEvent(handoff->ready)) return 10;
    return WaitForSingleObject(handoff->target, 5000) == WAIT_OBJECT_0 ? 0 : 11;
}
static void queued_handoff(unsigned kind)
{
    begin_case(kind == 0 ? "auto_event_two_waiters" : kind == 1 ? "manual_event_two_waiters" : "semaphore_two_waiters");
    HANDLE target = kind == 2 ? CreateSemaphoreW(NULL, 0, 2, NULL) : CreateEventW(NULL, kind == 1, FALSE, NULL);
    HANDLE threads[2] = {NULL, NULL}; BOOL joined = TRUE;
    if (!valid("queued_target", target)) return;
    /* Warm the same ordinary handle before queue insertion transfers authority. */
    check("queued_warm_signal", kind == 2 ? ReleaseSemaphore(target, 1, NULL) : SetEvent(target), TRUE);
    check("queued_warm_wait", WaitForSingleObject(target, 0), WAIT_OBJECT_0);
    if (kind == 1) check("queued_warm_reset", ResetEvent(target), TRUE);
    for (unsigned i = 0; i < 2; ++i)
    {
        handoffs[kind][i].target = target;
        handoffs[kind][i].ready = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (valid("queued_ready", handoffs[kind][i].ready))
            threads[i] = CreateThread(NULL, 0, waiting_worker, &handoffs[kind][i], 0, NULL);
    }
    if (valid("queued_thread_first", threads[0]) && valid("queued_thread_second", threads[1]))
    {
        for (unsigned i = 0; i < 2; ++i)
            check("worker_entered", WaitForSingleObject(handoffs[kind][i].ready, 5000), WAIT_OBJECT_0);
        Sleep(10); /* Allows scheduling; ready signals alone do not prove queue membership. */
        check("queued_signal_first", kind == 2 ? ReleaseSemaphore(target, 1, NULL) : SetEvent(target), TRUE);
        if (kind != 1)
        {
            DWORD first = WaitForMultipleObjects(2, threads, FALSE, 5000);
            check("one_worker_completed", first < WAIT_OBJECT_0 + 2, TRUE);
            if (first < WAIT_OBJECT_0 + 2)
                check("other_worker_still_waits", WaitForSingleObject(threads[1 - first], 0), WAIT_TIMEOUT);
            check("queued_signal_second", kind == 2 ? ReleaseSemaphore(target, 1, NULL) : SetEvent(target), TRUE);
        }
        DWORD wait = WaitForMultipleObjects(2, threads, TRUE, 10000);
        check("queued_workers_join", wait, WAIT_OBJECT_0); joined = wait == WAIT_OBJECT_0;
        if (joined)
            for (unsigned i = 0; i < 2; ++i)
            {
                DWORD code = 99;
                check("worker_exit_query", GetExitCodeThread(threads[i], &code), TRUE);
                check("worker_exit_success", code, 0);
            }
    }
    else
    {
        /* Normal signaling and bounded joins even when worker creation fails. */
        if (kind == 2) (void)ReleaseSemaphore(target, 2, NULL);
        else (void)SetEvent(target);
        for (unsigned i = 0; i < 2; ++i)
            if (threads[i] && WaitForSingleObject(threads[i], 10000) != WAIT_OBJECT_0) joined = FALSE;
    }
    if (joined)
    {
        close_handle(threads[0]); close_handle(threads[1]);
        close_handle(handoffs[kind][0].ready); close_handle(handoffs[kind][1].ready); close_handle(target);
    }
    /* Static storage and handles survive an unexpected failed join until normal process cleanup. */
}
void WINAPI mainCRTStartup(void)
{
    report_file = CreateFileW(L"pw-sync-ordinary.log", GENERIC_WRITE, FILE_SHARE_READ, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    HMODULE module = GetModuleHandleW(L"ntdll.dll");
    nt_set = (EventOp)(void *)GetProcAddress(module, "NtSetEvent");
    nt_reset = (EventOp)(void *)GetProcAddress(module, "NtResetEvent");
    nt_release = (ReleaseSem)(void *)GetProcAddress(module, "NtReleaseSemaphore");
    nt_create_event = (CreateEventNative)(void *)GetProcAddress(module, "NtCreateEvent");
    nt_create_sem = (CreateSemNative)(void *)GetProcAddress(module, "NtCreateSemaphore");
    nt_query_event = (QueryObject)(void *)GetProcAddress(module, "NtQueryEvent");
    nt_query_sem = (QueryObject)(void *)GetProcAddress(module, "NtQuerySemaphore");
    BOOL exports = nt_set && nt_reset && nt_release && nt_create_event && nt_create_sem && nt_query_event && nt_query_sem;
    check("native_exports", exports, TRUE);
    if (exports)
    {
        events(FALSE); events(TRUE); semaphores(); access_checks(); query_pulse(); multiwait();
        signal_alertable(); named_inherited(); aliases_reuse();
        queued_handoff(0); queued_handoff(1); queued_handoff(2);
    }
    check("case_count", cases, 12);
    struct line line = {{0}, 0};
    text(&line, "PW_SYNC_ORDINARY done cases="); hex(&line, cases);
    text(&line, " checks="); hex(&line, checks); text(&line, " failures="); hex(&line, failures);
    text(&line, failures ? " result=FAIL" : " result=PASS"); write_line(&line);
    if (report_file != INVALID_HANDLE_VALUE) (void)CloseHandle(report_file);
    ExitProcess(failures ? 1 : 0);
}
