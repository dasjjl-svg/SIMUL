#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "protocol.h"
#include "transport.h"

namespace simul {

enum class Verdict { Pending, Ok, Fail };

struct TestSettings {
    TransportConfig transport;
    std::chrono::seconds duration = std::chrono::hours(1);
    std::chrono::milliseconds interval = std::chrono::milliseconds(50);
};

struct TestSnapshot {
    bool running = false;
    Verdict verdict = Verdict::Pending;
    std::uint64_t sent = 0;
    std::uint64_t received = 0;
    std::uint64_t failed = 0;
    std::chrono::milliseconds elapsed{0};
    std::chrono::seconds duration{0};
    std::wstring lastError;
    std::wstring lastEvent;
};

// Timed loopback runner. One worker thread drives both TX and RX.
//
// Behaviour:
//   * Every `interval` (50 ms) a random 32-byte DATA block is wrapped in
//     a frame and sent through the transport.
//   * Each transmitted DATA block is recorded as expected.
//   * Received frames are matched to the oldest expected block that is
//     still outstanding; a mismatch, CRC error, or missing frame beyond
//     the window counts as a failure.
//   * Any single failure sets the verdict to Fail; the run also ends
//     early if the transport reports an error.
//   * When `duration` elapses with zero failures, the verdict is Ok.
class LoopbackTest {
public:
    LoopbackTest();
    ~LoopbackTest();
    LoopbackTest(const LoopbackTest&) = delete;
    LoopbackTest& operator=(const LoopbackTest&) = delete;

    bool start(const TestSettings& settings, std::wstring& error);
    void stop();
    TestSnapshot snapshot() const;

private:
    void run();

    TestSettings settings_{};
    std::unique_ptr<Transport> transport_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancel_{false};

    mutable std::mutex mutex_;
    TestSnapshot state_{};
    std::deque<Data> pending_;              // DATA blocks awaiting echo
    Buffer rxBuffer_;
    std::chrono::steady_clock::time_point startedAt_{};

    static constexpr std::size_t pendingLimit = 128;
};

}  // namespace simul
