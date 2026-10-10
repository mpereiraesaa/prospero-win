#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""A generated PRX descriptor links, validates and starts its module once."""
from __future__ import annotations
import contextlib
import io
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
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
#include <stdlib.h>
#include <sys/mman.h>

int answer(void) { return 42; }
int counter_value = 7;
#ifdef EXPECT_NATIVE_CAPS
unsigned __wine_prospero_native_wow64_caps(unsigned version) { return version == 1 ? 7 : 0; }
#endif
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
    assert(table->magic == PW_PRX_MAGIC && table->version == PW_PRX_VERSION);
#ifdef EXPECT_NATIVE_CAPS
    assert(table->count == 4);
    unsigned (*caps)(unsigned) = (void *)pw_prx_lookup(table, "__wine_prospero_native_wow64_caps");
    assert(caps == __wine_prospero_native_wow64_caps && caps(1) == 7 && caps(0) == 0);
#else
    assert(table->count == 3);
    assert(!pw_prx_lookup(table, "__wine_prospero_native_wow64_caps"));
#endif
    assert(((uintptr_t)table & (PW_PRX_ALIGN - 1)) == 0);
    assert(pw_prx_lookup(table, "answer") == (const void *)answer);
    assert(pw_prx_lookup(table, "counter_value") == &counter_value);
    assert(pw_prx_lookup(table, "module_start") == (const void *)module_start);
    assert(!pw_prx_lookup(table, "module_stop"));

    const PwPrxDescriptor *found = NULL;
    dl_iterate_phdr(collect, NULL);
    assert(pw_prx_find_descriptor(segments, segment_count, &found) == PW_PRX_OK && found == table);

#ifdef NO_RUNTIME_INIT
    /* Nothing ran .init_array: module_start runs it, once. */
    assert(constructed == 0);
#else
    /* The C runtime already ran .init_array: module_start does not repeat it. */
    assert(constructed == 1);
#endif
    assert(module_start(0, NULL) == 0 && constructed == 1);
    assert(module_start(0, NULL) == 0 && constructed == 1);
    return 0;
}

#ifdef NO_RUNTIME_INIT
/* Linked without start files: the dynamic loader initialises the C library
 * but nothing runs this executable's .init_array, as on PS5. */
__attribute__((noreturn)) void test_entry(void) { exit(main()); }
__asm__(".globl _start\n_start:\n\txor %ebp, %ebp\n\tand $-16, %rsp\n\tcall test_entry\n\thlt\n");
#endif
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
        cc = os.environ.get("CC", "cc")
        for variant, flags in (("runtime-init", []), ("no-runtime-init", ["-DNO_RUNTIME_INIT", "-nostartfiles"])):
            binary = work / variant
            subprocess.run([cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", *flags,
                            f"-I{ROOT / 'wine/ps5'}", str(work / "module.c"), str(work / "descriptor.c"),
                            str(ROOT / "wine/ps5/pw_wine_prx.c"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
        # Test the real dynamic symbol table and linked descriptor lookup:
        # a defined query is callable; older, hidden and undefined queries
        # never create a dangling PRX export or authorize native execution.
        query = "__wine_prospero_native_wow64_caps"
        inputs = {
            "present": f"unsigned {query}(unsigned v) {{ return v == 1 ? 7 : 0; }}",
            "absent": "unsigned older_runtime(void) { return 0; }",
            "hidden": f'__attribute__((visibility("hidden"))) unsigned {query}(unsigned v) {{ return v; }}',
            "undefined": f"extern unsigned {query}(unsigned); unsigned reference(void) {{ return {query}(1); }}",
        }
        # Exercise the ntdll descriptor command used by the actual builder,
        # with real ELF inputs and nm, without rebuilding all of Wine.
        build = work / "wine-build"
        (build / "dlls/ntdll").mkdir(parents=True)
        prx = work / "prx"
        (prx / "obj").mkdir(parents=True)
        sdk = work / "sdk"
        (sdk / "bin").mkdir(parents=True)
        (sdk / "bin/prospero-nm").symlink_to(shutil.which("nm"))
        builder = (ROOT / "tools/build_wine_ps5.sh").read_text()
        begin = builder.index('    python3 "$root/tools/gen_prx_descriptor.py" "$prx/obj/ntdll_desc.c"')
        end = builder.index('\n    python3 ', begin + 1)
        builder_command = builder[begin:end]
        builder_env = dict(os.environ, root=str(ROOT), build=str(build), prx=str(prx), sdk=str(sdk))
        for kind, source in inputs.items():
            source_path, elf = work / f"{kind}.c", work / f"{kind}.so"
            source_path.write_text(source)
            subprocess.run([cc, "-shared", "-fPIC", str(source_path), "-o", str(elf)], check=True)
            descriptor = work / f"{kind}-descriptor.c"
            subprocess.run([sys.executable, str(script), str(descriptor), "answer", "counter_value",
                            "--optional-from", str(elf), "--optional-export", query], check=True)
            binary = work / f"{kind}-lookup"
            flags = ["-DEXPECT_NATIVE_CAPS"] if kind == "present" else []
            subprocess.run([cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", *flags,
                            f"-I{ROOT / 'wine/ps5'}", str(work / "module.c"), str(descriptor),
                            str(ROOT / "wine/ps5/pw_wine_prx.c"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
            shutil.copyfile(elf, build / "dlls/ntdll/ntdll.so")
            subprocess.run(["sh", "-c", builder_command], env=builder_env, check=True)
            published = (prx / "obj/ntdll_desc.c").read_text()
            assert (f'{{ "{query}",' in published) == (kind == "present")
        for flags, message in ((["--optional-export", query], "require --optional-from"),
                               (["--optional-from", str(work / "missing.so"), "--optional-export", query],
                                "cannot inspect optional exports"),
                               (["--optional-from", str(work / "present.so"), "--nm", str(work / "missing-nm"),
                                 "--optional-export", query], "cannot inspect optional exports")):
            output = work / "failure.c"
            result = subprocess.run([sys.executable, str(script), str(output), "answer", *flags],
                                    capture_output=True, text=True)
            assert result.returncode == 1 and message in result.stderr, result.stderr
            assert not output.exists()
    print("gen_prx_descriptor passed: linked lookup, constructor lifetime and builder optional "
          "exports for present/absent/hidden/undefined queries")
    return 0


if __name__ == "__main__":
    sys.exit(main())
