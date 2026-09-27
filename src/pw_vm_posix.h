/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * POSIX memory backend: anonymous mappings, protection through mprotect and
 * a single alias for writes and execution.
 *
 * It is the host-test backend and also the plain path on the console for
 * images that are only mapped and inspected; FW 12.02 allows the read-write
 * to read-execute change it makes (prospero_win_vm.h).
 */
#ifndef PROSPERO_WIN_VM_POSIX_H
#define PROSPERO_WIN_VM_POSIX_H

#include "../include/prospero_win_vm.h"

int pw_vm_posix_backend(PwVmBackend *backend);

#endif
