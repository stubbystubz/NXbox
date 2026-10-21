# Exclusive Maxwell method profiling

Set `NXBOX_D3D12_METHOD_PROFILE=1` before starting Eden. Only the exact value `1`
enables it; restart after changing the environment. On Xbox UWP put the assignment
in `LocalState\nxbox_env.txt`. The generic environment loader already imports it.
This also enables FrameProfile, so `NXBOX_D3D12_FRAME_PROFILE=1` is optional.
Leave `NXBOX_GPU_METHOD_PROFILE` off to avoid mixing in the older sampled profiler.

The motivating MK8D capture (`r-mk8d-prof66/eden.txt`) has 36 steady windows,
993 draws/frame and a median 12.12 ms/frame in exclusive `maxwell3d_methods`.
Those existing logs cannot reveal the method distribution; capture again with
this gate enabled. This is CPU recording-thread wall time, not GPU execution time.

## Output and ranking

Every five-second FrameProfile window emits one additional line:

```text
NXBOX METHOD_PROFILE cb_data=2.300000,16000.000 inline_upload=0.100000,200.000 draw_begin=0.020000,993.000 draw_end=0.500000,993.000 draw_inline=0.000000,0.000 draw_indexed=0.010000,993.000 draw=0.000000,0.000 clear=0.030000,2.000 query_sync=0.020000,9.000 macro=0.040000,100.000 plain_registers=0.200000,20000.000 shadow_dirty=4.000000,36000.000 other=0.080000,5.000 other_0x123=0.080000,5.000 frames=133
```

Each pair is **exclusive milliseconds/frame, method words/frame**. Counts include
all payload words in bulk calls, not clock samples, host wrapper calls or draws.
Macro counts include parameter method words, rather than completed macro programs.
Shadow/dirty counts describe processed words and overlap the originating class's
counts; do not sum counts across classes. Empty startup windows log `frames=0`
and are ignored by the analyzer.

```sh
python3 tools/nxbox/method_rank.py eden.txt
python3 tools/nxbox/method_rank.py eden.txt --top 16
python3 tools/nxbox/method_rank.py eden.txt --json
```

The analyzer ranks the median ms/frame across valid windows, and separately takes
the median count/frame. Windows have equal weight. Logger prefixes are allowed;
malformed lines are counted and rejected. Exit status is 1 without usable windows.
Only the top 16 `other` registers by exclusive time (then count) are emitted per
window. Missing register details contribute zero, so their medians are lower
bounds. `other_0xNNN` uses **word register indices**, not byte offsets. These rows
detail the `other` aggregate: adding them to the class totals double counts time.

## Attribution

- `cb_data`: the sixteen LOAD_CONSTANT_BUFFER/CB_DATA data registers, including
  non-incrementing bulk uploads and incrementing CB runs.
- `inline_upload`: inline/upload engine data and launch methods.
- `draw_begin`, `draw_end`, `draw_inline`, `draw_indexed`, `draw`: draw boundaries,
  inline indices, indexed methods, and instanced array/texture draw methods.
- `clear`: clear-surface methods.
- `query_sync`: reports/semaphores, render conditions, counter reset, sync points,
  wait-for-idle and barrier methods.
- `macro`: dispatch and parameter handling for methods at/above `0xE00`.
- `plain_registers`: ordinary register method dispatch/enqueue overhead.
- `shadow_dirty`: shadow RAM, register value comparisons/writes, dirty flag
  processing and sink drain. Incrementing ordinary registers enqueue in the DMA
  phase; their enqueue counts are recorded here under `plain_registers`, but time
  outside `maxwell3d_methods` contributes zero. Their later drain belongs here.
- `other`: remaining executable methods, with the top 16 register indices.

Fence publication also emits `commit_async_flushes`, `create_fence`,
`queue_fence`, `flush_commands`, `invalidate_gpu_cache` and `fence_lock`.
ProcessQueryCondition emits `accelerate_conditional` and `fallback_condition`.
These are disjoint exclusive subdivisions moved out of the enclosing method;
their counts are scope invocations, while parent counts remain method words.
They use the same gate and phase clock, so Submit/FenceWait/download phase time
is still excluded. See [the sync audit](SYNC-METHODS.md) for interval definitions,
blocking paths and the independent opt-in `NXBOX_D3D12_SYNC_FAST=1` mitigation.

A collector reads FrameProfile's cumulative **exclusive Maxwell phase clock**
using its existing QueryPerformanceCounter clock (100 ns units). Phase changes
into macro execution, renderer/cache/upload work or waits automatically pause
method attribution. Nested method scopes subtract child time, including macro
sends back into Maxwell. Class times and shadow/dirty time are disjoint; renderer
work invoked by a draw does not inflate `draw_end`. Timing/classification/scope
bookkeeping overhead perturbs the enabled capture, and small gaps around scope
boundaries remain unassigned. Compare repeatable scenes, not these times against
an uninstrumented throughput run as if the overhead were zero.

The disabled entry has one predictable TLS-pointer branch per scalar/bulk call
or sink drain. Classification, counters, scope cleanup and clocks exist only in
the enabled template instantiation. No per-word gate, environment lookup, lock,
atomic, allocation or log is added to the normal payload loop. The rollback
`NXBOX_MAXWELL_METHODS=0` retains its old nested phase scopes without adding
per-word method-profile gates.

Enabled CB uploads, inline uploads/index data, ordinary repeated writes, CB
shadow processing and whole sink drains use run-level clocks. Only scalar calls
and the executable fallback that must interleave shadow/dirty processing with
observable side effects need individual shadow intervals. Moving shadow writes
out of that loop would change execution order. There is no per-word clock on
normal bulk data uploads or ordinary state runs.

The recording-thread collector uses fixed arrays (about 57 KiB), no per-method
heap allocation. Scopes unwind on exceptions. The exclusive clock stays monotonic
across window resets; a call still open when a submit closes a window contributes
its count and duration to its completion window. Such boundary calls can shift
class time relative to one FrameProfile window; medians over multiple windows are
more useful. The final partial window is not flushed at shutdown.

## Validation

```sh
python3 -m unittest discover -s tests/port
```

`test_maxwell_method_profile.py` checks the production exclusive clock with nested
classes, excluded renderer/macro phases, exception unwinding, window resets,
exact/latched environment parsing, a disabled template's assembly without timer
or collector hooks, and the analyzer CLI on a synthetic log. The frontend
scalar/bulk differential fixture runs both profiling gates and batching rollback
values under ASan/UBSan. Console compilation and an enabled MK8D capture remain
necessary to validate Xbox code generation and measure instrumentation overhead.
