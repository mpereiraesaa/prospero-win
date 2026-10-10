# Per-queue CPU Present-return interval counter

The portable accumulator receives QPC timestamps at successful vkQueuePresentKHR
returns. Its statistics describe CPU queue-return cadence, not display scanout,
GPU completion or frame latency. Callers keep separate state per queue and
serialize updates. Success includes VK_SUBOPTIMAL_KHR when the integration layer
chooses it. Failed presents, invalid frequency and backward timestamps re-prime
the next valid interval rather than counting a false loading/stall sample.

Intervals and maximum are reported in integer microseconds. Strict counts above
25, 33 and 50 milliseconds compare ticks before rounding. Totals saturate instead
of wrapping. Arithmetic avoids overflow for supported positive QPC frequencies;
implausibly large frequencies are rejected. The caller owns capacity/lifetime
management and diagnostic configuration.

This is the arithmetic component of shared PE32/PE64 cadence instrumentation,
extracted independently from the experimental replay branch. It introduces no
replay workers, admission changes, runtime hooks or logging itself. Integration
must capture timestamps immediately after the backend return and before taking
its diagnostic lock, then publish the same scope/format on both architectures.

The focused contract checks exact thresholds, fractions beyond thresholds,
separate queues, error/backward-clock re-prime, frequency rejection, arithmetic
saturation and saturating totals. Runtime logging and game route comparisons are
separate integration gates.
