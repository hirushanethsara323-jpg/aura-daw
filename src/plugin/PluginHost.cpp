// ============================================================================
// AURA DAW - src/plugin/PluginHost.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// STATUS OF THIS FILE (read this before assuming plug-ins work):
//   * Implemented and tested: the descriptor model, the persistent database
//     (scan results, favourites, categories, blacklist, search), the plug-in
//     chain plumbing (order, bypass, latency, state save/restore) and the
//     scanner, including its child-process mode.
//   * Implemented: VST 3 hosting (src/plugin/vst3/), enabled with
//     AURA_ENABLE_VST3 and verified against a real plug-in in CI. Scanning runs
//     in `aura_scan_host` with a per-bundle timeout; loading runs in-process.
//   * NOT implemented yet: CLAP hosting (AURA_ENABLE_CLAP is reserved, not
//     written) and crash isolation for a *loaded* plug-in, which is sandbox work
//     with its own milestone. Both are stated in docs/PLUGIN_HOST.md rather than
//     being discoverable only by reading this file.
// ============================================================================
#include "aura/plugin/PluginHost.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <system_error>
#include <thread>
#include <vector>

#include "ChildProcess.hpp"
#include "aura/core/FileSystem.hpp"
#include "aura/core/Json.hpp"
#include "aura/core/Log.hpp"
#include "aura/core/Strings.hpp"
#if defined(AURA_ENABLE_VST3)
#include "vst3/Vst3Host.hpp"
#endif

namespace aura::plugin {
namespace {
constexpr const char* kCategory = "Plugin";

/// FNV-1a 64: a stable hash for identity strings. It only has to be stable
/// across runs and machines (the cache and the project file both key on it), not
/// cryptographic.
std::uint64_t stableHash(std::string_view text) noexcept {
    std::uint64_t hash = 1469598103934665603ull;
    for (const char character : text) {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string hashPath(const std::string& path) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%016llx",
                  static_cast<unsigned long long>(stableHash(path)));
    return std::string(buffer);
}

/// Candidate bundles under one search path.
///
/// `filesystem::listFiles()` only returns regular files, which is the wrong shape
/// here: a VST 3 bundle is a *directory* on Windows and on Linux/macOS, so
/// filtering it for files made the scanner blind to every VST 3 plug-in on the
/// platform this project ships to. A bundle is also a leaf - the walk must not
/// descend into it, because the shared library inside is not a plug-in of its own.
std::vector<filesystem::fs::path> findBundleCandidates(const std::string& directory) {
    std::vector<filesystem::fs::path> found;
    std::error_code error;

    // A search path may BE a bundle rather than a folder of them: "scan this
    // plug-in I just downloaded" is a reasonable thing for a user to ask for, and
    // the walk below would otherwise look inside it and find nothing.
    const std::string selfExtension = filesystem::extensionLower(directory);
    if (selfExtension == "vst3" || selfExtension == "clap") {
        if (filesystem::fs::exists(directory, error))
            found.push_back(directory);
        return found;
    }

    if (!filesystem::fs::is_directory(directory, error))
        return found;
    for (auto it = filesystem::fs::recursive_directory_iterator(
             directory, filesystem::fs::directory_options::skip_permission_denied, error);
         it != filesystem::fs::recursive_directory_iterator(); it.increment(error)) {
        if (error)
            break;
        const filesystem::fs::path& path = it->path();
        const std::string extension = filesystem::extensionLower(path);
        if (extension != "vst3" && extension != "clap")
            continue;
        const bool directoryBundle = it->is_directory(error);
        const bool fileBundle = it->is_regular_file(error);
        if (!directoryBundle && !fileBundle)
            continue;
        if (directoryBundle)
            it.disable_recursion_pending();
        found.push_back(path);
    }
    return found;
}

#if defined(AURA_ENABLE_VST3)
/// Temporary file for the scanner child's JSON answer. Unique per call so two
/// scans cannot read each other's result.
std::string scannerOutputPath() {
    static std::atomic<std::uint64_t> counter{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::error_code error;
    const auto directory = filesystem::fs::temp_directory_path(error);
    return (error ? filesystem::fs::path(".") : directory).string() + "/aura-scan-" +
           std::to_string(now) + "-" +
           std::to_string(counter.fetch_add(1, std::memory_order_relaxed)) + ".json";
}
#endif // AURA_ENABLE_VST3

/// Descriptors for one bundle that could not be scanned, so the browser can show
/// the user why instead of the plug-in vanishing without explanation.
PluginDescriptor quarantineDescriptor(const std::string& path, PluginFormat format,
                                      const std::string& reason) {
    PluginDescriptor descriptor;
    descriptor.id = std::string(pluginFormatName(format)) + ":" + hashPath(path);
    descriptor.path = path;
    descriptor.format = format;
    descriptor.name = filesystem::fs::path(path).stem().string();
    descriptor.vendor = "Unknown";
    descriptor.incompatible = true;
    descriptor.incompatibleReason = reason;
    descriptor.lastScanTimeMs = static_cast<std::int64_t>(Log::nowMs());
    return descriptor;
}

#if defined(AURA_ENABLE_VST3)
struct ScanOutcome {
    std::vector<PluginDescriptor> descriptors;
    std::string failureReason; ///< non-empty when the bundle could not be scanned
    bool usedChildProcess = false;
};

/// Scans one bundle, preferring the helper process. Every failure mode comes back
/// as a reason rather than an exception: a scanner that cannot report why a
/// plug-in was quarantined is a scanner whose user cannot fix it.
ScanOutcome scanVst3Bundle(const std::string& path, bool preferChildProcess,
                           int timeoutSeconds) {
    ScanOutcome outcome;
    const std::string helper = preferChildProcess ? helperExecutablePath("aura_scan_host")
                                                  : std::string{};
    if (!helper.empty() && filesystem::exists(helper)) {
        const std::string output = scannerOutputPath();
        ChildProcessRequest request;
        request.executable = helper;
        request.arguments = {"--scan", path, "--out", output};
        request.timeoutSeconds = static_cast<double>(std::max(1, timeoutSeconds));
        const ChildProcessResult result = runChildProcess(request);
        outcome.usedChildProcess = true;

        std::string json;
        if (const auto text = filesystem::readTextFile(output))
            json = text.value();
        (void)filesystem::removeFile(output);

        if (result.timedOut) {
            outcome.failureReason =
                "the scanner did not finish within " +
                strings::fromDouble(request.timeoutSeconds, 0) +
                "s (the plug-in may be showing a dialog or waiting for a licence, or - on "
                "Windows - it may have crashed and be waiting for the system's crash "
                "reporting); the bundle was quarantined";
            return outcome;
        }
        if (result.crashed()) {
            outcome.failureReason = "the scanner crashed while loading this bundle (" +
                                    result.error + "); the bundle was quarantined";
            return outcome;
        }
        if (!result.started) {
            outcome.failureReason = "the scanner could not be started: " + result.error;
            return outcome;
        }
        if (json.empty()) {
            outcome.failureReason = "the scanner produced no answer (" +
                                    (result.error.empty() ? "empty output" : result.error) + ")";
            return outcome;
        }

        std::string scannedPath;
        std::string scanError;
        auto parsed = vst3::parseScanResult(json, scannedPath, scanError);
        if (!parsed) {
            outcome.failureReason = parsed.error().message +
                                    (scanError.empty() ? std::string{} : " - " + scanError);
            return outcome;
        }
        outcome.descriptors = parsed.value();
        return outcome;
    }

    // No helper next to the executable (a partial build, or a build tree that only
    // has the library). The scan is the risky half of hosting, so it says out loud
    // that it is running in-process.
    AURA_LOG_WARN(kCategory,
                  "aura_scan_host was not found next to the application; scanning %s "
                  "in-process, where a crashing plug-in would take AURA with it",
                  path.c_str());
    auto scanned = vst3::scanBundle(path);
    if (!scanned) {
        outcome.failureReason = scanned.error().message;
        if (!scanned.error().detail.empty())
            outcome.failureReason += " - " + scanned.error().detail;
        return outcome;
    }
    outcome.descriptors = scanned.value();
    return outcome;
}
#endif // AURA_ENABLE_VST3

} // namespace

PluginCategory inferPluginCategory(std::string_view categoryText, bool isInstrument) noexcept {
    if (isInstrument)
        return PluginCategory::Instrument;
    const std::string lower = strings::toLower(categoryText);
    if (lower.find("reverb") != std::string::npos)
        return PluginCategory::Reverb;
    if (lower.find("delay") != std::string::npos)
        return PluginCategory::Delay;
    if (lower.find("comp") != std::string::npos || lower.find("dynam") != std::string::npos ||
        lower.find("limit") != std::string::npos || lower.find("gate") != std::string::npos)
        return PluginCategory::Dynamics;
    if (lower.find("eq") != std::string::npos || lower.find("equal") != std::string::npos ||
        lower.find("filter") != std::string::npos)
        return PluginCategory::Equalizer;
    if (lower.find("distort") != std::string::npos || lower.find("drive") != std::string::npos ||
        lower.find("amp") != std::string::npos || lower.find("satur") != std::string::npos)
        return PluginCategory::Distortion;
    if (lower.find("chorus") != std::string::npos || lower.find("flanger") != std::string::npos ||
        lower.find("phaser") != std::string::npos || lower.find("modul") != std::string::npos)
        return PluginCategory::Modulation;
    if (lower.find("analy") != std::string::npos || lower.find("meter") != std::string::npos)
        return PluginCategory::Analyzer;
    if (lower.find("util") != std::string::npos || lower.find("tool") != std::string::npos)
        return PluginCategory::Utility;
    return PluginCategory::Effect;
}

const char* pluginFormatName(PluginFormat format) noexcept {
    switch (format) {
    case PluginFormat::Vst3: return "VST3";
    case PluginFormat::Clap: return "CLAP";
    case PluginFormat::Builtin: return "Built-in";
    case PluginFormat::Unknown: return "Unknown";
    }
    return "Unknown";
}

const char* pluginCategoryName(PluginCategory category) noexcept {
    switch (category) {
    case PluginCategory::Instrument: return "Instrument";
    case PluginCategory::Effect: return "Effect";
    case PluginCategory::Dynamics: return "Dynamics";
    case PluginCategory::Equalizer: return "EQ";
    case PluginCategory::Reverb: return "Reverb";
    case PluginCategory::Delay: return "Delay";
    case PluginCategory::Distortion: return "Distortion";
    case PluginCategory::Modulation: return "Modulation";
    case PluginCategory::Utility: return "Utility";
    case PluginCategory::Analyzer: return "Analyzer";
    case PluginCategory::Unknown: return "Unknown";
    }
    return "Unknown";
}

bool hostSupports(PluginFormat format) noexcept {
#if defined(AURA_ENABLE_VST3)
    if (format == PluginFormat::Vst3)
        return true;
#endif
#if defined(AURA_ENABLE_CLAP)
    if (format == PluginFormat::Clap)
        return true;
#endif
    return format == PluginFormat::Builtin || format == PluginFormat::Unknown;
}

// ---------------------------------------------------------------------------
// PluginDatabase
// ---------------------------------------------------------------------------
Status PluginDatabase::load() {
    descriptors_.clear();
    userData_.clear();
    if (cachePath_.empty() || !filesystem::isRegularFile(cachePath_))
        return success();

    auto document = json::parseFile(cachePath_);
    if (!document) {
        // A corrupt cache must never block the app: report and continue empty
        // (the scanner will rebuild it).
        AURA_LOG_WARN(kCategory, "Plug-in cache is unreadable (%s); it will be rebuilt",
                      document.error().toString().c_str());
        return failure(document.error());
    }

    const json::Value& root = document.value();
    for (const auto& entry : root["plugins"].elements()) {
        PluginDescriptor descriptor;
        descriptor.id = entry["id"].asString();
        descriptor.name = entry["name"].asString();
        descriptor.vendor = entry["vendor"].asString();
        descriptor.version = entry["version"].asString();
        descriptor.path = entry["path"].asString();
        descriptor.category = entry["category"].asString();
        descriptor.categoryId = static_cast<PluginCategory>(entry["categoryId"].asInt(0));
        descriptor.format = static_cast<PluginFormat>(entry["format"].asInt(0));
        descriptor.isInstrument = entry["isInstrument"].asBool(false);
        descriptor.numInputs = entry["numInputs"].asInt(0);
        descriptor.numOutputs = entry["numOutputs"].asInt(0);
        descriptor.hasEditor = entry["hasEditor"].asBool(false);
        descriptor.incompatible = entry["incompatible"].asBool(false);
        descriptor.incompatibleReason = entry["incompatibleReason"].asString();
        descriptor.lastScanTimeMs = entry["lastScanTimeMs"].asInt64();
        descriptor.loadTimeMs = entry["loadTimeMs"].asDouble(0.0);
        if (!descriptor.id.empty())
            descriptors_.push_back(std::move(descriptor));
    }
    for (const auto& entry : root["userData"].elements()) {
        PluginUserData data;
        data.favorite = entry["favorite"].asBool(false);
        data.blacklisted = entry["blacklisted"].asBool(false);
        data.blacklistReason = entry["blacklistReason"].asString();
        data.categoryOverride = static_cast<PluginCategory>(entry["categoryOverride"].asInt(0));
        data.lastUsedTimeMs = entry["lastUsedTimeMs"].asInt64();
        data.customName = entry["customName"].asString();
        userData_[entry["id"].asString()] = data;
    }
    AURA_LOG_INFO(kCategory, "Loaded plug-in cache: %zu plug-ins, %zu user entries",
                  descriptors_.size(), userData_.size());
    return success();
}

Status PluginDatabase::save() const {
    if (cachePath_.empty())
        return failure(makeError(ErrorCode::InvalidArgument, "No plug-in cache path configured"));

    json::Value root = json::Value::makeObject();
    root.set("format", "aura-plugin-cache");
    root.set("version", 1);
    json::Value plugins = json::Value::makeArray();
    for (const auto& descriptor : descriptors_) {
        json::Value entry = json::Value::makeObject();
        entry.set("id", descriptor.id);
        entry.set("name", descriptor.name);
        entry.set("vendor", descriptor.vendor);
        entry.set("version", descriptor.version);
        entry.set("path", descriptor.path);
        entry.set("category", descriptor.category);
        entry.set("categoryId", static_cast<int>(descriptor.categoryId));
        entry.set("format", static_cast<int>(descriptor.format));
        entry.set("isInstrument", descriptor.isInstrument);
        entry.set("numInputs", descriptor.numInputs);
        entry.set("numOutputs", descriptor.numOutputs);
        entry.set("hasEditor", descriptor.hasEditor);
        entry.set("incompatible", descriptor.incompatible);
        entry.set("incompatibleReason", descriptor.incompatibleReason);
        entry.set("lastScanTimeMs", descriptor.lastScanTimeMs);
        entry.set("loadTimeMs", descriptor.loadTimeMs);
        plugins.push(std::move(entry));
    }
    root.set("plugins", std::move(plugins));

    json::Value user = json::Value::makeArray();
    for (const auto& [id, data] : userData_) {
        json::Value entry = json::Value::makeObject();
        entry.set("id", id);
        entry.set("favorite", data.favorite);
        entry.set("blacklisted", data.blacklisted);
        entry.set("blacklistReason", data.blacklistReason);
        entry.set("categoryOverride", static_cast<int>(data.categoryOverride));
        entry.set("lastUsedTimeMs", data.lastUsedTimeMs);
        entry.set("customName", data.customName);
        user.push(std::move(entry));
    }
    root.set("userData", std::move(user));

    return json::writeFile(cachePath_, root, true);
}

void PluginDatabase::upsert(PluginDescriptor descriptor) {
    for (auto& existing : descriptors_) {
        if (existing.id == descriptor.id) {
            // Keep user metadata outside the descriptor, so nothing to merge here
            // beyond the scan results themselves.
            existing = std::move(descriptor);
            return;
        }
    }
    descriptors_.push_back(std::move(descriptor));
}

bool PluginDatabase::remove(const std::string& id) {
    const auto it = std::find_if(descriptors_.begin(), descriptors_.end(),
                                 [&id](const PluginDescriptor& descriptor) {
                                     return descriptor.id == id;
                                 });
    if (it == descriptors_.end())
        return false;
    descriptors_.erase(it);
    userData_.erase(id);
    return true;
}

const PluginDescriptor* PluginDatabase::find(const std::string& id) const noexcept {
    for (const auto& descriptor : descriptors_) {
        if (descriptor.id == id)
            return &descriptor;
    }
    return nullptr;
}

PluginDescriptor* PluginDatabase::find(const std::string& id) noexcept {
    return const_cast<PluginDescriptor*>(std::as_const(*this).find(id));
}

PluginUserData PluginDatabase::userData(const std::string& id) const {
    const auto it = userData_.find(id);
    return it == userData_.end() ? PluginUserData{} : it->second;
}

void PluginDatabase::setUserData(const std::string& id, PluginUserData data) {
    userData_[id] = std::move(data);
}

std::vector<const PluginDescriptor*> PluginDatabase::search(const std::string& query,
                                                            bool instruments, bool effects,
                                                            bool favoritesOnly,
                                                            bool includeBlacklisted) const {
    std::vector<std::pair<int, const PluginDescriptor*>> scored;
    for (const auto& descriptor : descriptors_) {
        const PluginUserData data = userData(descriptor.id);
        if (data.blacklisted && !includeBlacklisted)
            continue;
        if (favoritesOnly && !data.favorite)
            continue;
        if (descriptor.isInstrument && !instruments)
            continue;
        if (!descriptor.isInstrument && !effects)
            continue;

        // Match against the user's custom name too: renaming a plug-in should
        // also make it findable under the new name.
        const std::string haystack =
            data.customName.empty() ? descriptor.name + " " + descriptor.vendor
                                    : data.customName + " " + descriptor.vendor;
        const int score = query.empty() ? 0 : strings::fuzzyScore(haystack, query);
        if (score < 0)
            continue;
        scored.emplace_back(score, &descriptor);
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first)
            return a.first > b.first;
        return a.second->name < b.second->name;
    });
    std::vector<const PluginDescriptor*> results;
    results.reserve(scored.size());
    for (const auto& pair : scored)
        results.push_back(pair.second);
    return results;
}

std::vector<const PluginDescriptor*> PluginDatabase::recentlyUsed(std::size_t count) const {
    std::vector<std::pair<std::int64_t, const PluginDescriptor*>> used;
    for (const auto& descriptor : descriptors_) {
        const auto it = userData_.find(descriptor.id);
        if (it == userData_.end() || it->second.lastUsedTimeMs <= 0)
            continue;
        used.emplace_back(it->second.lastUsedTimeMs, &descriptor);
    }
    std::stable_sort(used.begin(), used.end(), [](const auto& a, const auto& b) {
        return a.first > b.first;
    });
    std::vector<const PluginDescriptor*> results;
    for (std::size_t i = 0; i < std::min(count, used.size()); ++i)
        results.push_back(used[i].second);
    return results;
}

std::size_t PluginDatabase::blacklistCount() const noexcept {
    std::size_t count = 0;
    for (const auto& [id, data] : userData_) {
        if (data.blacklisted)
            ++count;
    }
    return count;
}

std::vector<PluginCategory> PluginDatabase::availableCategories() const {
    std::vector<PluginCategory> categories;
    for (const auto& descriptor : descriptors_) {
        const PluginUserData data = userData(descriptor.id);
        const PluginCategory category = data.categoryOverride != PluginCategory::Unknown
                                            ? data.categoryOverride
                                            : descriptor.categoryId;
        if (std::find(categories.begin(), categories.end(), category) == categories.end())
            categories.push_back(category);
    }
    std::sort(categories.begin(), categories.end(),
              [](PluginCategory a, PluginCategory b) { return static_cast<int>(a) < static_cast<int>(b); });
    return categories;
}

// ---------------------------------------------------------------------------
// PluginChain
// ---------------------------------------------------------------------------
void PluginChain::add(std::unique_ptr<PluginInstance> instance) {
    if (!instance)
        return;
    instances_.push_back(Slot{std::move(instance), false});
}

bool PluginChain::remove(const std::string& id) {
    const auto it = std::find_if(instances_.begin(), instances_.end(),
                                 [&id](const Slot& slot) {
                                     return slot.instance && slot.instance->descriptor().id == id;
                                 });
    if (it == instances_.end())
        return false;
    instances_.erase(it);
    return true;
}

bool PluginChain::move(std::size_t from, std::size_t to) {
    if (from >= instances_.size() || to >= instances_.size() || from == to)
        return false;
    Slot slot = std::move(instances_[from]);
    instances_.erase(instances_.begin() + static_cast<std::ptrdiff_t>(from));
    instances_.insert(instances_.begin() + static_cast<std::ptrdiff_t>(to), std::move(slot));
    return true;
}

void PluginChain::clear() {
    instances_.clear();
}

void PluginChain::setBypassed(std::size_t index, bool bypassed) {
    if (index < instances_.size())
        instances_[index].bypassed = bypassed;
}

bool PluginChain::isBypassed(std::size_t index) const noexcept {
    return index < instances_.size() ? instances_[index].bypassed : true;
}

void PluginChain::process(dsp::AudioBlockView& audio, const dsp::ProcessContext& context,
                          std::vector<midi::Message>& midiBuffer, int& activeMidiCount) noexcept {
    for (auto& slot : instances_) {
        if (!slot.instance || slot.bypassed || slot.instance->hasFailed())
            continue;
        int midiOut = 0;
        midi::Message midiOutput[64];
        slot.instance->process(audio, context, midiBuffer.data(), activeMidiCount, midiOutput,
                               midiOut);
        // Plug-ins that generate MIDI (arpeggiators) hand it back into the buffer
        // for instruments further down the chain.
        for (int i = 0; i < midiOut && activeMidiCount < 64; ++i)
            midiBuffer[static_cast<std::size_t>(activeMidiCount++)] = midiOutput[i];
    }
}

int PluginChain::totalLatencySamples() const noexcept {
    int total = 0;
    for (const auto& slot : instances_) {
        if (slot.instance && !slot.bypassed)
            total += slot.instance->latencySamples();
    }
    return total;
}

AuraResult<json::Value> PluginChain::saveStates() const {
    json::Value array = json::Value::makeArray();
    for (const auto& slot : instances_) {
        if (!slot.instance)
            continue;
        json::Value entry = json::Value::makeObject();
        entry.set("id", slot.instance->descriptor().id);
        entry.set("name", slot.instance->descriptor().name);
        entry.set("bypassed", slot.bypassed);
        auto state = slot.instance->saveState();
        if (!state)
            return failure(state.error());
        // Base64-ish hex keeps the document valid JSON without a binary blob.
        std::string encoded;
        encoded.reserve(state.value().size() * 2);
        for (const std::uint8_t byte : state.value()) {
            char buffer[4];
            std::snprintf(buffer, sizeof(buffer), "%02x", byte);
            encoded += buffer;
        }
        entry.set("state", encoded);
        array.push(std::move(entry));
    }
    return success(std::move(array));
}

Status PluginChain::restoreStates(const json::Value& states) {
    // Matching is by plug-in id in chain order; missing instances are reported
    // by the caller (the project keeps the descriptor so the UI can offer to
    // re-instantiate).
    for (std::size_t i = 0; i < states.elements().size() && i < instances_.size(); ++i) {
        const json::Value& entry = states[i];
        if (!instances_[i].instance)
            continue;
        const std::string encoded = entry["state"].asString();
        std::vector<std::uint8_t> bytes;
        bytes.reserve(encoded.size() / 2);
        for (std::size_t c = 0; c + 1 < encoded.size(); c += 2) {
            std::uint32_t value = 0;
            if (!strings::fromHex(encoded.substr(c, 2), value))
                return failure(makeError(ErrorCode::ParseError, "Corrupt plug-in state blob"));
            bytes.push_back(static_cast<std::uint8_t>(value));
        }
        instances_[i].bypassed = entry["bypassed"].asBool(false);
        const Status restored = instances_[i].instance->restoreState(bytes);
        if (!restored)
            return restored;
    }
    return success();
}

// ---------------------------------------------------------------------------
// PluginScanner
// ---------------------------------------------------------------------------
std::vector<std::string> PluginScanner::defaultSearchPaths() {
    std::vector<std::string> paths;
#if defined(_WIN32)
    if (const char* programFiles = std::getenv("ProgramFiles")) {
        paths.push_back(std::string(programFiles) + "\\Common Files\\VST3");
        paths.push_back(std::string(programFiles) + "\\Common Files\\CLAP");
    }
    if (const char* programFilesX86 = std::getenv("ProgramFiles(x86)"))
        paths.push_back(std::string(programFilesX86) + "\\Common Files\\VST3");
#elif defined(__APPLE__)
    paths.push_back("/Library/Audio/Plug-Ins/VST3");
    paths.push_back(std::string(std::getenv("HOME") ? std::getenv("HOME") : "") +
                    "/Library/Audio/Plug-Ins/VST3");
#else
    paths.push_back("/usr/lib/vst3");
    paths.push_back("/usr/local/lib/vst3");
    if (const char* home = std::getenv("HOME"))
        paths.push_back(std::string(home) + "/.vst3");
#endif
    return paths;
}

bool PluginScanner::childProcessScanningAvailable() noexcept {
    // The capability is a property of the installation, not of the platform:
    // `aura_scan_host` is built next to the application on every platform, and a
    // build that did not produce it (or a user who deleted it) falls back to the
    // in-process scan with a warning instead of failing.
    const std::string helper = helperExecutablePath("aura_scan_host");
    return !helper.empty() && filesystem::exists(helper);
}

void PluginScanner::startScan(PluginDatabase& database,
                              std::function<void(int, int, const std::string&)> progress) {
    if (scanning_.exchange(true))
        return;
    cancel_.store(false, std::memory_order_relaxed);
    progress_.store(0.0f, std::memory_order_relaxed);

    worker_ = std::thread([this, &database, progress = std::move(progress)] {
        errors_.clear();

        // 1. Discover candidate bundles.
        struct Candidate {
            std::string path;
            PluginFormat format;
        };
        std::vector<Candidate> candidates;
        for (const auto& directory : options_.searchPaths) {
            for (const auto& file : findBundleCandidates(directory)) {
                const std::string extension = filesystem::extensionLower(file);
                const PluginFormat format =
                    extension == "vst3" ? PluginFormat::Vst3
                                        : (extension == "clap" ? PluginFormat::Clap
                                                               : PluginFormat::Unknown);
                if (format == PluginFormat::Unknown)
                    continue;
                candidates.push_back({file.string(), format});
            }
        }

        AURA_LOG_INFO(kCategory, "Scanning %zu plug-in bundles across %zu folders",
                      candidates.size(), options_.searchPaths.size());

        int index = 0;
        for (const auto& candidate : candidates) {
            if (cancel_.load(std::memory_order_relaxed))
                break;
            ++index;
            if (progress)
                progress(index, static_cast<int>(candidates.size()), candidate.path);

            // User data (favourites, category overrides, the blacklist) is keyed by
            // descriptor id and never touched by a scan, so a rescan cannot lose it.
            const auto stage = [&](PluginDescriptor descriptor) {
                const PluginUserData data = database.userData(descriptor.id);
                if (options_.skipBlacklisted && data.blacklisted)
                    return;
                database.upsert(std::move(descriptor));
            };

#if defined(AURA_ENABLE_VST3)
            if (candidate.format == PluginFormat::Vst3) {
                ScanOutcome outcome = scanVst3Bundle(candidate.path, options_.scanInChildProcess,
                                                     options_.timeoutSecondsPerPlugin);
                if (outcome.descriptors.empty()) {
                    errors_.push_back(candidate.path + ": " + outcome.failureReason);
                    PluginDescriptor quarantined = quarantineDescriptor(
                        candidate.path, PluginFormat::Vst3, outcome.failureReason);
                    stage(std::move(quarantined));
                } else {
                    // A bundle is a container: several classes, each its own
                    // descriptor. Entries that belong to this bundle but were not
                    // found this time are dropped, which is how a plug-in update
                    // that removes a class cleans up after itself.
                    std::vector<std::string> seen;
                    for (auto& descriptor : outcome.descriptors) {
                        descriptor.lastScanTimeMs = static_cast<std::int64_t>(Log::nowMs());
                        seen.push_back(descriptor.id);
                        stage(descriptor);
                    }
                    for (const auto& existing : database.descriptors()) {
                        if (existing.path != candidate.path)
                            continue;
                        const bool stillThere =
                            std::find(seen.begin(), seen.end(), existing.id) != seen.end();
                        if (!stillThere)
                            database.remove(existing.id);
                    }
                }
                progress_.store(static_cast<float>(index) /
                                    static_cast<float>(std::max<std::size_t>(1, candidates.size())),
                                std::memory_order_relaxed);
                continue;
            }
#endif

            // Formats this build cannot host are still listed, with the reason, so
            // the browser can tell "not installed" apart from "not supported here".
            const bool canHost = hostSupports(candidate.format);
            PluginDescriptor descriptor = quarantineDescriptor(
                candidate.path, candidate.format,
                canHost ? std::string{} :
                          std::string(pluginFormatName(candidate.format)) +
                              " hosting is not enabled in this build (see docs/PLUGIN_HOST.md)");
            descriptor.incompatible = !canHost;
            descriptor.categoryId = inferPluginCategory(descriptor.category, false);
            stage(std::move(descriptor));
            progress_.store(static_cast<float>(index) /
                                static_cast<float>(std::max<std::size_t>(1, candidates.size())),
                            std::memory_order_relaxed);
        }

        (void)database.save();
        progress_.store(1.0f, std::memory_order_relaxed);
        scanning_.store(false, std::memory_order_release);
        AURA_LOG_INFO(kCategory, "Plug-in scan complete: %d bundles examined", index);
    });
}

void PluginScanner::waitForCompletion() {
    if (worker_.joinable())
        worker_.join();
}

std::vector<std::string> PluginScanner::lastErrors() const {
    return errors_;
}

// ---------------------------------------------------------------------------
// Instantiation (honest "not in this build" path)
// ---------------------------------------------------------------------------
AuraResult<std::unique_ptr<PluginInstance>> instantiatePlugin(const PluginDescriptor& descriptor,
                                                              double sampleRate,
                                                              int maxBlockSize,
                                                              int numChannels) {
    (void)sampleRate;
    (void)maxBlockSize;
    (void)numChannels;
    if (!hostSupports(descriptor.format)) {
        return failure(makeError(
            ErrorCode::NotImplemented,
            "Plug-in hosting for " + std::string(pluginFormatName(descriptor.format)) +
                " is not compiled into this build",
            "descriptor: " + descriptor.id +
                " (enable AURA_ENABLE_VST3 / AURA_ENABLE_CLAP and see docs/PLUGIN_HOST.md)"));
    }
#if defined(AURA_ENABLE_VST3)
    if (descriptor.format == PluginFormat::Vst3)
        return vst3::instantiate(descriptor, sampleRate, maxBlockSize, numChannels);
#endif
    return failure(makeError(ErrorCode::NotImplemented,
                             "Plug-in instantiation for this format is not compiled into this "
                             "build",
                             std::string(pluginFormatName(descriptor.format)) + ": " +
                                 descriptor.id));
}

} // namespace aura::plugin
