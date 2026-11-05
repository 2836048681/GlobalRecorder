#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace lightrec {
enum class CaptureState { Idle, Starting, Recording, Stopping, Saved, Failed };
struct Chunk {
    std::wstring path;
    std::wstring source; // microphone / system
    double start = 0;
    double duration = 0;
};
class Recorder {
public:
    Recorder();
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;
    bool start(const std::wstring& folder, std::function<void(const Chunk&)> onChunk = {},
               uint32_t processId = 0);
    void stop();
    bool finished() const;
    CaptureState state() const;
    std::wstring error() const;
    std::wstring folder() const;
    double elapsed() const;
    float micLevel() const;
    float systemLevel() const;
private:
    struct Impl;
    Impl* impl_;
};
bool mixWaveFiles(const std::wstring& folder, std::wstring& error);
bool audioSelfTest(const std::wstring& folder, std::wstring& error);
bool processLoopbackSupported();
}
