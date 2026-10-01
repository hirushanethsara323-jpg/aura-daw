// ============================================================================
// AURA DAW - src/midi/Instrument.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include "aura/midi/Instrument.hpp"

#include <algorithm>
#include <cmath>

#include "aura/core/Json.hpp"
#include "aura/core/Log.hpp"
#include "aura/core/Math.hpp"

namespace aura::midi {
namespace {
constexpr const char* kCategory = "Midi.Instrument";

/// Equal temperament: A4 (MIDI 69) = 440 Hz.
double noteToHz(double note) noexcept {
    return 440.0 * std::pow(2.0, (note - 69.0) / 12.0);
}

float clamp01Param(float value) noexcept {
    return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

/// Maps a 0..1 parameter onto a range with a musically useful curve.
double scaledToRange(float value, double low, double high) noexcept {
    return low + static_cast<double>(clamp01Param(value)) * (high - low);
}

} // namespace

const char* instrumentKindName(InstrumentKind kind) noexcept {
    switch (kind) {
    case InstrumentKind::Synth: return "Synth";
    case InstrumentKind::Sampler: return "Sampler";
    case InstrumentKind::DrumKit: return "Drum kit";
    case InstrumentKind::External: return "External";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// PolySynth
// ---------------------------------------------------------------------------
PolySynth::PolySynth() {
    parameterInfo_.resize(ParamCount);
    auto add = [this](std::size_t index, const char* id, const char* nameKey, float minimum,
                      float maximum, float defaultValue, const char* unit) {
        InstrumentParameterInfo info;
        info.id = id;
        info.nameKey = nameKey;
        info.minimum = minimum;
        info.maximum = maximum;
        info.defaultValue = defaultValue;
        info.unit = unit;
        parameterInfo_[index] = std::move(info);
    };

    add(ParamOsc1Waveform, "osc1Wave", "instrument.osc1.wave", 0.0f, 4.0f, 2.0f, "");
    add(ParamOsc2Waveform, "osc2Wave", "instrument.osc2.wave", 0.0f, 4.0f, 3.0f, "");
    add(ParamOsc2DetuneCents, "osc2Detune", "instrument.osc2.detune", 0.0f, 1.0f, 0.25f, "");
    add(ParamOsc2Level, "osc2Level", "instrument.osc2.level", 0.0f, 1.0f, 0.35f, "");
    add(ParamSubLevel, "subLevel", "instrument.sub.level", 0.0f, 1.0f, 0.2f, "");
    add(ParamFilterCutoff, "cutoff", "instrument.filter.cutoff", 0.0f, 1.0f, 0.62f, "");
    add(ParamFilterResonance, "resonance", "instrument.filter.resonance", 0.0f, 1.0f, 0.2f, "");
    add(ParamFilterEnvelopeAmount, "filterEnv", "instrument.filter.envAmount", 0.0f, 1.0f, 0.35f, "");
    add(ParamAttack, "attack", "instrument.env.attack", 0.0f, 1.0f, 0.02f, "");
    add(ParamDecay, "decay", "instrument.env.decay", 0.0f, 1.0f, 0.35f, "");
    add(ParamSustain, "sustain", "instrument.env.sustain", 0.0f, 1.0f, 0.75f, "");
    add(ParamRelease, "release", "instrument.env.release", 0.0f, 1.0f, 0.3f, "");
    add(ParamLevel, "level", "instrument.level", 0.0f, 1.0f, 0.7f, "");
    add(ParamPitchBendRange, "bendRange", "instrument.pitchBendRange", 0.0f, 1.0f, 0.1667f, "");

    // Waveform choice lists (the oscillator supports five waveform shapes).
    const std::vector<std::string> waves{"sine", "triangle", "saw", "square", "noise"};
    parameterInfo_[ParamOsc1Waveform].choices = waves;
    parameterInfo_[ParamOsc2Waveform].choices = waves;

    for (std::size_t i = 0; i < ParamCount; ++i)
        parameters_[i] = parameterInfo_[i].defaultValue;

    sustainedNotes_.reserve(32);
    setParameter(ParamOsc1Waveform, parameters_[ParamOsc1Waveform]);
    setParameter(ParamOsc2Waveform, parameters_[ParamOsc2Waveform]);
}

PolySynth::~PolySynth() = default;

Status PolySynth::prepare(double sampleRate, int maxBlockSize, int numChannels) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    // One scratch buffer for voice rendering. Allocated here and never resized
    // afterwards: the audio thread must not allocate.
    voiceScratch_.assign(static_cast<std::size_t>(std::max(1, maxBlockSize)), 0.0f);
    numChannels_ = std::max(1, numChannels);

    for (Voice& voice : voices_) {
        voice.osc1.setAmplitude(1.0f);
        voice.sub.setAmplitude(subLevel_);
        voice.osc2.setAmplitude(osc2Level_);
        voice.osc1.prepare(sampleRate_);
        voice.osc2.prepare(sampleRate_);
        voice.sub.prepare(sampleRate_);
        voice.envelope.prepare(sampleRate_);
        voice.active = false;
        voice.held = false;
    }
    updateFilterCoefficients();
    AURA_LOG_DEBUG(kCategory, "PolySynth prepared: %.0f Hz, %d channels, %d voices", sampleRate_,
                   numChannels_, kMaxVoices);
    return success();
}

void PolySynth::reset() noexcept {
    for (Voice& voice : voices_) {
        voice.active = false;
        voice.held = false;
        voice.envelope.reset();
        voice.filter.reset();
    }
    sustainedNotes_.clear();
    sustainPedalDown_ = false;
}

void PolySynth::updateFilterCoefficients() noexcept {
    // Cutoff parameter 0..1 maps onto 30 Hz .. 18 kHz logarithmically - the
    // musically useful range, and always below Nyquist.
    const double nyquist = sampleRate_ * 0.5;
    const double maximum = std::min(18000.0, nyquist * 0.95);
    const double cutoff = std::min(maximum, 30.0 * std::pow(maximum / 30.0, static_cast<double>(parameters_[ParamFilterCutoff])));
    const double q = 0.5 + static_cast<double>(parameters_[ParamFilterResonance]) * 12.0;
    const dsp::BiquadCoefficients coefficients = dsp::BiquadDesign::lowPass(sampleRate_, cutoff, q);
    for (Voice& voice : voices_)
        voice.filter.setCoefficients(coefficients);
}

void PolySynth::updateVoiceFrequencies(Voice& voice) noexcept {
    const double note = static_cast<double>(voice.note) + voice.pitchBendSemitones;
    const double base = noteToHz(note);
    const double detuneRatio = std::pow(2.0, osc2DetuneCents_ / 1200.0);

    voice.osc1.setFrequency(base);
    voice.osc2.setFrequency(base * detuneRatio);
    voice.sub.setFrequency(base * 0.5);
    voice.osc2.setAmplitude(osc2Level_);
    voice.sub.setAmplitude(subLevel_);
}

float PolySynth::parameter(std::size_t index) const noexcept {
    return index < ParamCount ? parameters_[index] : 0.0f;
}

const InstrumentParameterInfo& PolySynth::parameterInfo(std::size_t index) const {
    static const InstrumentParameterInfo empty{};
    return index < parameterInfo_.size() ? parameterInfo_[index] : empty;
}

void PolySynth::setParameter(std::size_t index, float value) noexcept {
    if (index >= ParamCount)
        return;
    parameters_[index] = clamp01Param(value);

    switch (index) {
    case ParamOsc1Waveform: {
        const int choice = static_cast<int>(parameters_[index] * 4.0f + 0.5f);
        osc1Waveform_ = static_cast<dsp::Waveform>(std::clamp(choice, 0, 4));
        for (Voice& voice : voices_)
            voice.osc1.setWaveform(osc1Waveform_);
        break;
    }
    case ParamOsc2Waveform: {
        const int choice = static_cast<int>(parameters_[index] * 4.0f + 0.5f);
        osc2Waveform_ = static_cast<dsp::Waveform>(std::clamp(choice, 0, 4));
        for (Voice& voice : voices_)
            voice.osc2.setWaveform(osc2Waveform_);
        break;
    }
    case ParamOsc2DetuneCents: osc2DetuneCents_ = scaledToRange(parameters_[index], 0.0, 50.0); break;
    case ParamOsc2Level: osc2Level_ = parameters_[index]; break;
    case ParamSubLevel: subLevel_ = parameters_[index]; break;
    case ParamFilterCutoff:
    case ParamFilterResonance: updateFilterCoefficients(); break;
    case ParamLevel: level_ = parameters_[index]; break;
    case ParamPitchBendRange: pitchBendRange_ = scaledToRange(parameters_[index], 0.0, 24.0); break;
    case ParamAttack:
    case ParamDecay:
    case ParamSustain:
    case ParamRelease: {
        dsp::AdsrSettings settings;
        settings.attackMs = 1.0f + parameters_[ParamAttack] * 2000.0f;
        settings.decayMs = 5.0f + parameters_[ParamDecay] * 3000.0f;
        settings.sustainLevel = parameters_[ParamSustain];
        settings.releaseMs = 5.0f + parameters_[ParamRelease] * 4000.0f;
        for (Voice& voice : voices_)
            voice.envelope.setSettings(settings);
        break;
    }
    default: break;
    }
}

int PolySynth::findVoiceToSteal() noexcept {
    // Prefer a released voice, then the oldest held one: this keeps a held chord
    // intact for as long as possible.
    int oldestReleased = -1;
    int oldestHeld = -1;
    std::uint64_t releasedAge = 0;
    std::uint64_t heldAge = 0;

    for (int i = 0; i < kMaxVoices; ++i) {
        Voice& voice = voices_[i];
        if (!voice.active)
            return i;
        if (!voice.held) {
            if (oldestReleased < 0 || voice.age < releasedAge) {
                oldestReleased = i;
                releasedAge = voice.age;
            }
        } else if (oldestHeld < 0 || voice.age < heldAge) {
            oldestHeld = i;
            heldAge = voice.age;
        }
    }
    return oldestReleased >= 0 ? oldestReleased : oldestHeld;
}

void PolySynth::noteOn(int note, int velocity) noexcept {
    const int index = findVoiceToSteal();
    if (index < 0)
        return;
    Voice& voice = voices_[index];
    voice.active = true;
    voice.held = true;
    voice.note = note;
    voice.velocity = std::max(1, velocity) / 127.0f;
    voice.age = ++voiceAgeCounter_;
    voice.filter.reset();
    voice.envelope.noteOn();
    updateVoiceFrequencies(voice);
}

void PolySynth::noteOff(int note) noexcept {
    for (Voice& voice : voices_) {
        if (voice.active && voice.held && voice.note == note) {
            if (sustainPedalDown_) {
                // Held by the pedal: remember it so lifting the pedal releases it.
                // rt-audit: allow - capacity is reserved in prepare(); the guard
                // below makes growth (and therefore allocation) impossible.
                if (sustainedNotes_.size() < sustainedNotes_.capacity())
                    sustainedNotes_.push_back(note);
                voice.held = false;
            } else {
                voice.held = false;
                voice.envelope.noteOff();
            }
        }
    }
}

void PolySynth::allNotesOff(bool immediate) noexcept {
    for (Voice& voice : voices_) {
        voice.held = false;
        if (immediate) {
            voice.active = false;
            voice.envelope.reset();
            voice.filter.reset();
        } else {
            voice.envelope.noteOff();
        }
    }
    sustainPedalDown_ = false;
    sustainedNotes_.clear();
}

int PolySynth::activeVoices() const noexcept {
    int count = 0;
    for (const Voice& voice : voices_) {
        if (voice.active)
            ++count;
    }
    return count;
}

bool PolySynth::isProducingSound() const noexcept {
    return activeVoices() > 0;
}

void PolySynth::handleEvent(const Message& message) noexcept {
    switch (message.type) {
    case Message::Type::NoteOn:
        if (message.isNoteOn())
            noteOn(message.data1, message.data2);
        else
            noteOff(message.data1);
        break;
    case Message::Type::NoteOff: noteOff(message.data1); break;
    case Message::Type::PitchBend: {
        const float semitones = message.bendSemitones(static_cast<int>(pitchBendRange_));
        for (Voice& voice : voices_) {
            if (!voice.active)
                continue;
            voice.pitchBendSemitones = semitones;
            updateVoiceFrequencies(voice);
        }
        break;
    }
    case Message::Type::ControlChange:
        if (message.data1 == Message::kSustainController) {
            const bool down = message.data2 >= 64;
            if (sustainPedalDown_ && !down) {
                // Pedal lifted: release everything that was being sustained.
                for (const int note : sustainedNotes_)
                    noteOff(note);
                sustainedNotes_.clear();
            }
            sustainPedalDown_ = down;
        } else if (message.data1 == Message::kAllNotesOffController ||
                   message.data1 == Message::kAllSoundOffController) {
            allNotesOff(message.data1 == Message::kAllSoundOffController);
        }
        break;
    case Message::Type::ProgramChange:
    case Message::Type::PolyPressure:
    case Message::Type::ChannelPressure:
    case Message::Type::Meta:
        break; // no per-note pressure response in the built-in instrument
    }
}

void PolySynth::renderVoice(Voice& voice, float* left, float* right, int startFrame,
                            int endFrame) noexcept {
    const int frames = endFrame - startFrame;
    if (frames <= 0 || voiceScratch_.empty())
        return;
    // The scratch buffer is allocated once in prepare(): a voice never renders
    // more than one block, and the block size is bounded by the device.
    const int usable = std::min(frames, static_cast<int>(voiceScratch_.size()));
    float* mono = voiceScratch_.data();

    std::fill(mono, mono + usable, 0.0f);
    // Additive generation sums the three oscillators in a single pass.
    dsp::Oscillator* oscillators[3] = {&voice.osc1, &voice.osc2, &voice.sub};
    for (auto* oscillator : oscillators)
        oscillator->generate(mono, usable, true);

    float* channels[1] = {mono};
    voice.filter.processBlock(channels, 1, usable);

    const float velocityGain = voice.velocity * level_;
    for (int i = 0; i < usable; ++i) {
        const float sample = mono[i] * voice.envelope.nextSample() * velocityGain;
        left[startFrame + i] += sample;
        if (right)
            right[startFrame + i] += sample;
    }
    if (!voice.envelope.isActive())
        voice.active = false;
}

void PolySynth::process(dsp::AudioBlockView& audio, const Message* events, int numEvents,
                        const dsp::ProcessContext& context) noexcept {
    (void)context;
    if (!audio.channelPointers || audio.numFrames <= 0)
        return;

    const int frames = audio.numFrames;
    float* left = audio.channelPointers[0];
    float* right = audio.numChannels > 1 ? audio.channelPointers[1] : nullptr;

    // Events are applied at their sample offset: the block is rendered in
    // segments so a note that starts mid-block starts exactly on time.
    int cursor = 0;
    for (int i = 0; i < numEvents; ++i) {
        const int offset = static_cast<int>(
            std::clamp<std::int64_t>(events[i].sampleOffset, 0, frames));
        if (offset > cursor) {
            for (Voice& voice : voices_) {
                if (voice.active)
                    renderVoice(voice, left, right, cursor, offset);
            }
            cursor = offset;
        }
        handleEvent(events[i]);
    }
    if (cursor < frames) {
        for (Voice& voice : voices_) {
            if (voice.active)
                renderVoice(voice, left, right, cursor, frames);
        }
    }
}

json::Value PolySynth::saveState() const {
    json::Value root = json::Value::makeObject();
    root.set("instrument", id_);
    json::Value values = json::Value::makeArray();
    for (std::size_t i = 0; i < ParamCount; ++i) {
        json::Value entry = json::Value::makeObject();
        entry.set("id", parameterInfo_[i].id);
        entry.set("value", static_cast<double>(parameters_[i]));
        values.push(std::move(entry));
    }
    root.set("parameters", std::move(values));
    return root;
}

Status PolySynth::restoreState(const json::Value& state) {
    for (const auto& entry : state["parameters"].elements()) {
        const std::string id = entry["id"].asString();
        const float value = static_cast<float>(entry["value"].asDouble(0.0));
        for (std::size_t i = 0; i < ParamCount; ++i) {
            if (parameterInfo_[i].id == id) {
                setParameter(i, value);
                break;
            }
        }
    }
    return success();
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------
std::unique_ptr<Instrument> createBuiltinInstrument(const std::string& id) {
    if (id == "aura.basic")
        return std::make_unique<PolySynth>();
    return nullptr; // unknown id: the caller reports it, no silent fallback
}

std::vector<std::string> builtinInstrumentIds() {
    return {"aura.basic"};
}

} // namespace aura::midi
