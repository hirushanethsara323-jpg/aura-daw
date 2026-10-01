// ============================================================================
// AURA DAW - include/aura/core/Sort.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AURA DAW Project
//
// Allocation-free sorting for the audio thread.
//
// std::sort is fine on the audio thread (introsort sorts in place and does not
// allocate), but std::stable_sort allocates a temporary buffer of up to N/2
// elements and therefore must never run inside an audio callback. AURA needs a
// *stable* order in two places on the audio path: MIDI events that share a
// timestamp (a note-off must stay before a note-on at the same tick, or a
// legato phrase gets a stuck note), and messages collected from several clips
// that must keep their per-clip order.
//
// The sequences involved are tiny - the events inside one audio block, i.e. a
// handful of items at any realistic buffer size - so a stable insertion sort is
// not a compromise: it is faster than std::stable_sort at this size, uses no
// heap memory and never throws.
// ============================================================================
#pragma once

#include <iterator>

namespace aura::core {

/// Stable in-place insertion sort. Requires copy-constructible and
/// copy-assignable values (MIDI messages are trivially copyable PODs).
///
/// `compare(a, b)` must be a strict weak ordering, exactly as for std::sort.
template <typename Iterator, typename Compare>
void stableSortSmall(Iterator first, Iterator last, Compare compare) noexcept {
    if (first == last)
        return;
    for (Iterator current = std::next(first); current != last; ++current) {
        auto value = *current;
        Iterator hole = current;
        while (hole != first) {
            Iterator previous = std::prev(hole);
            if (!compare(value, *previous))
                break; // equal elements stop the walk: that is what keeps it stable
            *hole = *previous;
            hole = previous;
        }
        *hole = value;
    }
}

} // namespace aura::core
