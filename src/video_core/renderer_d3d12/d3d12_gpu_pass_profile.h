// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <compare>
#include <cstdint>
#include <memory>
#include <string_view>

namespace D3D12 {
class Context;
class CommandFacade;
enum class GpuPassKind { Draw, Compute, Copy, Clear, Blit };
struct GpuPassTarget {
    std::uint32_t width{}, height{}, format{}, depth{};
    std::uint64_t shader_hash{};
    auto operator<=>(const GpuPassTarget&) const = default;
};
// No allocation, clock read, query, or virtual call on the disabled path: the facade
// tests its null sink before entering any profiling code.
class GpuPassProfile {
public:
    static bool Enabled();
    explicit GpuPassProfile(Context& context);
    ~GpuPassProfile();
    void BeginSubmission(CommandFacade& commands);
    void EndSubmission();
    void EndFrame();
    void Work(GpuPassKind kind);
    void Boundary();
    void Label(std::string_view label);
    void Target(GpuPassTarget target);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace D3D12
