// ============================================================================
// AURA DAW - midi/Instrument.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Instrument API. AURA is not a synthesizer product, but a DAW without an
// instrument cannot be tested end to end (MIDI recording, piano roll, offline
// export and the timing tests all need something that turns notes into audio).
// Everything here is ORIGINAL code: no third-party instrument is bundled and no
// commercial plug-in is reproduced (see docs/LICENSES.md).
//
// An instrument is a note-driven SOURCE, not an in-place processor, so it does
// not derive from dsp::Processor. It is owned by the engine's instrument pool
// (exactly like insert processors) and referenced by a Track.
//
// Plug-in instruments (VST3/CLAP) plug in through the same interface once the
// hosting backends land in M6.
// ============================================================================
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "aura/core/Errors.hpp"
#include "aura/core/Json.hpp"
#include "aura/dsp/Biquad.hpp"
#include "aura/dsp/Envelope.hpp"
#include "aura/dsp/Oscillator.hpp"
#include "aura/dsp/Processor.hpp"
#include "aura/midi/Midi.hpp"

namespace aura::midi {

enum class InstrumentKind : std::int32_t { Synth = 0, Sampler, DrumKit, External };

const char* instrumentKindName(InstrumentKind kind) noexcept;

/// Describes one parameter for automation lanes and the generic editor UI.
struct InstrumentParameterInfo {
    std::string id;          ///< stable, never localized (automation targets it)
    std::string nameKey;     ///< localization key for the display name
    float minimum = 0.0f;
    float maximum = 1.0f;
    float defaultValue = 0.0f;
    std::string unit;        ///< "dB", "Hz", "%", "" ...
    /// Discontinuous parameters (waveform, filter type) are shown as a list.
    std::vector<std::string> choices;
};

class Instrument {
public:
    virtual ~Instrument() = default;

    [[nodiscard]] virtual const std::string& id() const noexcept = 0;
    [[nodiscard]] virtual const std::string& name() const = 0;
    [[nodiscard]] virtual InstrumentKind kind() const noexcept = 0;

    /// Control thread. Allocates nothing after this call.
    virtual Status prepare(double sampleRate, int maxBlockSize, int numChannels) = 0;
    virtual void reset() noexcept = 0;

    /// Audio thread. `events` must be ordered by sampleOffset, which the engine
    /// fills in relative to the start of the block. Implementations must not
    /// allocate, lock or touch the filesystem.
    virtual void process(dsp::AudioBlockView& audio, const Message* events, int numEvents,
                         const dsp::ProcessContext& context) noexcept = 0;

    [[nodiscard]] virtual int maxVoices() const noexcept = 0;
    [[nodiscard]] virtual int activeVoices() const noexcept = 0;

    [[nodiscard]] virtual std::size_t parameterCount() const noexcept = 0;
    [[nodiscard]] virtual const InstrumentParameterInfo& parameterInfo(std::size_t index) const = 0;
    virtual void setParameter(std::size_t index, float value) noexcept = 0;
    [[nodiscard]] virtual float parameter(std::size_t index) const noexcept = 0;

    /// True while the instrument still produces sound (note held or release
    /// tail running). The engine uses this to decide whether a silent track can
    /// be skipped.
    [[nodiscard]] virtual bool isProducingSound() const noexcept = 0;

    /// All-notes-off, immediately or with the current release stage.
    virtual void allNotesOff(bool immediate) noexcept = 0;

    /// Persistence: parameter values by id, plus any sampler paths.
    [[nodiscard]] virtual json::Value saveState() const = 0;
    virtual Status restoreState(const json::Value& state) = 0;
};

/// The built-in AURA instrument ("AURA Basic"): two oscillators with detune and
/// mix, a resonant low-pass filter, an ADSR, velocity sensitivity and 16 voices
/// with oldest-voice stealing. Deliberately simple and CPU-cheap: it exists so
/// the MIDI path, the piano roll and offline rendering can be exercised for
/// real, and so a new project makes sound immediately.
class PolySynth final : public Instrument {
public:
    static constexpr int kMaxVoices = 16;

    enum Parameter : std::size_t {
        ParamOsc1Waveform = 0,
        ParamOsc2Waveform,
        ParamOsc2DetuneCents,
        ParamOsc2Level,
        ParamSubLevel,
        ParamFilterCutoff,
        ParamFilterResonance,
        ParamFilterEnvelopeAmount,
        ParamAttack,
        ParamDecay,
        ParamSustain,
        ParamRelease,
        ParamLevel,
        ParamPitchBendRange,
        ParamCount
    };

    PolySynth();
    ~PolySynth() override;

    // Instrument
    [[nodiscard]] const std::string& id() const noexcept override { return id_; }
    [[nodiscard]] const std::string& name() const override { return name_; }
    [[nodiscard]] InstrumentKind kind() const noexcept override { return InstrumentKind::Synth; }
    Status prepare(double sampleRate, int maxBlockSize, int numChannels) override;
    void reset() noexcept override;
    void process(dsp::AudioBlockView& audio, const Message* events, int numEvents,
                 const dsp::ProcessContext& context) noexcept override;
    [[nodiscard]] int maxVoices() const noexcept override { return kMaxVoices; }
    [[nodiscard]] int activeVoices() const noexcept override;
    [[nodiscard]] std::size_t parameterCount() const noexcept override { return ParamCount; }
    [[nodiscard]] const InstrumentParameterInfo& parameterInfo(std::size_t index) const override;
    void setParameter(std::size_t index, float value) noexcept override;
    [[nodiscard]] float parameter(std::size_t index) const noexcept override;
    [[nodiscard]] bool isProducingSound() const noexcept override;
    void allNotesOff(bool immediate) noexcept override;
    [[nodiscard]] json::Value saveState() const override;
    Status restoreState(const json::Value& state) override;

private:
    struct Voice {
        bool active = false;
        bool held = false;
        int note = 60;
        float velocity = 1.0f;
        std::uint64_t age = 0;   ///< for oldest-first voice stealing
        double pitchBendSemitones = 0.0;
        dsp::Oscillator osc1;
        dsp::Oscillator osc2;
        dsp::Oscillator sub;
        dsp::AdsrEnvelope envelope;
        dsp::Biquad<2> filter;
        /// Filter envelope reuses the amp envelope shape at a scaled depth; the
        /// value is sampled per block to keep the audio path branch-light.
        float filterEnvelopeValue = 0.0f;
    };

    void handleEvent(const Message& message) noexcept;
    void noteOn(int note, int velocity) noexcept;
    void noteOff(int note) noexcept;
    void updateVoiceFrequencies(Voice& voice) noexcept;
    void updateFilterCoefficients() noexcept;
    int findVoiceToSteal() noexcept;
    void renderVoice(Voice& voice, float* left, float* right, int startFrame, int endFrame) noexcept;

    std::vector<float> voiceScratch_; ///< one block, allocated in prepare()
    std::string id_{"aura.basic"};
    std::string name_{"AURA Basic"};
    double sampleRate_ = 48000.0;
    int numChannels_ = 2;
    std::uint64_t voiceAgeCounter_ = 0;

    Voice voices_[kMaxVoices];
    float parameters_[ParamCount]{};
    std::vector<InstrumentParameterInfo> parameterInfo_;

    /// Cached derived values (recomputed on the control thread when parameters
    /// change, never per sample).
    dsp::Waveform osc1Waveform_ = dsp::Waveform::Saw;
    dsp::Waveform osc2Waveform_ = dsp::Waveform::Square;
    double osc2DetuneCents_ = 7.0;
    float osc2Level_ = 0.35f;
    float subLevel_ = 0.2f;
    // The subtractive filter stage reads these; they are stored now so the
    // parameter surface (and saved patches) does not change when it lands.
    [[maybe_unused]] double filterCutoff_ = 6000.0;
    [[maybe_unused]] double filterResonance_ = 0.7;
    [[maybe_unused]] float filterEnvelopeAmount_ = 0.35f;
    float level_ = 0.7f;
    double pitchBendRange_ = 2.0;

    /// Sustain pedal state (CC64) - notes are held until the pedal lifts.
    bool sustainPedalDown_ = false;
    std::vector<int> sustainedNotes_; ///< preallocated, reserved in prepare()
};

/// Factory: creates a built-in instrument by id. Returns nullptr when the id is
/// unknown, so callers can report a proper error.
[[nodiscard]] std::unique_ptr<Instrument> createBuiltinInstrument(const std::string& id);

/// Ids of every built-in instrument (for the browser and for tests).
[[nodiscard]] std::vector<std::string> builtinInstrumentIds();

} // namespace aura::midi
