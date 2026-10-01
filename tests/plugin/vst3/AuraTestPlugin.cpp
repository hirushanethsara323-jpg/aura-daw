// ============================================================================
// AURA DAW - tests/plugin/vst3/AuraTestPlugin.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A minimal but REAL VST 3 plug-in, built from this repository, used as the test
// subject for the host adapter. It is not shipped and it is not an instrument:
// it exists so the host is verified against the actual format instead of against
// a mock that agrees with whatever the host happens to do.
//
// What it deliberately does, because the host has to get each one right:
//   * two parameters (Gain 0..2, Delay 0..512 samples) with a real plain/normalised
//     mapping, so parameter conversion is exercised rather than assumed;
//   * a delay line whose length is what getLatencySamples() reports - reported
//     latency and actual latency cannot drift apart in this fixture;
//   * state that round-trips (gain, delay, and how many note events it has seen),
//     which is how the tests can check that MIDI reached the plug-in at all;
//   * no editor, so `hasEditor == false` is a case the host must handle.
//
// It is built as a proper VST 3 bundle by tests/CMakeLists.txt (on Linux:
// AuraTestPlugin.vst3/Contents/<arch>-linux/AuraTestPlugin.so), which is what
// makes `Module::create()` able to load it - the format has no such thing as a
// bare shared library.
// ============================================================================

#include <algorithm>
#include <cstring>
#include <vector>

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "public.sdk/source/main/pluginfactory.h"
// Include order is not cosmetic here: vstsinglecomponenteffect.h remaps
// IComponent::setState/getState to setEditorState/getEditorState for the duration of
// its own includes, and any SDK header parsed between the remap and the #undef sees
// the wrong names. Keep this first among the public.sdk includes.
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"

namespace {

using namespace Steinberg;
using namespace Steinberg::Vst;

// FUIDs are the format's identity: the host stores them in the project file, so
// they must never change once a project could reference them.
const FUID kAuraTestGainProcessorUID(0x5A555241, 0x54455354, 0x4741494E, 0x30303031);

constexpr ParamID kParamGain = 1000;
constexpr ParamID kParamDelay = 1001;
constexpr double kMaxGain = 2.0;
constexpr int kMaxDelaySamples = 512;

constexpr char kStateMagic[4] = {'A', 'T', 'G', '1'};

template <typename T>
void writeValue(IBStream* stream, const T& value) {
    int32 written = 0;
    stream->write(const_cast<T*>(&value), sizeof(T), &written);
}

template <typename T>
bool readValue(IBStream* stream, T& value) {
    int32 read = 0;
    return stream->read(&value, sizeof(T), &read) == kResultOk && read == sizeof(T);
}

// ---------------------------------------------------------------------------
// The plug-in: gain + delay, one class that is both component and controller.
// ---------------------------------------------------------------------------
class AuraTestGain final : public SingleComponentEffect {
public:
    AuraTestGain() { }

    static FUnknown* createInstance(void* /*context*/) {
        return static_cast<IAudioProcessor*>(new AuraTestGain);
    }

    // --- IPluginBase / IComponent -----------------------------------------
    tresult PLUGIN_API initialize(FUnknown* context) override {
        const tresult result = SingleComponentEffect::initialize(context);
        if (result != kResultOk)
            return result;

        addAudioInput(STR16("In"), SpeakerArr::kStereo);
        addAudioOutput(STR16("Out"), SpeakerArr::kStereo);

        // RangeParameter, not the plain Parameter: the range is part of what the
        // host has to respect, and a parameter declared 0..1 while the DSP reads
        // 0..2 would make every conversion test agree with itself and with nothing
        // else. Gain: 0..2x, default 1x (normalised 0.5). Delay: 0..512 samples,
        // default 0, stepped one sample at a time.
        parameters.addParameter(new RangeParameter(STR16("Gain"), kParamGain, STR16("x"), 0.0,
                                                   kMaxGain, 1.0, 0,
                                                   ParameterInfo::kCanAutomate));
        parameters.addParameter(new RangeParameter(STR16("Delay"), kParamDelay, STR16("samples"),
                                                   0.0, kMaxDelaySamples, 0.0, kMaxDelaySamples,
                                                   ParameterInfo::kCanAutomate));
        return kResultOk;
    }

    tresult PLUGIN_API terminate() override {
        delayLines_.clear();
        return SingleComponentEffect::terminate();
    }

    tresult PLUGIN_API setActive(TBool state) override {
        if (state) {
            delayLines_.assign(2, std::vector<float>(static_cast<std::size_t>(kMaxDelaySamples),
                                                     0.0f));
            writeIndices_ = {0, 0};
        }
        return SingleComponentEffect::setActive(state);
    }

    // --- IAudioProcessor --------------------------------------------------
    tresult PLUGIN_API setupProcessing(ProcessSetup& setup) override {
        sampleRate_ = setup.sampleRate;
        return SingleComponentEffect::setupProcessing(setup);
    }

    tresult PLUGIN_API canProcessSampleSize(int32 symbolicSampleSize) override {
        return symbolicSampleSize == kSample32 ? kResultTrue : kResultFalse;
    }

    uint32 PLUGIN_API getLatencySamples() override { return delaySamples_; }

    tresult PLUGIN_API process(ProcessData& data) override {
        if (data.numSamples <= 0)
            return kResultOk;

        // Count the note events the host delivered: the tests read this back out
        // of the saved state, which is how "MIDI actually reached the plug-in"
        // becomes an assertion instead of a hope.
        if (data.inputEvents) {
            const int32 eventCount = data.inputEvents->getEventCount();
            for (int32 index = 0; index < eventCount; ++index) {
                Event event{};
                if (data.inputEvents->getEvent(index, event) != kResultOk)
                    continue;
                if (event.type == Event::kNoteOnEvent || event.type == Event::kNoteOffEvent)
                    ++noteEventsSeen_;
            }
        }

        // Parameters arrive through ProcessData, which is exactly the contract the
        // host has to honour; reading them from anywhere else would make the test
        // pass while a real plug-in stayed at its defaults.
        applyParameterChanges(data);

        if (data.numOutputs == 0 || data.outputs[0].numChannels == 0)
            return kResultOk;
        const bool hasInput = data.numInputs > 0 && data.inputs[0].numChannels > 0;

        const int delay = std::clamp(delaySamples_, 0, kMaxDelaySamples);
        for (int32 channel = 0; channel < data.outputs[0].numChannels; ++channel) {
            float* output = data.outputs[0].channelBuffers32[channel];
            const float* input =
                hasInput && channel < data.inputs[0].numChannels
                    ? data.inputs[0].channelBuffers32[channel]
                    : nullptr;
            // One line per channel: a shared line would interleave the channels,
            // which is the kind of bug a test fixture must not have.
            const std::size_t lineIndex =
                std::min<std::size_t>(static_cast<std::size_t>(channel),
                                      delayLines_.size() - 1);
            std::vector<float>& line = delayLines_[lineIndex];
            int& writeIndex = writeIndices_[lineIndex];
            for (int32 sample = 0; sample < data.numSamples; ++sample) {
                const float in = input ? input[sample] : 0.0f;
                // Always write, even at zero delay: reading back the sample just
                // written makes zero delay bit-exact instead of "almost".
                line[static_cast<std::size_t>(writeIndex)] = in;
                const int readIndex =
                    (writeIndex + kMaxDelaySamples - delay) % kMaxDelaySamples;
                const float out = line[static_cast<std::size_t>(readIndex)];
                writeIndex = (writeIndex + 1) % kMaxDelaySamples;
                output[sample] = out * gain_;
            }
        }
        return kResultOk;
    }

    // --- state -------------------------------------------------------------
    tresult PLUGIN_API getState(IBStream* state) override {
        if (!state)
            return kResultFalse;
        int32 written = 0;
        state->write(const_cast<char*>(kStateMagic), sizeof(kStateMagic), &written);
        writeValue(state, gain_);
        writeValue(state, static_cast<int32>(delaySamples_));
        writeValue(state, noteEventsSeen_);
        writeValue(state, sampleRate_);
        return kResultOk;
    }

    tresult PLUGIN_API setState(IBStream* state) override {
        if (!state)
            return kResultFalse;
        char magic[sizeof(kStateMagic)] = {};
        int32 read = 0;
        if (state->read(magic, sizeof(magic), &read) != kResultOk ||
            std::memcmp(magic, kStateMagic, sizeof(magic)) != 0)
            return kResultFalse;
        int32 delay = 0;
        if (!readValue(state, gain_) || !readValue(state, delay) ||
            !readValue(state, noteEventsSeen_) || !readValue(state, sampleRate_))
            return kResultFalse;
        delaySamples_ = static_cast<int>(std::clamp(delay, 0, kMaxDelaySamples));
        syncParametersToValues();
        return kResultOk;
    }

    /// The controller half of a single-component plug-in is the same object, so
    /// the host's `setComponentState` call lands here.
    tresult PLUGIN_API setComponentState(IBStream* state) override { return setState(state); }

    /// A parameter written by the host has to be true for the DSP as well as for the
    /// controller: the two halves of this fixture are one object, and a single-
    /// component plug-in whose `getState()` disagreed with `getParamNormalized()`
    /// would be a plug-in with a state bug, not a test of the host's.
    ///
    /// The delay also *is* the latency, and the format's rule is that the host must be
    /// told: `restartComponent(kLatencyChanged)` is what makes delay compensation
    /// recompute instead of quietly drifting out of alignment. This is the non-real-
    /// time path (the host's own parameter writes), so calling the host from here is
    /// allowed - unlike from `process()`.
    tresult PLUGIN_API setParamNormalized(ParamID tag, ParamValue value) override {
        const tresult result = SingleComponentEffect::setParamNormalized(tag, value);
        if (result != kResultOk)
            return result;
        if (tag == kParamGain) {
            gain_ = static_cast<float>(value * kMaxGain);
        } else if (tag == kParamDelay) {
            const int delay = delayFromNormalised(value);
            if (delay != delaySamples_) {
                delaySamples_ = delay;
                if (auto* handler = getComponentHandler())
                    handler->restartComponent(kLatencyChanged);
            }
        }
        return result;
    }

private:
    static int delayFromNormalised(ParamValue value) {
        return std::clamp(static_cast<int>(value * kMaxDelaySamples + 0.5), 0, kMaxDelaySamples);
    }

    void applyParameterChanges(ProcessData& data) {
        if (!data.inputParameterChanges)
            return;
        const int32 count = data.inputParameterChanges->getParameterCount();
        for (int32 queueIndex = 0; queueIndex < count; ++queueIndex) {
            IParamValueQueue* queue =
                data.inputParameterChanges->getParameterData(queueIndex);
            if (!queue)
                continue;
            const int32 pointCount = queue->getPointCount();
            ParamValue value = 0.0;
            int32 sampleOffset = 0;
            if (pointCount > 0 &&
                queue->getPoint(pointCount - 1, sampleOffset, value) == kResultOk) {
                if (auto* parameter = parameters.getParameter(queue->getParameterId()))
                    parameter->setNormalized(value);
                if (queue->getParameterId() == kParamGain)
                    gain_ = static_cast<float>(value * kMaxGain);
                else if (queue->getParameterId() == kParamDelay)
                    // Same mapping as the controller path. No notification from here:
                    // the audio thread must not call the host, and a parameter that
                    // changes latency is not something a session should automate
                    // anyway - the host picks the change up on the next restart
                    // notification or activation.
                    delaySamples_ = delayFromNormalised(value);
            }
        }
    }

    void syncParametersToValues() {
        if (auto* gainParam = parameters.getParameter(kParamGain))
            gainParam->setNormalized(static_cast<ParamValue>(gain_ / kMaxGain));
        if (auto* delayParam = parameters.getParameter(kParamDelay))
            delayParam->setNormalized(static_cast<ParamValue>(delaySamples_) / kMaxDelaySamples);
    }

    std::vector<std::vector<float>> delayLines_;
    std::vector<int> writeIndices_{0, 0};
    int delaySamples_ = 0;
    float gain_ = 1.0f;
    double sampleRate_ = 44100.0;
    uint32 noteEventsSeen_ = 0;
};

} // namespace

// ---------------------------------------------------------------------------
// Entry point. The factory is what the host sees first, exactly like a commercial
// plug-in bundle.
// ---------------------------------------------------------------------------
BEGIN_FACTORY_DEF("AURA DAW Project", "https://github.com/hirushanethsara323-jpg/aura-daw",
                  "maintainer@example.invalid")

DEF_CLASS2(INLINE_UID_FROM_FUID(kAuraTestGainProcessorUID), PClassInfo::kManyInstances,
           kVstAudioEffectClass, "AURA Test Gain", Vst::kDistributable, "Fx|Utility",
           "1.0.0", kVstVersionString, AuraTestGain::createInstance)

END_FACTORY
