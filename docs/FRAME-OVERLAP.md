# Frame overlap: syncpoint publication before presentation

## Finding and limits of the captures

The concrete serialization in this checkout is:

```text
GPU recording thread reaches a Maxwell syncpoint
  -> IncrementGuest
     -> ready guest action: Composite -> D3D12 Present -> recorder acknowledgement / DXGI Present
  -> only now publish the native fence for that syncpoint
GPU fencing thread waits for native completion
  -> retire buffer / texture / query flushes
  -> IncrementHost
     -> nvhost_ctrl host action -> NVCTRL::NvEvent -> guest WaitSynchronization returns
```

The nvdrv wait is identifiable in both captures. The captures are five-second
aggregates, not per-frame traces: they do not prove that every frame takes this
branch, which syncpoint ID/threshold belongs to a particular frame, or which guest
instruction polls a result. The timeline below is a code-established ordering,
with observed waits attached, not a fabricated timestamp trace.

Evidence references below are one-based lines in the supplied local `.logs/`
files. Those files remain untracked.

| Evidence | MK8D (`mk8d-eden.txt`) | Wonder (`wonder-eden.txt`) |
| --- | --- | --- |
| BufferQueue event identity | L221: `0x2c5253c8908`, `BufferQueue:WaitEvent` | L237: `0x193cd43f178`, `BufferQueue:WaitEvent` |
| Display-vsync identity | L224: `0x2c5253c8998`, `DisplayVsync` | L240: `0x193cd43f208`, `DisplayVsync` |
| nvhost event identity, after earlier address reuse | L339: `0x2c5253c8f38`, `NVCTRL::NvEvent_0` | L388: `0x193cd440408`, `NVCTRL::NvEvent_0` |
| SVC containing the event wait | L4824: thread 79, svc `0x18` (WaitSynchronization), 201 calls, 1,397,734 us | L8352: thread 79, svc `0x18`, 247 calls, 937,083 us |
| Actual guest nvhost wait | L4830: thread 79, `sync_object=0x2c5253c8f38`, 201 calls, 1,396,836 us / 5,000 ms | L8358: thread 79, `sync_object=0x193cd440408`, 99 calls, 936,286 us / 5,000 ms |
| BufferQueue wait is small in the adjacent window | L4808: thread 82, 199 calls, 216 us / 5,008 ms | L8461: thread 83, 151 calls, 206 us / 5,012 ms |
| GPU recording CPU work | L3572: 162 frames, `gpu_thread_cpu_pct=80.93` | L8430: 150 frames, `gpu_thread_cpu_pct=59.06` |

Object addresses are reused during startup; match the last creation preceding the
sample, not the first occurrence of an address. In particular, MK8D's nvhost
address previously named audio events. The very long thread 110/114 waits at
`0x2c5253c8ea8` / `0x193cd440138` match `IAudioRendererEvent`, not presentation.
Neither display-vsync address appears as a selected `sync_object` in these logs.
This does not exclude timed-out waits or polling; object zero represents no
selected object, and sync-object wall time includes runnable scheduling delay.

The matching SVC and selected-object durations identify the event wait without
adding overlapping counters. MK8D L4822 also shows thread 79 executing guest code
for 3,129,500 us / 5,000 ms. Wonder L8356 reports another 1,733,499 us in svc `0x34`
(WaitForAddress); that address dependency is not identified by the event keys.
The captures do not describe an otherwise entirely sleeping game.

The initial premise needs two qualifications:

* MK8D L4925 has thread 101 in svc `0xb`: 835 calls, 4,880,015 us / 5,003 ms.
  `svc_thread.cpp::SleepThread` sleeps on a timer for positive arguments, or yields
  for special nonpositive arguments. The profile does not record those arguments,
  the guest PC, or a dependency behind a polling loop. It cannot identify this
  thread as the main render thread or attribute its sleep to an nvhost fence.
  Wonder's late samples do not show the same thread-101 pattern.
* Wonder is not uniformly 80% busy in the supplied capture. L8430 also has
  `wait_buffer_download_ms_per_frame=4.219`, 9.68 downloads/frame and 4.117 ms/frame
  of event blocking. These metrics overlap and must not be added. Readback remains
  a separate source of serialization; this patch does not remove it.

MK8D L4906 and Wonder L8430 both report `query_report_fence_waits=0` and
`forced_submits_query_readback_per_frame=0.00`. Query writes and buffer copies are
present, but these samples do not identify a native query-report fence wait as the
frame gate. Query callbacks remain behind GPU completion and retirement.

## Timeline of a frame through this path

1. The guest submits work through nvhost. `GPU::RequestComposite` in
   `src/video_core/gpu.cpp` schedules a sync operation, then registers guest
   actions for the framebuffer's acquire fences. With no fences, or already
   satisfied guest thresholds, Composite runs inline during that operation.
2. The GPU recording thread (or its serialized draw-pipeline consumer) records
   draws and reaches `D3D12::FenceManager::SignalSyncPoint`. Originally it first
   calls `IncrementGuest`. This indicates processing of guest commands, not native
   GPU completion. `host1x/syncpoint_manager.cpp::IncrementGuest` detaches ready
   actions under its lock, then executes them synchronously without the lock.
3. The last required guest action in `GPU::RequestComposite` calls
   `renderer->Composite`. `RendererD3D12::Composite` enters the serialized renderer;
   `Impl::Present` submits presentation commands, notifies deferred fences,
   acknowledges recorder progress and calls `swapchain->Present(1, 0)`. All this
   happens before the original SignalSyncPoint can publish its own host fence.
   The thread's work also includes any framebuffer preparation/readback.
4. Originally, only after that action returns does SignalSyncPoint call
   `SignalFence`. `video_core/fence_manager.h::SignalFence` commits async flushes,
   captures/queues the backend fence and queues the host increment callback.
   D3D12's `REQUIRE_GPU_COMPLETION=true` prevents an immediate guest-visible host
   signal regardless of accuracy settings. `InnerFence::Queue` normally submits
   the captured tick; fence batching instead uses `PendingFenceTick` and retains
   its existing submit/drain protocol.
5. `FenceManager::ReleaseThreadFunc` waits for submission and `WaitFence`, then
   `PopAsyncFlushes` retires texture, buffer and query results. Only afterward do
   callbacks call `IncrementHost`. In `nvdrv/core/syncpoint_manager.cpp::UpdateMin`,
   the nvdrv minimum comes from the **host**, not guest, counter.
   `nvhost_ctrl.cpp::IocCtrlEventWait` registers a **host** action which signals
   `event_.kevent`. Repeated failed waits may use `WaitHost` directly. Thus the
   observed thread-79 nvhost event cannot wake until this path completes.
6. Independently, VI's `Conductor::ProcessVsync` calls composition before signalling
   display vsync in the default mode. `HardwareComposer::ComposeLocked` waits for
   the preceding composite, acquires buffers and requests another composition;
   `GPU::WaitForComposite` waits for the sync operation, not a native GPU fence
   and not necessarily a future registered action. The existing
   `NXBOX_VI_INDEPENDENT_VSYNC=1` option moves notification onto CoreTiming's
   cadence in multicore mode. This patch neither enables it nor changes cadence,
   swap intervals, BufferQueue ownership or the number of in-flight composites.

There is therefore a demonstrable extra dependency between host-fence publication
and a synchronous presentation action. The log evidence identifies nvhost waits,
but cannot quantify how often presentation delays their publication. It does not
establish that this dependency alone explains the full FPS deficit.

## Change and safety boundary

`NXBOX_D3D12_OVERLAP_SYNCPOINT=1` changes step 2 to:

```text
publish guest counter and detach ready actions
  -> publish host fence covering preceding recorded commands and cache flushes
  -> run ready guest actions, including Composite/Present
```

The fence completion worker can now retire results and wake the guest while
presentation is still executing. No new thread or queue is introduced. Ready
actions still execute once, in their existing order and on their existing thread.
The publication hook runs outside the syncpoint lock, including when there are no
ready actions. The guest counter is visible before native completion can increment
the host counter. The hook orders actions detached by this increment; it does not
serialize an unrelated concurrent registration whose threshold is already met.
The production composite registration and GPU increment run on the serialized GPU
path.

The host counter still advances only after **real native GPU completion and cache
retirement**. This is not a recording-complete shortcut for CPU readback, queries
or nvdrv consumers. Moving the fence ahead of presentation excludes presentation
commands from the fence guarding the preceding guest commands. Those commands
remain on the same ordered native queue, with existing resource retention and
presentation reuse fences. Screenshot readback still performs its required Finish.

The switch is **default-off**: unset, `0`, or any value other than `1` preserves
legacy ordering. Use `NXBOX_D3D12_OVERLAP_SYNCPOINT=0` to explicitly disable it.
Although the guarded guest results still require GPU completion, earlier guest
execution changes timing relative to compositor source-buffer reuse. Host tests
cannot establish game-specific image correctness or Xbox driver behavior. Keep
this experimental until console image/capture and reuse validation passes. Startup
logs report the effective switch with the other D3D12 bisect settings.

With `NXBOX_D3D12_FENCE_BATCHING=1` or `NXBOX_D3D12_SYNC_FAST=1`, earlier publication
may still wait for a later submit. No early host signal or forced extra submit is
introduced to bypass that protocol. Measure those combinations separately.

Expected effect: remove presentation/recorder work from the publication latency of
syncpoints that trigger ready presentation actions; potentially overlap guest CPU
work with that presentation. Do not expect reduced Maxwell decoding cost, removal
of Wonder's downloads, or a guaranteed 60 FPS. Already-completed acquire fences,
no ready action, and guest CPU bottlenecks can yield little or no improvement.

## Host verification

`tests/port/test_frame_overlap.py` compiles the production D3D12 SignalSyncPoint
method and the production host1x syncpoint manager with platform mocks, under
ASan/UBSan. It holds a presentation action blocked with explicit handshakes and
checks:

* default/unset, `0`, and invalid options retain the late-publication order;
* opt-in publishes the fence while presentation is blocked, with profiling on/off;
* host counters and events stay pending until simulated GPU completion and valid
  readback/cache data, then the nvhost-style event wakes before presentation ends;
* guest publication precedes host publication, the hook can reenter the manager,
  and future-threshold actions execute once at the correct increment.

The existing native fence-manager replay also runs overlap off/on against all four
fence-batching/sync-fast combinations. Fence-completion/guest-visibility tests cover
production GPU wait and cache-retirement ordering; the host tests mock the backend
fence, not physical D3D12.
Run the full suite with scratch space outside the source tree:

```sh
cd tests/port
TMPDIR=/path/to/scratch python3 -m unittest -q
```

Validation on 2026-10-21: the complete port suite finished with:

```text
Ran 1001 tests in 552.060s
OK (skipped=45)
Differential cases: 651265
```

The host used the existing Homebrew Python 3.14.8, existing fmt/OpenSSL pkg-config
paths and existing cached fontTools through PYTHONPATH. No project dependency was
added. The initial system-Python run exposed an old one-argument syncpoint mock;
that mock now implements the publication hook and the native replay covers both
switch settings. Console image correctness and performance have not been measured.

## Console measurement

1. Use the same build/settings, warmed shader caches, race/level, camera and test
   duration. Capture a baseline with `NXBOX_D3D12_OVERLAP_SYNCPOINT=0`, then `=1`.
   Keep fence batching, sync-fast and independent-vsync settings unchanged.
2. Set the switches in Xbox `LocalState/nxbox_env.txt` before starting the game.
   Enable `NXBOX_D3D12_FRAME_PROFILE=1`, `NXBOX_THREAD_PROFILE=1` and
   `NXBOX_WAIT_DIAG=1`. Confirm the effective overlap switch in startup diagnostics.
   Match NVCTRL event identities anew on each run. Compare thread-79 nvhost event
   calls/wall time and timeout rates, not thread-101 sleep percentage alone.
3. Compare frame rate/frame-time tails, GPU-thread CPU percentage, queue idle,
   fence pending counts and wait times, forced submits, recorder backpressure,
   query pending counts and readback waits. Examine VI timing where available.
   Earlier ProcessFence sampling moves time between queued/execution buckets;
   total request-to-completion latency is the comparable fence metric.
4. Verify the same rendered scene and screenshot output, moving camera, transitions,
   framebuffer reuse over many frames, menus, overlay and screenshot capture,
   suspend/resume and exit. Reject black/stale/wrong frames, resource errors,
   missing query results or growing fence backlog even if FPS rises.
5. A per-frame causal claim still requires a console trace correlating submission,
   syncpoint ID/threshold, publication, native completion, NVCTRL signal and
   Composite/Present boundaries. The supplied aggregates cannot provide it.
