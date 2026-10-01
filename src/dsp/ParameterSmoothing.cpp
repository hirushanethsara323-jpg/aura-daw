// ============================================================================
// AURA DAW - src/dsp/ParameterSmoothing.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Header-only smoothers; this translation unit exists so the module has a
// stable object file for link-time checks and future non-inline additions.
// ============================================================================
#include "aura/dsp/ParameterSmoothing.hpp"

namespace aura::dsp {
namespace {
// Compile-time checks that the smoothers stay allocation-free and copyable.
static_assert(sizeof(LinearSmoothedValue) <= 64, "LinearSmoothedValue must stay small");
static_assert(std::is_trivially_copyable_v<LinearSmoothedValue> ||
                  std::is_copy_assignable_v<LinearSmoothedValue>,
              "smoothers must be copyable for snapshot-based parameter updates");
} // namespace
} // namespace aura::dsp
