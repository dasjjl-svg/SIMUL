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
// 판정: 송신 전 프레임이 echoTimeout 이내 동일 DATA로 회신될 때만 Ok.
class LoopbackTest {
public:
    LoopbackTest();
    ~LoopbackTest();
    LoopbackTest(const LoopbackTest&) = delete;
    LoopbackTest& operator=(const LoopbackTest&) = delete;

    bool start(const TestSettings& settings, std::wstring& error);
    void stop();
    TestSnapshot snapshot() const;

    // 반송 허용 지연. 초과 프레임은 실패 처리, 시험 종료 시 잔여 회신 대기 상한 겸용
    static constexpr std::chrono::milliseconds echoTimeout{1000};

private:
    struct Outstanding {
        Data data;
        std::chrono::steady_clock::time_point sentAt;
    };

    void run();
    void fail(const std::wstring& event);

    TestSettings settings_{};
    std::unique_ptr<Transport> transport_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancel_{false};

    mutable std::mutex mutex_;
    TestSnapshot state_{};
    std::deque<Outstanding> pending_;
    Buffer rxBuffer_;
    std::chrono::steady_clock::time_point startedAt_{};
};

}  // namespace simul
