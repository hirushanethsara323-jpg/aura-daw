// ============================================================================
// AURA DAW - src/plugin/vst3/Vst3Host.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The VST 3 side of the plug-in host. This header is private to src/ on purpose:
// it pulls in the Steinberg SDK, and the public API in aura/plugin/PluginHost.hpp
// has to stay compilable in builds where the SDK is not fetched (AURA_ENABLE_VST3
// is OFF by default, and the whole tree builds and tests without it).
//
// LICENSING: the VST 3 SDK is MIT licensed as of VST 3.8 (see
// docs/research/PLUGIN_FORMAT_RESEARCH.md and docs/LICENSES.md). It is fetched by
// CMake at a pinned tag, never vendored, so this repository carries no third-party
// SDK tree and the build is reproducible from that tag.
//
// The adapter's job is narrow on purpose: turn SDK types into AURA's
// PluginDescriptor / PluginInstance. Everything above it (chain order, bypass,
// latency compensation, state storage in the project, the browser) stays free of
// SDK types, which is what lets the same code host a CLAP plug-in or an in-process
// test double without a second implementation of the mixer.
// ============================================================================
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "aura/plugin/PluginHost.hpp"

namespace aura::plugin::vst3 {

/// Scans one VST 3 bundle: loads the module, walks the factory and returns one
/// descriptor per audio-effect/instrument class found inside it.
///
/// A bundle is not a plug-in — it is a container that may hold several classes
/// (an effect and its side-chain variant, for instance) — which is why this
/// returns a vector rather than a single descriptor. Runs anywhere; the scanner
/// calls it in a child process so a crashing bundle cannot take the host down.
[[nodiscard]] AuraResult<std::vector<PluginDescriptor>> scanBundle(const std::string& path);

/// Loads a class that was previously found by scanBundle(). `descriptor.id`
/// carries the VST 3 UID and the bundle path, so no SDK state has to be kept
/// between scanning and loading.
[[nodiscard]] AuraResult<std::unique_ptr<PluginInstance>> instantiate(
    const PluginDescriptor& descriptor, double sampleRate, int maxBlockSize, int numChannels);

/// Serialises scan results for the child-process scanner to hand back to the
/// parent (one JSON object, see docs/PLUGIN_HOST.md for the schema).
[[nodiscard]] std::string scanResultToJson(const std::string& path, bool ok,
                                           const std::string& error,
                                           const std::vector<PluginDescriptor>& descriptors);

/// Parses what scanResultToJson() wrote. Used by the parent process, which may be
/// built without the SDK and therefore cannot scan bundles itself.
[[nodiscard]] AuraResult<std::vector<PluginDescriptor>> parseScanResult(const std::string& json,
                                                                       std::string& pathOut,
                                                                       std::string& errorOut);

} // namespace aura::plugin::vst3
