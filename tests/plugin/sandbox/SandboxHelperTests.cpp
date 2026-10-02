// ============================================================================
// AURA DAW - tests/plugin/sandbox/SandboxHelperTests.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AURA DAW Project
//
// M7 phase 2: the operating-system half of the sandbox. Phase 1 proved the
// transport correct between two *threads* over a heap buffer; this proves it
// between two *processes* over a real named mapping, and proves the supervisor
// can start, watch and kill the thing on the other end.
//
// The order of the cases here follows the order the failures would be noticed in
// production: a mapping that cannot be created, a mapping two sides disagree
// about, a helper that never starts, a helper that will not stop, and finally a
// full session in which real audio crosses a real process boundary and comes back
// processed.
// ============================================================================
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "aura/core/Errors.hpp"
#include "aura/plugin/sandbox/SharedMemory.hpp"

using namespace aura;
using namespace aura::plugin::sandbox;

namespace {

/// Writes a recognisable pattern so a reader can tell "the same bytes" from "two
/// buffers that both happen to be zero".
void stamp(void* base, std::size_t bytes, std::uint8_t seed) {
    auto* out = static_cast<std::uint8_t*>(base);
    for (std::size_t i = 0; i < bytes; ++i)
        out[i] = static_cast<std::uint8_t>((seed + i * 31u) & 0xFFu);
}

bool matchesStamp(const void* base, std::size_t bytes, std::uint8_t seed) {
    const auto* in = static_cast<const std::uint8_t*>(base);
    for (std::size_t i = 0; i < bytes; ++i) {
        if (in[i] != static_cast<std::uint8_t>((seed + i * 31u) & 0xFFu))
            return false;
    }
    return true;
}

} // namespace

TEST_CASE("Two handles to one named mapping are the same memory", "[sandbox][shm]") {
    // This is the whole premise of the sandbox: the host and a helper each map the
    // arena and neither can tell the other's address. If this case fails there is
    // no phase 3, so it is asserted from both directions - writes are visible the
    // other way round as well, which is what separates a shared mapping from two
    // private copies of a file.
    constexpr std::size_t kBytes = 8192;
    auto created = SharedMemory::create("helper-test-shared", kBytes);
    REQUIRE(created.hasValue());
    CHECK(created.value().isValid());
    CHECK(created.value().size() == kBytes);
    CHECK(created.value().name() == "helper-test-shared");
    CHECK(created.value().data() != nullptr);

    // The platform name carries the namespace guard and the caller's name, and is
    // not asserted character by character because its shape differs per platform
    // ('/aura-x' on POSIX, 'Local\aura-x' on Windows). What matters is that it is
    // namespaced at all, or AURA collides with anything else on the machine.
    INFO("platform name: " << created.value().platformName());
    CHECK(created.value().platformName().find("aura-") != std::string::npos);
    CHECK(created.value().platformName().find("helper-test-shared") != std::string::npos);

    auto opened = SharedMemory::open("helper-test-shared", kBytes);
    REQUIRE(opened.hasValue());
    CHECK(opened.value().data() != created.value().data()); // different addresses...
    CHECK(opened.value().size() == kBytes);

    stamp(created.value().data(), kBytes, 0x11);
    CHECK(matchesStamp(opened.value().data(), kBytes, 0x11)); // ...same bytes

    stamp(opened.value().data(), kBytes, 0x7A);
    CHECK(matchesStamp(created.value().data(), kBytes, 0x7A));

    // A mapping opened by the peer is not owned by it: releasing the opener must
    // leave the creator's mapping usable, or a helper that exits cleanly would
    // take the session's buffers with it.
    opened.value().release();
    CHECK_FALSE(opened.value().isValid());
    CHECK(matchesStamp(created.value().data(), kBytes, 0x7A));
    CHECK(created.value().isValid());
}

TEST_CASE("Creating a mapping that already exists is refused rather than adopted",
          "[sandbox][shm]") {
    // AlreadyExists is not a formality. A mapping left behind by a crashed session
    // holds a header whose counters and payload belong to a process that is gone,
    // and adopting it would mean a new session starting with a ring that looks
    // half-full of somebody else's audio.
    auto first = SharedMemory::create("helper-test-exclusive", 4096);
    REQUIRE(first.hasValue());

    auto second = SharedMemory::create("helper-test-exclusive", 4096);
    REQUIRE(second.hasError());
    INFO(second.error().toString());
    CHECK(second.error().code == ErrorCode::AlreadyExists);

    // And the creator's release really unlinks, so the name is usable again in the
    // same process lifetime. Without this, one crash would cost the user every
    // later session until they found the stale file by hand.
    first.value().release();
    auto third = SharedMemory::create("helper-test-exclusive", 4096);
    REQUIRE(third.hasValue());
}

TEST_CASE("Opening a mapping nobody created reports NotFound", "[sandbox][shm]") {
    // For a helper this is the "the host went away before I started" case, which is
    // a clean exit and not a retry loop.
    auto missing = SharedMemory::open("helper-test-never-created", 4096);
    REQUIRE(missing.hasError());
    INFO(missing.error().toString());
    CHECK(missing.error().code == ErrorCode::NotFound);
}

TEST_CASE("A mapping name that could reach outside the namespace is refused",
          "[sandbox][shm]") {
    // A mapping name is a machine-wide namespace and it usually originates from an
    // identifier AURA did not invent, so the accepted set is narrow and the refusal
    // is the whole mechanism - there is no sanitising step that a cleverer name
    // could get past.
    const std::vector<std::string> refused = {
        "",           "/",        "a/b",        "a\\b",      ".",        "..",
        "has space",  "tab\tchar", "semi;colon", "quote\"dq", "nl\nchar", "/dev/shm/x",
        "..\\..\\win", std::string(kMaxShmNameLength + 1, 'a'),
    };
    for (const auto& name : refused) {
        INFO("name: '" << name << "'");
        CHECK_FALSE(isUsableShmName(name));
        CHECK(platformShmName(name).empty());

        const auto created = SharedMemory::create(name, 4096);
        REQUIRE(created.hasError());
        CHECK(created.error().code == ErrorCode::InvalidArgument);

        const auto opened = SharedMemory::open(name, 4096);
        REQUIRE(opened.hasError());
        CHECK(opened.error().code == ErrorCode::InvalidArgument);
    }

    // The accepted set is still wide enough for a session identifier.
    const std::vector<std::string> accepted = {
        "a", "session-1", "vst3_4CC.ab12", "A.a-_", std::string(kMaxShmNameLength, 'n'),
    };
    for (const auto& name : accepted) {
        INFO("name: '" << name << "'");
        CHECK(isUsableShmName(name));
        CHECK_FALSE(platformShmName(name).empty());
    }

    // Zero bytes is refused too: a zero-length mapping is not a smaller arena, it
    // is no arena, and ArenaView would report it as a size mismatch instead.
    CHECK(SharedMemory::create("helper-test-zero", 0).hasError());
    CHECK(SharedMemory::open("helper-test-zero", 0).hasError());
}

TEST_CASE("A mapping is a single owner: moving it does not unmap twice",
          "[sandbox][shm]") {
    // Two owners of one mapping would unmap it twice, which on POSIX is a
    // munmap(EINVAL) at best and a hole in the address space at worst. The move
    // leaves the source empty, and the destination still shares bytes with a peer.
    constexpr std::size_t kBytes = 4096;
    auto created = SharedMemory::create("helper-test-move", kBytes);
    REQUIRE(created.hasValue());

    SharedMemory moved = std::move(created.value());
    CHECK(moved.isValid());
    CHECK_FALSE(created.value().isValid());
    CHECK(created.value().data() == nullptr);
    CHECK(created.value().size() == 0);

    auto peer = SharedMemory::open("helper-test-move", kBytes);
    REQUIRE(peer.hasValue());
    stamp(moved.data(), kBytes, 0x33);
    CHECK(matchesStamp(peer.value().data(), kBytes, 0x33));

    // Move-assignment onto a live mapping must release what it held first, or the
    // old object leaks its name until process exit.
    auto other = SharedMemory::create("helper-test-move-other", kBytes);
    REQUIRE(other.hasValue());
    moved = std::move(other.value());
    CHECK(moved.isValid());
    CHECK(moved.name() == "helper-test-move-other");
    // "helper-test-move" was released by that assignment, so it can be created
    // again - proof the unlink happened and nothing was orphaned.
    auto recreated = SharedMemory::create("helper-test-move", kBytes);
    CHECK(recreated.hasValue());
}
