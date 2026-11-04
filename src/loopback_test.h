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

// Stopped: 사용자 중지로 시험 시간 또는 회신 확인 미완료, 이상 없음. 판정 보류
enum class Verdict { Pending, Ok, Fail, Stopped };

struct TestSettings {
    TransportConfig transport;
    std::chrono::seconds duration = std::chrono::hours(1);
    std::chrono::milliseconds interval{50};       // 송신 주기 50ms 고정
    // 회신 허용 시퀀스 수: 후속 전문 replyWindow건 송신 시점까지 미회신이면 이상
    std::uint32_t replyWindow = 5;
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
    std::uint64_t late = 0;     // 회신 허용 시퀀스 경과 후 도착(해당 전문은 미회신 이상 처리 완료)
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
// 송신 전문별 판정: 후속 전문 replyWindow건(기본 5) 송신 전까지 시퀀스·DATA 일치 회신 시 정상. 도착 순서 무관
// 기한 내 미회신은 이상. 기한 경과 후 도착분은 지연 회신 기록만 수행(이상 중복 계수 없음)
// 시험 시간 종료 후 기한은 송신 주기 기준 가상 시퀀스로 판정
// 수신 이상(전문 불량·대응 없는 전문·DATA 불일치·중복 회신·전문 외 바이트·회선 이상)과 송신 주기 이탈은 건별 이상
// RS-485 2선식 송신 반향 미발생 설정: 전문 선로 시간 1.5배(송·회신 합의 75%) 미만 사본 또는 송신 중 송신 전문 동일 바이트 수신은
// 송신 반향으로 판정(이상 계수 후 반향 제거 전환)
class LoopbackTest {
public:
    static constexpr std::size_t timelineSlots = 96;     // 판정 추이 구간 수. 시험 시간 균등 분할
    static constexpr std::size_t detailLogLimit = 100;   // 이상·지연 각각의 상세 기록 상한
    static constexpr std::size_t eventQueueLimit = 1000;
    static constexpr std::chrono::minutes summaryInterval{10};   // 상세 기록 상한 도달 후 요약 주기
    static constexpr std::chrono::milliseconds idleWait{100};    // 수신 대기 상한. 통상 대기는 다음 송신 주기까지

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

    // 송신 전문 상태. 시퀀스 값으로 직접 색인(시퀀스 1순환분)
    struct Slot {
        std::uint64_t number = 0;    // 전문 번호(1부터). 0: 미사용
        Data data{};
        Clock::time_point sentAt{};
        std::uint8_t wireBytes = 0;  // DLE 치환 후 회선상 전문 길이
        bool answered = false;
        bool lost = false;           // 회신 기한 경과 미회신(이상 처리 완료)
        bool localEchoSeen = false;
    };

    void run();
    // 치명 송신 오류 시 이미 도착한 수신분 판정 반영
    void collectReceived();
    void finish(const std::wstring& abortReason);

    // 이하 mutex_ 보유 상태 호출 전제
    void record(std::uint64_t number, const Frame& frame, std::size_t wireBytes, Clock::time_point at);
    void handleFrame(const ParseResult& parsed, const Frame& got, Clock::time_point at);
    void flushNoise();  // 누적 전문 외 수신 1건 계수
    // 후속 전문 latest번까지 송신 기준 회신 기한 경과 전문 이상 처리
    void expireThrough(std::uint64_t latest, Clock::time_point now);
    std::wstring echoHint(const Slot& slot) const;
    void fail(std::uint64_t number, const std::wstring& text, Clock::time_point at, bool log = true,
              std::uint64_t count = 1);
    void pass(Clock::duration latency, Clock::time_point at);
    void detail(const std::wstring& text, Clock::time_point at);
    void lateDetail(const std::wstring& text, Clock::time_point at);
    void startSummaries(Clock::time_point at);
    void summarize(Clock::time_point now);
    void note(std::wstring text);
    void remember(std::uint8_t mark, Clock::time_point at);
    std::chrono::milliseconds sinceStart(Clock::time_point at) const;
    // 반이중 선로 장치 회신 완료 하한: 송신·회신 전문 선로 시간 합의 75% (통신 속도 오차 여유)
    Clock::duration replyFloor(std::size_t wireBytes) const;
    // 시퀀스 = (전문 번호 - 1)의 하위 16비트
    Slot& slotOf(std::uint64_t number) { return slots_[static_cast<std::uint16_t>(number - 1)]; }

    TestSettings settings_{};
    std::unique_ptr<Transport> transport_;
    std::thread worker_;
    std::atomic<bool> cancel_{false};

    mutable std::mutex mutex_;
    TestSnapshot state_{};
    std::vector<Slot> slots_;          // 시퀀스 1순환분 송신 전문. 시험 시작 시 확보, 시험 중 추가 할당 없음
    std::uint64_t outstanding_ = 0;    // 미회신 전문 수
    std::uint64_t expireCursor_ = 1;   // 회신 기한 판정 대상 최고(最古) 전문 번호
    std::deque<TestEvent> events_;
    std::array<std::uint8_t, timelineSlots> timeline_{};
    std::uint64_t latencySumUs_ = 0;
    std::size_t loggedFailures_ = 0;
    std::size_t loggedLate_ = 0;
    bool summarizing_ = false;         // 상세 기록 상한 도달, 주기 요약 진행
    std::uint64_t summarizedFailures_ = 0;
    std::uint64_t summarizedLate_ = 0;
    Clock::time_point summarizedAt_{};
    bool completed_ = false;

    Buffer rxBuffer_;  // 수신 누적. 시험 시작 시 용량 선확보
    Buffer txFrame_;   // 송신 전문. 주기마다 재사용
    std::size_t lane_ = 0;
    Clock::time_point startedAt_{};
    bool halfDuplex_ = false;          // RS-485 2선식
    bool dropLocalEcho_ = false;       // 송신 전문당 최초 사본 판정 제외
    bool echoDetected_ = false;        // 반향 미발생 설정 중 자기 송신 반향 검출
    bool drainStarted_ = false;        // 시험 시간 경과, 잔여 회신 대기 진입
    Clock::duration settleWait_{};     // 정적 확인 구간: 잔여 회신 대기 종료·잡음 1회 확정 기준
    Clock::duration drainGrace_{};     // 잔여 회신 대기 기한 경과 시 수신 진행 중 전문의 완결 대기 상한
    std::uint64_t noiseBytes_ = 0;     // 계수 전 연속 전문 외 수신 바이트
    Clock::time_point noiseAt_{};      // 해당 잡음 최초 수신 시각
    double charMicros_ = 0.0;          // 직렬 문자 1개 선로 시간(us)
};

}  // namespace loopback
