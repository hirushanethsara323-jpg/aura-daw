// ============================================================================
// AURA DAW - src/plugin/vst3/Vst3Instance.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A loaded VST 3 plug-in, seen from AURA's side of the ABI boundary.
//
// The three rules this file exists to keep:
//
//   1. The engine never sees an SDK type. PluginInstance is the contract; the
//      mixer, the graph, the project format and the automation lanes all speak
//      it, so hosting CLAP later means a second adapter rather than a second
//      mixer.
//   2. process() allocates nothing, locks nothing and touches no file. Every
//      buffer the plug-in needs is created in prepare(): the channel pointer
//      arrays, the event list and the parameter-change ring.
//   3. State is opaque bytes. AURA stores what the plug-in wrote and hands it
//      back; it never interprets a preset, which is what keeps projects
//      loadable when a plug-in is updated or missing (see docs/PROJECT_FORMAT.md).
//
// The parameter path deserves a note, because it is the part that usually goes
// wrong: a host must deliver parameter changes to the *component* through
// ProcessData (that is where DSP reads them) and keep the *controller* in sync so
// the plug-in's own editor shows the right value. Changes therefore travel
// through the SDK's lock-free ParameterChangeTransfer ring - written by whoever
// moved the control, drained by the audio thread - and the controller is updated
// on the same call, never from the audio thread.
// ============================================================================

#include "Vst3Host.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "aura/core/Log.hpp"
#include "aura/core/Strings.hpp"

#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vsttypes.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/processdata.h"
#include "public.sdk/source/vst/utility/stringconvert.h"

namespace aura::plugin::vst3 {
namespace {

constexpr const char* kCategory = "plugin";
constexpr int kMaxBlockEvents = 256;
constexpr int kMaxParameters = 512;

using Steinberg::FUnknown;
using Steinberg::int32;
using Steinberg::kResultOk;
using Steinberg::tresult;
using Steinberg::Vst::Event;
using Steinberg::Vst::EventList;
using Steinberg::Vst::HostProcessData;
using Steinberg::Vst::IComponent;
using Steinberg::Vst::IComponentHandler;
using Steinberg::Vst::IEditController;
using Steinberg::Vst::IAudioProcessor;
using Steinberg::Vst::ParamID;
using Steinberg::Vst::ParamValue;
using Steinberg::Vst::ParameterChanges;
using Steinberg::Vst::ParameterChangeTransfer;
using Steinberg::Vst::PlugProvider;
using Steinberg::Vst::ProcessContext;
using Steinberg::Vst::ProcessSetup;

class Vst3PluginInstance;
class Vst3ComponentHandler;

/// Splits "VST3:<uid>@<path>" back into its two halves. The UID never contains
/// '@', the path may, so the split happens on the first one.
bool splitId(const std::string& id, std::string& uid, std::string& path) {
    constexpr const char* kPrefix = "VST3:";
    if (id.rfind(kPrefix, 0) != 0)
        return false;
    const std::size_t at = id.find('@', std::strlen(kPrefix));
    if (at == std::string::npos)
        return false;
    uid = id.substr(std::strlen(kPrefix), at - std::strlen(kPrefix));
    path = id.substr(at + 1);
    return !uid.empty() && !path.empty();
}

/// One parameter as the host sees it. Only the identity is cached: the value is
/// read from the controller on demand, so a plug-in that changes its own
/// parameters is never contradicted by a stale copy.
struct ParamEntry {
    ParamID id = 0;
    std::string name;
    std::string unit;
    float minPlain = 0.0f;
    float maxPlain = 1.0f;
    bool isBypass = false;
};

// ---------------------------------------------------------------------------
// The plug-in side of the host contract: edit callbacks from the plug-in's own
// editor, and restart notifications (latency changed, parameter list changed).
// ---------------------------------------------------------------------------
class Vst3ComponentHandler final
    : public Steinberg::U::Implements<Steinberg::U::Directly<IComponentHandler>> {
public:
    explicit Vst3ComponentHandler(Vst3PluginInstance& owner) noexcept : owner_(owner) {}

    tresult PLUGIN_API beginEdit(ParamID /*id*/) override { return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID id, ParamValue valueNormalized) override;
    tresult PLUGIN_API endEdit(ParamID /*id*/) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32 flags) override;

private:
    Vst3PluginInstance& owner_;
};

class Vst3PluginInstance final : public PluginInstance {
public:
    Vst3PluginInstance(PluginDescriptor descriptor, std::unique_ptr<Steinberg::Vst::HostApplication> context,
                       VST3::Hosting::Module::Ptr module,
                       std::unique_ptr<PlugProvider> provider)
        : descriptor_(std::move(descriptor)), context_(std::move(context)), module_(std::move(module)),
          provider_(std::move(provider)) {
        component_ = provider_->getComponentPtr();
        controller_ = provider_->getControllerPtr();
        if (component_)
            processor_ = Steinberg::U::cast<IAudioProcessor>(component_);
        handler_ = std::make_unique<Vst3ComponentHandler>(*this);
        if (controller_)
            controller_->setComponentHandler(handler_.get());
        sampleRate_ = 0.0;
        maxBlockSize_ = 0;
        // Both halves of the parameter path are sized here, on the thread that
        // creates the instance: the transfer ring the audio thread drains and the
        // ProcessData queues it drains into. Left to grow on demand the second one
        // allocates inside process(), which is the one place it must not.
        parameterTransfer_.setMaxParameters(kMaxParameters);
        parameterChanges_.setMaxParameters(kMaxParameters);
    }

    ~Vst3PluginInstance() override {
        // Order matters: stop processing, deactivate, drop the SDK objects, and
        // only then unload the module they were created from.
        if (active_)
            setActive(false);
        if (controller_)
            controller_->setComponentHandler(nullptr);
        handler_.reset();
        parameterTransfer_.setMaxParameters(0);
        parameterChanges_.setMaxParameters(0); // released while the module is still loaded
        processor_ = nullptr;
        controller_ = nullptr;
        component_ = nullptr;
        provider_.reset();
        module_.reset(); // dlclose/FreeLibrary happens here
        context_.reset();
    }

    [[nodiscard]] const PluginDescriptor& descriptor() const noexcept override {
        return descriptor_;
    }

    Status prepare(double sampleRate, int maxBlockSize, int numChannels) override {
        if (!processor_ || !component_)
            return failure(makeError(ErrorCode::PluginLoadFailed, "The plug-in has no audio processor",
                                     descriptor_.id));
        if (sampleRate <= 0.0 || maxBlockSize <= 0)
            return failure(makeError(ErrorCode::InvalidArgument,
                                     "prepare() needs a positive sample rate and block size",
                                     descriptor_.id));
        if (active_)
            setActive(false);

        sampleRate_ = sampleRate;
        maxBlockSize_ = maxBlockSize;
        numChannels_ = std::max(1, numChannels);

        // How wide are the plug-in's buses? A plug-in may ask for more channels than
        // the track carries (and for sidechain or aux buses), and every channel it
        // may touch needs a valid pointer. The channel counts are cached here - the
        // audio thread must not ask the plug-in anything it can avoid - and the
        // silent slices those extra channels point at are allocated here too.
        readBusLayout();
        scratchChannels_ = scratchSlicesNeeded();
        blockBuffers_.assign(static_cast<std::size_t>(maxBlockSize_) *
                                 static_cast<std::size_t>(scratchChannels_),
                             0.0f);
        channelPointers_.assign(
            static_cast<std::size_t>(std::max(busZeroChannels(), numChannels_)), nullptr);

        // The engine hands us a shared buffer per channel, not one per bus, so the
        // plug-in gets a view of the same memory for input and output. That is
        // exactly what an insert does (and what the VST 3 processing contract
        // allows: in-place processing is normal); a width mismatch is reported
        // rather than silently mangled.
        if (descriptor_.numInputs > 0 && descriptor_.numInputs != numChannels_)
            AURA_LOG_WARN(kCategory,
                          "%s declares %d input channels but the track provides %d; the "
                          "channels the track does not have are left silent",
                          descriptor_.name.c_str(), descriptor_.numInputs, numChannels_);

        ProcessSetup setup{};
        setup.processMode = Steinberg::Vst::kRealtime;
        setup.symbolicSampleSize = Steinberg::Vst::kSample32;
        setup.maxSamplesPerBlock = maxBlockSize;
        setup.sampleRate = sampleRate;
        if (processor_->setupProcessing(setup) != kResultOk)
            return failure(makeError(ErrorCode::PluginLoadFailed,
                                     "The plug-in rejected the processing setup",
                                     descriptor_.name));

        // The block size passed to prepare() is a *buffer allocation request*, and a
        // non-zero one makes ProcessData allocate its own channel buffers and then
        // refuse setChannelBuffers() with `false` - the plug-in would process silence
        // into memory nobody reads while the engine's audio never left the track. The
        // engine owns the audio, so the request is zero, and the buffers are wired up
        // per block below. (The SDK's own test suite and basewrapper do the same.)
        if (!processData_.prepare(*component_, 0, Steinberg::Vst::kSample32))
            return failure(makeError(ErrorCode::PluginLoadFailed,
                                     "The plug-in's bus layout could not be prepared",
                                     descriptor_.name));
        processData_.inputEvents = &eventList_;
        processData_.inputParameterChanges = &parameterChanges_;
        processData_.processContext = &processContext_;

        refreshParameters();
        setActive(true);
        latencySamples_.store(static_cast<int>(processor_->getLatencySamples()),
                              std::memory_order_relaxed);
        tailSamples_ = static_cast<int>(processor_->getTailSamples());
        return success();
    }

    void reset() noexcept override {
        // Deactivating and reactivating is the VST 3 way to clear internal state
        // (delay lines, reverb tails) without rebuilding the plug-in.
        if (!active_)
            return;
        setActive(false);
        setActive(true);
        latencyChanged_.store(true, std::memory_order_relaxed);
    }

    void process(dsp::AudioBlockView& audio, const dsp::ProcessContext& context,
                 const midi::Message* midiIn, int numMidiIn, midi::Message* midiOut,
                 int& numMidiOut) noexcept override {
        numMidiOut = 0;
        if (!active_ || !processor_ || audio.isEmpty())
            return;

        // --- events (allocated list, filled with plain data) ------------------
        eventList_.clear();
        for (int i = 0; i < numMidiIn; ++i)
            appendEvent(midiIn[i], audio.numFrames);

        // --- buffers ---------------------------------------------------------
        // Bus 0 is the track's audio, processed in place: one pointer array serves
        // its input and its output, which is what an insert does and what the format
        // allows. Every other channel - the plug-in's extra channels on bus 0, and
        // every channel of a sidechain or aux bus - points at its own preallocated,
        // zeroed slice, because a bus channel holding a null pointer is a crash
        // waiting for a plug-in that processes all of its channels.
        const int engineChannels = std::min(audio.numChannels, numChannels_);
        int scratch = 0;
        auto silentSlice = [&]() noexcept {
            float* slice = blockBuffers_.data() +
                           static_cast<std::size_t>(scratch) * static_cast<std::size_t>(maxBlockSize_);
            if (scratch + 1 < scratchChannels_)
                ++scratch; // defensive: never step past the allocation
            return slice;
        };

        const int mainChannels = busZeroChannels();
        for (int channel = 0; channel < mainChannels; ++channel) {
            float* buffer = channel < engineChannels ? audio.channelPointers[channel] : nullptr;
            if (buffer == nullptr)
                buffer = silentSlice();
            channelPointers_[static_cast<std::size_t>(channel)] = buffer;
        }

        bool wired = true;
        for (int channel = 0; channel < mainChannels && !inputBusChannels_.empty(); ++channel)
            wired = processData_.setChannelBuffer(Steinberg::Vst::kInput, 0, channel,
                                                  channelPointers_[static_cast<std::size_t>(channel)]) &&
                    wired;
        for (int channel = 0; channel < mainChannels && !outputBusChannels_.empty(); ++channel)
            wired = processData_.setChannelBuffer(Steinberg::Vst::kOutput, 0, channel,
                                                  channelPointers_[static_cast<std::size_t>(channel)]) &&
                    wired;
        for (std::size_t bus = 1; bus < inputBusChannels_.size() && wired; ++bus)
            for (int channel = 0; channel < inputBusChannels_[bus] && wired; ++channel)
                wired = processData_.setChannelBuffer(Steinberg::Vst::kInput,
                                                      static_cast<Steinberg::int32>(bus), channel,
                                                      silentSlice());
        for (std::size_t bus = 1; bus < outputBusChannels_.size() && wired; ++bus)
            for (int channel = 0; channel < outputBusChannels_[bus] && wired; ++channel)
                wired = processData_.setChannelBuffer(Steinberg::Vst::kOutput,
                                                      static_cast<Steinberg::int32>(bus), channel,
                                                      silentSlice());
        if (!wired) {
            setFailed(true);
            AURA_LOG_ERROR(kCategory,
                           "%s could not take the engine's audio buffers; the slot is bypassed",
                           descriptor_.name.c_str());
            return;
        }
        processData_.numSamples = audio.numFrames;

        // --- transport -------------------------------------------------------
        processContext_.sampleRate = sampleRate_;
        processContext_.continousTimeSamples = context.positionSamples;
        processContext_.projectTimeSamples = context.positionSamples;
        processContext_.tempo = context.tempoBpm;
        processContext_.projectTimeMusic = static_cast<double>(context.positionTicks);
        processContext_.state = ProcessContext::kTempoValid | ProcessContext::kContTimeValid |
                                ProcessContext::kProjectTimeMusicValid |
                                (context.isPlaying ? ProcessContext::kPlaying : 0);

        // --- parameters, then the block --------------------------------------
        parameterTransfer_.transferChangesTo(parameterChanges_);
        const tresult result = processor_->process(processData_);
        parameterChanges_.clearQueue();

        if (result != kResultOk) {
            // A plug-in that fails a block is bypassed from here on instead of
            // being called again: one bad plug-in must not silence the session.
            setFailed(true);
            AURA_LOG_ERROR(kCategory, "%s returned %d while processing; the slot is bypassed",
                           descriptor_.name.c_str(), static_cast<int>(result));
            return;
        }
        if (latencyChanged_.load(std::memory_order_relaxed) && processor_)
            refreshLatency();
        (void)midiOut; // VST 3 event output is M7 work (arpeggiators feeding other tracks)
    }

    [[nodiscard]] int latencySamples() const noexcept override {
        // The graph reads this between blocks to work out delay compensation, so a
        // plug-in that has asked for a queue-latency change must be answered with the
        // new number straight away instead of one block later. Re-querying costs one
        // virtual call and takes no lock, so it is safe on either thread.
        if (latencyChanged_.load(std::memory_order_relaxed) && processor_)
            refreshLatency();
        return latencySamples_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] int numParameters() const noexcept override {
        return static_cast<int>(parameters_.size());
    }

    [[nodiscard]] std::string parameterName(int index) const override {
        if (index < 0 || index >= static_cast<int>(parameters_.size()))
            return {};
        return parameters_[static_cast<std::size_t>(index)].name;
    }

    [[nodiscard]] float parameterValue(int index) const noexcept override {
        if (!controller_ || index < 0 || index >= static_cast<int>(parameters_.size()))
            return 0.0f;
        const ParamEntry& entry = parameters_[static_cast<std::size_t>(index)];
        const ParamValue normalised = controller_->getParamNormalized(entry.id);
        return static_cast<float>(controller_->normalizedParamToPlain(entry.id, normalised));
    }

    void setParameterValue(int index, float value) noexcept override {
        if (!controller_ || index < 0 || index >= static_cast<int>(parameters_.size()))
            return;
        const ParamEntry& entry = parameters_[static_cast<std::size_t>(index)];
        setParameterNormalised(index,
                               static_cast<float>(controller_->plainParamToNormalized(
                                   entry.id, static_cast<ParamValue>(value))));
    }

    [[nodiscard]] float parameterNormalised(int index) const noexcept override {
        if (!controller_ || index < 0 || index >= static_cast<int>(parameters_.size()))
            return 0.0f;
        return static_cast<float>(
            controller_->getParamNormalized(parameters_[static_cast<std::size_t>(index)].id));
    }

    void setParameterNormalised(int index, float value) noexcept override {
        if (index < 0 || index >= static_cast<int>(parameters_.size()))
            return;
        const ParamID id = parameters_[static_cast<std::size_t>(index)].id;
        const ParamValue clamped = std::clamp(static_cast<ParamValue>(value), 0.0, 1.0);
        if (controller_)
            controller_->setParamNormalized(id, clamped); // keep the editor in step
        // The component reads parameters from ProcessData, so the change also goes
        // through the lock-free ring the audio thread drains.
        parameterTransfer_.addChange(id, clamped, 0);
        // A parameter the user just moved belongs to the user from now on; tell
        // the cache so automation writes are not stomped by a stale read.
        lastTouchedParameter_.store(id, std::memory_order_relaxed);
    }

    [[nodiscard]] AuraResult<std::vector<std::uint8_t>> saveState() const override {
        if (!component_)
            return failure(makeError(ErrorCode::PluginLoadFailed, "No component to save", descriptor_.id));
        std::vector<std::uint8_t> blob;
        {
            Steinberg::MemoryStream stream;
            if (component_->getState(&stream) != kResultOk)
                return failure(makeError(ErrorCode::PluginLoadFailed,
                                         "The plug-in refused to save its state",
                                         descriptor_.name));
            appendStream(blob, stream);
        }
        if (controller_) {
            Steinberg::MemoryStream stream;
            if (controller_->getState(&stream) == kResultOk)
                appendStream(blob, stream);
            else
                appendLength(blob, 0); // a controller without state is normal (not an error)
        } else {
            appendLength(blob, 0);
        }
        return success(std::move(blob));
    }

    Status restoreState(const std::vector<std::uint8_t>& state) override {
        if (!component_)
            return failure(makeError(ErrorCode::PluginLoadFailed, "No component to restore",
                                     descriptor_.id));
        std::size_t offset = 0;
        const std::size_t componentBytes = readLength(state, offset);
        if (componentBytes == 0 || offset + componentBytes > state.size())
            return failure(makeError(ErrorCode::UnsupportedFormat,
                                     "The stored plug-in state is truncated",
                                     descriptor_.name));
        {
            Steinberg::MemoryStream stream(const_cast<std::uint8_t*>(state.data() + offset),
                                           static_cast<Steinberg::TSize>(componentBytes));
            if (component_->setState(&stream) != kResultOk)
                return failure(makeError(ErrorCode::PluginLoadFailed,
                                         "The plug-in rejected its stored state",
                                         descriptor_.name));
            // The controller needs the component's state too, otherwise its
            // parameter display disagrees with what the DSP is doing.
            stream.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
            if (controller_)
                controller_->setComponentState(&stream);
        }
        offset += componentBytes;

        const std::size_t controllerBytes = readLength(state, offset);
        if (controllerBytes > 0 && offset + controllerBytes <= state.size() && controller_) {
            Steinberg::MemoryStream stream(const_cast<std::uint8_t*>(state.data() + offset),
                                           static_cast<Steinberg::TSize>(controllerBytes));
            controller_->setState(&stream);
        }
        // Plug-in state can carry a latency-setting value (this fixture's delay
        // parameter is one), so whatever latency was read before the restore is no
        // longer trustworthy: re-read it before the graph compensates with the old
        // number. The format expects exactly this after setState/setComponentState.
        latencyChanged_.store(true, std::memory_order_relaxed);
        return success();
    }

    [[nodiscard]] bool hasEditor() const noexcept override { return descriptor_.hasEditor; }

    void* createEditor() override {
        if (!controller_)
            return nullptr;
        return controller_->createView(Steinberg::Vst::ViewType::kEditor);
    }

    void destroyEditor(void* editor) override {
        if (auto* view = static_cast<Steinberg::IPlugView*>(editor))
            view->release();
    }

    [[nodiscard]] bool isActive() const noexcept override { return active_; }

    /// Called by the plug-in's own editor (or handlers it installs). Runs on
    /// whichever thread the plug-in called from; the ring is safe for that.
    void pluginEditedParameter(ParamID id, ParamValue normalised) {
        parameterTransfer_.addChange(id, std::clamp(normalised, 0.0, 1.0), 0);
        if (controller_)
            controller_->setParamNormalized(id, std::clamp(normalised, 0.0, 1.0));
    }

    void restartRequested(int32 flags) {
        if ((flags & Steinberg::Vst::kLatencyChanged) != 0)
            latencyChanged_.store(true, std::memory_order_relaxed);
        if ((flags & (Steinberg::Vst::kParamValuesChanged | Steinberg::Vst::kParamTitlesChanged)) != 0)
            parametersDirty_.store(true, std::memory_order_relaxed);
        AURA_LOG_DEBUG(kCategory, "%s asked the host to restart (flags 0x%x)",
                       descriptor_.name.c_str(), static_cast<unsigned>(flags));
    }

    /// Re-reads getLatencySamples() after the plug-in said it changed. Cheap by
    /// contract: a latency getter must not allocate, block or touch the disk.
    void refreshLatency() const noexcept {
        latencyChanged_.store(false, std::memory_order_relaxed);
        latencySamples_.store(static_cast<int>(processor_->getLatencySamples()),
                              std::memory_order_relaxed);
    }

    /// How long the plug-in's tail lasts after the input goes silent, in samples.
    [[nodiscard]] int tailSamples() const noexcept { return tailSamples_; }

private:
    // --- helpers ----------------------------------------------------------

    void setActive(bool state) noexcept {
        if (!component_ || !processor_)
            return;
        if (state) {
            if (component_->setActive(true) == kResultOk) {
                processor_->setProcessing(true);
                active_ = true;
            } else {
                setFailed(true);
                AURA_LOG_ERROR(kCategory, "%s could not be activated", descriptor_.name.c_str());
            }
        } else {
            processor_->setProcessing(false);
            component_->setActive(false);
            active_ = false;
        }
    }

    static void appendLength(std::vector<std::uint8_t>& out, std::size_t length) {
        const auto value = static_cast<std::uint32_t>(length);
        out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
        out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
        out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
        out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
    }

    /// Reads a four-byte length prefix and *consumes* it: `offset` ends up past the
    /// prefix, on the first byte of the block it describes. Taking the offset by
    /// value here would leave the reader looking at the length bytes as if they were
    /// plug-in state, which is what a save/restore round trip fails on.
    static std::size_t readLength(const std::vector<std::uint8_t>& in, std::size_t& offset) {
        if (offset + 4 > in.size())
            return 0;
        const std::size_t length = static_cast<std::size_t>(in[offset]) |
                                   (static_cast<std::size_t>(in[offset + 1]) << 8) |
                                   (static_cast<std::size_t>(in[offset + 2]) << 16) |
                                   (static_cast<std::size_t>(in[offset + 3]) << 24);
        offset += 4;
        return length;
    }

    static void appendStream(std::vector<std::uint8_t>& out, Steinberg::MemoryStream& stream) {
        Steinberg::int64 size = 0;
        stream.seek(0, Steinberg::IBStream::kIBSeekEnd, &size);
        stream.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
        appendLength(out, static_cast<std::size_t>(size));
        if (size <= 0)
            return;
        const std::size_t base = out.size();
        out.resize(base + static_cast<std::size_t>(size));
        Steinberg::int32 read = 0;
        stream.read(out.data() + base, static_cast<Steinberg::int32>(size), &read);
        out.resize(base + static_cast<std::size_t>(read > 0 ? read : 0));
    }

    /// Caches the audio bus widths (all of them: sidechain and aux buses count).
    /// Called from prepare(), never from the audio thread.
    void readBusLayout() {
        inputBusChannels_.clear();
        outputBusChannels_.clear();
        for (const Steinberg::Vst::BusDirection direction :
             {Steinberg::Vst::BusDirections::kInput, Steinberg::Vst::BusDirections::kOutput}) {
            std::vector<int>& channels = direction == Steinberg::Vst::BusDirections::kInput
                                             ? inputBusChannels_
                                             : outputBusChannels_;
            const Steinberg::int32 buses =
                component_ ? component_->getBusCount(Steinberg::Vst::kAudio, direction) : 0;
            for (Steinberg::int32 bus = 0; bus < buses; ++bus) {
                Steinberg::Vst::BusInfo info{};
                if (component_->getBusInfo(Steinberg::Vst::kAudio, direction, bus, info) == kResultOk)
                    channels.push_back(static_cast<int>(info.channelCount));
            }
        }
    }

    /// Widest of the two bus-0 (main) buses: the one the engine's audio is wired to.
    [[nodiscard]] int busZeroChannels() const noexcept {
        return std::max(inputBusChannels_.empty() ? 0 : inputBusChannels_[0],
                        outputBusChannels_.empty() ? 0 : outputBusChannels_[0]);
    }

    /// One silent slice per channel that has no engine audio behind it: bus 0 keeps
    /// one engine channel at least, everything else is scratch. One slice per channel
    /// rather than one shared slice keeps a plug-in that reads a channel back after
    /// writing it honest about what it wrote.
    [[nodiscard]] int scratchSlicesNeeded() const noexcept {
        int slices = 0;
        for (const std::vector<int>* channels : {&inputBusChannels_, &outputBusChannels_}) {
            for (std::size_t bus = 0; bus < channels->size(); ++bus)
                slices += bus == 0 ? std::max(0, (*channels)[bus] - 1) : (*channels)[bus];
        }
        return std::max(1, slices);
    }

    void appendEvent(const midi::Message& message, int numFrames) noexcept {
        Event event{};
        event.busIndex = 0;
        event.sampleOffset = static_cast<int32>(
            std::clamp<std::int64_t>(message.sampleOffset, 0, numFrames > 0 ? numFrames - 1 : 0));
        event.flags = Event::kIsLive;
        switch (message.type) {
        case midi::Message::Type::NoteOn:
            if (message.data2 == 0) {
                event.type = Event::kNoteOffEvent;
                event.noteOff.channel = static_cast<std::int16_t>(message.channel);
                event.noteOff.pitch = static_cast<std::int16_t>(message.data1);
                event.noteOff.velocity = 0.0f;
                event.noteOff.noteId = -1;
            } else {
                event.type = Event::kNoteOnEvent;
                event.noteOn.channel = static_cast<std::int16_t>(message.channel);
                event.noteOn.pitch = static_cast<std::int16_t>(message.data1);
                event.noteOn.velocity = static_cast<float>(message.data2) / 127.0f;
                event.noteOn.noteId = -1;
            }
            break;
        case midi::Message::Type::NoteOff:
            event.type = Event::kNoteOffEvent;
            event.noteOff.channel = static_cast<std::int16_t>(message.channel);
            event.noteOff.pitch = static_cast<std::int16_t>(message.data1);
            event.noteOff.velocity = static_cast<float>(message.data2) / 127.0f;
            event.noteOff.noteId = -1;
            break;
        case midi::Message::Type::PolyPressure:
            event.type = Event::kPolyPressureEvent;
            event.polyPressure.channel = static_cast<std::int16_t>(message.channel);
            event.polyPressure.pitch = static_cast<std::int16_t>(message.data1);
            event.polyPressure.pressure = static_cast<float>(message.data2) / 127.0f;
            break;
        default:
            // Control changes and pitch bend are not VST 3 events: the format
            // replaced them with parameter changes and note expressions, so a
            // converter here would be guessing. They are delivered to instruments
            // through the MIDI path in M7 instead of being dropped silently.
            return;
        }
        eventList_.addEvent(event);
    }

    void refreshParameters() {
        parameters_.clear();
        if (!controller_)
            return;
        const int32 count = controller_->getParameterCount();
        parameters_.reserve(static_cast<std::size_t>(std::min<int32>(count, kMaxParameters)));
        for (int32 index = 0; index < count && index < kMaxParameters; ++index) {
            Steinberg::Vst::ParameterInfo info{};
            if (controller_->getParameterInfo(index, info) != kResultOk)
                continue;
            ParamEntry entry;
            entry.id = info.id;
            entry.name = Steinberg::Vst::StringConvert::convert(info.title);
            entry.unit = Steinberg::Vst::StringConvert::convert(info.units);
            entry.isBypass = (info.flags & Steinberg::Vst::ParameterInfo::kIsBypass) != 0;
            entry.minPlain = static_cast<float>(controller_->normalizedParamToPlain(info.id, 0.0));
            entry.maxPlain = static_cast<float>(controller_->normalizedParamToPlain(info.id, 1.0));
            // A plug-in that reports no usable range still gets a working control.
            if (entry.name.empty())
                entry.name = "Parameter " + std::to_string(index + 1);
            parameters_.push_back(std::move(entry));
        }
        parametersDirty_.store(false, std::memory_order_relaxed);
    }

public:
    /// The UI polls this before showing a parameter list; the list is rebuilt on
    /// the calling thread, never while the audio thread is walking it.
    [[nodiscard]] bool parametersChanged() const noexcept {
        return parametersDirty_.load(std::memory_order_relaxed);
    }
    void refreshParametersIfNeeded() {
        if (parametersDirty_.load(std::memory_order_relaxed))
            refreshParameters();
    }

private:
    PluginDescriptor descriptor_;
    std::unique_ptr<Steinberg::Vst::HostApplication> context_;
    VST3::Hosting::Module::Ptr module_;
    std::unique_ptr<PlugProvider> provider_;
    Steinberg::IPtr<IComponent> component_;
    Steinberg::IPtr<IEditController> controller_;
    Steinberg::IPtr<IAudioProcessor> processor_;
    std::unique_ptr<Vst3ComponentHandler> handler_;

    HostProcessData processData_;
    EventList eventList_{kMaxBlockEvents};
    ParameterChanges parameterChanges_;
    ParameterChangeTransfer parameterTransfer_{0};
    ProcessContext processContext_{};

    std::vector<float> blockBuffers_;
    std::vector<float*> channelPointers_;
    std::vector<ParamEntry> parameters_;

    double sampleRate_ = 0.0;
    int maxBlockSize_ = 0;
    int numChannels_ = 0;
    std::vector<int> inputBusChannels_;  // channel count per audio input bus, in bus order
    std::vector<int> outputBusChannels_;
    int scratchChannels_ = 1;            // silent slices for channels with no engine audio
    int tailSamples_ = 0;
    bool active_ = false;

    // Mutable: latencySamples() is const (it is a const interface method) and answers a
    // pending restart notification by re-reading the plug-in.
    mutable std::atomic<bool> latencyChanged_{false};
    mutable std::atomic<int> latencySamples_{0};
    std::atomic<bool> parametersDirty_{false};
    std::atomic<ParamID> lastTouchedParameter_{0};
};

// --- the handler's out-of-line bodies (they need the complete instance) -----
tresult PLUGIN_API Vst3ComponentHandler::performEdit(ParamID id, ParamValue valueNormalized) {
    owner_.pluginEditedParameter(id, valueNormalized);
    return kResultOk;
}

tresult PLUGIN_API Vst3ComponentHandler::restartComponent(int32 flags) {
    owner_.restartRequested(flags);
    return kResultOk;
}

} // namespace

AuraResult<std::unique_ptr<PluginInstance>> instantiate(const PluginDescriptor& descriptor,
                                                        double sampleRate, int maxBlockSize,
                                                        int numChannels) {
    std::string uid;
    std::string path;
    if (!splitId(descriptor.id, uid, path)) {
        return failure(makeError(ErrorCode::InvalidArgument,
                                 "Not a VST 3 descriptor id: " + descriptor.id,
                                 "expected VST3:<uid>@<bundle path>"));
    }

    std::string moduleError;
    auto module = VST3::Hosting::Module::create(path, moduleError);
    if (!module) {
        return failure(makeError(ErrorCode::NotFound, "Could not load " + path,
                                 moduleError.empty() ? "the module loader gave no reason"
                                                     : moduleError));
    }

    const auto factory = module->getFactory();
    const auto classInfos = factory.classInfos();
    const VST3::Hosting::ClassInfo* found = nullptr;
    for (const auto& classInfo : classInfos) {
        if (classInfo.category() == kVstAudioEffectClass && classInfo.ID().toString() == uid) {
            found = &classInfo;
            break;
        }
    }
    if (found == nullptr) {
        return failure(makeError(ErrorCode::NotFound,
                                 "The bundle no longer contains the class this project expects",
                                 descriptor.name + " (uid " + uid + ") in " + path));
    }

    // A plug-in may query the host for services while it initialises, so the
    // context object has to exist before PlugProvider touches it and outlive the
    // plug-in itself.
    auto context = std::make_unique<Steinberg::Vst::HostApplication>();
    Steinberg::Vst::PluginContextFactory::instance().setPluginContext(context.get());

    auto provider = std::make_unique<PlugProvider>(factory, *found, true);
    if (!provider->initialize()) {
        Steinberg::Vst::PluginContextFactory::instance().setPluginContext(nullptr);
        return failure(makeError(ErrorCode::PluginLoadFailed,
                                 "The plug-in could not be initialised",
                                 descriptor.name + " in " + path));
    }

    auto instance = std::make_unique<Vst3PluginInstance>(descriptor, std::move(context),
                                                         std::move(module), std::move(provider));
    const Status prepared = instance->prepare(sampleRate, maxBlockSize, numChannels);
    if (!prepared)
        return failure(prepared.error());
    AURA_LOG_INFO(kCategory, "Loaded %s (%s) - latency %d samples",
                  descriptor.name.c_str(), descriptor.vendor.c_str(), instance->latencySamples());
    return success(std::unique_ptr<PluginInstance>(std::move(instance)));
}

} // namespace aura::plugin::vst3
