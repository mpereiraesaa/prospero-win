/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include "../native/pw_lapy_elevation.h"
#include "../native/lapy_elevation_protocol.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define TEST_SOCKET 42
#define TEST_PID 1234

static uint8_t sent[8192];
static size_t sent_size;
static uint8_t received[256];
static size_t received_size;
static size_t received_at;
static int clone_calls;
static int clone_fails;
static int connect_calls;
static int option_calls;
static int close_calls;

int ps5log_ps5_socket(int domain, int type, int protocol)
{
    assert(domain == AF_INET && type == SOCK_STREAM && protocol == IPPROTO_TCP);
    return TEST_SOCKET;
}

int ps5log_ps5_connect(int socket, const struct sockaddr *address, socklen_t size)
{
    assert(socket == TEST_SOCKET && address != NULL && size == 16);
    connect_calls++;
    return 0;
}

long ps5log_ps5_send(int socket, const void *buffer, size_t size, int flags)
{
    assert(socket == TEST_SOCKET && flags == 0);
    if (size > 7) size = 7; /* force the client's send-all path */
    assert(sent_size + size <= sizeof(sent));
    memcpy(sent + sent_size, buffer, size);
    sent_size += size;
    return (long)size;
}

int ps5log_ps5_setsockopt(int socket, int level, int option,
                          const void *value, socklen_t size)
{
    assert(socket == TEST_SOCKET && level == 0xffff && value != NULL);
    assert(size == sizeof(uint32_t));
    assert(option == 0x1105 || option == 0x1106 || option == 0x1109);
    option_calls++;
    return 0;
}

int ps5log_ps5_close(int socket)
{
    assert(socket == TEST_SOCKET);
    close_calls++;
    return 0;
}

int sceNetRecv(int socket, void *data, size_t size, int flags)
{
    assert(socket == TEST_SOCKET && flags == 0);
    if (received_at == received_size) return 0;
    if (size > 5) size = 5; /* force the client's receive-all path */
    if (size > received_size - received_at) size = received_size - received_at;
    memcpy(data, received + received_at, size);
    received_at += size;
    return (int)size;
}

int *sceNetErrnoLoc(void)
{
    static int network_errno;
    return &network_errno;
}

int seteuid(uid_t uid)
{
    (void)uid;
    clone_calls++;
    if (clone_fails) { errno = EPERM; return -1; }
    return 0;
}

static uid_t test_geteuid(void)
{
    return 1000;
}

/* The target SDK supplies geteuid; keep the host test deterministic. */
uid_t geteuid(void)
{
    return test_geteuid();
}

static void queue_message(uint32_t kind, uint32_t status)
{
    struct lapy_elevation_message message = {
        LAPY_ELEVATION_MAGIC, LAPY_ELEVATION_VERSION,
        sizeof(struct lapy_elevation_message), kind,
        LAPY_ELEVATION_FILESYSTEM, TEST_PID, status
    };
    assert(received_size + sizeof(message) <= sizeof(received));
    memcpy(received + received_size, &message, sizeof(message));
    received_size += sizeof(message);
}

static void reset_case(void)
{
    sent_size = received_size = received_at = 0;
    clone_calls = clone_fails = connect_calls = option_calls = close_calls = 0;
    memset(sent, 0, sizeof(sent));
    memset(received, 0, sizeof(received));
}

static void assert_request_frame(void)
{
    const size_t elf_size = 7;
    assert(sent_size >= elf_size + sizeof(struct lapy_elevation_message));
    assert(memcmp(sent, "ELFTEST", elf_size) == 0);
    struct lapy_elevation_message request;
    memcpy(&request, sent + elf_size, sizeof(request));
    assert(request.magic == LAPY_ELEVATION_MAGIC);
    assert(request.version == LAPY_ELEVATION_VERSION);
    assert(request.size == sizeof(request));
    assert(request.kind == LAPY_ELEVATION_REQUEST);
    assert(request.capability == LAPY_ELEVATION_FILESYSTEM);
    assert(request.pid == TEST_PID && request.status == LAPY_ELEVATION_OK);
}

int main(void)
{
    FILE *helper = fopen(PW_LAPY_HELPER_PATH, "wb");
    assert(helper != NULL);
    assert(fwrite("ELFTEST", 1, 7, helper) == 7);
    assert(fclose(helper) == 0);

    /* Normal cooperative handshake: PREPARE -> native clone -> PREPARED -> OK. */
    reset_case();
    queue_message(LAPY_ELEVATION_PREPARE, LAPY_ELEVATION_OK);
    queue_message(LAPY_ELEVATION_RESPONSE, LAPY_ELEVATION_OK);
    assert(pw_lapy_elevation_request(TEST_PID) == 0);
    assert(clone_calls == 1 && connect_calls == 1 && option_calls == 3);
    assert(close_calls == 1);
    assert_request_frame();
    struct lapy_elevation_message prepared;
    memcpy(&prepared, sent + 7 + sizeof(prepared), sizeof(prepared));
    assert(prepared.kind == LAPY_ELEVATION_PREPARED);
    assert(prepared.status == LAPY_ELEVATION_OK);

    /* A successful terminal response without PREPARE must fail closed. */
    reset_case();
    queue_message(LAPY_ELEVATION_RESPONSE, LAPY_ELEVATION_OK);
    errno = 0;
    assert(pw_lapy_elevation_request(TEST_PID) == -1);
    assert(errno == EPROTO && clone_calls == 0 && close_calls == 1);
    assert_request_frame();

    /* Helper rejection before preparation must not clone credentials. */
    reset_case();
    queue_message(LAPY_ELEVATION_RESPONSE, LAPY_ELEVATION_TARGET_MISMATCH);
    errno = 0;
    assert(pw_lapy_elevation_request(TEST_PID) == -1);
    assert(errno == ESRCH && clone_calls == 0 && close_calls == 1);

    /* If native credential cloning fails, communicate PREPARE_FAILED and fail. */
    reset_case();
    clone_fails = 1;
    queue_message(LAPY_ELEVATION_PREPARE, LAPY_ELEVATION_OK);
    queue_message(LAPY_ELEVATION_RESPONSE, LAPY_ELEVATION_PREPARE_FAILED);
    errno = 0;
    assert(pw_lapy_elevation_request(TEST_PID) == -1);
    assert(errno == EPERM && clone_calls == 1 && close_calls == 1);
    memcpy(&prepared, sent + 7 + sizeof(prepared), sizeof(prepared));
    assert(prepared.kind == LAPY_ELEVATION_PREPARED);
    assert(prepared.status == LAPY_ELEVATION_PREPARE_FAILED);

    unlink(PW_LAPY_HELPER_PATH);
    puts("lapy elevation: fragmented I/O, cooperative handshake, and fail-closed replies passed");
    return 0;
}
