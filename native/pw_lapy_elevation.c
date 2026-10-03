/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include "pw_lapy_elevation.h"
#include "lapy_elevation_protocol.h"
#include "ps5log/ps5log_ps5_net.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef PW_LAPY_HELPER_PATH
#define PW_LAPY_HELPER_PATH "/app0/lapy.elf"
#endif
#define PW_LAPY_PORT 9021
#define PW_LAPY_TIMEOUT_US 5000000
#define PW_LAPY_MAX_ELF_SIZE (4u * 1024u * 1024u)

struct pw_lapy_sockaddr_in {
    uint8_t length;
    uint8_t family;
    uint16_t port;
    uint32_t address;
    uint16_t virtual_port;
    uint8_t zero[6];
};

extern int sceNetRecv(int socket, void *data, size_t length, int flags);
extern int *sceNetErrnoLoc(void);

static int network_result(int result)
{
    if (result < 0) {
        int *network_errno = sceNetErrnoLoc();
        errno = network_errno ? *network_errno : EIO;
        return -1;
    }
    return result;
}

static int send_all(int socket, const void *data, size_t size)
{
    const uint8_t *bytes = data;
    while (size) {
        long count = ps5log_ps5_send(socket, bytes, size, 0);
        if (count < 0) return -1;
        if (count == 0) { errno = EPIPE; return -1; }
        if ((size_t)count > size) { errno = EPROTO; return -1; }
        bytes += count;
        size -= (size_t)count;
    }
    return 0;
}

static int receive_all(int socket, void *data, size_t size)
{
    uint8_t *bytes = data;
    while (size) {
        int count = network_result(sceNetRecv(socket, bytes, size, 0));
        if (count < 0) return -1;
        if (count == 0) { errno = ECONNRESET; return -1; }
        if ((size_t)count > size) { errno = EPROTO; return -1; }
        bytes += count;
        size -= (size_t)count;
    }
    return 0;
}

static int message_matches(const struct lapy_elevation_message *message,
                           const struct lapy_elevation_message *request,
                           uint32_t kind)
{
    return message->magic == LAPY_ELEVATION_MAGIC &&
           message->version == LAPY_ELEVATION_VERSION &&
           message->size == sizeof(*message) && message->kind == kind &&
           message->capability == request->capability &&
           message->pid == request->pid;
}

static int status_errno(uint32_t status)
{
    switch (status) {
    case LAPY_ELEVATION_TARGET_MISMATCH: return ESRCH;
    case LAPY_ELEVATION_UNAVAILABLE: return ENOENT;
    case LAPY_ELEVATION_PREPARE_FAILED: return EPERM;
    case LAPY_ELEVATION_TRANSPORT_ERROR: return ETIMEDOUT;
    case LAPY_ELEVATION_OK: return 0;
    default: return EACCES;
    }
}

static int exchange(int socket, struct lapy_elevation_message *request)
{
    struct lapy_elevation_message message;
    if (send_all(socket, request, sizeof(*request)) ||
        receive_all(socket, &message, sizeof(message))) {
        errno = errno ? errno : EIO;
        return -1;
    }
    if (message_matches(&message, request, LAPY_ELEVATION_RESPONSE)) {
        /* A successful response is only valid after PREPARE/PREPARED. Without
         * that handshake the target credential was never cloned natively. */
        if (message.status == LAPY_ELEVATION_OK) {
            errno = EPROTO;
            return -1;
        }
        errno = status_errno(message.status);
        return -1;
    }
    if (!message_matches(&message, request, LAPY_ELEVATION_PREPARE)) {
        errno = EPROTO;
        return -1;
    }
    if (message.status != LAPY_ELEVATION_OK) {
        errno = status_errno(message.status);
        return -1;
    }

    /* This is the cooperative point: the title clones its credentials only
     * after the helper has validated the requested PID and title. */
    struct lapy_elevation_message prepared = *request;
    prepared.kind = LAPY_ELEVATION_PREPARED;
    if (seteuid(geteuid()) != 0) prepared.status = LAPY_ELEVATION_PREPARE_FAILED;
    if (send_all(socket, &prepared, sizeof(prepared)) ||
        receive_all(socket, &message, sizeof(message))) {
        errno = errno ? errno : EIO;
        return -1;
    }
    if (!message_matches(&message, request, LAPY_ELEVATION_RESPONSE)) {
        errno = EPROTO;
        return -1;
    }
    errno = status_errno(message.status);
    return message.status == LAPY_ELEVATION_OK ? 0 : -1;
}

int pw_lapy_elevation_request(int32_t pid)
{
    struct stat st;
    struct pw_lapy_sockaddr_in address;
    struct lapy_elevation_message request = {
        LAPY_ELEVATION_MAGIC, LAPY_ELEVATION_VERSION,
        sizeof(struct lapy_elevation_message), LAPY_ELEVATION_REQUEST,
        LAPY_ELEVATION_FILESYSTEM, (uint32_t)pid, LAPY_ELEVATION_OK
    };
    uint32_t timeout = PW_LAPY_TIMEOUT_US;
    uint8_t buffer[4096];
    int helper = -1, socket = -1, result = -1, saved_errno = EIO;

    if (pid <= 1) { errno = EINVAL; return -1; }
    helper = open(PW_LAPY_HELPER_PATH, O_RDONLY);
    if (helper < 0) return -1;
    if (fstat(helper, &st)) {
        saved_errno = errno ? errno : EIO;
        goto done;
    }
    if (!S_ISREG(st.st_mode) || st.st_size <= 0) {
        saved_errno = EINVAL;
        goto done;
    }
    if ((uint64_t)st.st_size > PW_LAPY_MAX_ELF_SIZE) {
        saved_errno = EFBIG;
        goto done;
    }
    socket = ps5log_ps5_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket < 0) { saved_errno = errno ? errno : EIO; goto done; }
    const int options[] = {0x1105, 0x1106, 0x1109};
    for (size_t i = 0; i < sizeof(options) / sizeof(options[0]); ++i) {
        if (ps5log_ps5_setsockopt(socket, 0xffff, options[i], &timeout,
                                  sizeof(timeout)) < 0) {
            saved_errno = errno ? errno : EIO;
            goto done;
        }
    }
    memset(&address, 0, sizeof(address));
    address.length = sizeof(address);
    address.family = AF_INET;
    address.port = (uint16_t)((PW_LAPY_PORT << 8) | (PW_LAPY_PORT >> 8));
    address.address = UINT32_C(0x0100007f);
    if (ps5log_ps5_connect(socket, (const struct sockaddr *)&address,
                           sizeof(address)) < 0) {
        saved_errno = errno ? errno : EIO;
        goto done;
    }

    for (;;) {
        ssize_t count = read(helper, buffer, sizeof(buffer));
        if (count == 0) break;
        if (count < 0) { saved_errno = errno ? errno : EIO; goto done; }
        if (send_all(socket, buffer, (size_t)count)) {
            saved_errno = errno ? errno : EIO;
            goto done;
        }
    }
    if (exchange(socket, &request) == 0) result = 0;
    else saved_errno = errno ? errno : EIO;

done:
    if (socket >= 0) (void)ps5log_ps5_close(socket);
    if (helper >= 0) (void)close(helper);
    if (result) errno = saved_errno;
    return result;
}
