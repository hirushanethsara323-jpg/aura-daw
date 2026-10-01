// ============================================================================
// AURA DAW - tests/api/DeviceLifetimeTests.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// REGRESSION LAYER: audio device ownership.
//
// The engine holds a *raw* pointer to its device (it has to: the real-time path
// may not join a shared_ptr's refcount traffic). Whoever owns the device is
// therefore obliged to tell the engine before destroying it. A soak run under
// AddressSanitizer found the exact failure this file pins down:
//
//     heap-use-after-free in AudioEngine::shutdown() -> device_->stop()
//     ... freed by the owner's unique_ptr destructor,
//     ... because AudioDeviceManager::close() did `device_.reset()` and left the
//         engine pointing at freed memory.
//
// Without the fix these tests abort under ASAN (and crash intermittently in a
// release build); with it they pass and document the contract.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <memory>

#include "aura/audioio/AudioDeviceManager.hpp"
#include "aura/engine/AudioEngine.hpp"

using aura::audioio::AudioDeviceInfo;
using aura::audioio::AudioDeviceManager;
using aura::audioio::DriverType;
using aura::engine::AudioEngine;
using aura::engine::EngineSettings;

namespace {

constexpr const char* kMockDeviceId = "mock:aura";

EngineSettings testSettings() {
    EngineSettings settings;
    settings.sampleRate = 48000.0;
    settings.bufferSize = 256;
    settings.numOutputChannels = 2;
    settings.numInputChannels = 2;
    return settings;
}

AudioDeviceInfo mockDeviceInfo() {
    AudioDeviceInfo info;
    info.id = "mock:test";
    info.name = "Mock";
    info.driver = DriverType::Mock;
    info.maxInputChannels = 2;
    info.maxOutputChannels = 2;
    info.defaultSampleRate = 48000.0;
    info.defaultBufferSize = 256;
    info.minimumBufferSize = 32;
    info.maximumBufferSize = 2048;
    return info;
}

} // namespace

TEST_CASE("engine: detachDevice releases the device and shutdown stays safe",
          "[api][engine][regression]") {
    auto device = AudioDeviceManager::createMockDevice(mockDeviceInfo());
    REQUIRE(device != nullptr);

    AudioEngine engine;
    const EngineSettings settings = testSettings();
    REQUIRE(static_cast<bool>(engine.initialise(device.get(), settings)));
    CHECK(engine.currentDevice() == device.get());
    CHECK(engine.isRunning());

    // The owner is about to destroy the device: one call and the engine holds
    // nothing that can dangle.
    engine.detachDevice();
    CHECK(engine.currentDevice() == nullptr);
    CHECK_FALSE(engine.isRunning());

    // Every one of these used to dereference the freed device.
    engine.shutdown();
    engine.shutdown();
    engine.setSettings(settings);
    CHECK(engine.currentDevice() == nullptr);
}

TEST_CASE("engine: open and close leave no dangling device reference",
          "[api][engine][regression][integration]") {
    AudioEngine engine;
    AudioDeviceManager manager;
    manager.attachEngine(&engine);
    REQUIRE(static_cast<bool>(manager.refresh()));

    REQUIRE(manager.findDevice(kMockDeviceId) != nullptr);

    REQUIRE(static_cast<bool>(manager.open(kMockDeviceId, testSettings())));
    REQUIRE(manager.isOpen());
    REQUIRE(engine.currentDevice() != nullptr);
    REQUIRE(engine.isRunning());

    // close() must detach *before* destroying the device.
    manager.close();
    CHECK_FALSE(manager.isOpen());
    CHECK(manager.currentDeviceId().empty());
    CHECK(engine.currentDevice() == nullptr);
    CHECK_FALSE(engine.isRunning());

    // The calls that tripped AddressSanitizer before the fix.
    engine.shutdown();
    engine.setSettings(testSettings());
}

TEST_CASE("engine: re-opening retires the previous device safely",
          "[api][engine][regression][integration]") {
    AudioEngine engine;
    AudioDeviceManager manager;
    manager.attachEngine(&engine);
    REQUIRE(static_cast<bool>(manager.refresh()));

    REQUIRE(static_cast<bool>(manager.open(kMockDeviceId, testSettings())));
    REQUIRE(engine.currentDevice() != nullptr);

    // Opening again while streaming: the old device must be detached and
    // destroyed before the new one is published. (Pointer identity is not a
    // usable assertion here -- the allocator happily hands back the same address
    // for the same-sized object, which is precisely why the engine has to be told
    // instead of the caller sniffing pointers.) What must hold is that the engine
    // is streaming on a live, running device.
    REQUIRE(static_cast<bool>(manager.open(kMockDeviceId, testSettings())));
    REQUIRE(engine.currentDevice() != nullptr);
    CHECK(engine.currentDevice()->isRunning());
    CHECK(engine.isRunning());

    manager.close();
    CHECK(engine.currentDevice() == nullptr);
    engine.shutdown();
}

TEST_CASE("engine: manager teardown order survives engine outliving it",
          "[api][engine][regression][integration]") {
    // The application tears down the manager first and the engine afterwards
    // (the manager is destroyed by the window, the engine by the app object).
    auto manager = std::make_unique<AudioDeviceManager>();
    AudioEngine engine;
    manager->attachEngine(&engine);
    REQUIRE(static_cast<bool>(manager->refresh()));
    REQUIRE(static_cast<bool>(manager->open(kMockDeviceId, testSettings())));

    manager.reset(); // closes: detach -> stop -> destroy
    CHECK(engine.currentDevice() == nullptr);
    engine.shutdown(); // must not touch the dead device
    CHECK_FALSE(engine.isRunning());
}

TEST_CASE("engine: a manager used without an engine still closes cleanly",
          "[api][engine][regression]") {
    // Format queries and device listing work without an engine attached; close()
    // must not require one.
    AudioDeviceManager manager;
    REQUIRE(static_cast<bool>(manager.refresh()));
    CHECK(manager.findDevice(kMockDeviceId) != nullptr);
    manager.close();
    CHECK_FALSE(manager.isOpen());
    CHECK(manager.formatSupport(kMockDeviceId).supportsBufferSize(256));
}
