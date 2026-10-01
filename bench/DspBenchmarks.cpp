// ============================================================================
// AURA DAW - bench/DspBenchmarks.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AURA DAW Project
//
// Micro-benchmarks for the DSP primitives (issue #1). One case per processor at
// the block sizes the engine actually uses, plus the two cases that matter for
// capacity planning: the standard track strip (EQ + compressor) and the master
// strip (limiter + loudness).
//
// Measurement notes that are easy to get wrong:
//   * every case processes ONE block per call, feeding the same input each time.
//     A delay or reverb therefore sees a steady-state signal after the warm-up,
//     not silence, and its feedback path stays exercised.
//   * the input is filled once, outside the timed region; `fillSine()` in the
//     timed loop would measure the generator instead of the processor.
//   * a checksum of the output is accumulated and printed by main(), so the
//     compiler cannot delete a case whose result nothing reads.
// ============================================================================
#include "Benchmarks.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "aura/core/Math.hpp"
#include "aura/dsp/Compressor.hpp"
#include "aura/dsp/Delay.hpp"
#include "aura/dsp/Distortion.hpp"
#include "aura/dsp/Equalizer.hpp"
#include "aura/dsp/GainProcessor.hpp"
#include "aura/dsp/Limiter.hpp"
#include "aura/dsp/LevelMeter.hpp"
#include "aura/dsp/Loudness.hpp"
#include "aura/dsp/PanProcessor.hpp"
#include "aura/dsp/Reverb.hpp"
#include "aura/midi/Instrument.hpp"

namespace aura::bench {
namespace {

constexpr double kSampleRate = 48000.0;

/// Output sink: the benchmarks write into `buffers`, and this keeps the result
/// observable so the optimiser cannot discard the work.
double g_checksum = 0.0;

struct Buffer {
    std::vector<float> left;
    std::vector<float> right;
    float* pointers[2] = {nullptr, nullptr};

    Buffer(int frames, bool fill)
        : left(static_cast<std::size_t>(frames)), right(static_cast<std::size_t>(frames)) {
        pointers[0] = left.data();
        pointers[1] = right.data();
        if (fill) {
            // 1 kHz plus a 7.3 kHz partial: a bare sine has silent regions in the
            // filter state and reads optimistically, and this is what tests use.
            for (int i = 0; i < frames; ++i) {
                const double t = static_cast<double>(i) / kSampleRate;
                const float value = static_cast<float>(0.4 * std::sin(math::kTwoPi * 1000.0 * t) +
                                                       0.2 * std::sin(math::kTwoPi * 7300.0 * t));
                left[static_cast<std::size_t>(i)] = value;
                right[static_cast<std::size_t>(i)] = value * 0.9f;
            }
        }
    }

    [[nodiscard]] dsp::AudioBlockView view(int frames) noexcept {
        return dsp::AudioBlockView{pointers, 2, frames};
    }

    void reduce() noexcept {
        g_checksum += static_cast<double>(left[0]) + static_cast<double>(right[0]);
    }
};

struct BenchContext {
    dsp::ProcessContext context;
    BenchContext() {
        context.sampleRate = kSampleRate;
        context.isPlaying = true;
    }
};

/// Registers one processor benchmark per block size.
template <typename MakeProcessor>
void addProcessor(const std::string& name, const std::string& description,
                  MakeProcessor make, const std::vector<int>& blockSizes = {64, 128, 256, 512}) {
    for (const int blockSize : blockSizes) {
        Case bench;
        bench.name = name + "/" + std::to_string(blockSize);
        bench.group = "dsp";
        bench.description = description + " (" + std::to_string(blockSize) + "-frame blocks)";
        bench.sampleRate = kSampleRate;
        bench.blockSize = blockSize;
        bench.channels = 2;

        // Setup is intentionally outside the timed callable: prepare() allocates
        // by design (ADR / Processor.hpp), so including it would measure the
        // allocator.
        std::shared_ptr<dsp::Processor> processor = make(blockSize);
        auto buffer = std::make_shared<Buffer>(blockSize, true);
        auto context = std::make_shared<BenchContext>();
        processor->prepare(kSampleRate, blockSize, 2);

        bench.call = [processor, buffer, context, blockSize]() {
            auto view = buffer->view(blockSize);
            processor->process(view, context->context);
            buffer->reduce();
        };
        registry().add(std::move(bench));
    }
}

} // namespace

void registerDspBenchmarks() {
    addProcessor("dsp/gain", "Gain with a static -6 dB setting",
                 [](int) { return std::make_unique<dsp::GainProcessor>(); });

    addProcessor("dsp/pan", "Constant-power pan at 25% right",
                 [](int) {
                     auto pan = std::make_unique<dsp::PanProcessor>();
                     pan->setPan(0.25f);
                     return pan;
                 });

    addProcessor("dsp/eq", "5-band EQ: HP + low shelf + bell + high shelf + LP, all active",
                 [](int) {
                     auto eq = std::make_unique<dsp::Equalizer>();
                     dsp::EqSettings settings;
                     settings.bands[0] = {true, 80.0, 0.707, 0.0, 0.7};
                     settings.bands[1] = {true, 200.0, 0.707, 2.5, 0.7};
                     settings.bands[2] = {true, 1500.0, 1.2, -3.0, 0.7};
                     settings.bands[3] = {true, 6000.0, 0.707, 1.5, 0.7};
                     settings.bands[4] = {true, 16000.0, 0.707, 0.0, 0.7};
                     eq->setSettings(settings);
                     return eq;
                 });

    addProcessor("dsp/compressor", "Compressor at 4:1, 10 ms attack, 120 ms release, auto makeup",
                 [](int) {
                     auto compressor = std::make_unique<dsp::Compressor>();
                     compressor->setThresholdDb(-18.0f);
                     compressor->setRatio(4.0f);
                     compressor->setAttackMs(10.0f);
                     compressor->setReleaseMs(120.0f);
                     compressor->setAutoMakeup(true);
                     return compressor;
                 });

    addProcessor("dsp/limiter", "Limiter with the default 1.5 ms look-ahead",
                 [](int) { return std::make_unique<dsp::Limiter>(); });

    addProcessor("dsp/delay", "Tempo-synced delay, 35% feedback, 25% damping",
                 [](int) { return std::make_unique<dsp::Delay>(); });

    addProcessor("dsp/reverb", "Algorithmic reverb, 1.8 s decay, modulation on",
                 [](int) { return std::make_unique<dsp::Reverb>(); });

    addProcessor("dsp/distortion", "Drive waveshaper (31-tap half-band, 2x oversampled)",
                 [](int) { return std::make_unique<dsp::Distortion>(); });

    // Metering: peak/RMS per track and LUFS on the master are on the audio path,
    // so their cost belongs in the same table as the processors themselves.
    for (const int blockSize : {128, 512}) {
        Case meter;
        meter.name = "dsp/levelmeter/" + std::to_string(blockSize);
        meter.group = "metering";
        meter.description = "Peak + RMS meter on a stereo block";
        meter.sampleRate = kSampleRate;
        meter.blockSize = blockSize;
        meter.channels = 2;
        auto buffer = std::make_shared<Buffer>(blockSize, true);
        auto levelMeter = std::make_shared<dsp::LevelMeter>();
        levelMeter->prepare(kSampleRate);
        meter.call = [buffer, levelMeter, blockSize]() {
            auto view = buffer->view(blockSize);
            levelMeter->process(view);
            g_checksum += static_cast<double>(levelMeter->snapshot().peakLeft);
        };
        registry().add(std::move(meter));

        Case loudness;
        loudness.name = "dsp/loudness/" + std::to_string(blockSize);
        loudness.group = "metering";
        loudness.description = "ITU-R BS.1770 loudness (K-weighting + gated integration)";
        loudness.sampleRate = kSampleRate;
        loudness.blockSize = blockSize;
        loudness.channels = 2;
        auto loudnessBuffer = std::make_shared<Buffer>(blockSize, true);
        auto loudnessMeter = std::make_shared<dsp::LoudnessMeter>();
        loudnessMeter->prepare(kSampleRate, 2);
        loudness.call = [loudnessBuffer, loudnessMeter, blockSize]() {
            auto view = loudnessBuffer->view(blockSize);
            loudnessMeter->process(view);
            g_checksum += static_cast<double>(loudnessMeter->snapshot().momentaryLufs);
        };
        registry().add(std::move(loudness));
    }

    // The polyphony budget in docs/PERFORMANCE.md is stated in voices, so it gets
    // a case measured in voices: four PolySynths at their full 16-voice polyphony
    // = 64 voices sounding at once. Note-on events are sent once per call through
    // a fixed array - no allocation, and every voice is asked to keep sounding.
    for (const int blockSize : {64, 128, 256}) {
        Case voices;
        voices.name = "dsp/instrument-64-voices/" + std::to_string(blockSize);
        voices.group = "instrument";
        voices.description = "4x PolySynth, 64 voices sounding (the budget case in PERFORMANCE.md)";
        voices.sampleRate = kSampleRate;
        voices.blockSize = blockSize;
        voices.channels = 2;

        constexpr int kInstruments = 4;
        // shared_ptr because two callables (the timed one and the sanity check)
        // need to reach the instruments; the harness intentionally does not give a
        // case ownership semantics beyond "keep my captures alive".
        auto synths = std::make_shared<std::vector<std::shared_ptr<midi::PolySynth>>>();
        auto events = std::make_shared<std::vector<midi::Message>>();
        for (int instrument = 0; instrument < kInstruments; ++instrument) {
            auto synth = std::make_shared<midi::PolySynth>();
            if (!synth->prepare(kSampleRate, blockSize, 2))
                std::fprintf(stderr, "bench: PolySynth prepare failed\n");
            for (int voice = 0; voice < 16; ++voice)
                events->push_back(midi::Message::noteOn(0, 48 + voice * 2, 100));
            synths->push_back(std::move(synth));
        }
        const int eventCount = static_cast<int>(events->size());

        auto buffer = std::make_shared<Buffer>(blockSize, false);
        auto context = std::make_shared<BenchContext>();
        voices.call = [synths, events, buffer, context, blockSize, eventCount]() {
            auto view = buffer->view(blockSize);
            for (std::size_t instrument = 0; instrument < synths->size(); ++instrument) {
                // Every instrument gets the shared event list: the measurement is
                // about 64 voices' worth of DSP, not about event dispatch.
                (*synths)[instrument]->process(view, events->data(), eventCount, context->context);
            }
            buffer->reduce();
        };
        voices.sanityCheck = [synths]() {
            for (const auto& synth : *synths) {
                if (!synth->isProducingSound() || synth->activeVoices() == 0)
                    return false;
            }
            return true;
        };
        registry().add(std::move(voices));
    }

    // The two strips an engine actually runs per track / per session.
    Case strip;
    strip.name = "chain/track-strip/256";
    strip.group = "chain";
    strip.description = "Gain + EQ + compressor in series, as one track insert chain";
    strip.sampleRate = kSampleRate;
    strip.blockSize = 256;
    strip.channels = 2;
    {
        const int blockSize = 256;
        auto gain = std::make_shared<dsp::GainProcessor>();
        auto eq = std::make_shared<dsp::Equalizer>();
        auto compressor = std::make_shared<dsp::Compressor>();
        dsp::EqSettings eqSettings;
        eqSettings.bands[0] = {true, 80.0, 0.707, 0.0, 0.7};
        eqSettings.bands[2] = {true, 1500.0, 1.2, -3.0, 0.7};
        eq->setSettings(eqSettings);
        compressor->setAutoMakeup(true);
        gain->prepare(kSampleRate, blockSize, 2);
        eq->prepare(kSampleRate, blockSize, 2);
        compressor->prepare(kSampleRate, blockSize, 2);
        auto buffer = std::make_shared<Buffer>(blockSize, true);
        auto context = std::make_shared<BenchContext>();
        strip.call = [gain, eq, compressor, buffer, context, blockSize]() {
            auto view = buffer->view(blockSize);
            gain->process(view, context->context);
            eq->process(view, context->context);
            compressor->process(view, context->context);
            buffer->reduce();
        };
    }
    registry().add(std::move(strip));

    Case master;
    master.name = "chain/master-strip/256";
    master.group = "chain";
    master.description = "Master limiter + loudness metering, as the engine runs them";
    master.sampleRate = kSampleRate;
    master.blockSize = 256;
    master.channels = 2;
    {
        const int blockSize = 256;
        auto limiter = std::make_shared<dsp::Limiter>();
        auto loudness = std::make_shared<dsp::LoudnessMeter>();
        limiter->prepare(kSampleRate, blockSize, 2);
        loudness->prepare(kSampleRate, 2);
        auto buffer = std::make_shared<Buffer>(blockSize, true);
        auto context = std::make_shared<BenchContext>();
        master.call = [limiter, loudness, buffer, context, blockSize]() {
            auto view = buffer->view(blockSize);
            limiter->process(view, context->context);
            loudness->process(view);
            g_checksum += static_cast<double>(loudness->snapshot().shortTermLufs);
        };
    }
    registry().add(std::move(master));
}

double dspBenchmarkChecksum() {
    return g_checksum;
}

} // namespace aura::bench
