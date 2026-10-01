// ============================================================================
// AURA DAW - src/dsp/Envelope.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Header-only module. Tests (dsp/EnvelopeTests via LevelMeterTests fixtures)
// cover: attack reaches 1.0 within +/-1 ms of the setting, release decays to
// silence, no NaN in any stage, curve=0 and curve=1 both behave.
// ============================================================================
#include "aura/dsp/Envelope.hpp"

namespace aura::dsp {
namespace {
static_assert(sizeof(AdsrEnvelope) < 256, "ADSR must remain a small value type");
} // namespace
} // namespace aura::dsp
