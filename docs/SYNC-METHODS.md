# D3D12 query/sync CPU audit

Current work: branch `p43/fence-scopes`, uncommitted. The p43 section separates
console evidence from source analysis and an unmeasured opt-in mitigation.
The earlier p42 audit/replays below use baseline
`95687cd04063eca3f812d71127e270ca557a52d1`; those benchmark results are **host CPU
replays**, not a measured Xbox improvement.

## p43: fence and conditional subdivisions

The later Xbox MK8D capture `r-mk8d-mp70/eden.txt`, with
`NXBOX_D3D12_METHOD_PROFILE=1`, contains 45 valid windows and one malformed line.
`method_rank.py` reproduces median `other_0x0b2` = **8.4073 ms/frame**, **3.321
words/frame**, and `other_0x556` = **1.6827 ms/frame**, **97.938 words/frame**.
The ratio of the separate medians for sync_info is approximately 2.53 ms/call;
it is not a measured latency distribution. Both FENCE_BATCHING and SUBMIT_TRACE
are explicitly logged as zero. These register details belong to the `other`
aggregate, not `query_sync`, in this revision. The earlier mp69 class totals
below describe an older classifier and must not be compared as identical bins.

### New output and interpretation

Under the same exact, latched METHOD_PROFILE gate, named scopes move time out
of their enclosing register/method scope into these disjoint classes:

| Key | Interval and what the next console split can distinguish |
| --- | --- |
| `commit_async_flushes` | Entire generic CommitAsyncFlushes, including acquiring the buffer/texture locks and all three cache commits. A high value localizes interval processing, staging/resource allocation, pending query writes or cache lock contention; it does not distinguish the three caches internally. |
| `create_fence` | CreateFence only: D3D12 make_shared of InnerFence. No D3D12 fence creation, submit, GPU wait or memory download. A high value means host allocation/scheduling cost. |
| `fence_lock` | Only the wait to acquire the async publication guard. It excludes the work performed while holding it. The completion thread releases this guard before WaitFence, PopAsyncFlushes and guest callbacks, so it does not hold this lock across the GPU wait or readback copy. |
| `queue_fence` | QueueFence → InnerFence::Queue → Submit, or PendingFenceTick with deferred submission. Includes before_submit/FlushBatch before Submit changes phase; excludes time spent in Submit/FenceWait phases. A small value does not rule out an expensive native submission. |
| `flush_commands` | Rasterizer::FlushCommands, including CommitPendingOperations and FlushDiskCache after Submit. The latter locks the disk-cache mutex and, when dirty, serializes/writes the pipeline library. Large CPU/file work can remain here even when the requested native submit is empty. |
| `invalidate_gpu_cache` | Feedback epoch, GPU::InvalidateGPUCache, System::GatherGPUDirtyMemory and every OnCacheInvalidation callback. Large values identify dirty-run/cache work or contention, not a whole-RAM scan. |
| `accelerate_conditional` | Entire AccelerateConditionalRendering attempt, including attempts returning false. Query lookup, predicate resource creation and any internal CPU-predicate ReadGuest path belong here. |
| `fallback_condition` | The Maxwell override/mode switch after acceleration returns false, including ReadBlock/FlushRegion when a comparison needs guest memory. True/False and forced overrides also count but perform no read. |

Counts for these new keys are **scope invocations**, not method words or bytes.
They can be entered by other fence users as well as sync_info. Parent register
counts remain unchanged, but parent exclusive time shrinks. The sum of the new
classes plus remaining parent time reconstructs attributed method-clock work,
not the old individual register ranking in isolation. Nested scopes subtract
children, including reentrant fence publication. method_rank prints the names
automatically, with a wider label column; the logger now iterates all named
classes while still computing `other` from the register samples separately.

All these intervals use the existing cumulative **exclusive Maxwell phase
clock**, not a new wall clock. Submit, FenceWait, UploadAllocation and explicit
buffer/texture download phases still pause attribution. Correlate `cpu_submit`,
`cpu_fence_wait`, download/allocation phases, `wait_allocator_*`,
`wait_finish_*`, forced-submit counters and query-report waits in FrameProfile.
A GPU wait excluded by a phase must not be inferred absent from a zero sub-key.
Uninstrumented overhead (policy checks, reserve, publication queues, notify,
etc.) remains in the enclosing method. Samples finishing across a window reset
retain the existing completion-window rule. No clock, lock, allocation or
environment lookup is added by a disabled subdivision; it checks the existing
recording-thread TLS collector pointer. Cache locks and native work themselves
are unchanged by instrumentation.

### Blocking and memory work verified in source

- **Cache commit:** BufferCache::CommitAsyncFlushesHigh subtracts overlapping
  committed ranges, visits buffers/download ranges, allocates staging and
  records copies. Cost grows with ranges/copies (the overlapping-range pass can
  be quadratic in the number of committed sets), not a fixed scalar signal.
  TextureCache's D3D12 policy implements async downloads: it totals pending
  images, allocates a dedicated mapped readback lease and records DownloadMemory.
  QueryCache commits NotifyWFI/SyncWrites, pending vectors, streamer locks and
  batch registration. None performs an unconditional Finish per normal async
  commit. CPU copies/unswizzling happen on PopAsyncFlushes after completion and
  scale with downloaded bytes; the producer can contend on their cache locks.
- **Indirect commit waits:** Commands() can open a recording during any copy or
  query write. In the direct recorder it calls
  WaitSubmitted(allocators[allocator_index].tick, "allocator") before Reset.
  The native recorder waits on its own allocator reuse; deferred profiling also
  protects timestamp-slot reuse. StagingRing::Request can Finish at its 64 MiB
  pressure limit. CreateBuffer/CreateResource can Finish on memory pressure or
  allocation retry. These calls are reachable from both cache commits and
  predicate creation. Those waits protect live resources and are retained.
- **Readback mapping:** CreateResource and the placed-buffer pool map new staging
  resources at allocation, before their GPU copies are issued; Map is not the
  completion protocol. The CPU reads persistently mapped bytes only after a
  native completion wait (or after Finish in a synchronous path). There is no
  unconditional "map waits for this download" in normal CommitAsyncFlushes.
  Image::Transfer's converted **GPU DMA destination** bridge explicitly Finishes
  and repacks bytes, but normal async texture commit supplies no GPU destination
  and does not take that bridge. Scaling/conversion/copy work and memory-pressure
  allocations can still be expensive; no claim that MK8D takes these branches
  is supported by mp70.
- **Queue/flush:** Submit itself does not unconditionally wait for a free
  allocator. It closes/executes/signals the current list, retains resources and
  queues completion work; before_submit flushes query reductions. Allocator
  recycling happens on the *next* Commands()/RecorderBegin. SUBMIT_TRACE adds
  WaitSubmitted after each submit, but is off in mp70. A queue driver call may
  block internally; source alone cannot establish its console latency. Submit
  also calls EndFrame, so a window may close inside a named scope. FlushCommands
  subsequently serializes the dirty disk pipeline library, O(library bytes),
  under its own mutex. This is CPU/storage work, not proof of a GPU wait.
- **Invalidation:** Gather exchanges each core's current dirty transform and
  swaps its vectors under a mutex; it iterates recorded transforms and contiguous
  dirty runs. OnCacheInvalidation flushes overlapping queued query reductions,
  then WriteMemory/InvalidateRegion visits affected buffer, texture and shader
  metadata under locks. Cost grows with dirty runs, covered cached objects and
  dirty ranges; it does not read every byte of guest RAM. There is no direct
  Finish in this chain; any command recording/allocation invoked by a pending
  query flush can encounter the allocator/pressure paths above.
- **Condition acceleration:** Cache-map and buffer-cache locks, SyncWrites,
  FlushBatch and fresh predicate allocation can do work proportional to pending
  reports or affected ranges. CompareConditional's CPU predicate/high-accuracy
  fallback ReadGuest copies eight bytes through a GPU readback and calls Finish
  once per operand. That is a real CPU/GPU serialization **inside the acceleration
  attempt**, even when it returns true. The native predication path avoids that
  explicit readback; allocator/staging/memory-pressure waits remain possible.
- **Maxwell fallback:** ReadBlock first drains the optional GPU pipeline, then
  FlushRegion on the 16-byte Compare range. FlushRegion can download overlapping
  buffers/images and finish/wait for them; query FlushRegion may request
  ReleaseFences for a host-managed report not yet guest-synced. The memory copy
  is small, but flushing an overlapping image can process its full contents.
  Removing these waits would allow comparisons against stale guest values.

### Opt-in mitigation: NXBOX_D3D12_SYNC_FAST=1

A native Switch syncpoint orders GPU work and advances on GPU completion; it
needs no host command-list boundary per signal. On D3D12 the default immediate
QueueFence submit fragments recordings and consumes bounded allocator slots.
Opening later lists can then serialize the recording CPU with GPU completion.
This is an avoidable host boundary, although the allocator-reuse wait itself is
required for correctness. It is **not established as the cause of the 8.4073 ms
exclusive sync_info time**, because its GPU wait is excluded from that clock.

SYNC_FAST reuses the existing FENCE_BATCHING protocol rather than introducing a
second queue/retirement implementation. Either exact flag value `1` enables
PendingFenceTick capture and suppresses the per-fence FlushCommands. Both flags
default off; SYNC_FAST works with FENCE_BATCHING=0 and is logged at startup.
Use the same enabled profiling scene with SYNC_FAST=0 and then 1; restart
between runs. The profile gate and the optimization gate are independent.

All cache commits, invalidation boundaries and FIFO callbacks remain. Guest
syncpoints increment immediately; host increments stay in callbacks after the
captured tick completes on the GPU. A tick not yet submitted is never passed to
the GPU-completion timeout. Command/DMA boundaries, presentation, forced drains
and the existing submission wakeups supply progress. A forced guest wait submits
the pending tick before sleeping; submit failure propagates instead of stranding
an unsubmitted fence. Completed ticks need no empty submission. The opt-in has
the existing batching policy's progress assumptions and potential latency tradeoff
for work before the next submission boundary; it does not enlarge allocator
rings, skip invalidation, publish early or relax predicate accuracy. See the
fence batching progress contract in `docs/nxbox-port.md`.

Expected evidence, **not measured results**: fewer forced guest-fence executes
and less allocator pressure, while cache-commit and invalidation work may stay
unchanged. Per-fence `flush_commands` invocations disappear; normal boundary
flushes still persist the pipeline library. If `invalidate_gpu_cache` or
`commit_async_flushes` owns the exclusive bottleneck, this mitigation need not
improve it. Compare total GPU-thread/frame wall time and correct scene output,
not only the smaller remaining `other_0x0b2` row. No Xbox build or new console
capture was performed for this patch.

### p43 validation

New tests execute production ProcessQueryCondition with acceleration accepted
and rejected, all five normal modes, and disabled instrumentation; check nested
exclusive named scopes, exceptions and reference returns; and parse production
logger output containing the new classes. The real D3D12 fence-manager source
runs against the existing fake GPU with all four combinations of SYNC_FAST and
FENCE_BATCHING. It verifies pre-completion host values, FIFO callbacks, deferred
progress without another guest command, forced drains, empty submits and failed
submissions. A collector on the producer also checks all six SignalFence scope
counts, while retirement runs on the completion thread.


Final p43 host validation (Mac arm64):

```text
python3 -m unittest discover -s tests/port
Ran 994 tests in 530.645s
FAILED (errors=2, skipped=44)
Differential cases: 651265
```

The only errors were the two known fontTools import failures. Repeating the
same discovery with an existing cached fontTools package on PYTHONPATH (no
installation or repository dependency changes) passed:

```text
Ran 992 tests in 541.805s
OK (skipped=44)
Differential cases: 651265
```

The count drops by two because successful imports remove unittest's synthetic
failed-import cases; these two files contain pytest functions, not unittest
cases. The seven method-profile tests and five fence-manager tests also pass
independently. Changed C++ ranges pass clang-format; changed Python ranges pass
Ruff formatting; `git diff --check` is clean. Logs and the reproduced mp70 ranking
are saved outside the repository in
`artifacts/compat-2026-10-18/p43-fence-scopes-validation/`. No commits were made.


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
