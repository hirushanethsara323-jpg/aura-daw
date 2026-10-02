// ============================================================================
// AURA DAW - tests/audio/AudioTests.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "aura/audio/AudioClip.hpp"
#include "aura/audio/Mixer.hpp"
#include "aura/audio/RecordingEngine.hpp"
#include "aura/audio/Track.hpp"
#include "aura/audio/WaveformCache.hpp"
#include "aura/core/FileSystem.hpp"
#include "aura/media/AudioFile.hpp"
#include "support/TestSignals.hpp"

using namespace aura;
using Catch::Approx;

namespace {

std::shared_ptr<graph::SampleStream> sineStream(int frames, double hz = 440.0,
                                                float amplitude = 0.5f) {
    std::vector<std::vector<float>> channels(2, std::vector<float>(static_cast<std::size_t>(frames)));
    test::fillSine(channels[0], hz, 48000.0, amplitude);
    test::fillSine(channels[1], hz, 48000.0, amplitude);
    return graph::MemoryStream::fromPlanar(std::move(channels), 48000.0);
}

} // namespace

TEST_CASE("WAV files survive a full write/read round trip", "[audio][file]") {
    for (const auto bits : {16, 24, 32}) {
        const std::string path = "/tmp/aura-wav-" + std::to_string(bits) + ".wav";
        std::vector<std::vector<float>> channels(2, std::vector<float>(4800));
        test::fillSine(channels[0], 1000.0, 48000.0, 0.5f);
        test::fillSine(channels[1], 1000.0, 48000.0, 0.25f);

        media::AudioFileWriter::Settings settings;
        settings.sampleRate = 48000;
        settings.numChannels = 2;
        settings.bitsPerSample = static_cast<std::uint16_t>(bits);
        settings.format = bits == 16   ? media::SampleFormat::PcmInt16
                          : bits == 24 ? media::SampleFormat::PcmInt24
                                       : media::SampleFormat::PcmInt32;

        REQUIRE(static_cast<bool>(media::writePlanarToFile(path, channels, 48000, settings)));

        auto info = media::probeFile(path);
        REQUIRE(info.hasValue());
        REQUIRE(info.value().sampleRate == 48000);
        REQUIRE(info.value().numChannels == 2);
        REQUIRE(info.value().lengthFrames == 4800);

        auto planar = media::readFileToPlanar(path);
        REQUIRE(planar.hasValue());
        REQUIRE(planar.value()->size() == 2);
        REQUIRE(planar.value()->at(0).size() == 4800);

        // Quantisation error is bounded by one LSB of the chosen depth; for 16
        // bits that is 1/32768 = 3.1e-5, and we allow a small margin.
        const float lsb = 1.0f / static_cast<float>(1u << (bits - 1));
        REQUIRE(test::maxDifference(planar.value()->at(0), channels[0]) <= lsb * 1.5f);
    }
}

TEST_CASE("A WAV whose header over-claims is read at its real length", "[audio][file][fuzz][regression]") {
    // REGRESSION, found by the WAV fuzzer (tests/core/FuzzSmokeTests.cpp): nothing
    // compared the data chunk's claimed size against the size of the file. A 60-byte
    // file claiming 0xFFFFFF00 bytes of 24-bit stereo made readFileToPlanar() resize
    // its buffers to ~1.4 billion frames per channel - about 5.7 GB each - and the
    // process was killed. Any file from the internet could do it, and so could a
    // recording interrupted midway, which is the case the clamp has to stay
    // *useful* for: the frames that were written must still be readable.
    const std::string path = "/tmp/aura-wav-overclaim.wav";

    // A valid 100-frame 24-bit stereo file, then the data chunk size field is
    // rewritten to claim far more than the file holds.
    const int frames = 100;
    const std::uint32_t bytesPerFrame = 2 * 3;
    const std::uint32_t realDataBytes = static_cast<std::uint32_t>(frames) * bytesPerFrame;
    const std::uint32_t claimedDataBytes = 0xFFFFFF00u;

    std::vector<std::uint8_t> file;
    const auto put32 = [&file](std::uint32_t value) {
        file.push_back(static_cast<std::uint8_t>(value & 0xFF));
        file.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
        file.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
        file.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    };
    const auto put16 = [&file](std::uint16_t value) {
        file.push_back(static_cast<std::uint8_t>(value & 0xFF));
        file.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    };
    const auto putId = [&file](const char* id) {
        for (int i = 0; i < 4; ++i)
            file.push_back(static_cast<std::uint8_t>(id[i]));
    };

    putId("RIFF");
    put32(36 + claimedDataBytes); // the RIFF size is a claim too
    putId("WAVE");
    putId("fmt ");
    put32(16);
    put16(1); // PCM
    put16(2); // channels
    put32(48000);
    put32(48000 * bytesPerFrame);
    put16(static_cast<std::uint16_t>(bytesPerFrame));
    put16(24);
    putId("data");
    put32(claimedDataBytes);
    for (std::uint32_t i = 0; i < realDataBytes; ++i)
        file.push_back(static_cast<std::uint8_t>(i & 0xFF));

    {
        std::FILE* out = std::fopen(path.c_str(), "wb");
        REQUIRE(out != nullptr);
        REQUIRE(std::fwrite(file.data(), 1, file.size(), out) == file.size());
        std::fclose(out);
    }

    auto info = media::probeFile(path);
    REQUIRE(info.hasValue());
    INFO("claimed 24-bit stereo frames: " << (claimedDataBytes / bytesPerFrame)
                                          << ", real: " << frames);
    REQUIRE(info.value().lengthFrames == frames); // clamped to the file, not the claim
    REQUIRE(info.value().truncated);
    REQUIRE(info.value().numChannels == 2);

    // The audio that does exist is still readable: the clamp must not turn an
    // interrupted recording into an unreadable file.
    auto planar = media::readFileToPlanar(path);
    REQUIRE(planar.hasValue());
    REQUIRE(planar.value()->size() == 2);
    REQUIRE(planar.value()->at(0).size() == static_cast<std::size_t>(frames));
    REQUIRE(planar.value()->at(1).size() == static_cast<std::size_t>(frames));

    // A file whose header over-claims by a little (a truncated tail) behaves the
    // same way, and a file with a corrupt sample rate is refused rather than
    // believed.
    std::vector<std::uint8_t> corruptRate = file;
    corruptRate[24] = 0xFF; // sample rate low byte of the fmt chunk
    corruptRate[25] = 0xFF;
    corruptRate[26] = 0xFF;
    corruptRate[27] = 0xFF;
    const std::string corruptPath = "/tmp/aura-wav-corrupt-rate.wav";
    std::FILE* out = std::fopen(corruptPath.c_str(), "wb");
    REQUIRE(out != nullptr);
    REQUIRE(std::fwrite(corruptRate.data(), 1, corruptRate.size(), out) == corruptRate.size());
    std::fclose(out);
    REQUIRE_FALSE(media::probeFile(corruptPath).hasValue());
}

TEST_CASE("Float WAV export is bit-exact", "[audio][file]") {
    const std::string path = "/tmp/aura-wav-float.wav";
    std::vector<std::vector<float>> channels(2, std::vector<float>(1024));
    test::fillSine(channels[0], 500.0, 48000.0, 0.75f);
    test::fillSine(channels[1], 700.0, 48000.0, 0.5f);

    media::AudioFileWriter::Settings settings;
    settings.sampleRate = 96000;
    settings.numChannels = 2;
    settings.bitsPerSample = 32;
    settings.format = media::SampleFormat::Float32;
    REQUIRE(static_cast<bool>(media::writePlanarToFile(path, channels, 96000, settings)));

    auto planar = media::readFileToPlanar(path);
    REQUIRE(planar.hasValue());
    REQUIRE(planar.value()->at(0).size() == 1024);
    REQUIRE(test::maxDifference(planar.value()->at(0), channels[0]) == 0.0f);
}

TEST_CASE("Clip rendering is non-destructive and window-limited", "[audio][clip]") {
    audio::Clip clip(1, audio::ClipType::Audio, "Take");
    clip.setStart(1000);
    clip.setLength(1000);
    clip.setSourceOffset(0);
    clip.setStream(sineStream(48000));
    REQUIRE(clip.isReady());
    REQUIRE(clip.end() == 2000);

    test::Block block(2, 512);
    auto view = block.view(512);
    // Before the clip: nothing is written.
    REQUIRE(clip.render(400, 512, view.channelPointers, 2) == 0);
    // Overlapping the clip start: render() returns how many frames it wrote, so a
    // window that half-covers the clip writes exactly the overlapping part
    // (500..1000 = 500 frames, capped by the 512-frame window -> 12).
    REQUIRE(clip.render(500, 512, view.channelPointers, 2) == 12);
    // Inside: the whole window is filled.
    REQUIRE(clip.render(1000, 512, view.channelPointers, 2) == 512);
    // After the clip end: nothing.
    REQUIRE(clip.render(5000, 512, view.channelPointers, 2) == 0);
}

TEST_CASE("A clip refuses a channel count its stack scratch cannot hold",
          "[audio][clip][realtime][regression]") {
    // render() keeps its per-channel write pointers in a fixed stack array
    // (audio::kMaxClipChannels) because building that array per block would put the
    // heap on the audio thread. This pins both ends of the boundary: a count inside
    // it renders, a count outside it renders nothing instead of walking off the end
    // of the array.
    //
    // Regression for a std::vector<float*> that lived here until rt_audit's function
    // extractor was fixed enough to see the function at all. `view` only owns two
    // channel pointers, so every refused call below would have been an out-of-bounds
    // write if the guard were missing or moved after the pointer setup.
    audio::Clip clip(1, audio::ClipType::Audio, "Boundary");
    clip.setStart(0);
    clip.setLength(1000);
    clip.setSourceOffset(0);
    clip.setStream(sineStream(48000));
    REQUIRE(clip.isReady());

    test::Block block(2, 64);
    auto view = block.view(64);

    // Inside the budget: the window is filled.
    REQUIRE(clip.render(0, 64, view.channelPointers, 2) == 64);
    // The widest scratch, one past it, and both non-positive counts: all refused.
    REQUIRE(audio::kMaxClipChannels == 64);
    REQUIRE(clip.render(0, 64, view.channelPointers, audio::kMaxClipChannels + 1) == 0);
    REQUIRE(clip.render(0, 64, view.channelPointers, 0) == 0);
    REQUIRE(clip.render(0, 64, view.channelPointers, -1) == 0);
}

TEST_CASE("Clip operations edit the window, not the source", "[audio][clip]") {
    audio::Clip clip(1, audio::ClipType::Audio, "Take");
    clip.setStart(0);
    clip.setLength(10000);
    clip.setStream(sineStream(20000));
    clip.setSourcePeak(0.5f);

    SECTION("split keeps the two halves contiguous") {
        audio::Clip right(2, audio::ClipType::Audio, "Take (right)");
        REQUIRE(audio::clipops::split(clip, 4000, right, 2));
        REQUIRE(clip.length() == 4000);
        REQUIRE(right.start() == 4000);
        REQUIRE(right.length() == 6000);
        REQUIRE(right.sourceOffset() == 4000);
    }

    SECTION("trim start moves the source window") {
        REQUIRE(audio::clipops::trimStart(clip, 2500));
        REQUIRE(clip.start() == 2500);
        REQUIRE(clip.length() == 7500);
        REQUIRE(clip.sourceOffset() == 2500);
    }

    SECTION("slip moves the source window inside the clip") {
        REQUIRE(audio::clipops::slip(clip, 500));
        REQUIRE(clip.start() == 0);
        REQUIRE(clip.sourceOffset() == 500);
    }

    SECTION("normalise computes gain from the measured source peak") {
        clip.setSourcePeak(0.5f);
        REQUIRE(audio::clipops::normalise(clip, -1.0f));
        // -1 dBFS = 0.8913 linear; from a 0.5 peak that is a gain of 1.7826.
        REQUIRE(clip.gain() == Approx(1.7826f).margin(0.01f));
    }

    SECTION("normalise refuses a silent or unsourced clip") {
        clip.setSourcePeak(0.0f);
        REQUIRE_FALSE(audio::clipops::normalise(clip, -1.0f));
    }
}

TEST_CASE("Mixer solo rules keep buses and sends audible", "[audio][mixer]") {
    audio::Mixer mixer;
    mixer.prepare(48000.0, 256, 2);

    const auto guitar = mixer.createTrack(audio::TrackKind::Audio, "Guitar");
    const auto drums = mixer.createTrack(audio::TrackKind::Audio, "Drums");
    const auto reverbBus = mixer.createTrack(audio::TrackKind::Aux, "Reverb");

    // Guitar sends into the reverb bus.
    audio::Send send;
    send.destination = reverbBus;
    send.level = 0.5f;
    send.enabled = true;
    mixer.findTrack(guitar)->addSend(send);

    REQUIRE(mixer.trackCount() == 3);
    REQUIRE_FALSE(mixer.anySoloed());

    mixer.findTrack(guitar)->setSoloed(true);
    REQUIRE(mixer.resolveSoloState());
    REQUIRE(mixer.anySoloed());

    // The soloed track stays audible, the aux bus that receives its send stays
    // audible (solo-safe), and the unrelated track is soloed out.
    REQUIRE_FALSE(mixer.findTrack(guitar)->isSoloedOut());
    REQUIRE_FALSE(mixer.findTrack(reverbBus)->isSoloedOut());
    REQUIRE(mixer.findTrack(drums)->isSoloedOut());
    REQUIRE_FALSE(mixer.master().isSoloedOut());

    mixer.findTrack(guitar)->setSoloed(false);
    REQUIRE(mixer.resolveSoloState());
    REQUIRE_FALSE(mixer.findTrack(drums)->isSoloedOut());
}

TEST_CASE("Mixer reports the insert latency used for compensation", "[audio][mixer]") {
    audio::Mixer mixer;
    mixer.prepare(48000.0, 512, 2);
    const auto track = mixer.createTrack(audio::TrackKind::Audio, "Vocal");

    dsp::Limiter limiter;
    limiter.prepare(48000.0, 512, 2);
    dsp::LimiterSettings settings;
    settings.ceilingDb = -1.0f;
    settings.lookaheadMs = 1.5f;
    limiter.setSettings(settings);

    audio::InsertSlot slot;
    slot.name = "Limiter";
    slot.processor = &limiter;
    REQUIRE(mixer.findTrack(track)->addInsert(slot));

    REQUIRE(mixer.maximumInsertLatencySamples() == limiter.latencySamples());
    REQUIRE(mixer.masterLatencySamples() == mixer.masterLimiter().latencySamples());
}

TEST_CASE("Waveform cache builds a pyramid with the documented bucket size",
          "[audio][waveform]") {
    audio::WaveformCache cache;
    REQUIRE(audio::WaveformCache::kBaseFramesPerBucket == 256);
    REQUIRE(audio::WaveformCache::kMaxLevels == 12);
    REQUIRE_FALSE(cache.isComplete());
    REQUIRE(cache.levelCount() == 0);

    const std::string path = "/tmp/aura-waveform-test.wav";
    std::vector<std::vector<float>> channels(2, std::vector<float>(48000));
    test::fillSine(channels[0], 100.0, 48000.0, 0.8f);
    test::fillSine(channels[1], 100.0, 48000.0, 0.4f);
    media::AudioFileWriter::Settings settings;
    settings.sampleRate = 48000;
    settings.numChannels = 2;
    settings.bitsPerSample = 24;
    REQUIRE(static_cast<bool>(media::writePlanarToFile(path, channels, 48000, settings)));

    REQUIRE(static_cast<bool>(cache.buildFor(path, 48000, 48000)));
    if (!cache.isComplete())
        cache.waitForCompletion(5000);

    REQUIRE(cache.isComplete());
    REQUIRE(cache.levelCount() >= 2);
    const auto* base = cache.level(0);
    REQUIRE(base != nullptr);
    REQUIRE(base->framesPerBucket == 256);
    REQUIRE(base->bucketCount() == 48000 / 256 + 1);
    REQUIRE(cache.overallPeak() == Approx(0.8f).margin(0.05f));
    // Level 1 must be coarser than level 0.
    REQUIRE(cache.level(1)->framesPerBucket == 512);
}

TEST_CASE("Recording engine writes a readable take through the disk thread",
          "[audio][recording]") {
    const std::string directory = "/tmp/aura-recording-test";
    (void)filesystem::createDirectories(directory);

    audio::RecordingEngine recorder;
    audio::RecordingSettings settings;
    settings.outputDirectory = directory;
    settings.temporaryDirectory = directory + "/incomplete";
    settings.sampleRate = 48000;
    settings.numChannels = 2;
    settings.bitsPerSample = 24;
    settings.minimumFreeMegabytes = 0; // the sandbox may report very little
    recorder.setSettings(settings);
    recorder.prepare(1u << 16);

    REQUIRE_FALSE(recorder.isRecording());
    REQUIRE(static_cast<bool>(recorder.startTake(0, "Unit Test Take")));
    REQUIRE(recorder.isRecording());
    REQUIRE_FALSE(recorder.currentTemporaryPath().empty());

    // 0.5 s of input in blocks, exactly as the audio thread would deliver it.
    std::vector<float> left(512);
    std::vector<float> right(512);
    test::fillSine(left, 440.0, 48000.0, 0.5f);
    test::fillSine(right, 440.0, 48000.0, 0.5f);
    const float* inputs[2] = {left.data(), right.data()};
    for (int block = 0; block < 46; ++block) {
        // Wrap the phase per block so the tone is continuous.
        for (std::size_t i = 0; i < left.size(); ++i) {
            const double t = static_cast<double>(block * 512 + static_cast<int>(i)) / 48000.0;
            const float value = 0.5f * static_cast<float>(std::sin(test::kTwoPiLocal * 440.0 * t));
            left[i] = value;
            right[i] = value;
        }
        recorder.processInput(inputs, 2, 512);
    }

    auto finished = recorder.stopTake();
    REQUIRE(finished.hasValue());
    REQUIRE_FALSE(recorder.isRecording());

    auto info = media::probeFile(finished.value());
    REQUIRE(info.hasValue());
    REQUIRE(info.value().numChannels == 2);
    REQUIRE(info.value().lengthFrames >= 20000);
    REQUIRE(recorder.droppedFrames() == 0);
}

TEST_CASE("Interrupted takes are recovered rather than lost", "[audio][recording][recovery]") {
    const std::string directory = "/tmp/aura-recording-recovery";
    (void)filesystem::createDirectories(directory + "/incomplete");

    // Simulate a crash: a leftover .partial.wav with a valid header.
    const std::string partial = directory + "/incomplete/Crashed Take.partial.wav";
    std::vector<std::vector<float>> channels(2, std::vector<float>(480));
    test::fillSine(channels[0], 200.0, 48000.0, 0.3f);
    test::fillSine(channels[1], 200.0, 48000.0, 0.3f);
    media::AudioFileWriter::Settings settings;
    settings.sampleRate = 48000;
    settings.numChannels = 2;
    settings.bitsPerSample = 24;
    REQUIRE(static_cast<bool>(media::writePlanarToFile(partial, channels, 48000, settings)));

    audio::RecordingEngine recorder;
    audio::RecordingSettings recordingSettings;
    recordingSettings.outputDirectory = directory;
    recordingSettings.temporaryDirectory = directory + "/incomplete";
    recorder.setSettings(recordingSettings);

    const auto recovered = recorder.recoverIncompleteTakes();
    REQUIRE(recovered.size() == 1);
    auto info = media::probeFile(recovered.front());
    REQUIRE(info.hasValue());
    REQUIRE(info.value().lengthFrames == 480);
}
