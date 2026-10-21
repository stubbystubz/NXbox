# Wonder 1-2: missing Wonder Seed after the Piranha Plants song

## Finding

The supplied evidence does not justify an audio, clock, or seed-spawn fix. The leading
working hypothesis is **guest progress delayed by rendering/VI backpressure, possibly
starving audio upstream**, while wall time continues. This is an inference from the
source and observed stalls, not an identification of Wonder's script. Neither the
level nor the song's start/end is marked in the logs. A slower song alone does not
explain a seed that never appears: a missed cue, stopped voice, or guest script state
still needs to be demonstrated.

The specific hypothesis that Xbox consumes audio once per presented frame is not
supported by the implementation. XAudio2 has an independent feeder and callback.
Renderer voice progress is decoded source samples, not XAudio2's hardware sample
cursor. Sink starvation can nevertheless hold up renderer work through backpressure.
No Xbox-specific forced clock/speed setting was found. Existing settings overrides
and actual audio progress are absent from the historical captures.

This change is documentation only. No behavior or defaults change, so no new
`NXBOX_<NAME>=0` opt-out or fix regression test is introduced. The existing opt-in
VI experiment is an appropriate console comparison; it is not a confirmed seed fix.

## Evidence from the supplied logs

Paths below are relative to `/Users/miguel/.local/share/nxbox/artifacts/`. Both
October directories, including `run1-fp2`, `run2`, and `panzer`, were inspected as
data. No log contents were executed.

- `asyl-2026-10-18/run1-fp2/eden_uwp_diag.txt` has 5,133 lines; it is an exact
  byte prefix of `run2/eden_uwp_diag.txt` (9,958 lines), which is an exact prefix
  of `asyl-2026-10-19/eden_uwp_diag.txt` (40,953 lines). The crash files have the
  same prefix relationship (62,936, 68,354, and 102,620 lines). These are overlapping
  histories, not three independent reproductions.
- The October 19 crash file identifies Wonder (`010015100B514000`) at lines
  47,660, 62,951, 65,085, and 96,822. The first also appears in the October 18
  `run1-fp2` crash history, although that diagnostic file contains no Wonder
  `GAME_TARGET`. Do not assign the entire file to its folder's named game.
- In the October 19 diagnostic file, Wonder `GAME_TARGET` entries are at lines
  5,167, 6,818, and 35,567. The first two are also in October 18 `run2`.
  Line 35,572 confirms `RESOLUTION title=010015100B514000 resolution_setup=3`.
  `asyl-2026-10-19/panzer/eden_uwp_diag.txt` contains no Wonder target.
- October 19 diagnostic line 5,146 lists the update package as
  `Super Mario Bros. Wonder [010015100B514800][v327680][US][Up v1.2.1].nsp:
  no control NCA`. This is a library-listing skip, not proof the update failed
  to mount or proof it was active. The launched base package's filename also
  does not establish the effective game version. Record that version on console.
- No `Audio`, `underrun`, `TIMING_POLICY`, `GAME_TIMING`, or `VI_TIMING` entry
  occurs in any supplied `eden_uwp_diag.txt` or `nxbox_crash.txt`. In particular,
  absence of an audio error in these two files is not a clean audio diagnostic:
  audio errors use Eden's separate logger, and underrun fill is silent in code.

Selected October 19 diagnostic lines (also present in October 18 `run2` where
the line number is below 9,959):

```text
6777: GAME_PRESENT frames=4 seconds=5.001 fps=0.80 game_fps=4.99 system_fps=1.00 frametime_ms=654.40 speed=100.0% memory=5091770368 memory_limit=5368709120 worst_gap_ms=3150 hitches=1 tick_ms=8389609
6779: GAME_PRESENT frames=0 seconds=5.008 fps=0.00 game_fps=0.40 system_fps=0.00 frametime_ms=-nan speed=100.0% memory=5368709120 memory_limit=5368709120 worst_gap_ms=0 hitches=0 tick_ms=8394609
9854: GAME_PRESENT frames=298 seconds=5.009 fps=59.49 game_fps=56.05 system_fps=59.49 frametime_ms=16.84 speed=100.0% memory=4913008640 memory_limit=5368709120 worst_gap_ms=36 hitches=0 tick_ms=9219718
9956: GAME_PRESENT frames=4 seconds=5.008 fps=0.80 game_fps=30.15 system_fps=0.80 frametime_ms=1163.74 speed=100.0% memory=5363625984 memory_limit=5368709120 worst_gap_ms=4229 hitches=3 tick_ms=9224734
40882: GAME_PRESENT frames=150 seconds=5.000 fps=30.00 game_fps=29.97 system_fps=30.00 frametime_ms=33.33 speed=100.0% memory=3978915840 memory_limit=5368709120 worst_gap_ms=45 hitches=0 tick_ms=1689484
```

There are real 30 FPS stretches, near-60 stretches, and multi-second stalls; these
logs do not establish a universal 30 FPS cap or the FPS during the reported song.
October 19 crash line 64,680 records `event=active_exception what=bad allocation`
at `tick_ms=8394453`, PID 1976, in the Wonder session that starts at line 62,951.
Line 68,243 records `D3D12 async graphics request=d92ca5a26c53db3e failed=bad allocation`
in the next Wonder session. These corroborate memory pressure in those runs, not
an audio command drop or the cause of the missing seed. The later run reaches
`GAME_STOPPED` at diagnostic line 40,938; its 30 FPS sample above is below the
memory limit. Do not explain every occurrence as out-of-memory.

## Source trace, in hypothesis order

### 1. Audio consumption and guest playback progress

| Source | Evidence and implication |
| --- | --- |
| `src/eden_uwp/game_session.cpp:835`, `:961`, `src/audio_core/sink/sink_details.cpp:48` | The game frontend initially chooses XAudio2 unless `NXBOX_AUDIO=null`. Global and then per-title settings can override that choice. `uwp_boot.cpp:53` selects null for a separate headless boot probe; it is not the normal game path. Check effective `sink_id`, not just the environment file. |
| `src/audio_core/sink/xaudio2_sink.cpp:39`, `:56`, `:71`, `:118`, `:144` | 48 kHz PCM; 480 frames per submission (10 ms), four buffers (40 ms). A dedicated `std::jthread` fills the queue; `OnBufferEnd` decrements its count and wakes it. No Present, VI event, GPU fence wait, or frequency-ratio change drives this loop. Host scheduling can still delay it. |
| `src/audio_core/sink/xaudio2_sink.cpp:136`, `:153` | Submission failures decrement the queue count but are not logged. `OnVoiceError` logs `XAudio2 voice error`; initialization/source-voice failures also log errors. There is no periodic queue-depth, callback-gap, or `SamplesPlayed` measurement. A silent submission failure cannot be excluded by these logs. |
| `src/audio_core/sink/sink_stream.cpp:168`, `:191`, `:217` | The shared output routine repeats `last_frame` when no guest buffer is available. Only `actual_frames_written` advances its sample count; filler does not. It does not resample to 30 FPS or accelerate on recovery. Starvation can therefore lengthen audible content without an underrun log. This is shared sink policy, not evidence of an Xbox-specific regression. |
| `src/audio_core/sink/sink_stream.cpp:224`, `src/audio_core/device/device_session.cpp:133` | `GetExpectedPlayedSampleCount` interpolates using global time, caps at frames actually consumed, and adds 1,200 frames (25 ms). Its caller is the device-session path used for audio in/out. It is not the renderer voice status, and on XAudio2 consumption here means filling host submissions, ahead of actual playback. |
| `src/audio_core/renderer/command/data_source/decode.cpp:340`, `:363`, `:400`, `src/audio_core/renderer/voice/voice_info.cpp:264` | Renderer `played_sample_count` increases by decoded **source** samples; loop flags may reset it. `WriteOutStatus` exposes that count, consumed wave buffers, and `voice_dropped` to the guest. New voices report zero. It is not a read of `IXAudio2SourceVoice::GetState().SamplesPlayed`. Whether Wonder uses this count for the musical cue is unverified. |
| `src/audio_core/renderer/system_manager.cpp:29`, `src/audio_core/adsp/apps/audio_renderer/audio_renderer.cpp:132`, `:197` | The renderer manager and DSP have their own high-priority threads. The manager generates commands, signals DSP, then waits for its response. DSP session zero calls `WaitFreeSpace` before processing commands. A slow sink can thus indirectly throttle decoding/status progress, even though it has no direct frame dependency. |
| `src/audio_core/sink/sink_stream.cpp:234`, `src/audio_core/adsp/apps/audio_renderer/audio_renderer.cpp:128` | Renderer ring size is four guest buffers. `WaitFreeSpace` waits up to 7 ms at the limit, then waits with cancellation if depth exceeds the limit by more than three. This is distinct from XAudio2's four 10 ms buffers; do not equate the two queue depths or assume 40 ms is total latency. |
| `src/audio_core/renderer/system.cpp:588`, `:725`, `:755`, `src/audio_core/adsp/apps/audio_renderer/command_list_processor.cpp:58` | Remaining commands are retained for another pass; voice dropping is based on estimated processing budget and disables voice commands. The processor logs invalid magic/buffer bounds, and processes enabled commands. No evidence here ties dropping to each missed presentation. Capture remaining commands, dropped voices and per-voice state before changing this behavior. |

The crucial distinction is **guest production → DSP decode → sink consumption →
XAudio2 playback**. Normal hardware playback of repeated filler does not prove normal
guest voice progress. Conversely, sink sample accounting stopping does not prove the
game's renderer voice counter stopped. Advancing either counter on filler could
fabricate progress and skip guest audio; it is not justified as a seed fix.

### 2. Clock and frame pacing policy

- Shared defaults: `src/common/settings.h:199`, `:206`, `:209`, `:243`, `:263`,
  `:346`, `:593`: multicore on, speed limit on at 100%, `sync_core_speed=false`,
  Normal CPU clock (`fast_cpu_time=0`), FIFO vsync, video-framerate pacing off.
  `game_session.cpp:835` onwards changes sink/rendering/CPU optimization choices,
  not those timing defaults. Global settings are applied at `:861`; per-title
  settings at `:961`. Effective policy is reported at `:1005` in current source.
- `src/core/core_timing.cpp:292` uses wall-clock CNTPCT in multicore; Boost and
  Overclock multiply by 1734/1020 and 2040/1020 (`:51`). `sync_core_speed` divides
  by the configured speed ratio. `GetGlobalTimeNs/Us` (`:390`) uses wall time
  without that multiplier. Thus `GAME_PRESENT speed=100.0%` does not establish
  correct guest CNTPCT or full-rate audio. Half rendered FPS does not itself halve
  the guest counter. Measure `GAME_TIMING clock_mhz` and `guest_mhz` against 19.2.
- `src/core/hle/service/vi/conductor.cpp:37`, `:118`, `:137` schedules composition
  at approximately 60 Hz at normal speed, but normally signals guest vsync only
  **after** composition on its worker. `hardware_composer.cpp:75` waits for the
  previous composite, through `nvdisp_disp0.cpp:97`. The worker's event can coalesce
  ticks while busy. This is a concrete rendering-to-guest-notification dependency;
  it can delay guest work, but no trace proves Wonder's music thread waits on it.
- The existing UWP multicore experiment `NXBOX_VI_INDEPENDENT_VSYNC=1`
  (`vi/vblank_timing.h:19`, `conductor.cpp:47`) moves notification to the timer
  callback; unset or `0` retains legacy order. It also uses base refresh cadence
  for notification (`conductor.cpp:175`). Compare the swap-interval histogram:
  this experiment is not exclusively a change to signal ordering in every case.
- The active native renderer calls `Present(1, 0)` at
  `src/video_core/renderer_d3d12/renderer_d3d12.cpp:545`; `use_vsync=0` is not a
  native-renderer Present A/B. `src/core/perf_stats.cpp:138` skips the general
  speed limiter in multicore. Neither establishes a fixed 30 FPS audio clock.

### 3. What the logs cannot decide

There is no cue timestamp, renderer update cadence, source voice identity, decoded
sample delta, underrun counter, submission result, or XAudio2 playback cursor in
these captures. Separate GPU allocation failures are present, but no captured
AudioRenderer error or dropped-command report can be associated with level 1-2.
Changing queue size, fabricating voice progress, forcing 60 Hz, or disabling voice
dropping globally would be speculative. See also `wonder-vblank-timing.md` for the
existing compositor experiment and its host regression.

## Console capture that would decide it

Preserve the save, original settings/environment, enabled mods, app build identifier,
and effective game/update version. Fully restart NXbox between runs. Start from the
same checkpoint, close the overlay, warm the same shaders, and record external video
**with sound** from before Wonder activation until at least twice the normal sequence
duration after the expected seed cue. Note host/console time and `tick_ms` at sequence
start, audible end, expected seed cue, and actual result. This distinguishes slower
completion from a permanently lost cue. Copy logs immediately before another launch
rotates them. Restore settings afterwards.

First capture as found. For controlled comparisons, set the following in
`LocalState/eden_settings.txt`; temporarily move aside
`LocalState/settings/010015100B514000.txt` so it cannot override the baseline:

```ini
use_multi_core=true
use_speed_limit=true
speed_limit=100
fast_cpu_time=0
sync_core_speed=false
use_video_framerate=false
```

Keep renderer, resolution, update and mods identical. Use these entries in
`LocalState/nxbox_env.txt` for run A (preserve unrelated required entries):

```ini
NXBOX_AUDIO=xaudio2
NXBOX_VI_INDEPENDENT_VSYNC=0
NXBOX_THREAD_PROFILE=1
NXBOX_D3D12_FRAME_PROFILE=1
```

Run B changes only `NXBOX_VI_INDEPENDENT_VSYNC=1`. Run C returns to A and changes
only `NXBOX_AUDIO=null`. Check `TIMING_POLICY sink_id` confirms the effective sink
because settings may override the environment's initial choice. C is only an
isolation experiment: `src/audio_core/sink/null_sink.h:26` discards appended
buffers, has no real playback callback, and bypasses normal queue backpressure.
Success in C implicates audio/pacing interaction but cannot distinguish hardware
failure from renderer pacing, nor establish a correct audio fix.

Collect **all three** logs: `eden_uwp_diag.txt`, `nxbox_crash.txt`, and
`LocalState/eden/log/eden_log.txt` (path construction:
`src/common/fs/path_util.cpp:118`, `:176`; filename: `src/common/fs/fs_paths.h:42`).
For a short error-focused capture, `LocalState/log_filter.txt` can contain
`*:Warning Audio:Info Audio.Sink:Info Service.Audio:Info` on its first line
(`game_session.cpp:792`). This records XAudio2 initialization and audio errors;
it does not enable underrun counters. Leave `dump_audio_commands` off initially
to avoid large command dumps perturbing timing.

Existing measurements and decision rules:

| Log lines | What the comparison decides |
| --- | --- |
| `TIMING_POLICY`, `GAME_TIMING` | Normal multicore should report both clocks near 19.2 MHz despite 30 FPS. Guest-only drift implicates clock/speed policy; both drifting implicates underlying clock calibration. Save the effective policy, not just file contents. |
| `VI_TIMING independent/ticks/signals/compositions/late_max_ms/signal_gap_max_ms/compose_max_ms/swap_*` | At normal cadence expect about 300 timer ticks per five seconds. Fewer legacy signals while composition is blocked, restored by B together with song timing and seed appearance, supports VI backpressure. Signals are broadcast attempts, not proof of guest wakeups. Large timer lateness in both modes points to scheduling starvation. |
| `GAME_PRESENT`, `GAME_STALL`, `D3D12_FRAME_PROFILE`, `THREAD_PROFILE`, crash memory heartbeats and `bad allocation` | Correlate cue failure with presentation gaps, queued work and memory; aggregate FPS alone cannot assign causality. Preserve the complete profile lines, including their actual prefixes. |
| Eden `Audio.Sink` / `Service.Audio` errors | Check `XAudio2 initialisation failed`, `XAudio2 CreateSourceVoice failed`, `XAudio2 voice error`, `Did not receive the expected render response`, invalid command magic/bounds, and voice decode/starvation errors. These can identify a failed path, but their absence cannot rule out silent underruns or submission failure. |

If A/B/C and the existing lines do not decide it, a second **instrumented** console
capture is required. The following are required new measurements, not existing
switches or promised output. No `NXBOX_AUDIO_TRACE` switch currently exists:

1. Per stream/session, monotonic host timestamp, requested sink frames, actual
   guest frames consumed, filler frames, upstream queue depth, feeder gap maximum,
   submitted/completed buffers, submission HRESULT failures and XAudio2
   `GetState` `SamplesPlayed`/`BuffersQueued`. Summarize every second off the audio
   callback; preserve transitions around the cue. A healthy running voice should
   advance hardware frames near 48,000/s, including any filler.
2. Renderer session command-list completions and duration, `WaitFreeSpace` blocked
   time, remaining commands, `num_voices_dropped`, and guest `System::Update`
   cadence. For the song's identified voice, record decoded source-sample deltas,
   input sample rate/pitch, loop/reset flags, play state, wave buffers consumed,
   and the exact status returned to the guest. Source samples are not necessarily
   48 kHz; resets and looping must not be mistaken for stalls.
3. Correlate those measurements with the same audible/seed markers. Hardware
   cursor/queue stopping while upstream data exists isolates sink/feeder failure.
   A 48 kHz hardware cursor with filler and slow decoding isolates upstream
   starvation. Decode progress continuing but reported status stuck isolates
   status propagation. Normal decoded/reported song completion without a seed
   requires a guest cue/script trace; emulator source alone cannot prove which
   game event spawns it. If necessary, trace the guest's status reads and cue
   transition to establish the alleged beat-locked dependency.

## Validation and unverified work

Existing host checks passed on this worktree:

```text
python3 -B -m unittest discover -s tests/port -p 'test_guest_timing.py'
Ran 2 tests — OK
python3 -B -m unittest discover -s tests/port -p 'test_vi_vblank.py'
Ran 3 tests — OK
```

These exercise timing telemetry and the existing blocked-compositor experiment,
not XAudio2, Wonder, or the missing seed. No new host test is claimed for this
documentation-only investigation. No new dependencies or temporary files were added.

Unverified: Xbox/UWP compilation, XAudio2 behavior under console load, the effective
historical timing/sink settings and game update, exact level timestamps, whether
Wonder uses renderer voice status for the cue, the console A/B/C outcomes, and
whether any proposed instrumentation identifies the cue. No console fix, restored
seed, or performance improvement is claimed.
