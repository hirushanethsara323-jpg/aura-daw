// ============================================================================
// AURA DAW - plugin/PluginHost.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Plug-in hosting architecture.
//
// LICENSING (researched October 2025 - see docs/research/VST3_RESEARCH.md):
//   * VST 3 SDK is MIT licensed as of VST 3.8 (was GPLv3+/proprietary).
//   * CLAP is MIT licensed (cleveraudio.org).
//   * ASIO SDK is GPLv3-or-proprietary as of October 2025.
//   AURA is GPL-3.0-or-later, so all three are compatible. The headers are NOT
//   vendored here: the SDKs are fetched by CMake when AURA_ENABLE_VST3 /
//   AURA_ENABLE_CLAP are enabled, keeping this repository free of third-party
//   licensed trees and its build reproducible from pinned tags.
//
// ARCHITECTURE:
//   PluginScanner  - out-of-process capable scanning (see below)
//   PluginDatabase - persistent cache of descriptors + user metadata (favourites,
//                    categories, blacklist) stored as JSON
//   PluginInstance - a loaded plug-in owned by the engine
//   PluginChain    - ordered instance list with wet/dry and bypass per slot
//
// PROCESS ISOLATION: scanning runs the untrusted half - a bundle's static
// initialisers and its factory - in a child process (tools/scan_host, driven by
// PluginScanner with a wall-clock limit per bundle), so a plug-in that hangs or
// crashes while being scanned costs a process, not the session. Loading a plug-in
// into the mixer still happens IN-PROCESS: a plug-in that crashes while it is
// being used takes the audio engine down with it. That limitation is explicit, it
// is what the architecture is shaped around, and PluginInstance is the boundary
// that makes the fix mechanical rather than structural - moving a loaded plug-in
// into a helper process later is a matter of swapping the transport (shared
// memory + IPC) instead of rewriting the mixer. See docs/PLUGIN_HOST.md.
// ============================================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <unordered_map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "aura/core/Errors.hpp"
#include "aura/core/Json.hpp"
#include "aura/core/Log.hpp"
#include "aura/dsp/Processor.hpp"
#include "aura/midi/Midi.hpp"

namespace aura::plugin {

enum class PluginFormat : std::int32_t { Unknown = 0, Vst3, Clap, Builtin };

const char* pluginFormatName(PluginFormat format) noexcept;

enum class PluginCategory : std::int32_t {
    Unknown = 0,
    Instrument,
    Effect,
    Dynamics,
    Equalizer,
    Reverb,
    Delay,
    Distortion,
    Modulation,
    Utility,
    Analyzer,
};

const char* pluginCategoryName(PluginCategory category) noexcept;

/// Best-effort classification of a vendor's category string, because vendors are
/// inconsistent and a plug-in that refuses to say what it is still has to appear
/// in the right folder. Only ever used as a suggestion: the user's override wins
/// (PluginUserData::categoryOverride), and nothing functional depends on it.
[[nodiscard]] PluginCategory inferPluginCategory(std::string_view categoryText,
                                                 bool isInstrument) noexcept;

/// Everything known about a plug-in without loading it.
struct PluginDescriptor {
    /// Stable identity: format + class id + path hash. Used as the key for the
    /// cache, favourites and blacklist, so it must survive a rescan.
    std::string id;
    std::string name;
    std::string vendor;
    std::string version;
    std::string path;         ///< bundle/file path
    std::string category;     ///< vendor-supplied category string
    PluginCategory categoryId = PluginCategory::Unknown;
    PluginFormat format = PluginFormat::Unknown;
    bool isInstrument = false;
    int numInputs = 0;
    int numOutputs = 0;
    bool hasEditor = false;
    /// True when the plug-in is known to be incompatible with the current host
    /// configuration (e.g. 32-bit on a 64-bit host).
    bool incompatible = false;
    std::string incompatibleReason;
    std::int64_t lastScanTimeMs = 0;
    double loadTimeMs = 0.0;
};

/// User-level metadata, kept separate from the scanned descriptor so a rescan
/// never loses the user's favourites or category overrides.
struct PluginUserData {
    bool favorite = false;
    bool blacklisted = false;
    std::string blacklistReason;
    PluginCategory categoryOverride = PluginCategory::Unknown;
    std::int64_t lastUsedTimeMs = 0;
    std::string customName;
};

/// Persistent plug-in cache. Plain JSON so users can inspect it, and so a
/// corrupt cache degrades into "rescan" rather than a broken app.
class PluginDatabase {
public:
    void setCachePath(std::string path) { cachePath_ = std::move(path); }
    [[nodiscard]] const std::string& cachePath() const noexcept { return cachePath_; }

    Status load();
    Status save() const;

    /// Adds or updates a descriptor (keeps user data).
    void upsert(PluginDescriptor descriptor);
    bool remove(const std::string& id);

    [[nodiscard]] const std::vector<PluginDescriptor>& descriptors() const noexcept {
        return descriptors_;
    }
    [[nodiscard]] const PluginDescriptor* find(const std::string& id) const noexcept;
    [[nodiscard]] PluginDescriptor* find(const std::string& id) noexcept;

    [[nodiscard]] PluginUserData userData(const std::string& id) const;
    void setUserData(const std::string& id, PluginUserData data);

    /// Queries used by the browser (must be instant: these are linear scans over
    /// the cached descriptor list, which is a few hundred entries in practice).
    [[nodiscard]] std::vector<const PluginDescriptor*> search(
        const std::string& query, bool instruments, bool effects, bool favoritesOnly,
        bool includeBlacklisted = false) const;

    [[nodiscard]] std::vector<const PluginDescriptor*> recentlyUsed(std::size_t count) const;

    [[nodiscard]] std::size_t blacklistCount() const noexcept;

    /// Categories offered by the browser, derived from the descriptors.
    [[nodiscard]] std::vector<PluginCategory> availableCategories() const;

private:
    std::string cachePath_;
    std::vector<PluginDescriptor> descriptors_;
    std::unordered_map<std::string, PluginUserData> userData_;
};

/// A loaded plug-in. The ABI-specific implementation lives behind this
/// interface so the engine, mixer and UI never see VST3/CLAP types.
class PluginInstance {
public:
    virtual ~PluginInstance() = default;

    [[nodiscard]] virtual const PluginDescriptor& descriptor() const noexcept = 0;

    virtual Status prepare(double sampleRate, int maxBlockSize, int numChannels) = 0;
    virtual void reset() noexcept = 0;
    /// Real-time safe: no allocation, no locking, no file IO.
    virtual void process(dsp::AudioBlockView& audio, const dsp::ProcessContext& context,
                         const midi::Message* midiIn, int numMidiIn,
                         midi::Message* midiOut, int& numMidiOut) noexcept = 0;

    [[nodiscard]] virtual int latencySamples() const noexcept = 0;
    [[nodiscard]] virtual int numParameters() const noexcept = 0;
    [[nodiscard]] virtual std::string parameterName(int index) const = 0;
    [[nodiscard]] virtual float parameterValue(int index) const noexcept = 0;
    virtual void setParameterValue(int index, float value) noexcept = 0;
    /// Normalised 0..1 access used by automation lanes (mapped through the
    /// plug-in's own parameter info where available).
    [[nodiscard]] virtual float parameterNormalised(int index) const noexcept = 0;
    virtual void setParameterNormalised(int index, float value) noexcept = 0;

    /// State is opaque bytes: AURA never interprets a plug-in's preset.
    [[nodiscard]] virtual AuraResult<std::vector<std::uint8_t>> saveState() const = 0;
    virtual Status restoreState(const std::vector<std::uint8_t>& state) = 0;

    [[nodiscard]] virtual bool hasEditor() const noexcept = 0;
    /// Editor hosting is a UI concern; this returns an opaque handle the app
    /// layer may cast to its own windowing type.
    virtual void* createEditor() = 0;
    virtual void destroyEditor(void* editor) = 0;

    /// True while the plug-in still produces tail audio (reverb tails etc.).
    [[nodiscard]] virtual bool isActive() const noexcept = 0;

    /// Crash marker: set when a plug-in misbehaved in the previous block so the
    /// engine can bypass it and warn the user instead of repeating the crash.
    void setFailed(bool failed) noexcept { failed_.store(failed, std::memory_order_relaxed); }
    [[nodiscard]] bool hasFailed() const noexcept {
        return failed_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<bool> failed_{false};
};

/// Ordered plug-in chain for a track, with per-slot bypass and wet/dry.
class PluginChain {
public:
    void add(std::unique_ptr<PluginInstance> instance);
    bool remove(const std::string& id);
    bool move(std::size_t from, std::size_t to);
    void clear();

    [[nodiscard]] std::size_t size() const noexcept { return instances_.size(); }
    /// Slot access by index: instances are exposed through at() so the Slot
    /// wrapper (which also carries the bypass flag) stays private.
    [[nodiscard]] PluginInstance* at(std::size_t index) noexcept {
        return index < instances_.size() ? instances_[index].instance.get() : nullptr;
    }
    [[nodiscard]] const PluginInstance* at(std::size_t index) const noexcept {
        return index < instances_.size() ? instances_[index].instance.get() : nullptr;
    }

    void setBypassed(std::size_t index, bool bypassed);
    [[nodiscard]] bool isBypassed(std::size_t index) const noexcept;

    /// Processes the whole chain. A plug-in that has failed is skipped (dry).
    void process(dsp::AudioBlockView& audio, const dsp::ProcessContext& context,
                 std::vector<midi::Message>& midiBuffer, int& activeMidiCount) noexcept;

    [[nodiscard]] int totalLatencySamples() const noexcept;

    /// Saves every slot's state for the project file.
    [[nodiscard]] AuraResult<json::Value> saveStates() const;
    Status restoreStates(const json::Value& states);

private:
    struct Slot {
        std::unique_ptr<PluginInstance> instance;
        bool bypassed = false;
    };
    std::vector<Slot> instances_;
};

/// Scanner. Runs in a worker thread and, by default, in a CHILD PROCESS so a
/// crashing plug-in cannot take AURA down: `tools/scan_host` loads the bundle,
/// the parent kills it if it exceeds `timeoutSecondsPerPlugin`, and a bundle that
/// hangs or dies is quarantined (recorded as unscannable, with the reason) rather
/// than retried. `scanInChildProcess = false` runs the same scan in-process -
/// faster, and the only option in a build that has no helper next to the
/// executable - which is why the option is documented rather than hidden.
class PluginScanner {
public:
    struct Options {
        std::vector<std::string> searchPaths; ///< default VST3/CLAP locations
        bool scanInChildProcess = true;
        int timeoutSecondsPerPlugin = 10;
        bool skipBlacklisted = true;
    };

    /// Default plug-in search locations for Windows (VST3 + CLAP).
    [[nodiscard]] static std::vector<std::string> defaultSearchPaths();

    /// True when the child-process scanner is available in this build.
    [[nodiscard]] static bool childProcessScanningAvailable() noexcept;

    void setOptions(Options options) { options_ = std::move(options); }
    [[nodiscard]] const Options& options() const noexcept { return options_; }

    /// Scans asynchronously. `progress` is called from the worker thread with
    /// (current, total, name) and must be thread-safe.
    void startScan(PluginDatabase& database,
                   std::function<void(int, int, const std::string&)> progress);
    void cancel() noexcept { cancel_.store(true, std::memory_order_relaxed); }
    void waitForCompletion();

    [[nodiscard]] bool isScanning() const noexcept {
        return scanning_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] float progress() const noexcept {
        return progress_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::vector<std::string> lastErrors() const;

private:
    Options options_{};
    std::atomic<bool> scanning_{false};
    std::atomic<bool> cancel_{false};
    std::atomic<float> progress_{0.0f};
    std::vector<std::string> errors_;
    std::thread worker_;
};

/// Loads a plug-in for real, through the adapter for its format. Returns a clear,
/// honest error when AURA was built without the corresponding SDK - the default
/// build hosts VST 3 (AURA_ENABLE_VST3), and the error says so by name rather
/// than pretending the plug-in is broken.
AuraResult<std::unique_ptr<PluginInstance>> instantiatePlugin(const PluginDescriptor& descriptor,
                                                              double sampleRate,
                                                              int maxBlockSize,
                                                              int numChannels);

/// True when plug-in hosting is compiled in for the given format.
[[nodiscard]] bool hostSupports(PluginFormat format) noexcept;

} // namespace aura::plugin
