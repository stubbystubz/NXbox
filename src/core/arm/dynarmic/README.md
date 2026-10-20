# Xbox JIT policy

UWP Auto uses normal desktop memory-context emission. The previous Xbox
diagnostic policy wrote the guest PC with two immediate host stores before every
memory IR operation and preserved otherwise redundant register stores across
those operations. `NXBOX_JIT_FAST_MEMORY_CONTEXT=0` restores that policy;
`NXBOX_JIT_PRECISE_MEMORY=1` also requests precise crash context. Debugger use,
non-Auto accuracy modes, and protection/watchpoint exits retain precise context
by default. A protection/watchpoint exit always retains it, even when the legacy
`NXBOX_JIT_PRECISE_MEMORY=0` switch is set.

Normal execution uses the existing desktop emitter and register elimination
rules, with unchanged guest memory results and FP/synchronization flags. Fatal
crash snapshots can report a block-level PC/register snapshot rather than the
exact faulting memory operation in Auto. The disk-cache configuration signature
already includes this setting, so diagnostic and normal blocks cannot mix.

## Expected performance

There is no new Xbox measurement for this change. It removes two host stores
per memory IR operation and enables register-store elimination across memory
operations. At 3.6 GHz, an illustrative store-throughput floor of one store per
cycle means 1 million such operations remove roughly 0.56 ms of store work;
overlap, dependencies, instruction-cache effects and eliminated register stores
make this **neither a measured saving nor a frame-time prediction**. A GPU-thread
limited frame can gain zero fps. Compare with
`NXBOX_JIT_FAST_MEMORY_CONTEXT=0` on the same gameplay and cache state.

The supplied 2026-10-18 profiles contain no guest CPU-thread busy samples and no
Odyssey profile. In their final three windows:

| Game | GPU thread CPU occupancy | Recorder busy/frame |
| --- | --- | --- |
| BotW | 84.66 / 88.44 / 91.56% | 18.858 / 18.842 / 17.067 ms |
| MK8D | 72.49 / 84.02 / 82.48% | 6.956 / 6.709 / 7.186 ms |
| Wonder | 47.16 / 57.81 / 46.56% | 4.582 / 3.958 / 4.578 ms |

These are evidence of substantial renderer CPU work, not proof of an exclusive
guest CPU bottleneck. `gpu_busy_ms` is accumulated over the profiling window;
the nested CPU buckets must not be added together. Wonder's swap interval cap
cannot be removed by JIT configuration.

## Existing knobs and constraints

- Block linking, RSB, fast dispatch and safe IR optimizations are already enabled.
  Existing `NXBOX_JIT_BLOCK_LINKING=0`, `NXBOX_JIT_RSB=0`,
  `NXBOX_JIT_FAST_DISPATCH=0`, `NXBOX_JIT_CONTEXT_ELIMINATION=0`,
  `NXBOX_JIT_CONST_PROP=0`, `NXBOX_JIT_MISC_IR=0`, `NXBOX_JIT_CODE_SPEED=0`
  and `NXBOX_JIT_DISABLE_VERIFICATION=0` remove them individually.
- A64 Auto already enables unfused FMA and ignores the global monitor. Existing
  `NXBOX_JIT_UNFUSE_FMA=0` and `NXBOX_JIT_IGNORE_GLOBAL_MONITOR=0` remove them.
  These are existing accuracy tradeoffs, not newly established safe behavior.
  Zen 2 supports native FMA; unfusing is not automatically a win on this CPU.
- Inaccurate NaNs and reduced FP error remain opt-in through Unsafe accuracy.
  Changing NaN handling or reciprocal estimates can change game results. No
  supplied profile establishes their safety in BotW, MK8D or Odyssey. Existing
  `NXBOX_JIT_INACCURATE_NAN=0` and `NXBOX_JIT_REDUCED_ERROR_FP=0` disable them.
- UWP probes a placeholder arena and lazy 64 KiB pagefile sections through the
  dynamically resolved `*FromApp` APIs. Only first touches create committed
  sections; mappings and clears of untouched DRAM do not commit the guest heap.
  Whole-region clears remove every alias and close the section to release commit.
  Partial clears preserve neighboring bytes. `NXBOX_FASTMEM=0` keeps the private
  demand-committed backing and checked page-table path. The existing
  `NXBOX_JIT_FASTMEM=0` and `NXBOX_JIT_FASTMEM_EXCLUSIVES=0` opt-outs also remain.
  UWP retains guest address bounds checks in Auto and Unsafe modes.
- A64 UWP already uses 192 MiB/core (768 MiB for four cores), with
  `NXBOX_JIT_LARGE_CACHE=0` restoring 128 MiB/core. A32 stays at 128 MiB/core;
  null JIT stays at 8 MiB. The x64 backend evacuates its cache below 1 MiB free,
  resets RSB and clears emitter links. Explicit and range invalidation remain
  necessary for correctness. None of the supplied logs provides capacity-clear
  counts justifying another memory-budget increase; the clear policy is unchanged.

Run the focused regression with
`python3 tests/port/test_jit_memory_context_policy.py -v`.

## UWP fastmem console verification

Run the allocator, selection, and unwind-registration regression on the host:

```sh
python3 tests/port/test_uwp_fastmem.py -v
```

The harness compiles the production UWP allocator and VEH against Windows API
mocks, including shared section views, fault injection, partial unmaps, mirrors,
protection changes, reclamation, and concurrent first touches. On Apple Silicon,
aligned views use real shared mappings and Windows subpage views are emulated
because macOS uses 16 KiB pages. These tests do not establish Xbox API support,
real exception dispatch, or performance.

For each console run, collect all of the following:

- `NXBOX MEMORY mode=fastmem backing=regional_sections ... committed_mib=0
  probe=passed veh=ready` confirms the section/4 KiB alias/protection/recreation
  probe and VEH registration succeeded. The arena is reserved address space.
- `NXBOX FASTMEM handler=seh unwind_registered=true fallback=none` must appear
  for each x64 JIT code buffer. Failure disables fastmem emission through
  `ExceptionHandler::SupportsFastmem()`, even if an arena was allocated.
- `NXBOX FAULT CONFIG ... fastmem=true` confirms the A64 configuration requested
  fastmem. Read it together with the unwind-registration result, not alone.
- `SPARSE ... guest_backing_committed=...MiB` reports current section commit.
  Reclamation summaries (at most one per 64 MiB reclaimed) include `committed_mib`,
  `reclaimed_mib`, `backing_faults`, `alias_faults`, and `alias_failures`.
  Successful alias faults demonstrate real VEH dispatch; expect zero failures.
  Confirm commit falls after full-region clears and does not jump to full DRAM
  size during a heap clear. Sections, handles, and mapping metadata have overhead
  beyond the guest commit counter; also inspect total app memory on Series S.
- Exercise a rasterizer-protected access that must leave fastmem: the x64 SEH
  callback must return through the normal memory callback and continue correctly.
  When stall profiling is enabled, its existing fastmem-fault counter can confirm
  that path. An accepted unwind table alone does not prove real SEH dispatch.
- Repeat the same workload with `NXBOX_FASTMEM=0`. Expect
  `mode=page_table fastmem=unavailable reason=NXBOX_FASTMEM=0` and A64
  `fastmem=false`. Compare frame time and app memory with identical game settings.

Missing exports or a failed startup probe select private demand backing and log
`mode=page_table fastmem=unavailable reason=<failed_stage> error=<win32_error>`.
A failed runtime alias map/protect falls through to Dynarmic's SEH/page-table
callback; unrecoverable reservation/unmap failures must not silently retain a
stale guest alias. No 60 fps or console compatibility claim follows from host tests.

The backing deliberately avoids a monolithic `SEC_COMMIT` section (eager guest
commit), `SEC_RESERVE` plus commit-on-view (previously rejected on Xbox), and
`DiscardVirtualMemory` (retains commit and leaves contents undefined). See the
Microsoft contracts for [FromApp sections](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-createfilemappingfromapp),
[placeholder view alignment](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-mapviewoffile3),
[discard semantics](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-discardvirtualmemory),
and [dynamic unwind registration](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtladdfunctiontable).
