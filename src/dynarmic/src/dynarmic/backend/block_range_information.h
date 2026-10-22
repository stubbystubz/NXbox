// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <set>

#include <boost/icl/interval_map.hpp>
#include <boost/icl/interval_set.hpp>

#include "common/container/unordered_map.h"
#include "common/container/unordered_set.h"
#include "common/nxbox_memory.h"
#include "dynarmic/ir/location_descriptor.h"

namespace Dynarmic::Backend {

template<typename P>
class BlockRangeInformation {
public:
    void AddRange(boost::icl::discrete_interval<P> range, IR::LocationDescriptor location);
    void ClearCache();
    ::Common::unordered_set<IR::LocationDescriptor> InvalidateRanges(const boost::icl::interval_set<P>& ranges);
    using LocationSet = ::Common::unordered_set<
        IR::LocationDescriptor,
        std::hash<IR::LocationDescriptor>,
        std::equal_to<IR::LocationDescriptor>,
        NxboxMemory::JitAllocator<IR::LocationDescriptor>>;
    boost::icl::interval_map<P, LocationSet, boost::icl::partial_absorber, std::less, boost::icl::inplace_plus, boost::icl::inter_section, boost::icl::discrete_interval<P>, NxboxMemory::JitAllocator> block_ranges;
};

}  // namespace Dynarmic::Backend
