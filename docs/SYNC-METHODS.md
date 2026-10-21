# D3D12 query/sync CPU audit

Baseline: `95687cd04063eca3f812d71127e270ca557a52d1`, branch
`p42/sync-methods`. Changes are uncommitted. Measurements below are **host CPU
replays**, not a measured Xbox improvement.

## Console evidence and workload limits

```sh
python3 tools/nxbox/method_rank.py \
  /Users/miguel/.local/share/nxbox/artifacts/compat-2026-10-18/r-mk8d-mp69/eden.txt
```

The 42 valid windows (one malformed line rejected) give median `query_sync`
**10.1472 ms/frame**, **740.321 method words/frame**. Dividing those separate
medians gives approximately 13.71 us/word, not the measured latency of each
individual method. Everything else is below 1 ms/frame. This is exclusive
recording-thread wall time, including unscoped blocking work, not GPU time.

The capture's startup line explicitly records `NXBOX_D3D12_FENCE_BATCHING=0`.
Its input environment enables FrameProfile and MethodProfile only. In valid
FrameProfile windows the independent medians are:

| Counter | Per frame |
| --- | ---: |
| ExecuteCommandLists | 16.285 |
| forced_submits_guest_fence | 6.475 |
| forced_submits_query_readback | 0 |
| query_report_fence_waits | 0.99 |
| gpu_resolved_query_reports | 7.19 |
| query_batch_dispatches | 0 |

These counters rule out **740 native submissions per frame** in this capture.
The method-class count does not establish 740 semaphore releases either.
Searching the available `eden.txt` traces for `query_report_begin`,
`condition_begin` and `report_semaphore` did not find a selected-frame method
stream documenting MK8D's exact report types/flags. The class profile has no
register or payload histogram. The predominantly Payload-release/syncpoint mix
supplied with the task is therefore a workload hypothesis, not a verified trace
reconstruction. In Maxwell, Release + short_query sets IsAFence; long Release
and ReportOnly can take the immediate Payload path at non-high accuracy.

The replay brackets that hypothesis with 740 calls/frame:

- `fence`: 680 short Payload fence releases, 20 long immediate Payload releases,
  40 syncpoint signals.
- `immediate`: 20 short Payload fence releases, 680 long immediate Payload
  releases, 40 syncpoint signals.

Both interleave those methods in twenty 37-call groups. They are sensitivity
cases, not a claim that MK8D issues 720/60 native fences per frame. Per-register
console measurements and a payload/flag histogram remain necessary before
assigning the 10.1472 ms to a particular backend operation.

## Audit by path

| Guest method / production entry | CPU work and expensive branches |
| --- | --- |
| report_semaphore.query → ProcessQueryGet → RasterizerD3D12::Query → CounterReport | Address translation; streamer slot construction; device pointers; optional guest clock capture; cache_mutex and two-level cached-query insertion/rewrite for deferred reports; a large allocating std::function closure. Short releases go through SignalFence. Non-fence Payload at non-high accuracy writes guest memory immediately, with no native query readback. The deferred publication callback checks invalidation/final-value flags and retains timestamp/value semantics. |
| Native counter reports → CounterStreamer::WriteCounter → ReadCounter | pending_mutex, snapshot/trace maps and query queues; staging allocation and mapped-memory copies, buffer-cache mutex/lookup, resource transitions, reduction dispatches and ResolveQueryData. SynchronousQueries uses Collect → Finish; bank reuse may Wait. Default GPU reports snapshot to readback without a per-report CPU wait. Staging/buffer pressure can still submit. Payload uses GuestStreamer rather than ReadCounter. |
| render_enable.mode → ProcessQueryCondition / AccelerateHostConditionalRendering | Guest query-map lookups under cache_mutex. Trivial true/false ends predication. CompareConditional flushes pending query work and synchronizes streamer writes. GPU predication allocates a **fresh default-heap buffer** per predicate in SetGpuPredicate; CPU predicate fallback also allocates fresh storage after ReadGuest. These resource/driver allocations are credible multi-microsecond candidates, unlike a disabled tracing branch. ReadGuest copies 8 bytes and Finish submits/waits. Host-managed reports deliberately fall back to guest query-cache reads. The existing non-high-accuracy two-report policy can render unconditionally; this patch does not broaden it. |
| clear_report_value → CounterReset → ResetCounter | RequireClosed; optional Collect/Finish for synchronous queries. Otherwise PauseHostConditionalRendering, AccumulateGpu, and a queued or immediate GPU reset dispatch. AccumulateGpu copies metadata for newly accumulated draws, not just one scalar register. |
| sync_info → ProcessSyncPoint → SignalSyncPoint | IncrementGuest immediately; atomic diagnostic counters and occasional formatted D3D12_SYNCPOINTS messages; SignalFence with a deferred IncrementHost callback, optional latency profiling closure. Host signaling is never moved ahead of GPU completion. |
| wait_for_idle → WaitForIdle | NotifyWFI visits streamers; pending GuestStreamer writes build a temporary vector and issue WriteValue/CopyValue (staging, buffer-cache locks, transitions/copies). SignalOrdering locks buffer_cache.mutex and accumulates dirty intervals. Normally records GlobalUavBarrier without a CPU wait. NXBOX_D3D12_WFI_FINISH explicitly enables Finish and a forced guest-fence drain. Already under SyncWait phase, so much of this is excluded from exclusive method attribution. |
| fragment_barrier / tiled_cache_barrier | Increment feedback epoch and record a global UAV barrier; tiled delegates to fragment. No direct fence submission or guest-memory scan. Commands() can open a recording, so allocation/retirement at a command-list boundary is still possible. |
| invalidate_texture_data_cache → InvalidateGPUCache + WaitForIdle | Feedback epoch advance, GPU::InvalidateGPUCache → System::GatherGPUDirtyMemory. Each core manager exchanges its dirty transform under a mutex, swaps dirty vectors, and emits contiguous dirty runs. OnCacheInvalidation then touches the affected caches under their locks. This is already incremental; it does **not** scan all guest RAM. Many dirty runs, cache invalidation and producer contention can dominate a call. WFI follows separately. |

### Common fence machinery

`src/video_core/fence_manager.h` commits texture/buffer/query flush batches for
every SignalFence. It locks the buffer and texture caches together; query commit
runs NotifyWFI, scans streamer flags under flush_guard, and queues
UnregisterPending through SyncOperation **before** the guest signal callback.
D3D12 allocates a shared InnerFence and wraps callbacks in std::function objects.
The async manager locks guard, publishes a callback batch and fence, wakes its
completion thread, then calls InvalidateGPUCache. The completion thread waits
for the native tick, pops every matching cache batch, invokes callbacks in FIFO
order and retains the fence under ring_guard. Contended locks/wakeups are a
plausible part of a microsecond-scale call; no per-call latency is inferred from
this source inspection alone.

With batching disabled, InnerFence::Queue calls Context::Submit and SignalFence
also requests FlushCommands. Submit avoids executing/signaling an already empty
recording. A nonempty native Submit closes the list, constructs a contents/name
string, sets its debug name, polls the device/InfoQueue, executes command lists,
publishes completion/retention batches under completion_mutex, and signals the
queue fence. It may format periodic diagnostics even with submission trace off.
Those driver calls and boundary allocations can readily be more expensive than
the frontend register dispatch. NXBOX_D3D12_SUBMIT_TRACE additionally forces a
wait after every submit; it must stay off for throughput measurement.

Attribution matters: Context::Submit switches to FramePhase::Submit, and native
blocking WaitSubmitted switches to FenceWait. Those intervals suspend the
exclusive Maxwell clock. They are real end-to-end CPU/wall costs but cannot
simply be assigned to the 10.1472 ms of query_sync. WFI similarly switches to
SyncWait. Unscoped cache locks, callback-batch construction, dirty invalidation,
predicate allocation and completion collection remain stronger candidates for
that specific exclusive measurement. CreateBuffer calls Collect/CompleteThrough
and the placed-buffer pool before creating/reusing the resource; allocation can
therefore also retire earlier completion callbacks, acquire completion locks and
scan pool metadata. It is not merely the cost of storing eight predicate bytes.

The existing `NXBOX_D3D12_FENCE_BATCHING=1` already captures PendingFenceTick
instead. Forced drains, DMA sync, command-list completion and presentation supply
progress and wake deferred fences. It remains opt-in: enabling it globally here
would combine an unmeasured console policy change with the allocation repair.
See `docs/nxbox-port.md` for its progress/timeout contract and console test plan.
There is no second queue signal batching implementation in this patch.

### Tracing and optional GPU pipeline

SubmissionTrace::Log tests Active before incrementing events or calling
fmt::format. Disabled calls here pass scalar addresses/values, so they do not
construct formatted strings first. NxboxWait diagnostics similarly gate their
recorder locks/clocks. BisectEnabled has a thread-local pointer-keyed cache; its
hot calls search that small array rather than reading the environment again.
SignalSyncPoint's periodic D3D12_SYNCPOINTS diagnostic is separate and is not
SubmissionTrace. Existing debug names and periodic submit logging remain.

If NXBOX_GPU_PIPELINE is explicitly enabled, each synchronous rasterizer method
can additionally CaptureDrawSnapshot/Apply and Invoke on the pipeline worker.
Invoke allocates a packaged_task/shared future, locks the queue and waits for a
round trip. That is a credible ~10 us class of cost, but it is not enabled by the
supplied environment. Removing snapshots or making those calls asynchronous
requires a separate ownership/order audit; it is not done here.

## Changes and preserved semantics

1. CounterReport now performs its existing immediate Payload write/free/return
   before constructing the unused large callback. Address translation, streamer
   WriteCounter, pending-sync handling, payload width and processing-vs-publication
   timestamp policy are unchanged. Deferred fences still register before
   publication and still use the original callback.
2. Fence callback batches use vector instead of deque, reserving two slots before
   CommitAsyncFlushes for the common query-cleanup + guest-signal pair. Moving a
   deque can allocate replacement internal storage; moving a vector transfers its
   storage without that replacement. Larger batches grow normally. Async and
   synchronous retirement both use the same new representation. Commit/pop
   counts, FIFO callback order, completion-thread waits, queue submissions,
   invalidation boundaries and forced-drain behavior are unchanged. This generic
   representation change also applies to Vulkan/OpenGL, without changing their
   fence policies.

The patch deliberately keeps required callback/fence allocations; it does not
recycle objects whose lifetime extends onto the completion thread. No cache
invalidation is skipped and conditional-rendering decisions are unchanged.

## Reproducible host benchmark

```sh
python3 tests/port/bench_sync_methods.py \
  --revision 95687cd04063eca3f812d71127e270ca557a52d1 --frames 5000 --repeat 7
python3 tests/port/bench_sync_methods.py --frames 5000 --repeat 7
python3 tests/port/bench_sync_methods.py --sanitize --frames 3 --repeat 1
```

`bench_sync_methods.py` compiles `sync_methods.cpp` against the production generic
FenceManager header and extracted **production CounterReport body**, from either
the revision or worktree. It reuses the existing fence-manager port doubles.
Assertions remain enabled. Every frame checks pre-completion visibility, final
Payload values, host/guest syncpoint equality only after completion, FIFO report
callbacks, and balanced cache commit/pop counts. It includes a cleanup callback
per query-cache commit, as production does. There are 50 warm-up frames; one
separate frame counts operator-new allocations with a thread-local counter;
allocation counting is off throughout the timed loop. Compilation, construction
and warm-up are not timed. JSON includes every sample and medians.

The reduced streamer uses stable query slots; the address maps are std::map
substitutes for the production flat maps. Caches and guest memory are std-only
doubles. Fences are deferred until a simulated per-frame completion and retired
on the producer, using production synchronous retirement. The production async
protocol is covered separately by test_d3d12_fence_manager. **No native D3D12,
ExecuteCommandLists, queue fence driver work, completion-thread contention,
conditional rendering, dirty-gather traffic, or GPU pipeline is timed.** Validation,
map cleanup and fixture callback wrapping are included in both measurements.
Do not scale these times to Xbox or subtract them from 10.1472 ms/frame.

Mac arm64; Apple clang 21.0.0 (`clang-2100.1.1.101`), C++20 -O3; seven fresh
processes, 5,000 measured frames each, baseline then worktree:

| Replay | Before us/frame | After us/frame | Before ns/call | After ns/call | Reduction | Allocations/frame before → after |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Fence-heavy | 195.545 | 149.302 | 264.249 | 201.759 | 23.6% | 4,233 → 3,489 |
| Immediate-heavy | 27.0004 | 11.2948 | 36.4871 | 15.2633 | 58.2% | 923 → 183 |

Raw us/frame samples:

- Fence before: 212.824, 195.545, 198.643, 194.323, 193.665, 195.875, 194.370.
- Fence after: 150.415, 149.302, 148.837, 147.862, 149.507, 144.258, 150.368.
- Immediate before: 27.5324, 26.9794, 26.7624, 26.9039, 27.0450, 27.1718, 27.0004.
- Immediate after: 11.4491, 11.2684, 11.6125, 11.3559, 11.2438, 11.2948, 11.2194.

This establishes a repeatable allocation reduction, not the cause of the entire
console bottleneck. The next console split should prioritize report_semaphore,
render_enable and sync_info separately; correlate their counts with forced
submits, query waits and actual predicate resource allocations. Repeat the same
scene before/after with batching unchanged, then test the existing batching gate
separately. Keep cold load, CPU waits and conditional rendering in that check.

## Validation

- The 740-call replay passes ASan/UBSan in both workload modes.
- Existing query timestamp coverage checks short/long reports, both accuracy
  settings, immediate/deferred values, timestamp policy, invalidation and tracing.
- Existing fence tests exercise async wakeup, ordered cache retirement, deferred
  tick submission, forced drain, pending-operation tails and backend policies.
- `test_sync_methods.py` executes both replay modes and checks the allocation
  regression without imposing a noisy elapsed-time threshold.

```text
python3 -m unittest discover -s tests/port
Ran 990 tests in 532.212s
FAILED (errors=2, skipped=44)
Differential cases: 651265
```

944 tests passed, with no assertion failures. The only errors are the two known
`ModuleNotFoundError: No module named 'fontTools'` imports in test_button_layout
and test_xbox_button_icons. The new replay regression test passed within that
run and independently after formatting. Clang-format and Ruff formatting checks,
and `git diff --check`, are clean. No console build/capture was performed and no
commits were created.
