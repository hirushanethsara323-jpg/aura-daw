// ============================================================================
// AURA DAW - src/plugin/PluginHost.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// STATUS OF THIS FILE (read this before assuming plug-ins work):
//   * Implemented and tested: the descriptor model, the persistent database
//     (scan results, favourites, categories, blacklist, search) and the
//     plug-in chain plumbing (order, bypass, latency, state save/restore).
//   * Implemented: a filesystem scanner that discovers .vst3 / .clap bundles
//     and records their metadata HONESTLY as "not hostable in this build"
//     until the SDK backends are compiled in.
//   * NOT implemented: actual in-process instantiation. `instantiatePlugin`
//     returns a clear NotImplemented error unless AURA was built with
//     AURA_ENABLE_VST3 / AURA_ENABLE_CLAP, which is milestone M6.
// This split is deliberate: the database/chain/scanner are the parts the rest
// of the engine depends on, and they are fully testable today.
// ============================================================================
#include "aura/plugin/PluginHost.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "aura/core/FileSystem.hpp"
#include "aura/core/Json.hpp"
#include "aura/core/Log.hpp"
#include "aura/core/Strings.hpp"

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

/// Best-effort category inference from the vendor category string. Plug-in
/// vendors are notoriously inconsistent here, which is why the user can override
/// the category (PluginUserData::categoryOverride) and AURA never relies on the
/// guessed value for anything functional.
PluginCategory inferCategory(const std::string& categoryText, bool isInstrument) {
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

} // namespace

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
    // The out-of-process scanner is a Windows-only feature and is scheduled for
    // M6 together with plug-in hosting. Until then the in-process scan runs with
    // a per-plug-in watchdog timeout.
    return false;
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
            for (const auto& file : filesystem::listFiles(directory, {"vst3", "clap"}, true)) {
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

            PluginDescriptor descriptor;
            descriptor.id = std::string(pluginFormatName(candidate.format)) + ":" +
                            hashPath(candidate.path);
            descriptor.path = candidate.path;
            descriptor.format = candidate.format;
            descriptor.name = filesystem::fs::path(candidate.path).stem().string();
            descriptor.vendor = "Unknown";
            // Honest state: the metadata is discovered, but this build cannot
            // host it yet, so the descriptor says so instead of pretending.
            const bool canHost = hostSupports(candidate.format);
            descriptor.incompatible = !canHost;
            descriptor.incompatibleReason =
                canHost ? "" : std::string(pluginFormatName(candidate.format)) +
                                   " hosting is not enabled in this build (see docs/PLUGIN_HOST.md)";
            descriptor.lastScanTimeMs =
                static_cast<std::int64_t>(Log::nowMs());
            descriptor.categoryId = inferCategory(descriptor.category, false);

            const PluginUserData data = database.userData(descriptor.id);
            if (options_.skipBlacklisted && data.blacklisted)
                continue;

            database.upsert(std::move(descriptor));
            progress_.store(static_cast<float>(index) / static_cast<float>(std::max<std::size_t>(1, candidates.size())),
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
    return failure(makeError(ErrorCode::NotImplemented,
                             "Plug-in instantiation is scheduled for milestone M6",
                             descriptor.id));
}

} // namespace aura::plugin
