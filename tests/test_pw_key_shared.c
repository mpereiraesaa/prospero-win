/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE
#include "../wine/ps5/input/pw_key_shared.h"
#include <assert.h>
#include <sys/mman.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/syscall.h>

struct input { uint64_t seq, id; uint8_t keys[256]; uint32_t locked, padding; uint64_t serial; };
struct desktop { uint64_t seq, id, serial; };
/* Fault a later field read after the initial sequence/ID reads. The handler
 * changes the actual object, then lets the same unmodified reader continue. */
static void *fault_page;
static uint64_t *mutated_seq, *mutated_id;
static volatile sig_atomic_t faults;
static void mutate_during_read(int signal, siginfo_t *info, void *context)
{
    (void)context;
    if (signal != SIGSEGV || (uintptr_t)info->si_addr < (uintptr_t)fault_page ||
        (uintptr_t)info->si_addr >= (uintptr_t)fault_page + 4096) _exit(2);
    __atomic_store_n(mutated_seq, 1, __ATOMIC_RELEASE);
    if (mutated_id) __atomic_fetch_add(mutated_id, 1, __ATOMIC_RELEASE);
    if (syscall(SYS_mprotect, fault_page, 4096, PROT_READ | PROT_WRITE)) _exit(3);
    ++faults;
}
static void test_mid_read_mutation(void)
{
    char *memory = mmap(NULL, 16384, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    struct input *i;
    struct desktop *s;
    struct pw_key_shared d = {0};
    struct sigaction action = {0}, previous;
    int16_t result;
    unsigned mode;
    assert(memory != MAP_FAILED && (uintptr_t)memory <= UINT32_MAX - 16384);
    i = (struct input *)(memory + 4096 - 64);
    s = (struct desktop *)(memory + 12288 - 16);
    d.version = PW_KEY_VERSION;
    d.input_object = (uintptr_t)i; d.desktop_object = (uintptr_t)s;
    d.input_id = 31; d.desktop_id = 41;
    d.state = (uintptr_t)i->keys; d.lock = (uintptr_t)&i->locked;
    d.input_serial = (uintptr_t)&i->serial; d.desktop_serial = (uintptr_t)&s->serial;
    action.sa_sigaction = mutate_during_read;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    assert(!sigaction(SIGSEGV, &action, &previous));
    for (mode = 0; mode < 3; ++mode)
    {
        i->seq = s->seq = 0; i->id = 31; s->id = 41;
        i->serial = s->serial = 71; i->keys[65] = 0x80;
        fault_page = memory + (mode == 2 ? 12288 : 4096);
        mutated_seq = mode == 2 ? &s->seq : &i->seq;
        mutated_id = mode == 1 ? &i->id : NULL;
        faults = 0;
        assert(!mprotect(fault_page, 4096, PROT_NONE));
        assert(!pw_key_get_state(&d, 65, &result));
        assert(faults == 1);
    }
    assert(!sigaction(SIGSEGV, &previous, NULL));
    assert(!munmap(memory, 16384));
}
int main(void)
{
    struct input *i = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    struct desktop *s;
    struct pw_key_shared d;
    int16_t value;
    assert(i != MAP_FAILED && (uintptr_t)i <= UINT32_MAX - 4096);
    s = (struct desktop *)((char *)i + 1024);
    memset(&d, 0, sizeof(d));
    d.version = PW_KEY_VERSION;
    d.input_object = (uintptr_t)i; d.desktop_object = (uintptr_t)s;
    d.input_id = i->id = 31; d.desktop_id = s->id = 41;
    d.state = (uintptr_t)i->keys; d.lock = (uintptr_t)&i->locked;
    d.input_serial = (uintptr_t)&i->serial; d.desktop_serial = (uintptr_t)&s->serial;
    i->serial = s->serial = 71;
    i->keys[65] = 0x80;
    assert(pw_key_get_state(&d, 65, &value) && value == -128);
    i->keys[65] = 0x7f;
    assert(pw_key_get_state(&d, 65 + 256, &value) && value == 1);
    assert(pw_key_get_state(&d, 65 - 256, &value) && value == 1);
    s->serial++;
    assert(!pw_key_get_state(&d, 65, &value));
    i->locked = 1;
    assert(pw_key_get_state(&d, 65, &value));
    i->seq = 1;
    assert(!pw_key_get_state(&d, 65, &value));
    i->seq = 2; i->id++;
    assert(!pw_key_get_state(&d, 65, &value));
    i->id--; i->locked = 0; s->serial = i->serial; s->id++;
    assert(!pw_key_get_state(&d, 65, &value));
    s->id--; s->seq = 1;
    assert(!pw_key_get_state(&d, 65, &value));
    s->seq = 2; d.reserved = 1;
    assert(!pw_key_get_state(&d, 65, &value));
    d.reserved = 0; d.state = UINT32_MAX;
    assert(!pw_key_get_state(&d, 65, &value));
    munmap(i, 4096);
    test_mid_read_mutation();
    puts("Shared keys: sign/toggle/wrap, lock/serial, object reuse and bounded seqlock tests passed");
    return 0;
}
