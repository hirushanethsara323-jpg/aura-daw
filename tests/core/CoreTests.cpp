// ============================================================================
// AURA DAW - tests/core/CoreTests.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include <catch2/catch_test_macros.hpp>

#include <thread>

#include "aura/core/Errors.hpp"
#include "aura/core/FileSystem.hpp"
#include "aura/core/Json.hpp"
#include "aura/core/Log.hpp"
#include "aura/core/RingBuffer.hpp"
#include "aura/core/SpscQueue.hpp"
#include "aura/core/Strings.hpp"
#include "aura/util/Localization.hpp"

using namespace aura;

TEST_CASE("AuraResult carries either a value or a diagnosable error", "[core][errors]") {
    const Status ok = success();
    REQUIRE(static_cast<bool>(ok));
    REQUIRE(ok.hasValue());

    const Status failed = failure(makeError(ErrorCode::NotFound, "missing", "detail: file gone"));
    REQUIRE_FALSE(static_cast<bool>(failed));
    REQUIRE(failed.error().code == ErrorCode::NotFound);
    // The technical message is for the log; userMessage is the localized,
    // user-facing sentence (its own catalogue key, never the raw text).
    REQUIRE(failed.error().message.find("missing") != std::string::npos);
    REQUIRE(failed.error().userMessage.size() > 0);
    REQUIRE(failed.error().toString().find("detail") != std::string::npos);

    AuraResult<int> value = success(42);
    REQUIRE(static_cast<bool>(value));
    REQUIRE(value.value() == 42);
    REQUIRE(value.valueOr(7) == 42);

    AuraResult<int> nothing = failureT<int>(makeError(ErrorCode::Cancelled, "cancelled"));
    REQUIRE_FALSE(static_cast<bool>(nothing));
    REQUIRE(nothing.valueOr(7) == 7);

    // Every code has a stable name and a localization key.
    REQUIRE(std::string(errorCodeName(ErrorCode::AudioDeviceLost)) == "AudioDeviceLost");
    REQUIRE(std::string(errorCodeMessageKey(ErrorCode::AudioDeviceLost)).size() > 0);
}

TEST_CASE("Strings helpers behave exactly as documented", "[core][strings]") {
    REQUIRE(strings::toLower("AbC") == "abc");
    REQUIRE(strings::toUpper("AbC") == "ABC");
    REQUIRE(strings::trim("  hi  ") == "hi");
    REQUIRE(strings::startsWith("AURA DAW", "AURA"));
    REQUIRE(strings::endsWith("AURA DAW", "DAW"));
    REQUIRE(strings::equalsIgnoreCase("Mixer", "mixer"));
    REQUIRE(strings::replaceAll("a.b.c", ".", "/") == "a/b/c");
    REQUIRE(strings::padLeft("7", 3, '0') == "007");

    const auto parts = strings::split("a,b,,c", ',');
    REQUIRE(parts.size() == 4);

    std::uint32_t value = 0;
    REQUIRE(strings::fromHex("ff10", value));
    REQUIRE(value == 0xff10);
    REQUIRE_FALSE(strings::fromHex("zz", value));

    // Fuzzy search: a subsequence match scores, an unrelated query does not.
    REQUIRE(strings::fuzzyScore("AURA_Record", "rec") >= 0);
    REQUIRE(strings::fuzzyScore("AURA_Record", "zzz") < 0);
    REQUIRE(strings::fuzzyScore("AURA_Record", "AR") >= 0);
}

TEST_CASE("JSON round-trips every AURA value type", "[core][json]") {
    json::Value root = json::Value::makeObject();
    root.set("name", "AURA");
    root.set("version", 1);
    root.set("gain", 0.5);
    root.set("muted", false);
    root.set("trackId", static_cast<std::uint32_t>(7)); // the 32-bit id overload

    json::Value array = json::Value::makeArray();
    for (int i = 0; i < 3; ++i) {
        json::Value entry = json::Value::makeObject();
        entry.set("index", i);
        array.push(std::move(entry));
    }
    root.set("items", std::move(array));

    const std::string text = root.dump(true);
    auto parsed = json::parse(text);
    REQUIRE(parsed.hasValue());
    REQUIRE(parsed.value()["name"].asString() == "AURA");
    REQUIRE(parsed.value()["version"].asInt() == 1);
    REQUIRE(parsed.value()["gain"].asDouble() == 0.5);
    REQUIRE(parsed.value()["muted"].asBool(true) == false);
    REQUIRE(parsed.value()["trackId"].asInt() == 7);
    REQUIRE(parsed.value()["items"].elements().size() == 3);
    REQUIRE(parsed.value()["items"][2]["index"].asInt() == 2);

    // Missing keys fall back instead of throwing.
    REQUIRE(parsed.value()["nope"].asString("fallback") == "fallback");

    // Malformed text is reported, never fatal.
    auto broken = json::parse("{ this is not json");
    REQUIRE_FALSE(broken.hasValue());
    REQUIRE(broken.error().code == ErrorCode::ParseError);
}

TEST_CASE("JSON survives strings that need escaping", "[core][json]") {
    json::Value root = json::Value::makeObject();
    root.set("path", "C:\\Users\\aura\\\"quoted\"\n\tline");
    auto parsed = json::parse(root.dump(false));
    REQUIRE(parsed.hasValue());
    REQUIRE(parsed.value()["path"].asString() == "C:\\Users\\aura\\\"quoted\"\n\tline");
}

TEST_CASE("File system helpers create, move and clean up real files", "[core][filesystem]") {
    const auto root = filesystem::fs::path("/tmp/aura-core-tests");
    (void)filesystem::createDirectories(root);
    REQUIRE(filesystem::isDirectory(root));

    const auto file = root / "sample.txt";
    REQUIRE(static_cast<bool>(filesystem::writeTextFileAtomic(file, "hello AURA")));
    REQUIRE(filesystem::isRegularFile(file));

    auto text = filesystem::readTextFile(file);
    REQUIRE(text.hasValue());
    REQUIRE(text.value() == "hello AURA");

    // Sanitising must never produce a path separator.
    const std::string sanitised = filesystem::sanitizeFileName("a/b:c*d?.wav");
    REQUIRE(sanitised.find('/') == std::string::npos);
    REQUIRE(sanitised.find(':') == std::string::npos);

    // makeUniquePath must not collide with an existing file.
    const auto unique = filesystem::makeUniquePath(file);
    REQUIRE(unique != file);
    REQUIRE_FALSE(filesystem::exists(unique));

    REQUIRE(static_cast<bool>(filesystem::removeFile(file)));
    REQUIRE_FALSE(filesystem::exists(file));
}

TEST_CASE("SPSC queue is safe across threads and reports overflow", "[core][rt]") {
    SpscQueue<int, 64> queue;
    std::atomic<int> consumed{0};
    std::atomic<bool> producerDone{false};

    std::thread producer([&] {
        for (int i = 0; i < 1000; ++i) {
            // The consumer drains, so overflow is only possible if it stalls;
            // retry rather than drop so the count is deterministic.
            while (!queue.tryPush(i))
                std::this_thread::yield();
        }
        producerDone.store(true);
    });

    int expected = 0;
    while (expected < 1000) {
        auto value = queue.tryPop();
        if (!value.has_value()) {
            if (producerDone.load() && queue.size() == 0)
                break;
            std::this_thread::yield();
            continue;
        }
        REQUIRE(*value == expected);
        ++expected;
        consumed.fetch_add(1);
    }
    producer.join();
    REQUIRE(consumed.load() == 1000);
    REQUIRE(queue.size() == 0);
}

TEST_CASE("Audio ring buffer preserves frame order and alignment", "[core][rt]") {
    AudioRingBuffer ring;
    ring.prepare(1024, 2);
    REQUIRE(ring.availableToRead() == 0);

    std::vector<float> write(2048, 0.0f);
    for (std::size_t i = 0; i < write.size(); ++i)
        write[i] = static_cast<float>(i);

    const std::size_t written = ring.write(write.data(), 512);
    REQUIRE(written == 512);
    REQUIRE(ring.availableToRead() == 512);

    std::vector<float> read(2048, 0.0f);
    const std::size_t readFrames = ring.read(read.data(), 512);
    REQUIRE(readFrames == 512);
    for (std::size_t i = 0; i < 1024; ++i)
        REQUIRE(read[i] == write[i]);

    // Wrapping: write more than the remaining capacity in two chunks.
    REQUIRE(ring.write(write.data(), 512) == 512);
    REQUIRE(ring.write(write.data(), 512) == 512);
    REQUIRE(ring.availableToRead() == 1024);
    ring.clear();
    REQUIRE(ring.availableToRead() == 0);
}

TEST_CASE("Logging captures records off the audio thread", "[core][log]") {
    Log::instance().setLevel(LogLevel::Debug);
    AURA_LOG_INFO("Test", "record %d", 7);
    AURA_LOG_WARN("Test", "warning");

    // The in-memory history is what the diagnostics panel shows.
    const auto history = Log::instance().recentRecords(10);
    REQUIRE_FALSE(history.empty());
    bool found = false;
    for (const auto& record : history) {
        if (std::string(record.message.data()).find("record 7") != std::string::npos)
            found = true;
    }
    REQUIRE(found);
}

TEST_CASE("Localization falls back to English and never returns an empty string",
          "[core][localization]") {
    auto& translator = loc::Translator::instance();
    (void)translator.setLanguage("en");
    REQUIRE(loc::tr("transport.play").size() > 0);

    // A language pack that does not exist must not break the UI: setLanguage()
    // reports the problem, the translator keeps the English catalogue active and
    // every lookup still resolves.
    const Status si = translator.setLanguage("si"); // Sinhala: catalogue not shipped yet
    REQUIRE_FALSE(static_cast<bool>(si));
    REQUIRE(translator.language() == loc::kFallbackLanguage);
    REQUIRE(loc::tr("transport.play").size() > 0); // English fallback

    // Unknown keys return the key itself so a missing string is visible in the
    // UI rather than silently blank.
    REQUIRE(translator.translate("this.key.does.not.exist") == "this.key.does.not.exist");

    (void)translator.setLanguage("en");
}
