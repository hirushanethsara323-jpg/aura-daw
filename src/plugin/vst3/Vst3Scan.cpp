// ============================================================================
// AURA DAW - src/plugin/vst3/Vst3Scan.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Scanning a VST 3 bundle is the part of hosting that runs untrusted code
// (a plug-in's static initialisers and its factory function) before the user has
// asked for anything. Two consequences shape this file:
//
//   * Scanning is deliberately separable from instantiation, and the caller
//     (PluginScanner) runs it in a child process when it can. Everything here is
//     plain C++ that can live in a tiny scanner executable, so the crash-prone
//     half of hosting does not have to share an address space with the mixer.
//   * A bundle is a container, not a plug-in: one .vst3 may hold an effect, its
//     side-chain variant and an instrument. The host therefore stores one
//     descriptor per class and keeps the class UID in the descriptor id, which is
//     what makes `instantiate()` possible in a different process than the scan.
//
// The SDK types never leave this file: they are translated into AURA's own
// PluginDescriptor, and the instance lives behind the SDK-free PluginInstance
// interface (see Vst3Instance.cpp).
// ============================================================================

#include "Vst3Host.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "aura/core/Json.hpp"
#include "aura/core/Log.hpp"
#include "aura/core/Strings.hpp"

// The SDK's hosting helpers: module loading + factory wrapper. These are the
// headers the SDK itself documents for hosts (see public.sdk/source/vst/hosting/).
#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/vsttypes.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"

namespace aura::plugin::vst3 {
namespace {

constexpr const char* kCategory = "plugin";

using Steinberg::FUnknown;
using Steinberg::Vst::BusDirections;
using Steinberg::Vst::BusInfo;
using Steinberg::Vst::IComponent;
using Steinberg::Vst::IEditController;
using Steinberg::Vst::MediaTypes;
using Steinberg::Vst::ParameterInfo;

/// The descriptor id is the host's whole memory of "which class in which file",
/// so it has to be stable across rescans, unique per class and parseable without
/// the SDK (instantiate() runs in the parent process, which may not have it).
///   VST3:<32 hex UID>@<bundle path>
std::string makeId(const std::string& uid, const std::string& path) {
    return "VST3:" + uid + "@" + path;
}

/// Reads a bundle and probes one of its classes far enough to fill a descriptor:
/// bus counts come from the component's bus layout, the parameter count from the
/// controller, and `hasEditor` from whether a plug-in view can actually be
/// created. All of it is done before the user ever loads the plug-in, which is
/// why it happens in a process the host is willing to lose.
struct Probe {
    int numInputs = 0;
    int numOutputs = 0;
    int numParameters = 0;
    bool hasEditor = false;
    bool usable = false;
    std::string error;
};

Probe probeClass(const VST3::Hosting::PluginFactory& factory, const VST3::Hosting::ClassInfo& info,
                 FUnknown* hostContext) {
    Probe probe;
    auto component = factory.createInstance<IComponent>(info.ID());
    if (!component) {
        probe.error = "the bundle's factory listed the class but could not create it";
        return probe;
    }
    if (component->initialize(hostContext) != Steinberg::kResultOk) {
        probe.error = "IComponent::initialize() failed";
        return probe;
    }

    for (const auto direction : {BusDirections::kInput, BusDirections::kOutput}) {
        const int busCount = component->getBusCount(MediaTypes::kAudio, direction);
        int channels = 0;
        for (int bus = 0; bus < busCount; ++bus) {
            BusInfo busInfo{};
            if (component->getBusInfo(MediaTypes::kAudio, direction, bus, busInfo) !=
                Steinberg::kResultOk)
                continue;
            // Only enabled buses count towards the channel total: a disabled
            // side-chain input is not a channel the engine has to feed.
            if ((busInfo.flags & BusInfo::kDefaultActive) != 0)
                channels += static_cast<int>(busInfo.channelCount);
        }
        if (direction == BusDirections::kInput)
            probe.numInputs = channels;
        else
            probe.numOutputs = channels;
    }

    // A single-component plug-in is its own controller; a split one names the
    // controller class that has to be created separately.
    Steinberg::TUID controllerCid{};
    Steinberg::IPtr<IEditController> controller;
    bool separateController = false;
    if (component->getControllerClassId(controllerCid) == Steinberg::kResultOk) {
        // A split plug-in names its controller class; it has to be created and
        // initialised on its own. Created through the raw factory interface because
        // the UID here is a TUID from getControllerClassId, not one of the SDK's
        // wrapper types.
        IEditController* raw = nullptr;
        if (factory.get()->createInstance(controllerCid, IEditController::iid,
                                          reinterpret_cast<void**>(&raw)) ==
                Steinberg::kResultOk &&
            raw != nullptr) {
            controller = Steinberg::owned(raw);
            separateController = true;
        }
    } else {
        // Single-component plug-in: the component is its own controller, and
        // initialising it a second time is not allowed - it has already been
        // initialised above, and doing it twice is how a scanner ends up refusing
        // perfectly good plug-ins.
        controller = Steinberg::U::cast<IEditController>(component);
    }
    if (controller) {
        if (separateController && controller->initialize(hostContext) != Steinberg::kResultOk) {
            probe.error = "IEditController::initialize() failed";
            component->terminate();
            return probe;
        }
        probe.numParameters = static_cast<int>(controller->getParameterCount());
        if (auto* view = controller->createView(Steinberg::Vst::ViewType::kEditor)) {
            probe.hasEditor = true;
            view->release();
        }
        if (separateController)
            controller->terminate();
    }

    component->terminate();
    probe.usable = true;
    return probe;
}

/// VST 3 puts the useful classification in the sub-categories string
/// ("Fx|Dynamics|Compressor", "Instrument|Synth"), not in the class category,
/// which is always "Audio Module Class" for anything with audio buses.
bool isInstrumentClass(const VST3::Hosting::ClassInfo& info) {
    const std::string subCategories = info.subCategoriesString();
    return strings::toLower(subCategories).find(strings::toLower(Steinberg::Vst::PlugType::kInstrument)) !=
           std::string::npos;
}

} // namespace

AuraResult<std::vector<PluginDescriptor>> scanBundle(const std::string& path) {
    std::string moduleError;
    auto module = VST3::Hosting::Module::create(path, moduleError);
    if (!module) {
        return failure(makeError(ErrorCode::NotFound,
                                 "Could not load the VST 3 bundle: " + path,
                                 moduleError.empty() ? "the module loader gave no reason"
                                                     : moduleError));
    }

    // A plug-in may ask the host for services while it initialises; the SDK ships
    // the standard answer (an IHostApplication with an interface-support list).
    Steinberg::Vst::HostApplication hostContext;

    const auto factory = module->getFactory();
    std::vector<PluginDescriptor> descriptors;
    for (const auto& classInfo : factory.classInfos()) {
        if (classInfo.category() != kVstAudioEffectClass)
            continue; // bypass/component handler classes are not loadable plug-ins
        const Probe probe = probeClass(factory, classInfo, &hostContext);
        if (!probe.usable) {
            AURA_LOG_WARN(kCategory, "Skipped a class in %s: %s", path.c_str(),
                          probe.error.c_str());
            continue;
        }

        PluginDescriptor descriptor;
        descriptor.id = makeId(classInfo.ID().toString(), path);
        descriptor.name = classInfo.name().empty() ? "Unnamed plug-in" : classInfo.name();
        descriptor.vendor = classInfo.vendor().empty() ? "Unknown" : classInfo.vendor();
        descriptor.version = classInfo.version();
        descriptor.path = path;
        descriptor.format = PluginFormat::Vst3;
        descriptor.category = classInfo.subCategoriesString();
        descriptor.isInstrument = isInstrumentClass(classInfo);
        descriptor.categoryId = inferPluginCategory(descriptor.category, descriptor.isInstrument);
        descriptor.numInputs = probe.numInputs;
        descriptor.numOutputs = probe.numOutputs;
        descriptor.hasEditor = probe.hasEditor;
        descriptors.push_back(std::move(descriptor));

        AURA_LOG_INFO(kCategory, "Found %s by %s (%d in / %d out, %d parameters%s)",
                      descriptors.back().name.c_str(), descriptors.back().vendor.c_str(),
                      probe.numInputs, probe.numOutputs, probe.numParameters,
                      probe.hasEditor ? ", has editor" : "");
    }

    if (descriptors.empty()) {
        return failure(makeError(ErrorCode::UnsupportedFormat,
                                 "No VST 3 audio class found in " + path,
                                 "a bundle that loads but exposes no 'Audio Module Class' "
                                 "cannot be hosted"));
    }
    return success(std::move(descriptors));
}

// ---------------------------------------------------------------------------
// Child-process protocol
//
// The scanner parent and child may be different binaries built with different
// feature sets, so the hand-over is a documented JSON object rather than shared
// memory or SDK types. See docs/PLUGIN_HOST.md for the schema.
// ---------------------------------------------------------------------------
std::string scanResultToJson(const std::string& path, bool ok, const std::string& error,
                             const std::vector<PluginDescriptor>& descriptors) {
    json::Value root = json::Value::makeObject();
    root.set("schema", "aura.plugin.scan.v1");
    root.set("path", path);
    root.set("ok", ok);
    root.set("error", error);
    json::Value classes = json::Value::makeArray();
    for (const auto& descriptor : descriptors) {
        json::Value entry = json::Value::makeObject();
        entry.set("id", descriptor.id);
        entry.set("name", descriptor.name);
        entry.set("vendor", descriptor.vendor);
        entry.set("version", descriptor.version);
        entry.set("category", descriptor.category);
        entry.set("categoryId", static_cast<int>(descriptor.categoryId));
        entry.set("format", static_cast<int>(descriptor.format));
        entry.set("isInstrument", descriptor.isInstrument);
        entry.set("numInputs", descriptor.numInputs);
        entry.set("numOutputs", descriptor.numOutputs);
        entry.set("hasEditor", descriptor.hasEditor);
        classes.push(std::move(entry));
    }
    root.set("classes", std::move(classes));
    return root.dump(true);
}

AuraResult<std::vector<PluginDescriptor>> parseScanResult(const std::string& text,
                                                          std::string& pathOut,
                                                          std::string& errorOut) {
    auto parsed = json::parse(text);
    if (!parsed)
        return failure(parsed.error());
    const json::Value& root = parsed.value();
    if (!root.isObject() || root["schema"].asString() != "aura.plugin.scan.v1")
        return failure(makeError(ErrorCode::UnsupportedFormat,
                                 "Unrecognised scanner output",
                                 "expected an \"aura.plugin.scan.v1\" object"));
    pathOut = root["path"].asString();
    errorOut = root["error"].asString();
    if (!root["ok"].asBool(false)) {
        return failure(makeError(ErrorCode::UnsupportedFormat,
                                 "The scanner reported that it could not scan the bundle",
                                 errorOut));
    }

    std::vector<PluginDescriptor> descriptors;
    const json::Value& classes = root["classes"];
    for (std::size_t i = 0; i < classes.size(); ++i) {
        const json::Value& entry = classes[i];
        PluginDescriptor descriptor;
        descriptor.id = entry["id"].asString();
        descriptor.name = entry["name"].asString();
        descriptor.vendor = entry["vendor"].asString();
        descriptor.version = entry["version"].asString();
        descriptor.category = entry["category"].asString();
        descriptor.categoryId =
            static_cast<PluginCategory>(entry["categoryId"].asInt(0));
        descriptor.format = static_cast<PluginFormat>(entry["format"].asInt(0));
        descriptor.isInstrument = entry["isInstrument"].asBool(false);
        descriptor.numInputs = entry["numInputs"].asInt(0);
        descriptor.numOutputs = entry["numOutputs"].asInt(0);
        descriptor.hasEditor = entry["hasEditor"].asBool(false);
        descriptor.path = pathOut;
        if (descriptor.id.empty())
            continue;
        descriptors.push_back(std::move(descriptor));
    }
    if (descriptors.empty())
        return failure(makeError(ErrorCode::UnsupportedFormat, "The scanner returned no classes",
                                 pathOut));
    return success(std::move(descriptors));
}

} // namespace aura::plugin::vst3
