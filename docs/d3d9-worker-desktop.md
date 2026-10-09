# Fresh worker desktop recognition

The PS5 server creates a desktop without a process-local `WND`. Wine recognizes
that handle using `user_thread_info.top_window` and `msg_window`. A fresh native
DXVK worker can reach `NtUserGetAncestor(GA_ROOT)` through Vulkan presentation
before any call has initialized those per-thread handles. The ancestor walk then
fails when it reaches the desktop. This also affects ordinary guest workers.

Patch 0910 queries **existing** desktop handles before the PS5 ancestor walk.
It uses `get_desktop_window(force=FALSE)` directly; it does not invoke the full
client initialization routine, create windows, install drivers, or register
classes. There is no loader/thread-attach hook, recursive `get_win_ptr` call, or
RPC while the ancestor walk holds a USER object lock. A failed query leaves the
cache empty and a later call retries. A `desktop_cache_only` marker is appended
to the Unix thread structure: recognizing handles must not make subsequent full
`get_desktop_window()` initialization return early. The full routine runs its
original driver/class-registration tail and clears the marker before callbacks,
preserving the existing reentrant fast path. Changing thread desktop clears both
handles and the marker. Other platforms retain the existing ancestor path.

The surface rectangle helper additionally returns empty virtual/monitor rectangles
when the root or its rectangle cannot be fetched. It cannot publish uninitialized
stack contents. This guard applies to all platforms. It does not fabricate a
successful ancestor or a valid surface size.

## Evidence and limits

The R4 console log showed native worker 004c reporting root zero and alternating
surface origins `(50331560,32)`, `(50841576,32)`, and `(50841704,32)`. All retained
1920×1080 extents. Parent 0048 reported the correct root. These are stored internal
rectangles, not unrelated trace arguments. PS5's fallback client-surface update
and present callbacks are no-ops; Vulkan validates actual HWND surface dimensions
separately. This defect does not establish a cause for game behavior or prove
that the actual display moved.

Run the focused source contract against a pinned Wine tree before 0910:

```sh
python3 tests/lab/d3d9_worker_desktop.py --wine-source /path/to/wine/source --output /fresh/contract
```

It compiles the exact ancestor walk, desktop recognition/cache, and rectangle
functions from Wine. Controlled server replies reproduce the failed old walk,
check cached and retry paths, and force ancestor/rectangle failures. Normal and
ASan/UBSan runs assert zero failure outputs and no subsequent mapping.

For actual PE32 and native PE64 worker execution, also pass `--wine-build` and
`--prefix`. Build an isolated host Wine overlay with `WINE_PS5_USER_DRIVER` for
`dce`, `driver`, `ps5drv`, `window`, `winstation`, and `vulkan`, then relink win32u.
The corrected overlay must additionally recompile `syscall.c`: its sole
`get_user_thread_info()` allocation uses the extended structure's `sizeof`.
Existing member offsets are unchanged. This is a private Unix structure, with no
PE32/PE64 wire mirror or separate native-domain allocation.
The runner disables explorer so the desktop has the same server-only ownership
as the console. A regular explorer-owned X11 desktop does not reproduce this
failure and is not a substitute. `--expect-worker-failure` records the old-runtime
negative; omit it for the corrected runtime. Each process creates a parent
window and then makes the child's first USER call `GetAncestor`, checks client
extent, repeats lookup, then creates, reads/writes and destroys an unsubclassed
STATIC on that worker before joining it and destroying the parent window.

Wine build scripts discover numerically named patches automatically, so no
builder manifest edit is needed. Runtime deployment needs the corrected win32u
Unix module; no PE wire protocol or D3D9 proxy/service change is required. This
patch is independent of surface-object lifetime fixes and is not in earlier
immutable console candidates.

The persistent 2026-10-09 evidence under `worker-desktop` records three baseline
processes with zero roots in both ABIs (`negative-r2`), and three corrected
processes with correct roots/extents (`positive-r1`). Normal and sanitizer source
contracts passed in the same runner. `source-closure.json` records both runtime
source trees and the runner metadata-only difference. An initial explorer-owned
negative attempt returned valid roots and was retained as a failed expectation.
No console execution is claimed for 0910.

Review found that the original recognition-only candidate could suppress later
full USER initialization. Its receipts remain historical evidence for root
recognition only. The corrected controlled regression explicitly starts with no
driver/class initialization, resolves the root, then enters full initialization
and verifies both callbacks once, including reentrant desktop lookup. It also
checks retry/reset state. Corrected runtime proof uses the STATIC follow-up.
