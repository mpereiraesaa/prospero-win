/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_QUEUE_TICKET_H
#define PW_D3D9_QUEUE_TICKET_H
/* A private shell pin, never a public COM reference. Admission acquires it while
 * holding the session gate; completion detaches it under that gate and drops it
 * only after unlocking. Drop can retire the shell and release its parent. */
struct pw_d3d9_queue_ticket { void *context; void (*drop)(void *); };
static inline void pw_d3d9_queue_ticket_drop(struct pw_d3d9_queue_ticket *ticket)
{
    void *context=ticket->context;void (*drop)(void *)=ticket->drop;
    ticket->context=0;ticket->drop=0;if(drop)drop(context);
}
#endif
