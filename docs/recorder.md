# Native gameplay recording

The native D3D12 renderer records its final display image as H.264 MP4. The existing FPS overlay,
when enabled, and the small red recording dot are included. This is **video only**: XAudio2 feeds
separate source voices into a mastering voice; there is no existing final mixed-output tap.
Recording those source buffers directly would miss the final mix, gain and playback timing.

## Controls

- In Settings → System → **Record video / Gravar vídeo**, press A to cycle through Manual (720p30),
  720p30, 720p60, 1080p30 and 1080p60. A resolution selection arms automatic recording for the next
  game. Manual requires the shortcut. The choice is stored in `LocalState/recording.txt` (values 0
  through 4).
- During gameplay, **hold View and click the right stick** to start or stop. Holding the buttons
  does not repeat. View + Menu remains HOME; moving the right stick while holding View remains the
  motion gesture. The recording click does not reach the guest and cancels the short-View Minus
  gesture.
- A red dot at the upper right means frames are being recorded. Opening the encoder can take a
  moment. See `RECORDER failed` if the dot never appears.
- Every recording stops after at most 120 seconds. Returning from a game drains the GPU, joins the
  worker and finalizes the MP4. Suspending requests a stop and allows up to two seconds for
  finalization before completing the suspend deferral. A forced termination, device removal or power
  loss can leave an unfinished MP4.

Add these lines to the existing `LocalState/nxbox_env.txt`, preserving unrelated settings, and fully
restart NXbox for an unattended capture:

```ini
NXBOX_RECORD=20
NXBOX_RECORD_DELAY=5
```

`NXBOX_RECORD` accepts a whole number of seconds, clamped to 120. `0` disables the automatic start,
including a settings-armed start, but leaves the shortcut usable with a 120-second limit. Without
the variable, the settings selection controls automatic start. `NXBOX_RECORD_DELAY` is an optional
whole-second delay after the first game image reaches presentation, clamped to 120; default 0.
Automatic start fires once per game session, including if a clip is stopped manually. Resolution and
frame rate come from the settings selection; the default is 1280×720 at 30 fps. 1080p uses 12 Mbps;
720p uses 8 Mbps. These are encoder targets, not a promise of constant bitrate or a guarantee that a
slow game produces 30/60 unique frames.

`NXBOX_RECORD_SOFTWARE=1` forces ordinary NV12 memory samples with hardware transforms disabled for
fallback validation. Otherwise D3D11 texture input is tried first.

## Files and storage

Files are written to:

```text
LocalState/recordings/<16-digit-title-id>-<yyyyMMdd-HHmmss>.mp4
```

The timestamp is console local time. An unavailable title ID becomes sixteen zeroes. A same-second
filename collision never overwrites an earlier recording. Wait for
`RECORDER stopped ... finalize_hr=0` before retrieving a clip.

The recordings directory is a managed archive with a 512 MiB budget. Before a new clip, the worker
removes the oldest MP4s until 192 MiB is reserved within that budget and at least 192 MiB is free on
the volume. If space still cannot be reserved, recording fails without stopping emulation. The
worker checks clip size once per encoded second and stops near 184 MiB, leaving space for mux
finalization. The time limit and configured bitrate normally produce smaller clips. Do not put MP4s
you want to preserve permanently in this managed directory; pull them out.

In Xbox Device Portal, open File explorer → the NXbox package → LocalState → recordings, then
download the finished MP4. The existing FTP server exposes the same directory as
**`/interno/recordings/`**. Turn FTP on in NXbox Settings, use the address and password shown there,
and download with an FTP client. For example, with the displayed IP and a filename from the listing:

```sh
curl --user nxbox --list-only "ftp://<xbox-ip>:2121/interno/recordings/"
curl --user nxbox "ftp://<xbox-ip>:2121/interno/recordings/<filename>.mp4" -o gameplay.mp4
```

Curl prompts for the displayed FTP password. FTP runs while the library is open; return to the
library before downloading. No new server or public endpoint is required.

## API and threading path

`d3d12_present/recorder.cpp` hooks the swapchain image after the FPS overlay and also observes
generated presentations. A bilinear GPU pass scales the complete RGBA display to the configured
size. Three fixed slots own render targets and persistently mapped readback buffers. Per-slot
SRVs/RTVs remain unchanged until the worker releases that slot. The producer records a
texture-to-buffer copy and publishes the existing Context fence tick; it never calls Finish/Wait,
waits for a slot, maps synchronously, encodes or writes a file. Occupied slots cause a drop.
Ordinary renderer submission carries the copy, including deferred command replay. GPU resources and
the small scaling PSO are allocated/compiled lazily on the worker at first request. Readiness is
published only after initialization and encoder opening; the render thread continues presenting
while they complete.

The worker polls completed fence values, processes slots in capture order, converts the downscaled
copy to limited-range BT.709 NV12, and uses **IMFSinkWriter**:

1. Try a hardware D3D11 device, **IMFDXGIDeviceManager**, and `MF_SINK_WRITER_D3D_MANAGER`, with
   `MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS=TRUE`. Upload NV12 to an independent D3D11 texture and
   pass an `MFCreateDXGISurfaceBuffer` sample to the H.264 writer.
2. If setup or the first surface sample is rejected, recreate the writer with hardware transforms
   disabled and ordinary NV12 memory samples. A later write failure stops the clip instead of
   silently restarting its timeline.

This implementation uses a **downscaled GPU→CPU readback bridge even on the hardware-encoder path**,
followed by D3D11 upload. It does not share D3D12 textures with the encoder or claim zero-copy
encoding. The independent surface per sample lets Media Foundation retain input without blocking
reuse of the capture ring. The fallback works without D3D11 texture input. Actual hardware encoder
availability on Xbox UWP remains a console check; setting the preference alone does not prove
hardware selection. `IMFSinkWriterEx::GetTransformForStream` inspects encoder attributes to log
hardware/software; an inaccessible transform is logged unknown.

Timestamps use elapsed monotonic capture time. Drops leave gaps rather than speeding up playback.
Deadline advancement preserves 30/60 cadence across jitter without emitting catch-up bursts.
Conversion and WriteSample run only on the worker; encoder backpressure fills three slots and then
drops new frames. The worker also stops on elapsed time when gameplay stops presenting. The close
gate and producer guard prevent finalization from racing a late slot publication.

At 1080p the three RGBA targets plus readbacks require about 48 MiB, plus NV12 conversion/surface
storage and the encoder's internal allocations. At 720p the ring is about 21 MiB. Context accounts
the ring allocations against its normal resource budget. Optional capture allocation fails under
pressure instead of using the normal allocator's fence-draining eviction/retry path. No capture
resources, worker thread or Media Foundation startup are needed when recording is unused.

`RECORDER started` reports file, dimensions, fps, input path, encoder and `audio=none`.
`RECORDER stopped` reports file, encoded frames, ring drops, encoder, average
conversion/upload/WriteSample milliseconds and finalization HRESULT. Finalization time is excluded
from the average. An unavailable encoder or other recorder failure is logged and disables recording
for that game session; emulation continues. Failed-finalization and empty files are removed when
possible.

The API preferences follow Microsoft's documentation for
[hardware transforms](https://learn.microsoft.com/en-us/windows/win32/medfound/mf-readwrite-enable-hardware-transforms)
and the
[Sink Writer device manager](https://learn.microsoft.com/en-us/windows/win32/medfound/mf-sink-writer-d3d-manager).
No third-party dependency was added. Windows builds link d3d11, mfplat, mfreadwrite, mfuuid and
runtimeobject.

## Verification and exact console check

Host acceptance (use a Python environment with FontTools, required by the existing Xbox button icon
tests; the recorder adds no Python dependency):

```sh
scratch=$(mktemp -d /tmp/nxbox-recorder-tests.XXXXXX)
(cd tests/port && TMPDIR="$scratch" python3 -m unittest -q)
rm -rf -- "$scratch"
```

`test_recorder.py` compiles the production portable policies and exercises safe filenames, malformed
input, all four output modes and persistence failures, shortcut edges/HOME precedence, full-ring
drops and reuse, producer/consumer ordering, first-frame delay, one-shot start, cap, timestamps,
stalls and sustained 30/60 cadence. The Windows recorder translation unit was also cross-compiled to
an object with MinGW on the Mac. The local MinGW SDK lacks the pre-existing renderer's
`ID3D12GraphicsCommandList8`, so that check supplied a temporary compatibility declaration for that
interface only; it is not a Windows SDK/UWP build or link. The temporary object and declaration are
discarded after checking.

Host tests cannot establish encoder availability, UWP linking, color/orientation on Xbox, playable
mux output or GPU performance.

After building with the Windows SDK (`tools\nxbox\build-uwp.cmd`) and deploying the resulting
package, perform this check on **both Series X and Series S**:

1. Select native DirectX 12, show FPS, and Record video → 720p30. Set `NXBOX_RECORD=20` and
   `NXBOX_RECORD_DELAY=5` in `nxbox_env.txt`. Fully restart and launch a title with visible motion
   and audio. Verify no dot during the initial five seconds, a red dot during capture, then
   automatic stop without input.
2. Pull `LocalState/eden_uwp_diag.txt` and the MP4. Require exactly one started and stopped pair,
   nonzero frames, a sane drop count, an honest encoder selection, and `finalize_hr=0`. Decode the
   entire file and inspect metadata:

   ```sh
   ffprobe -v error -show_entries stream=codec_name,width,height,r_frame_rate -show_entries format=duration,size -of json gameplay.mp4
   ffmpeg -v error -i gameplay.mp4 -f null -
   ```

   Expect H.264, 1280×720, nominal 30 fps, roughly 20 seconds, a valid final seek, FPS overlay and
   dot in the correct corners, correct colors/orientation, and no audio stream. Compare motion and
   aspect ratio to the TV/Portal image.

3. Repeat with 720p60, 1080p30 and 1080p60. Repeat once with `NXBOX_RECORD_SOFTWARE=1` and require
   software / CPU-NV12 in the logs. Record which encoder/input path each console actually selected.
   Repeat with frame generation enabled; generated display frames should be eligible for capture
   without corrupting the real one.
4. Set `NXBOX_RECORD=0`, restart, hold View and click the right stick. Require one start while held,
   a stop only on a fresh click, no guest RStick/Minus press, working View-stick tilt, and unchanged
   View + Menu HOME. Start again and return to the library; require finalization and successful FTP
   download. Also suspend while recording and inspect finalization before resuming.
5. Remove the duration override and let a clip reach the 120-second cap. Test a full archive with
   disposable files: oldest MP4s should be removed, unrelated files preserved, and an unreservable
   volume should produce RECORDER failed while the game continues. Preserve any real recordings
   before this check.
6. Compare the same warm gameplay scene with recording off/on. Under encoder pressure drops should
   increase while emulation continues; capture must not add GPU-thread fence waits. Confirm no
   recorder D3D12 validation errors and no growth beyond the three-slot ring across repeated
   recordings and resolution changes between game sessions.

These console checks are required before claiming Xbox playback or hardware encoding has been
validated. They have not been performed by the host test suite.
