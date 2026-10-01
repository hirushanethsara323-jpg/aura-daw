// ============================================================================
// AURA DAW - tests/dsp/DspTests.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// DSP verification. Every test states the tolerance and why it is what it is:
// a "sounds fine" assertion is not a test, and a tolerance that hides a real
// defect is worse than no test at all.
// ============================================================================
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "aura/dsp/Compressor.hpp"
#include "aura/dsp/Delay.hpp"
#include "aura/dsp/Distortion.hpp"
#include "aura/dsp/Envelope.hpp"
#include "aura/dsp/Equalizer.hpp"
#include "aura/dsp/GainProcessor.hpp"
#include "aura/dsp/Limiter.hpp"
#include "aura/dsp/LevelMeter.hpp"
#include "aura/dsp/Loudness.hpp"
#include "aura/dsp/Oscillator.hpp"
#include "aura/dsp/PanProcessor.hpp"
#include "aura/dsp/ParameterSmoothing.hpp"
#include "aura/dsp/Reverb.hpp"
#include "support/TestSignals.hpp"

using namespace aura;
using Catch::Approx;

namespace {
constexpr double kSampleRate = 48000.0;
constexpr int kFrames = 24000; // 0.5 s: enough for every measurement below

dsp::ProcessContext makeContext() {
    dsp::ProcessContext context;
    context.sampleRate = kSampleRate;
    context.isPlaying = true;
    return context;
}
} // namespace

TEST_CASE("Gain applies the requested decibels", "[dsp][gain]") {
    dsp::GainProcessor gain;
    gain.prepare(kSampleRate, kFrames, 2);
    gain.setGainDb(-6.0f);

    test::Block block(2, kFrames);
    test::fillSine(block.channels[0], 1000.0, kSampleRate, 1.0f);
    test::fillSine(block.channels[1], 1000.0, kSampleRate, 1.0f);
    auto view = block.view(kFrames);
    gain.process(view, makeContext());

    // -6 dB = 0.501187, and the smoothed gain is given the whole block to settle.
    const float measuredPeak = test::peakFrom(block.channels[0], 4096);
    REQUIRE(measuredPeak == Approx(0.501187f).margin(0.002f));
    REQUIRE(test::allFinite(block.channels[0]));
}

TEST_CASE("Pan follows the documented law", "[dsp][pan]") {
    dsp::PanProcessor pan;
    pan.prepare(kSampleRate, kFrames, 2);

    SECTION("hard left silences the right channel") {
        pan.setPan(-1.0f);
        test::Block block(2, kFrames);
        test::fillSine(block.channels[0], 1000.0, kSampleRate);
        test::fillSine(block.channels[1], 1000.0, kSampleRate);
        auto view = block.view(kFrames);
        pan.process(view, makeContext());
        REQUIRE(test::peakFrom(block.channels[1], 4096) < 0.01f);
        REQUIRE(test::peakFrom(block.channels[0], 4096) > 0.9f);
    }

    SECTION("centre leaves both channels intact") {
        pan.setPan(0.0f);
        test::Block block(2, kFrames);
        test::fillSine(block.channels[0], 1000.0, kSampleRate);
        test::fillSine(block.channels[1], 1000.0, kSampleRate);
        auto view = block.view(kFrames);
        pan.process(view, makeContext());
        // A balance law must not boost at centre: both channels stay at unity.
        REQUIRE(test::peakFrom(block.channels[0], 4096) == Approx(1.0f).margin(0.02f));
        REQUIRE(test::peakFrom(block.channels[1], 4096) == Approx(1.0f).margin(0.02f));
    }
}

TEST_CASE("Biquad low-pass matches its analytic response", "[dsp][biquad]") {
    // Measure at 100 Hz and at 10 kHz with a 1 kHz cutoff (Q = 0.707). The
    // analytic magnitude comes from the same coefficient designer, so this test
    // verifies the *implementation* (state handling, block processing) rather
    // than re-deriving the formula.
    for (const double probe : {100.0, 1000.0, 10000.0}) {
        const auto coefficients = dsp::BiquadDesign::lowPass(kSampleRate, 1000.0, 0.70710678);
        dsp::Biquad<2> filter;
        filter.setCoefficients(coefficients);

        test::Block block(2, kFrames);
        // Unit amplitude on purpose: magnitudeDbAtFrequency() reports the
        // absolute level of the measured signal, so an input of 0.5 would show up
        // as a systematic -6.02 dB against the analytic unity-gain response.
        test::fillSine(block.channels[0], probe, kSampleRate, 1.0f);
        test::fillSine(block.channels[1], probe, kSampleRate, 1.0f);
        auto view = block.view(kFrames);
        filter.processBlock(view.channelPointers, 2, kFrames);

        const double measuredDb = test::magnitudeDbAtFrequency(block.channels[0], probe, kSampleRate);
        const double expectedDb = dsp::BiquadDesign::magnitudeDb(coefficients, probe, kSampleRate);
        // 0.5 dB covers the finite-length measurement window and the fact that
        // the analytic response is for the true steady state.
        INFO("probe " << probe << " Hz: measured " << measuredDb << " dB, analytic " << expectedDb);
        REQUIRE(measuredDb == Approx(expectedDb).margin(0.5));
    }
}

TEST_CASE("Parametric EQ boosts only the target band", "[dsp][eq]") {
    dsp::Equalizer eq;
    eq.prepare(kSampleRate, kFrames, 2);
    // Band 1, 1 kHz, +12 dB, moderate Q.
    eq.setBandEnabled(dsp::EqBand::Bell, true);
    eq.setBandFrequency(dsp::EqBand::Bell, 1000.0);
    eq.setBandGainDb(dsp::EqBand::Bell, 12.0);
    eq.setBandQ(dsp::EqBand::Bell, 1.0);

    test::Block block(2, kFrames);
    test::fillSine(block.channels[0], 1000.0, kSampleRate, 0.25f);
    test::fillSine(block.channels[1], 1000.0, kSampleRate, 0.25f);
    auto view = block.view(kFrames);
    eq.process(view, makeContext());

    const double boostDb =
        test::magnitudeDbAtFrequency(block.channels[0], 1000.0, kSampleRate) - 20.0 * std::log10(0.25);
    INFO("boost at 1 kHz: " << boostDb << " dB");
    REQUIRE(boostDb == Approx(12.0).margin(1.0));

    // A band an octave and a half away must stay close to unity.
    dsp::Equalizer flat;
    flat.prepare(kSampleRate, kFrames, 2);
    test::Block far(2, kFrames);
    test::fillSine(far.channels[0], 100.0, kSampleRate, 0.25f);
    test::fillSine(far.channels[1], 100.0, kSampleRate, 0.25f);
    auto farView = far.view(kFrames);
    eq.process(farView, makeContext());
    const double lowDb =
        test::magnitudeDbAtFrequency(far.channels[0], 100.0, kSampleRate) - 20.0 * std::log10(0.25);
    INFO("level at 100 Hz: " << lowDb << " dB");
    REQUIRE(std::abs(lowDb) < 2.0);
}

TEST_CASE("Compressor static curve matches the ideal ratio", "[dsp][compressor]") {
    const float threshold = -20.0f;
    const float ratio = 4.0f;

    for (const float inputDb : {-40.0f, -20.0f, -10.0f, 0.0f}) {
        dsp::Compressor compressor;
        compressor.prepare(kSampleRate, kFrames, 2);
        compressor.setThresholdDb(threshold);
        compressor.setRatio(ratio);
        compressor.setKneeDb(0.0f);
        compressor.setAttackMs(1.0f);
        compressor.setReleaseMs(50.0f);
        compressor.setMakeupDb(0.0f);

        const float amplitude = std::pow(10.0f, inputDb / 20.0f);
        test::Block block(2, kFrames);
        test::fillSine(block.channels[0], 1000.0, kSampleRate, amplitude);
        test::fillSine(block.channels[1], 1000.0, kSampleRate, amplitude);
        auto view = block.view(kFrames);
        compressor.process(view, makeContext());

        // Measure the settled part of the signal (after the attack has finished).
        const double outputRms = test::rmsFrom(block.channels[0], 8192);
        const double inputRms = test::rmsFrom(block.channels[0], 0) / 1.0; // pre-processing reference
        (void)inputRms;

        const double expectedDb = inputDb <= threshold
                                      ? inputDb
                                      : threshold + (inputDb - threshold) / ratio;
        // RMS of a sine is 3.01 dB below its peak, so compare like with like.
        const double measuredDb = 20.0 * std::log10(outputRms) + 3.0103;

        INFO("input " << inputDb << " dB -> measured " << measuredDb << " dB, ideal " << expectedDb);
        REQUIRE(measuredDb == Approx(expectedDb).margin(0.5));
    }
}

TEST_CASE("Limiter respects its ceiling and reports honest latency", "[dsp][limiter]") {
    dsp::Limiter limiter;
    limiter.prepare(kSampleRate, kFrames, 2);
    dsp::LimiterSettings settings;
    settings.ceilingDb = -1.0f;
    settings.lookaheadMs = 1.5f;
    limiter.setSettings(settings);

    REQUIRE(limiter.latencySamples() > 0);

    test::Block block(2, kFrames);
    // +6 dB over the ceiling.
    test::fillSine(block.channels[0], 440.0, kSampleRate, 2.0f);
    test::fillSine(block.channels[1], 440.0, kSampleRate, 2.0f);
    auto view = block.view(kFrames);
    limiter.process(view, makeContext());

    const float measuredPeak = test::peakFrom(block.channels[0], 4096);
    const float ceiling = std::pow(10.0f, -1.0f / 20.0f);
    INFO("peak " << measuredPeak << " vs ceiling " << ceiling);
    // 0.05 dB of overshoot tolerance: the limiter's gain ramp is finite, so a
    // sample can slip marginally above the ceiling - exactly what the docs say.
    REQUIRE(measuredPeak <= ceiling * 1.006f);
    REQUIRE(test::allFinite(block.channels[0]));
}

TEST_CASE("Delay produces the requested delay time", "[dsp][delay]") {
    dsp::Delay delay;
    delay.prepare(kSampleRate, kFrames, 2);
    dsp::DelaySettings delaySettings;
    delaySettings.timeLeftMs = 10.0f;
    delaySettings.timeRightMs = 10.0f;
    delaySettings.syncToTempo = false;
    delaySettings.feedback = 0.0f;
    delaySettings.mix = 1.0f; // fully wet
    delay.setSettings(delaySettings);

    test::Block block(2, kFrames);
    // Impulse at sample 0.
    block.channels[0][0] = 1.0f;
    block.channels[1][0] = 1.0f;
    auto view = block.view(kFrames);
    delay.process(view, makeContext());

    const int expectedDelaySamples = static_cast<int>(0.010 * kSampleRate); // 480
    // Find the peak after the dry impulse.
    int peakIndex = -1;
    float peakValue = 0.0f;
    for (int i = 1; i < kFrames; ++i) {
        if (std::abs(block.channels[0][static_cast<std::size_t>(i)]) > peakValue) {
            peakValue = std::abs(block.channels[0][static_cast<std::size_t>(i)]);
            peakIndex = i;
        }
    }
    INFO("delay peak at " << peakIndex << " (expected " << expectedDelaySamples << ")");
    REQUIRE(peakValue > 0.5f);
    REQUIRE(std::abs(peakIndex - expectedDelaySamples) <= 1);
}

TEST_CASE("Reverb tail decays and stays finite", "[dsp][reverb]") {
    dsp::Reverb reverb;
    reverb.prepare(kSampleRate, 96000, 2);
    dsp::ReverbSettings reverbSettings;
    reverbSettings.mix = 0.5f;
    reverbSettings.decaySeconds = 1.5f;
    reverb.setSettings(reverbSettings);

    test::Block block(2, 96000);
    // A short burst, then silence: the tail must decay rather than ring forever.
    for (int i = 0; i < 2400; ++i) {
        block.channels[0][static_cast<std::size_t>(i)] =
            0.5f * static_cast<float>(std::sin(test::kTwoPiLocal * 440.0 * i / kSampleRate));
        block.channels[1][static_cast<std::size_t>(i)] = block.channels[0][static_cast<std::size_t>(i)];
    }
    auto view = block.view(96000);
    reverb.process(view, makeContext());

    REQUIRE(test::allFinite(block.channels[0]));
    const double earlyTail = test::rmsFrom(block.channels[0], 3000);
    const double lateTail = test::rmsFrom(block.channels[0], 80000);
    INFO("early tail RMS " << earlyTail << ", late tail RMS " << lateTail);
    REQUIRE(earlyTail > lateTail);
    REQUIRE(lateTail < earlyTail * 0.5);
}

TEST_CASE("Oscillator generates the expected waveforms", "[dsp][oscillator]") {
    dsp::Oscillator oscillator;
    oscillator.prepare(kSampleRate);
    oscillator.setFrequency(1000.0);
    oscillator.setWaveform(dsp::Waveform::Sine);
    oscillator.setAmplitude(1.0f);

    std::vector<float> buffer(4800);
    oscillator.generate(buffer.data(), static_cast<int>(buffer.size()));

    REQUIRE(test::peak(buffer) == Approx(1.0f).margin(0.001f));
    REQUIRE(test::magnitudeDbAtFrequency(buffer, 1000.0, kSampleRate) == Approx(0.0).margin(0.1));
    // A sine has almost no second harmonic.
    REQUIRE(test::magnitudeDbAtFrequency(buffer, 2000.0, kSampleRate) < -60.0);
}

TEST_CASE("Envelope follows its ADSR stages", "[dsp][envelope]") {
    dsp::AdsrEnvelope envelope;
    envelope.prepare(kSampleRate);
    dsp::AdsrSettings settings;
    settings.attackMs = 10.0f;
    settings.decayMs = 10.0f;
    settings.sustainLevel = 0.5f;
    settings.releaseMs = 20.0f;
    envelope.setSettings(settings);

    envelope.noteOn();
    std::vector<float> attack(48000);
    for (auto& sample : attack)
        sample = envelope.nextSample();

    const float early = attack[48];      // 1 ms in
    const float afterAttack = attack[600]; // 12.5 ms in: past the peak
    const float sustain = attack[24000];
    REQUIRE(early < afterAttack);
    REQUIRE(sustain == Approx(0.5f).margin(0.05f));

    envelope.noteOff();
    float last = 0.0f;
    // The default curve is exponential, so the release approaches zero
    // asymptotically: reaching the -80 dB idle threshold from 0.5 takes about
    // 8.5 time constants (8.5 * 20 ms here), not the 20 ms of the linear case.
    for (int i = 0; i < 48000; ++i) // 1 s: comfortably past the release
        last = envelope.nextSample();
    REQUIRE(last < 0.01f);
    REQUIRE_FALSE(envelope.isActive());
}

TEST_CASE("Level meter reports peak and RMS", "[dsp][meter]") {
    dsp::LevelMeter meter;
    meter.prepare(kSampleRate);
    // The RMS reading is a 300 ms one-pole integration (the broadcast standard):
    // it needs a couple of seconds of signal to settle. Peak is instantaneous.
    test::Block block(2, 96000);
    test::fillSine(block.channels[0], 1000.0, kSampleRate, 0.5f);
    test::fillSine(block.channels[1], 1000.0, kSampleRate, 0.25f);
    // Feed in 100 ms blocks. Peak is instantaneous - it is read right after the
    // first block, before the peak-hold starts decaying. RMS is a 300 ms one-pole
    // integrator, so it needs the whole two seconds to settle.
    dsp::MeterSnapshot firstBlockPeak;
    for (int offset = 0; offset < 96000; offset += 4800) {
        test::Block slice(2, 4800);
        for (int channel = 0; channel < 2; ++channel)
            for (int i = 0; i < 4800; ++i)
                slice.channels[static_cast<std::size_t>(channel)][static_cast<std::size_t>(i)] =
                    block.channels[static_cast<std::size_t>(channel)][static_cast<std::size_t>(offset + i)];
        auto sliceView = slice.view(4800);
        meter.process(sliceView);
        if (offset == 0)
            firstBlockPeak = meter.snapshot();
    }

    const auto snapshot = meter.snapshot();
    REQUIRE(firstBlockPeak.peakLeft == Approx(0.5f).margin(0.01f));
    REQUIRE(firstBlockPeak.peakRight == Approx(0.25f).margin(0.01f));
    // RMS of a 0.5 sine is 0.3536.
    REQUIRE(snapshot.rmsLeft == Approx(0.3536f).margin(0.02f));
}

TEST_CASE("Loudness meter reports a calibrated LUFS reading", "[dsp][loudness]") {
    dsp::LoudnessMeter loudness;
    loudness.prepare(kSampleRate, 2);

    test::Block block(2, 48000 * 3); // 3 s: long enough for an integrated reading
    test::fillSine(block.channels[0], 1000.0, kSampleRate, 0.1f);  // -20 dBFS
    test::fillSine(block.channels[1], 1000.0, kSampleRate, 0.1f);
    auto view = block.view(48000 * 3);
    loudness.process(view);

    const auto loudnessSnapshot = loudness.snapshot();
    const float integrated = loudnessSnapshot.integratedLufs;
    // BS.1770 sums the (K-weighted) mean squares of the two channels:
    //   mean square = (0.1/sqrt(2))^2 = 0.005 per channel -> 0.010 summed
    //   -0.691 + 10*log10(0.010) = -20.69 LUFS, plus ~0.4 dB of K-weighting at
    //   1 kHz. (The sine's own RMS is 3 dB below its amplitude - that is the
    //   -23 not -20 dBFS. The +3.01 dB of summing two channels exactly cancels
    //   it again, which is why -20.7, not -23, is the right expectation here.)
    INFO("integrated " << integrated << " LUFS");
    REQUIRE(integrated == Approx(-20.3f).margin(1.2f));
    REQUIRE(loudnessSnapshot.momentaryLufs < 0.0f);
}

TEST_CASE("Parameter smoothing ramps without steps", "[dsp][smoothing]") {
    dsp::LinearSmoothedValue smoothed;
    smoothed.reset(kSampleRate, 0.0f, 0.010f); // 10 ms ramp from silence
    smoothed.setTarget(1.0f);

    std::vector<float> values;
    for (int i = 0; i < 4800; ++i) {
        smoothed.advance(1);
        values.push_back(smoothed.current());
    }

    REQUIRE(values.front() < 0.01f);
    REQUIRE(values.back() == Approx(1.0f).margin(0.001f));
    // No jump larger than the per-sample step of a 10 ms ramp (allowing slack).
    float worstStep = 0.0f;
    for (std::size_t i = 1; i < values.size(); ++i)
        worstStep = std::max(worstStep, std::abs(values[i] - values[i - 1]));
    REQUIRE(worstStep < 0.01f);
}
