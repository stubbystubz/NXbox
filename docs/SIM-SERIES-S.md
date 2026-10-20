# Series S performance simulation on Series X

This diagnostic mode approximates reduced CPU and GPU throughput while testing the
native D3D12 renderer on a Series X. It does not identify or reconfigure hardware.
It is off by default.

## Enable

Add these lines to the existing UWP `LocalState/nxbox_env.txt` runtime environment
file, then fully restart NXbox:

```ini
NXBOX_SIM_SERIES_S=1
NXBOX_SIM_GPU_FACTOR=3.0
NXBOX_SIM_CPU_FACTOR=1.05
```

The factor lines are optional. On a desktop Windows build, set the same environment
variables before launching NXbox with its native D3D12 renderer. Remove the enable
line or set it to `0` and restart to disable. Only the exact enable value `1`
activates the feature; factor overrides alone do not. Configuration is cached for
the process lifetime, so use separate launches for A/B comparisons.

GPU factors must be finite numbers in `[1, 10]`; CPU factors in `[1, 2]`.
Missing, malformed or out-of-range values use defaults of `3.0` and `1.05`.
A factor of `1` disables that component's extra work.

## What runs

* GPU: timestamp queries bracket each graphics-queue command list. Before each
  presentation, the shader sums that frame's segments, excluding CPU gaps between
  submissions, DXGI presentation and previous ballast. Query results resolve to a
  default-heap buffer consumed directly by the compute shader on the **same direct
  queue**. The target extra time is `(gpu_factor - 1) * frame_gpu_time`.
* A fixed 128 groups of 64 threads run a dependent integer ALU loop with observable
  UAV writes. The existing embedded-HLSL pipeline and runtime DXC `cs_6_0` compiler
  build it only when enabled. Completed ballast timestamps calibrate iterations
  per GPU tick; early frames use a short 4096-iteration calibration dispatch.
  There is no CPU wait to read the current frame's time. Eight resource/query
  slots rotate, with a fence wait only when an in-flight slot is reused.
* CPU: each Dynarmic AArch32/AArch64 `Run()` timeslice spins for
  `(cpu_factor - 1) * elapsed_host_time` after returning. Guest timing and cycle
  accounting remain unchanged. Factor `1` and disabled mode read no clocks.
* Disabled mode creates no simulation resources, compiles no ballast shader and
  records no simulation GPU commands. Only cached configuration/enable branches
  remain in the hooks; there are no per-frame environment reads or log messages.

Every five seconds of active presentation, the diagnostic log includes:

```text
NXBOX SIM_SERIES_S gpu_factor=3.00 cpu_factor=1.05 ballast_ms_avg=8.123 samples=120 capped_frames=0
```

`ballast_ms_avg` is **measured**, not requested, GPU dispatch time averaged over
samples completed in that reporting window. It excludes the game's own GPU work.
No completed samples yields zero. Warm up until calibration settles before recording
performance; compare the same scene, render resolution, renderer options and cache
state with the mode disabled and enabled.

## Limits

Both consoles retain the same **5120 MiB app memory cap**. This mode deliberately
does not reserve memory. The approximate factors reflect Series S CPU clocks
3.4/3.6 GHz versus Series X 3.6/3.8 GHz (about 5%), and approximately 4 versus
12 TFLOPS of GPU throughput (about 3x).

Added ALU work is a throughput stress approximation, not a Series S hardware model.
It cannot reproduce memory bandwidth, cache sizes, compute-unit occupancy,
texture/raster throughput, contention or clock/power behavior. CPU spinning also
adds host contention and includes callbacks, compilation and waits inside a JIT
timeslice. CPU-bound or vsync-limited scenes need not become three times slower;
the factor targets active GPU work, not total wall-clock frame duration or FPS.

The optional `NXBOX_D3D12_PARALLEL_RECORD` mode is suppressed while simulation is
enabled so each closing timestamp includes all work in its list. Deferred recording
remains supported. Compare against a baseline with parallel recording disabled.
GPU frame profilers may include ballast; the dedicated log isolates its cost.

Ballast targets are capped at 100 ms and loops at 4,194,304 iterations per thread.
These bounds reduce runaway work but are not a hard GPU wall-clock guarantee.
`capped_frames` reports completed dispatches whose requested work reached either
ceiling. Large frames can therefore fall short of the requested factor. At most
1024 command-list segments per presented frame are measured; reaching that limit
logs a diagnostic and further work in that frame is underweighted. Hidden windows
or games that stop presenting do not dispatch ballast or emit periodic logs.

## Validation

```sh
python3 -m unittest discover -s tests/port
```

Portable tests exercise environment defaults/validation, native CPU stretching and
static GPU integration contracts. A Windows/UWP build and real Series X GPU captures
are needed to validate shader execution, convergence and observed frame-time ratios.
The port suite's two known `fontTools` import errors are environmental.
