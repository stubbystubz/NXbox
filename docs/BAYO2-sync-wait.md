# Bayonetta 2: sync_wait after GPU depth reinterpretation

## Finding

The apparent 5.78 ms/frame stall is overwhelmingly the GPU emulation thread
waiting for **new guest commands**, not waiting for the D3D12 GPU. This patch
corrects attribution; it does not claim an FPS improvement. Removing this wait
would busy-spin on an empty queue. The supplied trace does not justify removing
GPU ordering, making readbacks asynchronous, or increasing frames in flight.

The root path is `src/video_core/gpu_thread.cpp:87`: a
`WaitScope{Wait::GpuQueueIdle}` wraps `state.queue.PopWait`. The queue's
`src/common/bounded_threadsafe_queue.h:122` waits on a condition variable until
the producer publishes an entry (or shutdown interrupts it). Before this patch,
**every** `WaitScope` entered `Phase::SyncWait`, including queue idle and even
calls with `waiting=false`. The corrected selection is in
`src/common/gpu_thread_profile.h:205`.

The misleading `_cpu_ms` suffix matters: phase switches use a wall clock in
`src/video_core/renderer_d3d12/d3d12_context.cpp:511`, not thread CPU time.
`gpu_thread_cpu_pct` uses real thread CPU time separately. Thus a sleeping GPU
thread can accumulate large `sync_wait_cpu_ms` values.

## Log evidence

Source directory:
`/Users/miguel/.local/share/nxbox/artifacts/compat-2026-10-18/`.
Use the same steady-window selection as `_analysis/profile_rank.py`: only
`D3D12_FRAME_PROFILE` windows with `frames >= 100`, paired with the immediately
following `D3D12_GUEST_WAITS` window. There are 38 steady windows in
`r-bayo2-gdr-prof/eden.txt`, and 25 in `r-bayo2-prof/eden.txt`. Times below are
medians of per-window milliseconds per frame, not sums of independent medians.

| Counter | Before (`r-bayo2-prof`) | GPU depth fix (`r-bayo2-gdr-prof`) |
| --- | ---: | ---: |
| `sync_wait_cpu_ms / frames` | 0.168 | 5.779 |
| `gpu_queue_idle_ms_per_frame` | 0.122 | 5.700 |
| `(sync_wait_cpu_ms - gpu_queue_idle_total_ms) / frames` | 0.046 | 0.059 |
| `fence_wait_wall_ms / frames` | 1.855 | 0.032 |
| `forced_submits_texture_download_per_frame` | 2.02 | 0.04 |
| `forced_drain_ms_per_frame` | 0.791 | 0.508 |
| `guest_fence_completion_ms_per_frame` | 2.385 | 2.335 |

The cited pre-fix ~1.86 ms matches **fence_wait**, rather than sync_wait, in this
supplied pre-fix trace. Keep those categories separate when comparing runs.

Concrete paired evidence in `r-bayo2-gdr-prof/eden.txt:1040` and `:1041`:

```text
frames=107 sync_wait_cpu_ms=189.447 fence_wait_wall_ms=3.127
frames=107 gpu_queue_idle_count=321 gpu_queue_idle_total_ms=183.633
puller_acquire_count=0 forced_drain_total_ms=43.422
```

Here 183.633 of 189.447 ms is empty-queue time; the residual is
5.814 / 107 = 0.0543 ms/frame. The faster depth path leaves the consumer idle
longer. Which upstream producer/pacing mechanism determines command arrival is
not established by these counters.

Other candidate waits in all 38 steady post-fix windows:

- Query bank/readback forced submits and `query_report_fence_waits`: zero.
- Buffer-download forced submits and index downloads: zero. Small buffer
  downloads still exist (median 120.02 bytes/frame); bytes alone imply no stall.
- Memory pressure, allocation retry and descriptor exhaustion: zero.
- Staging pressure: median zero; one isolated forced submit (`eden.txt:526`), insufficient to
  explain a recurring 5.78 ms/frame category.
- WFI forced submits and implicit fence-wait forced submits: zero. Guest WFI
  uses a GPU UAV barrier by default (`d3d12_rasterizer.cpp:4000`).
- Guest puller acquire, guest/host syncpoint waits, synchronous TickGPU,
  request-flush and wait-idle counters: zero. Request-sync-operation has median
  zero, with five calls only in the first qualifying window (`eden.txt:290`):
  163.100 ms total over 269 frames, dominated by one 161.186 ms call. This
  caller-side wait (`gpu.cpp:142`) does not explain recurring GPU-thread idle.
- Software Fermi2D blits and CPU reinterpret bytes: zero. Remaining texture
  download finishes account for the small `finish` fence wait above.
- Present submits: one/frame, which counts submissions, not blocking finishes.
  No `present`/allocator reuse wait appears in `waits_top`. Swapchain buffer
  reuse already waits only on the reused buffer (`renderer_d3d12.cpp:254`).

`guest_fence_completion` is collected across threads: the asynchronous fencing
thread calls `InnerFence::Wait` (`d3d12_fence_manager.cpp:28`); its ~2.33 ms/frame
must not be added to GPU-thread sync_wait. Forced drains are separately timed in
`d3d12_rasterizer.cpp:3983`, explicitly outside a SyncWait scope. The supplied
file contains no literal `THREAD_PROFILE` lines; this diagnosis relies on its
frame and guest-wait lines, rather than inventing thread-profile samples.

## Change and validation

- Give queue idle its own exclusive phase; emit it as
  `gpu_queue_idle_wall_ms` (window total), outside the `_cpu_ms` ranking.
- Keep the existing `D3D12_GUEST_WAITS gpu_queue_idle_*` counters for correlation.
- Make `waiting=false` preserve the caller's phase and leave counters untouched.
- Preserve actual fence, guest semaphore, query and readback synchronization.
  This is instrumentation only, so no risky optimization or environment
  opt-out is introduced. The queue still sleeps until work arrives.

`tests/port/test_d3d12_gpu_thread_profile.py` exercises deterministic exclusive
phase accounting (80 idle, 20 sync, 30 parser ticks), inactive waits nested in an
idle phase, exception restoration, enabled idle counters, and log wiring. The
C++ harness runs with address/undefined-behavior sanitizers.

Validation: `python3 -m unittest discover -s tests/port` ran 966 tests in
892.240 seconds: no test failures, 44 skips, and only the two known environmental
`ModuleNotFoundError: No module named 'fontTools'` import errors. The focused
profiling and stall-profile tests passed; the final profiling harness was also
rerun after the added counter assertions. Changed code was formatted with
clang-format and Ruff, and `git diff --check` passed.

The host suite cannot verify Xbox D3D12 compilation or measure FPS. On the next Series X capture, compare
`gpu_queue_idle_wall_ms / frames` against the existing guest idle counter;
`sync_wait_cpu_ms / frames` should now reflect the small residual. Upstream
command production/pacing requires its own trace before pursuing a speedup.
