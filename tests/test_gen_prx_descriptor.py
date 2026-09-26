#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""A generated PRX descriptor links, validates and starts its module once."""
from __future__ import annotations
import contextlib
import io
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import gen_prx_descriptor  # noqa: E402

# A module with one function, one variable and one constructor, checked
# through the same scan the PS5 loader performs on the module's segments.
MODULE = r"""
#define _GNU_SOURCE
#include "pw_wine_prx.h"
#include <assert.h>
#include <link.h>
#include <stdint.h>
#include <sys/mman.h>

int answer(void) { return 42; }
int counter_value = 7;
/* volatile: clang would otherwise fold the constructor into the initial value */
static volatile int constructed;
__attribute__((constructor)) static void construct(void) { constructed++; }
extern const char pw_prx_exports[];
int module_start(size_t argc, const void *argv);

static PwPrxSegment segments[PW_PRX_MAX_SEGMENTS];
static uint32_t segment_count;

static int collect(struct dl_phdr_info *info, size_t size, void *data)
{
    (void)size; (void)data;
    for (int i = 0; i < info->dlpi_phnum; i++)
    {
        const ElfW(Phdr) *phdr = &info->dlpi_phdr[i];
        if (phdr->p_type != PT_LOAD) continue;
        assert(segment_count < PW_PRX_MAX_SEGMENTS);
        /* PS5 module info reports page-aligned segments; so does this. */
        uintptr_t start = info->dlpi_addr + phdr->p_vaddr, page = start & ~(uintptr_t)4095;
        segments[segment_count++] = (PwPrxSegment){
            (const void *)page, (uint32_t)(phdr->p_memsz + (start - page)),
            (phdr->p_flags & PF_R ? PROT_READ : 0) | (phdr->p_flags & PF_W ? PROT_WRITE : 0) |
            (phdr->p_flags & PF_X ? PROT_EXEC : 0) };
    }
    return 1;  /* the executable comes first */
}

int main(void)
{
    const PwPrxDescriptor *table = (const PwPrxDescriptor *)(const void *)pw_prx_exports;
    assert(table->magic == PW_PRX_MAGIC && table->version == PW_PRX_VERSION && table->count == 3);
    assert(((uintptr_t)table & (PW_PRX_ALIGN - 1)) == 0);
    assert(pw_prx_lookup(table, "answer") == (const void *)answer);
    assert(pw_prx_lookup(table, "counter_value") == &counter_value);
    assert(pw_prx_lookup(table, "module_start") == (const void *)module_start);
    assert(!pw_prx_lookup(table, "module_stop"));

    const PwPrxDescriptor *found = NULL;
    dl_iterate_phdr(collect, NULL);
    assert(pw_prx_find_descriptor(segments, segment_count, &found) == PW_PRX_OK && found == table);

    /* The host C runtime already ran the constructor; on PS5 nothing does,
     * so module_start runs .init_array, and only on its first call. */
    assert(constructed == 1);
    assert(module_start(0, NULL) == 0 && constructed == 2);
    assert(module_start(0, NULL) == 0 && constructed == 2);
    return 0;
}
"""


def main() -> int:
    for bad in ([], ["1st"], ["has-dash"], ["module_start"], ["pw_prx_exports"], ["a", "a"]):
        try:
            gen_prx_descriptor.render(bad)
        except ValueError:
            continue
        raise AssertionError(f"accepted {bad}")
    with contextlib.redirect_stderr(io.StringIO()) as usage:
        assert gen_prx_descriptor.main(["gen", "out.c"]) == 2
    assert "Usage: gen_prx_descriptor.py OUTPUT.c NAME..." in usage.getvalue()
    script = ROOT / "tools/gen_prx_descriptor.py"
    with tempfile.TemporaryDirectory() as directory:
        work = Path(directory)
        result = subprocess.run([sys.executable, str(script), str(work / "bad.c"), "x-y"],
                                capture_output=True, text=True)
        assert result.returncode == 1 and "not a C identifier: x-y" in result.stderr
        assert not (work / "bad.c").exists()
        subprocess.run([sys.executable, str(script), str(work / "descriptor.c"),
                        "answer", "counter_value"], check=True)
        (work / "module.c").write_text(MODULE)
        binary = work / "module"
        cc = os.environ.get("CC", "cc")
        subprocess.run([cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        f"-I{ROOT / 'wine/ps5'}", str(work / "module.c"), str(work / "descriptor.c"),
                        str(ROOT / "wine/ps5/pw_wine_prx.c"), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print("gen_prx_descriptor passed: table validates in a linked image, module_start runs once")
    return 0


if __name__ == "__main__":
    sys.exit(main())
