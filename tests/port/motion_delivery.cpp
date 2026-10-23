// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>

#include "common/common_types.h"
#include "common/input.h"
#include "eden_uwp/stick_motion.h"
#include "hid_core/frontend/input_converter.h"
#include "hid_core/frontend/motion_input.h"
#include "input_common/input_engine.h"
#include "input_common/input_poller.h"

// The host driver needs no logging thread. Keep all input and motion code real.
namespace Common::Log {
void FmtLogMessageImpl(Class, Level, const char*, unsigned int, const char*, fmt::string_view,
                       const fmt::format_args&) {}
} // namespace Common::Log
void AssertFailSoftImpl() {
    std::abort();
}
void AssertFatalImpl() {
    std::abort();
}

namespace {
class MotionEngine final : public InputCommon::InputEngine {
public:
    MotionEngine() : InputEngine("nxbox") {}
    using InputEngine::SetMotion;
};
Common::ParamPackage MotionMapping(std::size_t slot) {
#include "motion_mapping.h"
    // Also exercise the settings serialization used when HID loads each mapping.
    return Common::ParamPackage{motion.Serialize()};
}

void CheckDelivery(std::size_t slot, bool physical_pad) {
    using namespace std::chrono_literals;
    auto engine = std::make_shared<MotionEngine>();
    InputCommon::InputFactory factory{engine};
    const PadIdentifier pad{.port = slot};
    const auto parameters = MotionMapping(slot);
    assert(parameters.Get("engine", "") == "nxbox");
    std::array<std::unique_ptr<Common::Input::InputDevice>, 2> devices;
    std::array<Core::HID::MotionInput, 2> filters;
    std::array<unsigned int, 2> callbacks{};
    std::array<Common::Vec<float, 3>, 2> delivered{};
    for (std::size_t index = 0; index < devices.size(); ++index) {
        devices[index] = factory.Create(parameters);
        devices[index]->SetCallback({[&, index](const Common::Input::CallbackStatus& callback) {
            const auto raw = Core::HID::TransformToMotion(callback);
            assert(raw.delta_timestamp > 0 && raw.delta_timestamp <= 100000);
            assert(raw.gyro.x.raw_value == raw.gyro.x.value);
            assert(raw.gyro.z.raw_value == raw.gyro.z.value);
            auto& filter = filters[index];
            // Same order as EmulatedController::SetMotion.
            filter.SetAcceleration({raw.accel.x.value, raw.accel.y.value, raw.accel.z.value});
            filter.SetGyroscope({raw.gyro.x.value, raw.gyro.y.value, raw.gyro.z.value});
            filter.SetUserGyroThreshold(raw.gyro.x.properties.threshold);
            filter.UpdateRotation(raw.delta_timestamp);
            filter.UpdateOrientation(raw.delta_timestamp);
            delivered[index] = filter.GetGyroscope();
            ++callbacks[index];
        }});
    }
    EdenXbox::StickMotion synthesis;
    const auto start = EdenXbox::StickMotion::Clock::time_point{};
    const auto send = [&](const EdenXbox::StickMotionSample& sample) {
        const BasicMotion value{
            .gyro_x = sample.gyro[0],
            .gyro_y = sample.gyro[1],
            .gyro_z = sample.gyro[2],
            .accel_x = sample.accel[0],
            .accel_y = sample.accel[1],
            .accel_z = sample.accel[2],
            .delta_timestamp = sample.delta_timestamp,
        };
        engine->SetMotion(pad, 0, value);
    };
    // Both physical and remote sources converge before StickMotion and SetMotion.
    const auto axis = [physical_pad](float pad_axis, bool remote_down) {
        return physical_pad ? pad_axis : (remote_down ? 1.0f : 0.0f);
    };
    send(synthesis.Update(true, false, 0, 0, 0, 0, start));
    for (int frame = 1; frame <= 420; ++frame) {
        const bool yaw = frame <= 240; // Four seconds right, then three seconds up.
        const auto sample =
            synthesis.Update(true, true, 0, 0, axis(yaw ? 1.0f : 0.0f, yaw),
                             axis(yaw ? 0.0f : 1.0f, !yaw), start + frame * 16667us);
        assert(sample.suppress_sticks && !sample.guest_l3);
        send(sample);
        for (std::size_t index = 0; index < devices.size(); ++index) {
            const auto gyro = delivered[index];
            if (yaw) {
                // 0.5 turns/s survives conversion/filtering: pi rad/s at guest output.
                assert(std::abs(gyro[2] + .5f) < .0001f);
            } else {
                assert(std::abs(gyro[0] - .5f) < .0001f);
            }
            assert(callbacks[index] == static_cast<unsigned int>(frame + 1));
        }
    }
    send(synthesis.Update(true, false, 0, 0, 0, 0, start + 8s));
    for (const auto& gyro : delivered)
        assert(gyro.Length() == 0);
    // Small intentional rates survive as well; unrelated ports cannot feed these sensors.
    BasicMotion slow{.gyro_x = .02f, .accel_z = -1.0f, .delta_timestamp = 16667};
    engine->SetMotion(pad, 0, slow);
    for (const auto& gyro : delivered)
        assert(std::abs(gyro[0] - .02f) < .0001f);
    const PadIdentifier other{.port = slot + 4};
    engine->PreSetController(other);
    engine->PreSetMotion(other, 0);
    const auto before_other = callbacks;
    engine->SetMotion(other, 0, slow);
    assert(callbacks == before_other);
    // A below-deadband sample still filters out, rather than disabling drift protection.
    BasicMotion noise{.gyro_x = .001f, .accel_z = -1.0f, .delta_timestamp = 16667};
    engine->SetMotion(pad, 0, noise);
    for (const auto& gyro : delivered)
        assert(gyro.Length() == 0);
}
} // namespace

int main() {
    CheckDelivery(0, false);
    for (std::size_t slot = 0; slot < 4; ++slot)
        CheckDelivery(slot, true);
    std::cout
        << "motion delivery: remote-equivalent player 1 and pad-equivalent ports 0-3 passed\n";
}
