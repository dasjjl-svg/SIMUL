#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "protocol.h"
#include "transport.h"

namespace loopback {

// Stopped: 사용자 중지로 시험 시간 미완료, 이상 없음. 판정 보류
enum class Verdict { Pending, Ok, Fail, Stopped };

struct TestSettings {
    TransportConfig transport;
    std::chrono::seconds duration = std::chrono::hours(1);
    std::chrono::milliseconds interval{50};       // 송신 주기 50ms 고정
    std::chrono::milliseconds echoTimeout{1000};  // 송신 시각 기준 회신 허용 시간
};

struct TestEvent {
    std::chrono::system_clock::time_point wall;
    std::wstring text;
};

struct TestSnapshot {
    bool running = false;
    Verdict verdict = Verdict::Pending;
    std::uint64_t sent = 0;
    std::uint64_t received = 0;
    std::uint64_t matched = 0;
    std::uint64_t failed = 0;
    std::chrono::microseconds maxLatency{0};
    std::chrono::microseconds meanLatency{0};
    std::chrono::milliseconds elapsed{0};
    std::chrono::seconds duration{0};
    bool hasFailure = false;
    std::chrono::milliseconds firstFailureAt{0};
    std::wstring firstFailure;
    std::wstring abortReason;  // 송·수신 치명 오류로 인한 시험 중단 사유
    std::vector<std::uint8_t> timeline;  // 시험 시간 균등 분할 구간별 판정. 0 미도래, 1 정상, 2 이상
};

// 채널별 반송 시험 엔진. 작업 스레드 1개가 송·수신 처리
// 송신 전문별 판정: 동일 DATA 회신 시 정상, echoTimeout 초과 시 이상
// 수신 이상(전문 불량·대응 없는 전문·전문 외 바이트·회선 이상)과 송신 주기 이탈은 건별 이상
// 회신 대조는 미회신 대기열 전체 탐색. 1건 손실 시 후속 전문 연쇄 오판 방지
// 시간 초과 판정 후 도착 회신은 지연 회신 기록만 수행, 중복 계수 제외. 회신 완료 전문 재수신은 중복 회신 이상
// RS-485 2선식 송신 반향 미발생 설정: 전문 선로 시간 1.5배(송·회신 합의 75%) 미만 사본 또는 송신 중 송신 전문 동일 바이트 수신은
// 송신 반향으로 판정(이상 계수 후 반향 제거 전환)
class LoopbackTest {
public:
    static constexpr std::size_t timelineSlots = 96;       // 판정 추이 구간 수. 시험 시간 균등 분할
    static constexpr std::size_t expiredCapacity = 4096;   // 지연 회신 판별 이력. 50ms 주기 약 200s분
    static constexpr std::size_t answeredCapacity = 4096;  // 중복 회신 판별 이력. 50ms 주기 약 200s분
    static constexpr std::size_t failureLogLimit = 100;
    static constexpr std::size_t eventQueueLimit = 1000;
    static constexpr std::chrono::minutes summaryInterval{10};      // 상세 기록 상한 도달 후 요약 주기
    static constexpr std::chrono::milliseconds idleWait{100};       // 수신 대기 상한. 통상 대기는 다음 송신 주기까지

    LoopbackTest();
    ~LoopbackTest();
    LoopbackTest(const LoopbackTest&) = delete;
    LoopbackTest& operator=(const LoopbackTest&) = delete;

    // open: 전송로 개방만 수행. 채널 간 병렬 호출 허용
    // begin: 작업 스레드 기동. lane: 논리 프로세서 분산 배치 순번(채널 번호)
    bool open(const TestSettings& settings, std::wstring& error);
    void begin(std::size_t lane);
    // cancel: 중지 신호만 전달. stop: 작업 스레드 종료 대기 후 전송로 폐쇄
    void cancel();
    void stop();
    // 외부 요인(시스템 절전 등)에 의한 시험 구간 단절. 실패 1건 계수(판정 FAIL)
    void interrupt(const std::wstring& reason);
    TestSnapshot snapshot() const;
    std::vector<TestEvent> takeEvents();

private:
    using Clock = std::chrono::steady_clock;

    struct Outstanding {
        std::uint64_t seq = 0;
        Data data{};
        Clock::time_point sentAt{};
        std::size_t wireBytes = 0;  // DLE 치환 후 회선상 전문 길이
        bool localEchoSeen = false;
    };

    // 고정 용량 판정 이력. 용량 초과 시 최고(最古) 항목 덮어쓰기, 시험 중 추가 할당 없음
    // seq 0 항목은 소비 완료분으로 탐색 제외
    class History {
    public:
        void reset(std::size_t capacity);
        void push(const Outstanding& item);
        Outstanding* find(const Data& data);  // 최신 항목 우선

    private:
        std::vector<Outstanding> items_;
        std::size_t next_ = 0;
        std::size_t size_ = 0;
    };

    void run();
    void finish(const std::wstring& abortReason);

    // 이하 mutex_ 보유 상태 호출 전제
    void handleFrame(const ParseResult& parsed, const Data& got, Clock::time_point at);
    void expire(Clock::time_point now);
    void fail(std::uint64_t seq, const std::wstring& text, Clock::time_point at, bool log = true,
              std::uint64_t count = 1);
    void pass(Clock::duration latency, Clock::time_point at);
    void detail(const std::wstring& text, Clock::time_point at);
    void summarize(Clock::time_point now);
    void note(std::wstring text);
    void remember(bool ok, Clock::time_point at);
    std::chrono::milliseconds sinceStart(Clock::time_point at) const;
    // 반이중 선로 장치 회신 완료 하한: 송신·회신 전문 선로 시간 합의 75% (통신 속도 오차 여유)
    Clock::duration replyFloor(std::size_t wireBytes) const;

    TestSettings settings_{};
    std::unique_ptr<Transport> transport_;
    std::thread worker_;
    std::atomic<bool> cancel_{false};

    mutable std::mutex mutex_;
    TestSnapshot state_{};
    std::vector<Outstanding> pending_;  // 송신 순 미회신 전문. 회신 제한 구간분 용량 선확보
    History expired_;
    History answered_;
    std::deque<TestEvent> events_;
    std::array<std::uint8_t, timelineSlots> timeline_{};
    std::uint64_t latencySumUs_ = 0;
    std::size_t loggedFailures_ = 0;
    std::uint64_t summarizedFailures_ = 0;
    Clock::time_point summarizedAt_{};
    bool completed_ = false;

    Buffer rxBuffer_;  // 수신 누적. 시험 시작 시 용량 선확보
    Buffer txFrame_;   // 송신 전문. 주기마다 재사용
    std::size_t lane_ = 0;
    Clock::time_point startedAt_{};
    bool halfDuplex_ = false;      // RS-485 2선식
    bool dropLocalEcho_ = false;   // 송신 전문당 최초 사본 판정 제외
    bool echoDetected_ = false;    // 반향 미발생 설정 중 자기 송신 반향 검출
    bool drainStarted_ = false;    // 시험 시간 경과, 잔여 회신 대기 진입
    double charMicros_ = 0.0;      // 직렬 문자 1개 선로 시간(us)
};

}  // namespace loopback
