/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The payload SDK's libunwind finds unwind tables through these four
 * symbols. Each Wine PRX links its own libunwind, so each defines them,
 * hidden, over its own .eh_frame_hdr and .eh_frame; used after the
 * foundation's ps5-pie.ld as a second -T script. */
PROVIDE_HIDDEN(__eh_frame_hdr_start = ADDR(.eh_frame_hdr));
PROVIDE_HIDDEN(__eh_frame_hdr_end = ADDR(.eh_frame_hdr) + SIZEOF(.eh_frame_hdr));
PROVIDE_HIDDEN(__eh_frame_start = ADDR(.eh_frame));
PROVIDE_HIDDEN(__eh_frame_end = ADDR(.eh_frame) + SIZEOF(.eh_frame));
