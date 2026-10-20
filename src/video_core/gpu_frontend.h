// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdlib>

namespace Tegra {
// First engine/pusher construction occurs after the environment overrides load.
// Store this latched value in each engine/pusher, keeping static guards off hot paths.
inline bool GpuFrontendFastPathEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("NXBOX_GPU_FRONTEND_FASTPATH");
        return !value || value[0] != '0' || value[1] != '\0';
    }();
    return enabled;
}
// Keep a narrow rollback for bulk method profiling and the last-macro lookup.
inline bool MaxwellMethodBatchingEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("NXBOX_MAXWELL_METHODS");
        return GpuFrontendFastPathEnabled() && (!value || value[0] != '0' || value[1] != '\0');
    }();
    return enabled;
}
} // namespace Tegra
