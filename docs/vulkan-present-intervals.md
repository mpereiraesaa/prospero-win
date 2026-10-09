# Vulkan present return intervals

With `PW_VK_BATCH_STATS=1`, the PE32 intercept emits one
`PW_VK_PRESENT_INTERVAL version=1` line immediately after each raw
vkQueuePresentKHR return. The QPC timestamp is captured before acquiring the
small diagnostics lock. This applies with batching disabled, synchronous replay,
and asynchronous replay. No driver call occurs under the diagnostics lock.

Each of up to eight distinct queues has an independent accumulator. Records
include raw tick/frequency, interval microseconds, cumulative interval count,
maximum interval, and strict counts over25,33,50ms. Raw ticks retain sub-microsecond
precision; displayed microseconds are rounded down. First returns, returns after
errors, and a backwards clock sample re-prime timing and produce valid=0. Failed
presents are logged but not counted. SUBOPTIMAL_KHR remains a successful present.
A full queue table emits an explicit dropped record; destroying any device
resets all queue accumulators so reused handles cannot inherit old timing.

These measure CPU present-return cadence, not display scanout, individual image
completion, or GPU frame time. One present can contain multiple swapchains.
Intervals include application pacing, queue waits and diagnostic overhead.
Compare identical diagnostics settings and moving-route windows; exclude load
screens explicitly by timestamp. Use per-record raw ticks to compute window
percentiles and threshold counts, since cumulative maxima include loading.
Existing per-present batch logging plus the extra record can itself perturb
cadence; confirm perceived stutter with the HUD and a diagnostics-off route.

The standalone accumulator fixture covers exact threshold boundaries,
sub-microsecond threshold crossings, independent queues, invalid clocks,
re-priming after failed calls, saturation and long intervals. It participates in
`make all` and `make sanitize`. Actual PE compilation and console evidence are
required before the recorder is considered ready for deployment.
