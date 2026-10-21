// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdlib>
#include <string_view>

namespace D3D12 {
constexpr bool Int64AtomicSwitchEnabled(const char* value) {
    return !value || std::string_view{value} != "0";
}
inline bool Int64AtomicsEnabled() {
    static const bool enabled = Int64AtomicSwitchEnabled(std::getenv("NXBOX_D3D12_INT64_ATOMICS"));
    return enabled;
}
inline bool NativeInt64AtomicsEnabled() {
    static const bool enabled =
        Int64AtomicSwitchEnabled(std::getenv("NXBOX_D3D12_NATIVE_INT64_ATOMICS"));
    return enabled;
}
constexpr bool UseNativeInt64Atomics(bool enabled, bool native_enabled, bool sm66,
                                     bool int64_arithmetic, bool descriptor_heap_atomics) {
    return enabled && native_enabled && sm66 && int64_arithmetic && descriptor_heap_atomics;
}
} // namespace D3D12
