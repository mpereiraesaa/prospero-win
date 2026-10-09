/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include "pw_d3d9_failure_wait.h"
#include "pw_d3d9_bridge_wire.h"
#include "pw_d3d9_command_batch.h"
struct fixture {
    void *memory; size_t bytes;
    struct pw_d3d9_channel client, service;
    HANDLE ready, go, published, cancel;
    HWND window; unsigned dispatched, quarantined, retired;
    int legacy;
};
static LRESULT CALLBACK proc(HWND w, UINT message, WPARAM a, LPARAM b)
{ return message == WM_APP ? 73 : DefWindowProcW(w, message, a, b); }
static DWORD WINAPI service(void *parameter)
{
    struct fixture *f = parameter; unsigned char scratch[8192], output[32]; struct pw_d3d9_message m;
    assert(!pw_d3d9_channel_receive(&f->service, &m, scratch, sizeof(scratch)));
    assert(!pw_d3d9_channel_ready(&f->service)); m.sequence = 0;
    assert(!pw_d3d9_channel_send(&f->service, &m, NULL));
    WNDCLASSW cls = {.lpfnWndProc = proc, .hInstance = GetModuleHandleW(NULL), .lpszClassName = L"PW_FAILURE_ACK"};
    RegisterClassW(&cls);
    f->window = CreateWindowW(cls.lpszClassName, L"", 0, 0, 0, 1, 1, NULL, NULL, cls.hInstance, NULL);
    assert(f->window); SetEvent(f->ready);
    assert(WaitForSingleObject(f->go, 5000) == WAIT_OBJECT_0);
    assert(!pw_d3d9_channel_receive(&f->service, &m, scratch, sizeof(scratch))); f->dispatched++;
    struct pw_d3d9_batch_reply reply = {1, 2, 1, 0, 0x8876086cu};
    assert(!pw_d3d9_batch_reply_encode(output, sizeof(output), &reply));
    m.sequence = 0; m.result = (int32_t)reply.hresult; m.payload_bytes = sizeof(output);
    assert(!pw_d3d9_channel_send(&f->service, &m, output));
    if (f->legacy) {
        /* Exact old ordering: next receive, then failed_result guard, then
         * shared cancellation. The already queued ACK becomes unreadable. */
        assert(!pw_d3d9_channel_receive(&f->service, &m, scratch, sizeof(scratch)));
        pw_d3d9_channel_cancel(&f->service, 1); SetEvent(f->published);
    } else {
        SetEvent(f->published);
        assert(pw_d3d9_failure_wait(f->cancel) == WAIT_OBJECT_0);
    }
    DestroyWindow(f->window); return 0;
}
static void cycle(int legacy, int lost_ack)
{
    struct fixture f = {0}; f.legacy = legacy; f.quarantined = 2;
    f.bytes = pw_d3d9_channel_bytes(8192, 8192);
    f.memory = VirtualAlloc(NULL, f.bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); assert(f.memory);
    f.ready = CreateEventW(NULL, TRUE, FALSE, NULL); f.go = CreateEventW(NULL, TRUE, FALSE, NULL);
    f.published = CreateEventW(NULL, TRUE, FALSE, NULL); f.cancel = CreateEventW(NULL, TRUE, FALSE, NULL);
    assert(f.ready && f.go && f.published && f.cancel);
    assert(!pw_d3d9_channel_init(f.memory, f.bytes, 7, 8192, 8192));
    assert(!pw_d3d9_channel_open(&f.client, f.memory, f.bytes, 7, PW_D3D9_CLIENT));
    assert(!pw_d3d9_channel_open(&f.service, f.memory, f.bytes, 7, PW_D3D9_SERVICE));
    struct pw_d3d9_message m = {.opcode = PW_D3D9_HELLO};
    assert(!pw_d3d9_channel_send(&f.client, &m, NULL));
    HANDLE thread = CreateThread(NULL, 0, service, &f, 0, NULL); assert(thread);
    assert(WaitForSingleObject(f.ready, 5000) == WAIT_OBJECT_0);
    unsigned char scratch[8192]; assert(!pw_d3d9_channel_receive(&f.client, &m, scratch, sizeof(scratch)));
    struct pw_d3d9_command_batch batch; struct pw_d3d9_command command = {.method = 57, .args = {7, 1}};
    unsigned char payload[PW_D3D9_BATCH_MAX]; size_t bytes;
    assert(!pw_d3d9_batch_init(&batch, 1)); assert(!pw_d3d9_batch_append(&batch, &command));
    assert(!pw_d3d9_batch_append(&batch, &command)); assert(!pw_d3d9_batch_encode(payload, sizeof(payload), &bytes, &batch));
    m = (struct pw_d3d9_message){.opcode = PW_D3D9_COMMAND_BATCH_CALL, .device = 1, .object = 2, .generation = 1, .payload_bytes = (uint32_t)bytes};
    assert(!pw_d3d9_channel_send(&f.client, &m, payload));
    assert(!pw_d3d9_channel_send(&f.client, &m, payload)); /* both queued before any ACK read */
    SetEvent(f.go); assert(WaitForSingleObject(f.published, 5000) == WAIT_OBJECT_0);
    Sleep(30); /* deliberately delay ACK consumption */
    assert(f.dispatched == 1 && f.retired == 0);
    if (legacy) {
        assert(pw_d3d9_channel_receive(&f.client, &m, scratch, sizeof(scratch)) == PW_D3D9_CLOSED);
        assert(f.service.next_receive == 4);
    } else {
        assert(!pw_d3d9_channel_error(&f.client) && f.service.next_receive == 3);
        DWORD_PTR result = 0;
        assert(SendMessageTimeoutW(f.window, WM_APP, 0, 0, SMTO_ABORTIFHUNG, 1000, &result) && result == 73);
        assert(WaitForSingleObject(thread, 0) == WAIT_TIMEOUT);
        if (!lost_ack) {
            assert(!pw_d3d9_channel_receive(&f.client, &m, scratch, sizeof(scratch)));
            struct pw_d3d9_batch_reply reply;
            assert(!pw_d3d9_batch_reply_decode(&reply, scratch + 64, m.payload_bytes, 1, 2));
            assert(reply.hresult == 0x8876086cu && reply.attempted == 1 && reply.failed_index == 0);
            f.quarantined--; f.retired++; /* ACK authorizes first retirement only */
        }
    }
    pw_d3d9_channel_cancel(&f.client, 1); SetEvent(f.cancel);
    assert(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
    DWORD code; assert(GetExitCodeThread(thread, &code) && code == 0);
    f.retired += f.quarantined; f.quarantined = 0; assert(f.retired == 2);
    CloseHandle(thread); CloseHandle(f.ready); CloseHandle(f.go); CloseHandle(f.published); CloseHandle(f.cancel);
    VirtualFree(f.memory, 0, MEM_RELEASE);
}
int main(void)
{
    cycle(1, 0); cycle(0, 0); cycle(0, 1);
    puts("failure ACK legacy-negative/fixed/dropped-ACK PASS"); return 0;
}
