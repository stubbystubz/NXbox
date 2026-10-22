// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace D3D12 {
inline std::uint64_t LightingTraceFrame() {
    static const std::uint64_t frame = [] {
        std::uint64_t selected{};
        const char* value = std::getenv("NXBOX_D3D12_LIGHTING_TRACE_FRAME");
        if (!value)
            return selected;
        const std::string_view text{value};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), selected);
        return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() ? selected : 0;
    }();
    return frame;
}

// A one-frame metadata capture, with no readbacks or resource ownership. Count attempts as
// well as submitted draws: rejected draws must not bypass the diagnostic's output budget.
class LightingTraceBudget {
public:
    static constexpr std::uint32_t MaxAttempts = 1024;
    static constexpr std::uint32_t MaxBindings = 8192;

    std::uint32_t Begin(std::uint64_t selected, std::uint64_t frame) {
        if (!selected || selected != frame || attempts == MaxAttempts)
            return 0;
        return ++attempts;
    }

    bool Binding() {
        if (!attempts || bindings == MaxBindings)
            return false;
        ++bindings;
        return true;
    }

    bool BindingLimitReached() const {
        return bindings == MaxBindings;
    }

private:
    std::uint32_t attempts{};
    std::uint32_t bindings{};
};
} // namespace D3D12
