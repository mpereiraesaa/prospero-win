# Image-view file descriptor lifetime

Status: experimental. Patch 0880 selects 0870's image-view cleanup by default
alongside the shared mutex backend, following console acceptance of the
combined runtime pair. Native/sanitizer checks, complete SDK builds and
ordinary console comparisons pass. Broader section-lifetime and debugger
behavior still needs real Wine coverage.

The 600-second fixed-route console capture records 68 image descriptor
releases, 22/22 replay steps and no recorded file-limit error. The regular
file descriptor census stays at 343 in the matched gameplay window, versus
402–405 in earlier diagnostic captures. Descriptor counts do not establish
exact kernel file-object occupancy. This run ends by the route runner's
ordinary `close-timeout`, not Wine-exit.

HL2 timedemo results reported by the console owner are 59.41 FPS with the
option off and 59.37 FPS with it on, both with Wine-exit. An immediate
same-console default-module control records 59.38 FPS with Wine-exit;
the lower absolute result than earlier ~59.8 runs affects all three arms.
The module comparison does not show a timing regression. These measurements
use the accepted NTDLL and the image-FD server candidate; they are not a
shared-mutex performance comparison or evidence of the 58/50 FPS goal.

The later combined default-on pair passes the ordinary image-section PE
fixture on both reference and candidate modules: six cases and 147 checks,
with Wine-exit. The fixture checks later views from a live section, retained
views after section closure, independent section/view lifetimes, file sharing
until the final view closes, and unchanged data/anonymous mappings. The
combined pair also passes the mutex fixture, HL2, load and 600-second city
checks reported by the console owner. That city capture averages 53.1
approximate FPS with a 45.4 sampled minimum in the 200–440-second window,
reports no file-limit error and retains the 94.7-second load proxy. It includes
other runtime and translator changes, so it does not isolate this option's
performance. See [the combined acceptance receipt](SHARED_MUTEX_BACKEND.md).

Wine retains an FD object for each nonremovable image view. That object
also carries file sharing restrictions, its inode, names and the reserved
image address. Keeping this metadata is necessary after section handles
close, even when a completed private image mapping no longer needs the
native descriptor.

Patch 0870 releases that descriptor at image mapping-object destruction
only when all remaining FD object references are completed, unshared
`SEC_IMAGE` views. A census across current processes verifies their type,
shared backing and debugger state; the reference count must equal those
views plus the mapping owner being destroyed. Another live mapping or
temporary owner prevents release, so open section handles remain usable
for later views. The ordinary image-mapping path is unchanged.

Release additionally requires an inode-backed image mapping FD with no
user, delete disposition, pending inode close, inode or FD locks, async
queues, completion association or active poll dependency. The idle poll
registration is removed before close. Both native descriptor fields become
`-1`, preventing a later metadata destructor from closing a reused
descriptor number. Inode/sharing/name/address metadata remains alive until
the views release it. New sections skip descriptor-less objects when
searching for reusable mapping backing and instead duplicate their live
source file normally.

Data-file mappings, anonymous sections and writable shared image backing
are unchanged. Non-image entries, including any NLS data mappings, are not
implicitly eligible. Resource savings must be measured; inode groups and
descriptor counts do not prove the number of distinct open file objects.

Already attached debuggers prevent release. If a debugger attaches after
release, its image event has no file handle for that metadata-only view.
The image name and information remain available. The patch deliberately
does not reopen by filename: rename, unlink or replacement could make the
path refer to another file. This observable limitation remains in the
accepted console default. Disable the option when later debugger attachment
requires the original image-file handle; the ordinary fixture does not
validate that debugger workflow.

Select `WINE_PS5_IMAGE_VIEW_FD_RELEASE=1`, or place exactly `1` with an
optional final newline in the prefix-local `pw_image_view_fd_release`.
An explicit environment value overrides the file. With 0880, an absent
setting in a valid prefix defaults on. An explicit `0` disables cleanup;
malformed settings, read errors, other open errors and an unavailable prefix
directory keep it off. The setting is read once and preserves `errno`;
non-PS5 builds always retain existing behavior. The combined default-on
revision was accepted and merged after the console checks recorded above.
Later revisions still require their own matching-pair comparisons.

`python3 tests/test_wine_image_view_fds.py` compiles the actual patched
selection, release, census, mapping destruction, reuse and debugger-file
bodies with bounded fixture metadata. It checks 18 dependency rejection
guards, zero/mismatched view counts, multiple mapping owners and processes,
late debugger attachment, shared/data exclusions, metadata reuse, single
close and 17 strict configuration cases, including absent settings and
read/open errors. A real host `mmap` remains readable
after descriptor close. These are native contract checks, not a complete
Wine execution or a console resource-limit test. Run normally and with
clang ASan/UBSan; console validation must compare the same module pair with
selection off/on and ordinary image load/unload, section reuse, sharing,
HL2 and the fixed gameplay route.
