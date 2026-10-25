#include "ipc/pipe_server.h"

#include <windows.h>

#include <string>

#include "diagnostics.h"
#include "input/active_device.h"
#include "ipc/outbox.h"
#include "ipc/protocol.h"
#include "log.h"

namespace pipe_server {

namespace {

const char* kPipeName = "\\\\.\\pipe\\nms_overlay";
// Bigger than any document, so the mod's write returns at once instead of stalling its frame.
constexpr DWORD kBufferSize = 64 * 1024;
constexpr size_t kMaxLine = 1 << 20;   // longer is a broken sender, not a mod

void send(HANDLE pipe, const std::string& line) {
    std::string message = line + "\n";
    DWORD written = 0;
    if (!WriteFile(pipe, message.c_str(), (DWORD)message.size(), &written, nullptr))
        LOG_DEBUG("pipe: reply not delivered (winerror=%lu)", GetLastError());
}

// A pipe is a byte stream: a line can arrive split across reads.
class LineReader {
public:
    template <typename OnLine>
    void feed(const char* data, size_t size, OnLine on_line) {
        pending_.append(data, size);
        size_t start = 0;
        for (size_t nl; (nl = pending_.find('\n', start)) != std::string::npos; start = nl + 1) {
            std::string line = pending_.substr(start, nl - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) on_line(line);
        }
        pending_.erase(0, start);
        if (pending_.size() > kMaxLine) {
            LOG_WARN("pipe: %zu bytes with no newline, dropping", pending_.size());
            pending_.clear();
        }
    }

private:
    std::string pending_;
};

std::string device_line(active_device::Device device) {
    return std::string("INPUT ") + active_device::name(device);
}

// Drains everything waiting, so the buffer never fills. False once the mod is gone.
bool read_available(HANDLE pipe, LineReader& reader) {
    DWORD available = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) return false;
    char buffer[16 * 1024];
    while (available > 0) {
        DWORD read = 0;
        if (!ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) || read == 0) break;
        diagnostics::count_wire_bytes(read);
        available = available > read ? available - read : 0;
        reader.feed(buffer, read, [&](const std::string& line) {
            diagnostics::count_wire_message();
            std::string reply = protocol::handle(line.c_str());
            if (!reply.empty()) send(pipe, reply);
        });
    }
    return true;
}

void serve(HANDLE pipe) {
    send(pipe, "HELLO ui-format " + std::to_string(protocol::kUiFormat));
    outbox::clear();   // clicks nobody heard belonged to a mod that is gone
    auto deviceSent = active_device::Device::None;   // so a late mod still learns the device
    LineReader reader;
    while (read_available(pipe, reader)) {
        for (const std::string& event : outbox::take()) send(pipe, event);
        auto device = active_device::current();
        if (device != deviceSent) {
            send(pipe, device_line(device));
            deviceSent = device;
        }
        Sleep(2);
    }
    protocol::disconnected();
}

DWORD WINAPI run(LPVOID) {
    for (;;) {
        HANDLE pipe = CreateNamedPipeA(kPipeName, PIPE_ACCESS_DUPLEX,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1,
                                       kBufferSize, kBufferSize, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) { Sleep(500); continue; }
        LOG("pipe: server ready, waiting for the mod to connect...");
        if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
            CloseHandle(pipe);
            continue;
        }
        LOG("pipe: mod connected.");
        serve(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        LOG("pipe: mod disconnected; its documents are closed until it reconnects.");
    }
}

}  // namespace

void start() {
    CreateThread(nullptr, 0, run, nullptr, 0, nullptr);
}

}  // namespace pipe_server
