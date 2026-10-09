# Async Vulkan adapter host fixture

Run the adapter fixture against the exact repository and full pinned Wine tree:

```sh
python3 tests/lab/vk_replay_async.py --source /path/to/wine \
  --build /path/to/configured/wine-build --output /tmp/async-replay-check
```

`--runtime-root` optionally selects a separate repository worktree. The runner
copies its actual Unix adapter, uses real Wine parameter/object declarations,
and links the real stream framing, manual Vulkan wire codec, and pthread replay
scheduler. A controlled native driver receives push constants. Client objects
and batch buffers occupy real low addresses to exercise the batch ABI's 32-bit
pointers; the test does not invent invalid command-buffer objects.

Two independent command pools hold both workers inside driver callbacks at the
same time. The test overwrites all producer input after each async return, then
checks values only after releasing those callbacks. It checks per-buffer order,
exclusion between two buffers from one pool, and a target buffer wait completing
while an unrelated pool remains blocked. Pool wait and lane removal must remain
blocked until their real driver callbacks finish. Recreating a removed lane
checks reuse and final scheduler statistics, including both workers doing work,
peak concurrency of two, and no remaining owned bytes.

The async contract requires version 3 admission and its `unix_count + 1`
admission-only sentinel. Version 2 rejects that sentinel. A separate process checks that
version 2 remains synchronous and does not start workers. Another process feeds
a deliberately malformed owned stream to a valid lane, then calls the actual
buffer wait hook. Success requires `SIGABRT` and the replay-failure diagnostic;
returning from that lifecycle hook after failed replay is rejected.

Both the ordinary and ASan/UBSan builds must pass. The receipt contains command
outputs and SHA-256 identities. This fixture exercises manual recording codecs;
its generated decoder deliberately returns failure and cannot fake successful
generated dispatch. The existing generated-codec and classifier fixtures cover
that separate path. The fixture calls the adapter's scoped wait hooks directly;
it does not claim to test every patched Wine submit/lifecycle call site or any
console driver behavior.
