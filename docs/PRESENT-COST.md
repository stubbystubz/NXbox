# D3D12 Composite CPU cost

Branch: `p45/composite-cost`. Source audit and host regressions; Xbox validation
of these changes is pending. No measured speedup is claimed.

## Evidence and attribution

MK8D's supplied sync split attributes approximately 8.4 ms/frame at 3.3
calls/frame to `sync_info`, versus approximately 0.15 ms/frame across SignalFence
subdivisions. `GPU::RequestComposite` registers a guest syncpoint action;
`SyncpointManager::IncrementGuest` executes it synchronously after detaching
ready actions from its lock. Thus Composite runs inside that method on the GPU
thread. The preceding `guest_syncpoint_action` timer still needs console
confirmation; the source call chain does not establish which presentation step
owns the entire 8 ms.

The supplied `r-mk8d-mp72/eden.txt` FrameProfile results are approximately
2.56 forced texture-download submissions/frame, 2.56 `wait_finish` calls/frame
and 1.07 ms/frame waiting there, one forced present submission/frame,
11.3 MB/frame feedback copies and 9.4 MB/frame staging copies. These are
whole-window counters, not proof that all downloads originate in Composite.
Feedback copies are GPU resource copies, not evidence of CPU memcpy volume.
Staging bytes likewise do not identify the presenter without the new split.

## New METHOD_PROFILE keys

Enable the existing exact, latched `NXBOX_D3D12_METHOD_PROFILE=1` gate; it also
enables FrameProfile. No extra profiling flag is needed. Keys use the existing
exclusive Maxwell phase clock and nested child subtraction. Each pair remains
`exclusive ms/frame, scope invocations/frame`. All keys are emitted by the
existing logger and understood by `tools/nxbox/method_rank.py`.

| Key | Scope, excluding nested named scopes |
| --- | --- |
| `composite_rest` | Entire renderer Composite, including layer filtering, frame callbacks, collection, diagnostics and other unattributed presentation work. Subtracts from `guest_syncpoint_action`. |
| `present_lookup` | FramebufferTexture, including acquiring the texture-cache mutex, candidate lookup, view/SRV selection, MSAA resolve and presentation diagnostic snapshots. |
| `present_prepare` | PauseHostConditionalRendering and PrepareImageForPresentation/alias synchronization. |
| `present_download` | Only the presenter fallback's DownloadMemory, including ordered overlapping alias downloads and CPU swizzle/guest writes. |
| `present_upload` | UploadFramebuffer: allocation, guest read, CPU unswizzle/conversion/row copies and upload recording. |
| `present_draw` | Compositor setup, source vector construction, descriptor setup, lazy FSR initialization and bilinear/final draw recording. Also used by a non-FSR final Presenter pass; nested instances contribute disjoint time to the same key. |
| `present_easu` | FSR EASU preprocessing: scratch allocation, descriptors, transitions and draw recording. |
| `present_rcas` | FSR RCAS final blended draw recording. |
| `present_capture` | Screenshot target/readback allocation and capture recording. Nested lookup/draw, Finish and CPU row copy have separate keys. |
| `present_copy` | Screenshot's completed readback-to-caller CPU row memcpy. |
| `present_swapchain` | Actual IDXGISwapChain::Present(1, 0), including driver/refresh/queue blocking. |
| `present_submit` | Display Context::Submit wrapper, excluding phases switched out by Submit itself. |
| `present_wait` | Context::WaitSubmitted reached inside Composite/lookup/draw/capture, including indirect allocator/pressure waits and completion retirement. |
| `present_finish` | Context::Finish reached inside those scopes, including screenshot, resize, fallback, staging pressure and allocation retry. |
| `present_map` | Native Map calls on newly allocated placed/committed staging buffers while presentation is active. |
| `present_frame_end` | RendererFrameEndNotify and rasterizer TickFrame/cache aging, including its nested work. |

**These are CPU recording-thread phase times, not GPU durations or inclusive
wall timers.** Submit, TextureDownload, TextureUpload, UploadAllocation and
FenceWait phases pause the method clock. Consequently a small `present_wait`,
`present_finish`, `present_map`, `present_easu` or `present_submit` does not mean
that call cannot block. Correlate with FrameProfile's `wait_present_*`,
`wait_present upscale_*`, `wait_allocator_*`, `wait_finish_*`, fence-wait/submit
phases and forced-submit reasons. Map inside an excluded allocation phase can
have a count but zero time. `present_swapchain` has no enclosing phase switch
and captures its blocking wall interval when the caller is in the Maxwell
phase (as in the syncpoint action path).

Scopes preserve exception unwinding and the collector's completion-window rule.
A window can close inside Composite or Submit; medians across repeated steady
windows are preferable to one window. The disabled NamedScope reads the TLS
collector pointer without clocks, environment lookup, allocation or locks.
Indirect wait/map scopes also check the active presentation ancestry only when
a collector exists. Profiling adds bookkeeping overhead when enabled.

## Blocking and whole-frame work in source

1. **Cached framebuffer path:** TryFindFramebufferImageView requires matching
   base address, 2D geometry (`stride` by height) and compatible format. It selects
   the newest eligible alias. PrepareImageForPresentation can synchronize aliases,
   upload CPU-modified content or invoke format conversions. MSAA presentation
   uses ResolveFramebuffer, a GPU resolve rather than a CPU full-frame copy.
   Debug pass snapshots may synchronously download full images and write files
   when PASS_DUMP is enabled. Cache mutex contention is CPU blocking too, though
   not directly a GPU wait.
2. **Fallback:** DownloadMemory is **already only reached when there is no usable
   cached view/resource**. Moving it after lookup is not a new optimization.
   With `NXBOX_PRESENT_FALLBACK_DOWNLOAD` default on, the one-byte address query
   finds overlapping downloadable images, sorts by modification tick, downloads
   each whole image, calls runtime.Finish for each, then SwizzleImage writes the
   completed bytes into guest memory. The queried size does not bound download
   size to one byte. Transfer may scale down/up, convert formats or scatter MSAA
   samples on the GPU. Its converted GPU-DMA-destination bridge also Finishes
   and repacks, but the normal presenter readback supplies no GPU destination.
3. **UploadFramebuffer:** Reads the full tiled guest framebuffer into a CPU
   vector, unswizzles to another vector, converts packed color if needed and
   copies every visible row into mapped upload staging. Records another GPU
   copy into the display texture. It runs every fallback presentation, even
   when the fallback texture resource itself is reused. Omitting the preceding
   download can show stale/black frames in MK8D, Odyssey and Wonder.
4. **Backbuffer and FSR reuse:** Draw waits for the slot's presentation tick,
   through Presenter::BeginFrame or directly for bilinear. This protects both
   descriptor slices and FSR scratch; availability of a DXGI buffer alone does
   not justify overwriting these resources before their recorded GPU work ends.
5. **Screenshot:** Capture allocates a BGRA target and persistently mapped
   readback, draws layers, records CopyTextureRegion, calls Finish, then copies
   `width * height * 4` bytes row by row into the caller's buffer. Its completion
   callback currently promises those bytes are ready before returning.
   GetAppletCaptureBuffer separately returns a zero-initialized tiled buffer;
   it does not invoke this GPU readback path.
6. **Present:** Submit executes/signals the compositor list. Present(1, 0) may
   block when flip/refresh pacing or the presentation queue is full. There is
   no per-frame unconditional Finish in display Present. ResizeBuffers drains
   through Finish before releasing old buffers. Startup and renderer/Presenter
   teardown also drain and are lifecycle costs, not steady-frame policy.
7. **Indirect waits:** Commands/RecorderBegin can wait for allocator or timestamp
   slot reuse from lookup, uploads, resolves, FSR or screenshot recording.
   StagingRing::Request can Finish at staging pressure; CreateResource/CreateBuffer
   can Finish for memory pressure or allocation retry. SUBMIT_TRACE, if enabled,
   waits after submission. Deferred Context::Acknowledge waits for the CPU
   recorder to execute/signal the submitted tick; recorder backpressure can
   therefore block the producer even without a direct GPU-completion wait.
   Context::Collect/CompleteThrough acquire completion locks and retire callbacks,
   whose cache downloads/copies and guest callbacks can do additional work. TickFrame may download buffers,
   retire fences or garbage-collect images under cache locks.
8. **Map:** Context maps newly allocated readback/upload heaps before issuing
   their GPU copies. These Maps establish persistent CPU addresses; they are
   not the completion protocol. Removing Finish before a CPU read of those
   addresses would read incomplete data. Native Map/driver allocation can itself
   stall and is now separately scoped where the method clock is active.

9. **Optional diagnostics/simulation:** SimSeriesS::PrepareFrame can wait on
   `simulation slot reuse` before reading its completed timestamp/calibration
   data; EndFrame records simulation work and polls completed samples. This can
   be reached when presentation opens a command list. GpuPassFrame only marks a
   pending frame end; timestamp readback is read in completion callbacks, with
   small scalar CPU copies rather than a whole display frame. FPS overlay work
   can allocate/upload its small glyph geometry and incur the same indirect
   allocator/pressure waits. These optional features are not a reason to remove
   normal display synchronization.

## Implemented opt-in: three presentation slots

Set `NXBOX_D3D12_PRESENT_FAST=1` and restart. Only the exact value `1` enables
it. The flag is independent of profiling, FSR, SYNC_FAST and fallback download.
Startup diagnostics report the flag and actual swapchain buffer count.

Fast mode creates/resizes a **three-buffer flip-discard swapchain** instead of
two. Capacity for three slots is shared by renderer backbuffers, presentation
ticks, compositor descriptor slices and Presenter scratch/descriptor arrays.
Screenshot uses a separate fourth descriptor slice/RTV. Scratch resources remain
lazy. All per-slot GPU completion waits, lifecycle drains, FIFO submissions,
vsync, crop, scaling, blend, color format and fallback readback semantics remain.

This can provide more room before slot/Present backpressure; it cannot guarantee
nonblocking Present or eliminate sustained GPU/refresh pressure. It adds a
backbuffer (about 7.9 MiB at 1080p) and potentially a third FSR scratch allocation
(about 15.8 MiB for one RGBA16F 1080p EASU target), and may increase queued latency.
Default swapchain count remains two. No frames are dropped or knowingly reused
from an earlier guest presentation.

## Further fixes and correctness requirements

- **Prefer a GPU image:** Investigate why the lookup fails for the measured
  display aliases. A GPU reinterpret/conversion or guest-layout shader bridge
  could avoid the download/guest swizzle/unswizzle/upload round trip. Do not loosen
  geometry/format/recency checks: a newer incompatible alias can represent
  different bytes, and a compatible older image can still be stale. Validate
  pixels across MK8D, Odyssey and Wonder, including CPU writes, scaled surfaces,
  stride, crop, alpha, rotation and multiple aliases.
- **Batch fallback readbacks:** Retain staging leases for all ordered aliases,
  record their copies, Finish once, then swizzle them in the same order. Bound
  staging bytes and preserve alias-conversion dependencies, pressure behavior
  and failure handling before enabling this. It could reduce the 2.56 waits
  without removing completion synchronization; those waits are not yet proven
  to be all presentation fallbacks.
- **Async fallback/capture:** Requires frame-owned snapshots, staging leases and
  completion ticks. Resolve a completed snapshot next frame while keeping its
  layer config, crop, source version and callback lifetime together. Simply
  reading last frame's memory or returning before screenshot completion changes
  image/timing semantics. Ring pressure needs a bounded wait/fallback; a retained
  ResourcePtr alone does not freeze a mutable image's pixels. Not implemented.
- **Avoid Finish where possible:** Keep rendering/copying on the GPU and retire
  resources by tick. For unavoidable CPU fallback reads and the current screenshot
  contract, Finish cannot safely be deleted. Required allocator/descriptor waits
  likewise cannot be skipped.
- **Waitable swapchain:** Evaluate DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT
  with IDXGISwapChain2::SetMaximumFrameLatency and a bounded frame-latency handle
  wait, preserving creation flags on resize and closing the owned handle. Confirm
  Xbox UWP/CoreWindow support and error recovery first. This moves backpressure
  to a controlled point before recording, rather than making overload disappear.
  No waitable flags or unsupported API assumptions are introduced here.

## Validation

Run `python3 -m unittest discover -s tests/port`.
Presenter selection tests execute production FramebufferTexture under ASan/UBSan
for fallback default/0/1 crossed with fast unset/0/1/10/true. They verify fresh
GPU-owned alias pixels reach fallback uploads, cached resources bypass downloads,
missing resources still download, exact/latched fast parsing, two/three-slot
policy and enabled/disabled profiling subdivision counts. Existing lookup tests
exercise geometry, formats, recency and view creation. The production FSR
BeginFrame fixture now exercises slot 2 and rejects slot 3, and retains its
crop/transform/pass tests. Existing exclusive-clock/logger tests cover automatic
registration of named classes and window behavior.

Console build, DXIL compilation, rendered pixels, full-chain latency and an
A/B capture in the same scene remain to be verified. Compare fast=0 and fast=1
with all other gates unchanged, METHOD_PROFILE enabled, then repeat without
profiling to measure throughput. Preserve the default-on fallback in both runs.

Final host result (Mac arm64): 993 tests in 538.841 seconds, OK, 44 skipped;
651,265 differential cases. The run used the existing cached fontTools 4.67.0
package through PYTHONPATH, removing the two known failed-import placeholders
without changing installed packages. The new NamedScope regression adds one
test. Formatting checks and `git diff --check` pass. No commits were made.

## GPU lookup (p46), on by default

`NXBOX_D3D12_PRESENT_GPU_LOOKUP` now defaults to on; set it to `0` to return to
the guest-memory download/upload path. Console results on 0.3.823 (chain76,
Series X): every title resolved `result=gpu_view`, the image was intact, and
`present_upload`/`present_download` left the method profile.

| Title | Before (fps) | GPU lookup (fps) | Luma |
| --- | ---: | ---: | ---: |
| Mario Kart 8 Deluxe | 30.4 | 30.3 | 106 |
| Super Mario Odyssey | 24.8 | 23.0 | 74 |
| Super Mario Bros. Wonder | 27.2 | 30.0 | 176 |
| Mario Party Superstars | 54.5 | 59.9 | 139 |
| MK8D, Series S simulation | 22.1 | 23.2 | 102 |
