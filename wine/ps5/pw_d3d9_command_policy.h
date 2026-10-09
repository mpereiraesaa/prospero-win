/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_COMMAND_POLICY_H
#define PW_D3D9_COMMAND_POLICY_H
#include "pw_d3d9_command_wire.h"
/* Pinned DXVK5fde742b immediate-S_OK subset, for already owned command data.
 * Zero means synchronous fallback, not an API error. Does not authorize queue
 * admission: session health, ordering, device pins and negotiated policy still
 * require validation. No pointers are read and no command is modified. */
int pw_d3d9_command_can_queue(const struct pw_d3d9_command *);
#endif
