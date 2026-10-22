// SPDX-License-Identifier: GPL-3.0-or-later
// GPU/DXGI doubles surround the actual presenter observation, policy and
// generated path.
#include "video_core/renderer_d3d12/d3d12_present/frame_generation_floor.h"
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <memory>
#include <string_view>
using UINT = unsigned;
constexpr unsigned DXGI_PRESENT_DO_NOT_WAIT = 1;
constexpr int DXGI_ERROR_WAS_STILL_DRAWING = -1, DXGI_ERROR_INVALID_CALL = -2;
struct Resource {
  struct Handle {
    Resource *Get() { return nullptr; }
  } handle;
};
using ResourcePtr = std::shared_ptr<Resource>;
enum State {
  D3D12_RESOURCE_STATE_COPY_SOURCE,
  D3D12_RESOURCE_STATE_COPY_DEST,
  D3D12_RESOURCE_STATE_PRESENT
};
enum class FrameCount { ForcedSubmitPresent };
struct Commands {
  void CopyResource(Resource *, Resource *) {}
};
struct Profile {
  double native_fps{};
  bool frame_generation_gated{true};
};
struct Context {
  struct Profile profile;
  struct Commands commands;
  struct Profile &Profile() { return profile; }
  struct Commands *Commands() { return &commands; }
  void Transition(const ResourcePtr &, State) {}
  void Retain(const ResourcePtr &) {}
  unsigned Submit(FrameCount) { return 1; }
  void Acknowledge(unsigned) {}
  void WaitSubmitted(unsigned, const char *) {}
  void *Device() { return nullptr; }
};
struct Swapchain {
  unsigned index{};
  int Present(unsigned interval, unsigned flags) {
    assert((interval == 1 || interval == 2) && flags <= 1);
    index ^= 1;
    return 0;
  }
  unsigned GetCurrentBackBufferIndex() { return index; }
};
struct RasterizerD3D12 {
  void NotifyFencesSubmitted() {}
};
struct Generator {
  unsigned resets{};
  void Reset() { ++resets; }
  ResourcePtr Current() { return {}; }
};
struct Recorder {
  void Capture(const ResourcePtr &, unsigned, bool) {}
};
namespace Common::FpsOverlay {
std::atomic<unsigned> frame_generation_target{};
void Presented() {}
} // namespace Common::FpsOverlay
namespace fmt {
template <class... Args>
std::string_view format(std::string_view text, Args...) {
  return text;
}
} // namespace fmt
void Diagnostic(std::string_view) {}
void Check(int status, const char *, void *) { assert(status == 0); }
struct Presenter {
  Context context;
  Swapchain chain;
  Swapchain *swapchain{&chain};
  Generator generator;
  Generator *frame_generator{&generator};
  Recorder *recorder{};
  D3D12::Present::NativeFrameGenerationGate native_frame_gate;
  bool frame_generation_enabled{true}, frame_generation_failed{},
      logged_native_gate{};
  unsigned effective_frame_generation_mode{}, logged_native_mode{},
      target_interval{1};
  double output_hz{}, now{};
  unsigned real_calls{}, generated_calls{};
  std::array<ResourcePtr, 2> buffers{};
  std::array<unsigned, 2> present_ticks{};
  unsigned Rtv(unsigned index) { return index; }
  double PresentationTime() const { return now; }
  void RecordPresentation() {}
  void CountPresentCall(bool generated) {
    if (generated)
      ++generated_calls;
    else
      ++real_calls;
  }
  // OBSERVATION
  // GENERATED
  void RealPresent(double refresh) {
    ObserveNativeFrame();
    // POLICY
    CountPresentCall(false);
    swapchain->Present(target_interval, 0);
  }
};
int main() {
  RasterizerD3D12 rasterizer;
  for (double refresh : {59.94, 60., 119.88, 120.}) {
    for (bool alternating : {false, true}) {
      Presenter presenter;
      unsigned transitions = 0;
      bool enabled = false;
      for (unsigned frame = 0; frame < 5940; ++frame) {
        // Same 29.7 wall FPS in both cases. Alternating real intervals model
        // vblank quantization: ~16.7/50.7 ms, not a uniform 33.7 ms stream.
        const double interval =
            alternating ? (frame % 2 == 0 ? 1 / 59.94 : 2 / 29.7 - 1 / 59.94)
                        : 1 / 29.7;
        presenter.now += interval;
        presenter.RealPresent(refresh);
        const bool next_enabled =
            presenter.effective_frame_generation_mode != 0;
        if (next_enabled != enabled)
          ++transitions;
        enabled = next_enabled;
        if (presenter.now >= 5) {
          assert(enabled);
          assert(transitions ==
                 1); // Only initial qualification, zero thereafter.
          assert(std::abs(presenter.context.profile.native_fps - 29.7) < 0.6);
        }
        assert(presenter.context.profile.frame_generation_gated == !enabled);
        if (enabled) {
          const double native_rate = presenter.native_frame_gate.NativeFps();
          const unsigned resets = presenter.generator.resets;
          // Real -> generated -> real, executing the production generated
          // path on alternating swapchain buffers. No native observation.
          presenter.now += interval / 4;
          UINT index = presenter.swapchain->GetCurrentBackBufferIndex();
          presenter.PresentGenerated({}, rasterizer, index);
          presenter.now -= interval / 4;
          assert(presenter.native_frame_gate.NativeFps() == native_rate);
          assert(presenter.generator.resets == resets);
          assert(!presenter.context.profile.frame_generation_gated);
        }
      }
      assert(transitions == 1 && presenter.generator.resets == 2);
      assert(presenter.real_calls == 5940 && presenter.generated_calls > 5800);
    }
  }
}
