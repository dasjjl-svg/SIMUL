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
    std::chrono::milliseconds interval = std::chrono::milliseconds(50);  // 송신 주기 50ms 고정
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

// 반송 시험 러너. 작업 스레드 1개가 송·수신 양방향 처리.
// 한 건이라도 수신 전문이 송신 적재와 상이하면 verdict=Fail.
// duration 완료 시 failed==0 이면 verdict=Ok.
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
    std::deque<Data> pending_;
    Buffer rxBuffer_;
    std::chrono::steady_clock::time_point startedAt_{};

    // 미회신 적재 상한. 20Hz × 6.4s = 128프레임
    static constexpr std::size_t pendingLimit = 128;
};

}  // namespace simul
