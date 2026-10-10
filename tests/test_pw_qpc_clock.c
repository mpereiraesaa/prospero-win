/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../wine/ps5/time/pw_qpc_clock.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    struct pw_qpc_anchor a;
    uint64_t out, prior, hz;
    assert(!pw_qpc_make_anchor(&a, 0, 0, 3200000000, 0));
    assert(!pw_qpc_make_anchor(&a, 0, 0, 0, 1));
    assert(!pw_qpc_make_anchor(&a, 0, UINT64_MAX, 3200000000, 1));
    for (hz = PW_QPC_MIN_TSC_HZ; hz <= 10000000000; hz = hz < 1000000000 ? hz * 10 : hz + 1000000000)
    {
        unsigned i;
        assert(pw_qpc_make_anchor(&a, 100, 12345, hz, 1));
        prior = a.counter;
        for (i = 0; i <= 1000; ++i)
        {
            uint64_t delta = hz * i / 1000;
            uint64_t reference = a.counter + delta * PW_QPC_FREQUENCY / hz;
            assert(pw_qpc_read_anchor(&a, a.tsc + delta, &out));
            assert(out >= prior && out <= reference && reference - out <= 3);
            prior = out;
        }
        assert(!pw_qpc_read_anchor(&a, a.tsc - 1, &out));
        assert(!pw_qpc_read_anchor(&a, a.tsc + hz + 1, &out));
    }
    assert(pw_qpc_make_anchor(&a, 100, INT64_MAX - 1, 3200000000, 1));
    assert(!pw_qpc_read_anchor(&a, a.tsc + 3200000000, &out));
    a.version++;
    assert(!pw_qpc_read_anchor(&a, a.tsc, &out));
    assert(pw_qpc_make_anchor(&a, 100, 0, 3200000000, 1));
    a.multiplier = UINT64_MAX;
    assert(!pw_qpc_read_anchor(&a, a.tsc + 2, &out));
    puts("QPC anchor: bounded arithmetic, drift rounding, stale/backward and ABI checks passed");
    return 0;
}
