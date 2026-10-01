// ============================================================================
// AURA DAW - plugin/sandbox/ControlChannel.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
#include "ControlChannel.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <string>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace aura::plugin::sandbox {
namespace {

/// Frames are little-endian so the two sides of the same machine cannot disagree
/// about byte order (the sandbox is local by definition - a helper on another
/// machine would be a different design, not a flag).
void encodeLength(std::uint32_t length, unsigned char* out) {
    out[0] = static_cast<unsigned char>(length & 0xFFu);
    out[1] = static_cast<unsigned char>((length >> 8) & 0xFFu);
    out[2] = static_cast<unsigned char>((length >> 16) & 0xFFu);
    out[3] = static_cast<unsigned char>((length >> 24) & 0xFFu);
}

std::uint32_t decodeLength(const unsigned char* in) {
    return static_cast<std::uint32_t>(in[0]) | (static_cast<std::uint32_t>(in[1]) << 8) |
           (static_cast<std::uint32_t>(in[2]) << 16) | (static_cast<std::uint32_t>(in[3]) << 24);
}

} // namespace

json::Value makeMessage(std::string_view kind) {
    json::Value message = json::Value::makeObject();
    message.set("protocol", kProtocolVersion);
    message.set("kind", kind);
    return message;
}

std::string messageKind(const json::Value& message, std::string& error) {
    if (!message.isObject()) {
        error = "the sandbox sent a message that is not an object";
        return {};
    }
    if (!message.has("protocol") || !message["protocol"].isNumber()) {
        error = "the sandbox sent a message with no protocol version";
        return {};
    }
    const int protocol = message["protocol"].asInt();
    if (protocol != kProtocolVersion) {
        error = "the sandbox speaks protocol " + std::to_string(protocol) +
                ", this build speaks " + std::to_string(kProtocolVersion);
        return {};
    }
    if (!message.has("kind") || !message["kind"].isString() || message["kind"].asString().empty()) {
        error = "the sandbox sent a message with no kind";
        return {};
    }
    return message["kind"].asString();
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

ControlChannel::~ControlChannel() { close(); }

ControlChannel::ControlChannel(ControlChannel&& other) noexcept {
    *this = std::move(other);
}

ControlChannel& ControlChannel::operator=(ControlChannel&& other) noexcept {
    if (this != &other) {
        close();
#if defined(_WIN32)
        readHandle_ = other.readHandle_;
        writeHandle_ = other.writeHandle_;
        childReadHandle_ = other.childReadHandle_;
        childWriteHandle_ = other.childWriteHandle_;
        other.readHandle_ = nullptr;
        other.writeHandle_ = nullptr;
        other.childReadHandle_ = nullptr;
        other.childWriteHandle_ = nullptr;
#else
        fd_ = other.fd_;
        childFd_ = other.childFd_;
        other.fd_ = -1;
        other.childFd_ = -1;
#endif
        error_ = std::move(other.error_);
    }
    return *this;
}

void ControlChannel::close() noexcept {
#if defined(_WIN32)
    const auto closeHandle = [](void*& handle) {
        if (handle != nullptr) {
            CloseHandle(static_cast<HANDLE>(handle));
            handle = nullptr;
        }
    };
    closeHandle(readHandle_);
    closeHandle(writeHandle_);
    closeHandle(childReadHandle_);
    closeHandle(childWriteHandle_);
#else
    if (fd_ >= 0)
        ::close(fd_);
    if (childFd_ >= 0)
        ::close(childFd_);
    fd_ = -1;
    childFd_ = -1;
#endif
}

bool ControlChannel::valid() const noexcept {
#if defined(_WIN32)
    return readHandle_ != nullptr && writeHandle_ != nullptr;
#else
    return fd_ >= 0;
#endif
}

std::uintptr_t ControlChannel::handle() const noexcept {
#if defined(_WIN32)
    return static_cast<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(readHandle_));
#else
    return static_cast<std::uintptr_t>(fd_ >= 0 ? fd_ : -1);
#endif
}

// ---------------------------------------------------------------------------
// Creation
// ---------------------------------------------------------------------------

#if defined(_WIN32)

ControlChannel ControlChannel::createPair(std::string& error) {
    ControlChannel channel;
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE; // only the child ends keep this; see below

    HANDLE childReads = nullptr;
    HANDLE hostWrites = nullptr;
    HANDLE hostReads = nullptr;
    HANDLE childWrites = nullptr;
    if (CreatePipe(&childReads, &hostWrites, &attributes, 0) == FALSE ||
        CreatePipe(&hostReads, &childWrites, &attributes, 0) == FALSE) {
        error = "CreatePipe failed (" + std::to_string(GetLastError()) + ")";
        for (HANDLE handle : {childReads, hostWrites, hostReads, childWrites}) {
            if (handle != nullptr)
                CloseHandle(handle);
        }
        return channel;
    }

    // The host's ends must NOT be inheritable: an inheritable handle is inherited by
    // every process this one starts, and a future helper would then hold a writer to
    // a channel it has no business touching - which also means the host could never
    // see the reader's end close, because the reader's end would not be the last one.
    if (SetHandleInformation(hostReads, HANDLE_FLAG_INHERIT, 0) == FALSE ||
        SetHandleInformation(hostWrites, HANDLE_FLAG_INHERIT, 0) == FALSE) {
        error = "SetHandleInformation failed (" + std::to_string(GetLastError()) + ")";
        for (HANDLE handle : {childReads, hostWrites, hostReads, childWrites}) {
            if (handle != nullptr)
                CloseHandle(handle);
        }
        return channel;
    }

    channel.readHandle_ = hostReads;
    channel.writeHandle_ = hostWrites;
    channel.childReadHandle_ = childReads;
    channel.childWriteHandle_ = childWrites;
    return channel;
}

ControlChannel ControlChannel::adopt(std::uintptr_t raw, std::string& error) {
    return adoptHandles(raw, raw, error);
}

ControlChannel ControlChannel::adoptHandles(std::uintptr_t readHandle, std::uintptr_t writeHandle,
                                            std::string& error) {
    ControlChannel channel;
    if (readHandle == 0 || writeHandle == 0) {
        error = "the sandbox helper was started without a control channel";
        return channel;
    }
    channel.readHandle_ = reinterpret_cast<void*>(readHandle);
    channel.writeHandle_ = reinterpret_cast<void*>(writeHandle);
    return channel;
}

#else // POSIX

ControlChannel ControlChannel::createPair(std::string& error) {
    ControlChannel channel;
    int pair[2] = {-1, -1};
    // A socketpair, not two pipes: one descriptor per side, full duplex, and the
    // close semantics the host needs (the helper closing its end is a readable
    // event, which is how a helper exiting is noticed without a signal handler).
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        error = std::string("socketpair failed: ") + std::strerror(errno);
        return channel;
    }
    // The host's end must not survive into the helper: if it did, the helper would
    // hold a reference to its own channel's other side and the host could never see
    // the channel close.
    if (::fcntl(pair[0], F_SETFD, FD_CLOEXEC) != 0) {
        error = std::string("fcntl failed: ") + std::strerror(errno);
        ::close(pair[0]);
        ::close(pair[1]);
        return channel;
    }
    channel.fd_ = pair[0];
    channel.childFd_ = pair[1];
    return channel;
}

ControlChannel ControlChannel::adopt(std::uintptr_t raw, std::string& error) {
    return adoptHandles(raw, raw, error);
}

ControlChannel ControlChannel::adoptHandles(std::uintptr_t readHandle, std::uintptr_t /*writeHandle*/,
                                            std::string& error) {
    ControlChannel channel;
    const int fd = static_cast<int>(readHandle);
    if (fd < 0) {
        error = "the sandbox helper was started without a control channel";
        return channel;
    }
    channel.fd_ = fd;
    return channel;
}

#endif

std::uintptr_t ControlChannel::childHandle() const noexcept {
#if defined(_WIN32)
    return static_cast<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(childReadHandle_));
#else
    return static_cast<std::uintptr_t>(childFd_ >= 0 ? childFd_ : -1);
#endif
}

std::uintptr_t ControlChannel::childWriteHandle() const noexcept {
#if defined(_WIN32)
    return static_cast<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(childWriteHandle_));
#else
    return 0;
#endif
}

void ControlChannel::closeChildEnd() noexcept {
#if defined(_WIN32)
    if (childReadHandle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(childReadHandle_));
        childReadHandle_ = nullptr;
    }
    if (childWriteHandle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(childWriteHandle_));
        childWriteHandle_ = nullptr;
    }
#else
    if (childFd_ >= 0) {
        ::close(childFd_);
        childFd_ = -1;
    }
#endif
}

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

bool ControlChannel::sendFrame(std::string_view payload, std::string& error) {
    if (!valid()) {
        error = "the control channel is not open";
        return false;
    }
    if (payload.size() > kMaxFrameBytes) {
        error = "refusing to send a " + std::to_string(payload.size()) +
                "-byte control frame (the limit is " + std::to_string(kMaxFrameBytes) + ")";
        return false;
    }

    std::array<unsigned char, 4> header{};
    encodeLength(static_cast<std::uint32_t>(payload.size()), header.data());

#if defined(_WIN32)
    size_t written = 0;
    if (WriteFile(static_cast<HANDLE>(writeHandle_), header.data(), 4, nullptr, nullptr) == FALSE) {
        error = "WriteFile failed (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    const char* cursor = payload.data();
    size_t remaining = payload.size();
    while (remaining > 0) {
        DWORD chunk = 0;
        if (WriteFile(static_cast<HANDLE>(writeHandle_), cursor, static_cast<DWORD>(remaining),
                      &chunk, nullptr) == FALSE ||
            chunk == 0) {
            error = "WriteFile failed (" + std::to_string(GetLastError()) + ")";
            return false;
        }
        cursor += chunk;
        remaining -= static_cast<size_t>(chunk);
        written += static_cast<size_t>(chunk);
    }
    (void)written;
#else
    // Pipe writes are the one place a partial write is normal rather than a fault:
    // the buffer fills and the rest goes out as the reader drains it.
    const auto writeAll = [&error](int descriptor, const void* data, size_t bytes) {
        const auto* cursor = static_cast<const char*>(data);
        size_t remaining = bytes;
        while (remaining > 0) {
            const ssize_t written = ::send(descriptor, cursor, remaining, MSG_NOSIGNAL);
            if (written < 0) {
                if (errno == EINTR)
                    continue;
                error = std::string("send failed: ") + std::strerror(errno);
                return false;
            }
            cursor += written;
            remaining -= static_cast<size_t>(written);
        }
        return true;
    };
    if (!writeAll(fd_, header.data(), header.size()))
        return false;
    if (!writeAll(fd_, payload.data(), payload.size()))
        return false;
#endif
    return true;
}

ChannelStatus ControlChannel::receiveFrame(std::string& payload, double timeoutSeconds,
                                           std::string& error) {
    if (!valid()) {
        error = "the control channel is not open";
        return ChannelStatus::Error;
    }

#if defined(_WIN32)
    const auto waitForBytes = [&](DWORD wanted, double timeout) {
        // Anonymous pipes have no select(); PeekNamedPipe plus a short sleep is the
        // documented way to wait on one without a reader thread. The sleep is 1 ms,
        // which is four orders of magnitude below the timescales this channel is
        // used at (a handshake, a state blob, a scan result).
        const ULONGLONG start = GetTickCount64();
        for (;;) {
            DWORD available = 0;
            if (PeekNamedPipe(static_cast<HANDLE>(readHandle_), nullptr, 0, nullptr, &available,
                              nullptr) == FALSE) {
                error = "PeekNamedPipe failed (" + std::to_string(GetLastError()) + ")";
                return ChannelStatus::Error;
            }
            if (available >= wanted)
                return ChannelStatus::Frame;
            if (timeout >= 0.0 &&
                static_cast<double>(GetTickCount64() - start) >= timeout * 1000.0)
                return ChannelStatus::Timeout;
            Sleep(1);
        }
    };

    const auto readAll = [&](void* destination, DWORD bytes) {
        auto* cursor = static_cast<char*>(destination);
        DWORD remaining = bytes;
        while (remaining > 0) {
            DWORD chunk = 0;
            if (ReadFile(static_cast<HANDLE>(readHandle_), cursor, remaining, &chunk, nullptr) ==
                FALSE) {
                const DWORD code = GetLastError();
                if (code == ERROR_BROKEN_PIPE) {
                    // The helper closed its end. Anything mid-frame means the message
                    // is gone; either way there is nothing more to read.
                    return ChannelStatus::Closed;
                }
                error = "ReadFile failed (" + std::to_string(code) + ")";
                return ChannelStatus::Error;
            }
            if (chunk == 0)
                return ChannelStatus::Closed;
            cursor += chunk;
            remaining -= chunk;
        }
        return ChannelStatus::Frame;
    };

    std::array<unsigned char, 4> header{};
    ChannelStatus status = waitForBytes(4, timeoutSeconds);
    if (status != ChannelStatus::Frame)
        return status;
    status = readAll(header.data(), 4);
    if (status != ChannelStatus::Frame)
        return status;

    const std::uint32_t length = decodeLength(header.data());
    if (length > kMaxFrameBytes) {
        error = "the sandbox announced a " + std::to_string(length) +
                "-byte control frame (the limit is " + std::to_string(kMaxFrameBytes) + ")";
        return ChannelStatus::Error;
    }
    payload.assign(length, '\0');
    if (length == 0)
        return ChannelStatus::Frame;
    status = readAll(payload.data(), length);
    return status;
#else
    const auto waitForReadable = [&](double timeout) {
        // poll() until *something* is readable: for the header a byte is enough to
        // decide there is a frame coming, and for the body the read below can block
        // for the rest of a frame that is already on its way. (The Windows branch
        // needs a byte *count* because PeekNamedPipe reports one; POSIX does not.)
        int milliseconds = -1;
        if (timeout >= 0.0)
            milliseconds = static_cast<int>(timeout * 1000.0);
        pollfd descriptor{};
        descriptor.fd = fd_;
        descriptor.events = POLLIN;
        for (;;) {
            const int ready = ::poll(&descriptor, 1, milliseconds);
            if (ready > 0) {
                // POLLHUP without POLLIN is the peer closing; treat it as closed here
                // and let the read below confirm.
                if ((descriptor.revents & (POLLIN | POLLHUP | POLLERR)) == 0)
                    continue;
                return ChannelStatus::Frame;
            }
            if (ready == 0)
                return ChannelStatus::Timeout;
            if (errno == EINTR)
                continue;
            error = std::string("poll failed: ") + std::strerror(errno);
            return ChannelStatus::Error;
        }
    };

    const auto readAll = [&](void* destination, std::size_t bytes) {
        auto* cursor = static_cast<char*>(destination);
        std::size_t remaining = bytes;
        while (remaining > 0) {
            const ssize_t got = ::recv(fd_, cursor, remaining, 0);
            if (got < 0) {
                if (errno == EINTR)
                    continue;
                error = std::string("recv failed: ") + std::strerror(errno);
                return ChannelStatus::Error;
            }
            if (got == 0)
                return ChannelStatus::Closed;
            cursor += got;
            remaining -= static_cast<std::size_t>(got);
        }
        return ChannelStatus::Frame;
    };

    std::array<unsigned char, 4> header{};
    ChannelStatus status = waitForReadable(timeoutSeconds);
    if (status != ChannelStatus::Frame)
        return status;
    status = readAll(header.data(), header.size());
    if (status != ChannelStatus::Frame)
        return status;

    const std::uint32_t length = decodeLength(header.data());
    if (length > kMaxFrameBytes) {
        error = "the sandbox announced a " + std::to_string(length) +
                "-byte control frame (the limit is " + std::to_string(kMaxFrameBytes) + ")";
        return ChannelStatus::Error;
    }
    payload.assign(length, '\0');
    if (length == 0)
        return ChannelStatus::Frame;
    return readAll(payload.data(), length);
#endif
}

bool ControlChannel::sendMessage(const json::Value& message, std::string& error) {
    std::string kind = messageKind(message, error);
    if (kind.empty())
        return false;
    return sendFrame(message.dump(), error);
}

ChannelStatus ControlChannel::receiveMessage(json::Value& message, double timeoutSeconds,
                                             std::string& error) {
    std::string payload;
    const ChannelStatus status = receiveFrame(payload, timeoutSeconds, error);
    if (status != ChannelStatus::Frame)
        return status;

    auto parsed = json::parse(payload);
    if (parsed.hasError()) {
        error = "the sandbox sent a frame that is not JSON: " + parsed.error().message;
        return ChannelStatus::Error;
    }
    message = std::move(parsed).value();
    if (messageKind(message, error).empty())
        return ChannelStatus::Error;
    return ChannelStatus::Frame;
}

} // namespace aura::plugin::sandbox
