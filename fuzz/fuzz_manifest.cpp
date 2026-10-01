// ============================================================================
// AURA DAW - fuzz/fuzz_manifest.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AURA DAW Project
//
// Fuzzes the project document loader: the JSON text goes straight into
// Project::deserializeDocument(), which is exactly what Open, the backup fallback
// and crash recovery call (see src/project/Project.cpp). A project file arrives
// from other people over the network, so it is untrusted input by definition.
//
// Beyond "does not crash", the target asserts two properties that matter more than
// the memory safety of the parse:
//   * a document that loads must be a usable project - iterate the tracks, clips
//     and markers, and serialise it back out (a loader that produces a half-built
//     object is a crash waiting for the first UI redraw, which the fuzzer would
//     never see);
//   * a document that loads must survive a save/load cycle: serializeDocument()
//     then deserializeDocument() again must succeed, or saving a project would
//     quietly produce a file that cannot be opened.
// ============================================================================
#include <cstddef>
#include <cstdint>
#include <string>

#include "aura/project/Project.hpp"
#include "fuzz/FuzzSupport.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    // Bound the input: the loader is for documents, not for gigabytes of noise.
    if (size > 512 * 1024)
        return 0;
    const std::string text(reinterpret_cast<const char*>(data), size);

    auto loaded = aura::project::Project::deserializeDocument("/nonexistent-fuzz-dir", text);
    if (!loaded)
        return 0; // rejected: the error path is what a bad file should hit

    aura::project::Project& project = *loaded.value();
    (void)project.sampleRate();
    (void)project.metadata().name;
    for (const auto& track : project.mixer().tracks()) {
        if (!track)
            __builtin_trap(); // a null track in the mix is not "loaded"
        (void)track->clips().size();
        (void)track->inserts().size();
        (void)track->sends().size();
    }
    (void)project.mixer().allTracksIncludingMaster().size();
    (void)project.markers().size();
    (void)project.mediaPool().items().size();

    // Save/load cycle. The serialiser is part of the contract: a document that can
    // be loaded but not written back is a data-loss bug on the next save.
    const std::string reserialized = project.serializeDocument();
    auto reloaded = aura::project::Project::deserializeDocument("/nonexistent-fuzz-dir", reserialized);
    if (!reloaded)
        __builtin_trap();

    return 0;
}
