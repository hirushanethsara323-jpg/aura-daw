// ============================================================================
// AURA DAW - tests/plugin/SandboxEchoHelper.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A deliberately tiny helper process, used by tests/plugin/ControlChannelTests.cpp.
//
// It is not the sandbox helper (that is `aura_plugin_host`, phase 2's real work): it
// is the *smallest possible second process* that proves the parts a unit test cannot
// prove from inside one process - that the control channel survives being spawned
// with inherited handles, that frames cross a real process boundary intact, that a
// helper crash is observable as a signal/exit code rather than as a hang, and that a
// wedged helper runs into the host's deadline instead of freezing it.
//
// Protocol: read a message, answer with {"protocol":1,"kind":"echo","body":<same>},
// until a "bye" arrives (exit 0) or the channel closes (exit 0). "crash" aborts;
// "sleep" stops answering and waits to be killed.
// ============================================================================
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#include "ControlChannel.hpp"
#include "aura/core/Json.hpp"

using namespace aura;
using namespace aura::plugin::sandbox;

namespace {

/// Reads `--control-fd N` (POSIX) and `--control-write N` (Windows). Returns -1/NULL
/// when the argument is missing, which the caller treats as a usage error: a helper
/// started without a channel cannot do anything useful, and failing loudly is how the
/// host learns it spawned the wrong binary.
std::uintptr_t controlArgument(int argc, char** argv, const char* name, bool required) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::string(argv[index]) == name)
            return static_cast<std::uintptr_t>(std::strtoull(argv[index + 1], nullptr, 10));
    }
    return required ? 0u : 0u;
}

} // namespace

int main(int argc, char** argv) {
    const std::uintptr_t readHandle = controlArgument(argc, argv, "--control-fd", true);
    if (readHandle == 0)
        return 2; // usage error, the same code the scanner helper uses

    std::string error;
    ControlChannel channel = ControlChannel::adopt(readHandle, error);
#if defined(_WIN32)
    // On Windows the two directions are two pipes, so the write end arrives as its
    // own handle value. On POSIX one descriptor is both.
    const std::uintptr_t writeHandle = controlArgument(argc, argv, "--control-write", false);
    if (writeHandle != 0)
        channel = ControlChannel::adoptPair(readHandle, writeHandle, error);
#endif
    if (!channel.valid())
        return 2;

    for (;;) {
        json::Value message;
        const ChannelStatus status = channel.receiveMessage(message, -1.0, error);
        if (status != ChannelStatus::Frame)
            return 0; // the host closed the channel or went away: nothing to do

        const std::string kind = message["kind"].asString();
        if (kind == "bye") {
            json::Value reply = makeMessage("bye");
            channel.sendMessage(reply, error);
            return 0;
        }
        if (kind == "crash")
            std::abort(); // an abnormal death, on purpose: the host must survive it
        if (kind == "sleep") {
            // Never answers: the host's own deadline is what has to end this.
            std::this_thread::sleep_for(std::chrono::seconds(60));
            return 0;
        }

        json::Value reply = makeMessage("echo");
        reply.set("body", message);
        if (!channel.sendMessage(reply, error))
            return 1;
    }
}
