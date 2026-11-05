#include "loopback_test.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <iterator>

#include "format.h"
#include "frame_diff.h"
#include "scheduling.h"

namespace loopback {

namespace {

constexpr std::size_t kCompareBytes = MAX_FRAME_SIZE * 2;  // 대조 대상 수신 바이트 상한
constexpr std::uint64_t kCompareMargin = 64;   // 대응 후보: 최근 송신 전문(회신 허용 시퀀스 + 64건)
constexpr std::uint64_t kCompareSpan = 1024;   // 대응 후보 최근 송신 전문 상한
constexpr std::size_t kFinalists = 4;          // 정렬 대조 대상 상위 후보 수
constexpr std::size_t kMinAnchored = 4;        // 대응 인정 하한: 연속 일치 구간 내 DATA·CRC 바이트 수
constexpr std::size_t kNoiseCompareMin = 8;    // 전문 외 수신의 송신 전문 대조 하한 길이
constexpr std::size_t kSummaryItems = 3;       // 상이 요약 항목 상한
constexpr std::size_t kEvidenceItems = 64;     // 대조표 상이 항목 상한

const wchar_t* describe(FrameError error)
{
    switch (error) {
    case FrameError::Resync:  return L"STX 중복 (전문 절단)";
    case FrameError::Escape:  return L"DLE 치환 오류";
    case FrameError::Length:  return L"본문 길이 오류";
    case FrameError::Trailer: return L"트레일러 FF FF 불일치";
    case FrameError::Crc:     return L"CRC 불일치";
    case FrameError::None:    break;
    }
    return L"형식 오류";
}

std::wstring sequenceText(std::uint16_t sequence)
{
    wchar_t text[8]{};
    std::swprintf(text, std::size(text), L"0x%04X", static_cast<unsigned>(sequence));
    return text;
}

std::wstring firstLine(const std::wstring& text)
{
    return text.substr(0, text.find(L'\r'));
}

std::uint64_t millisecondsOf(std::chrono::steady_clock::duration value)
{
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(value).count();
    return ms > 0 ? static_cast<std::uint64_t>(ms) : 0;
}

}  // namespace

LoopbackTest::LoopbackTest() = default;

LoopbackTest::~LoopbackTest() { stop(); }

bool LoopbackTest::open(const TestSettings& settings, std::wstring& error)
{
    if (worker_.joinable()) { error = L"시험 실행 중."; return false; }
    settings_ = settings;
    const SerialConfig& serial = settings.transport.serial;
    halfDuplex_ = settings.transport.kind == TransportKind::Serial &&
                  serial.standard == SerialStandard::Rs485TwoWire;
    const double stop = serial.stopBits == ONESTOPBIT ? 1.0 : serial.stopBits == ONE5STOPBITS ? 1.5 : 2.0;
    const double bits = 1.0 + serial.dataBits + (serial.parity != NOPARITY ? 1.0 : 0.0) + stop;
    charMicros_ = serial.baud ? bits * 1e6 / static_cast<double>(serial.baud) : 0.0;
    // 정적 확인 구간: 직렬 문자 24개(수신 FIFO 2회 전달분)와 20ms(USB 변환기 전달 지연 포함) 중 큰 값
    const bool serialKind = settings.transport.kind == TransportKind::Serial;
    const auto chars = std::chrono::microseconds(static_cast<std::chrono::microseconds::rep>(
        serialKind ? charMicros_ * 24.0 : 0.0));
    settleWait_ = (std::max)(Clock::duration(std::chrono::milliseconds(20)), Clock::duration(chars));
    // 기한 경과 시점 수신 진행 중 전문의 완결 대기: 최대 전문 선로 시간 + 정적 확인 구간
    drainGrace_ = Clock::duration(std::chrono::microseconds(static_cast<std::chrono::microseconds::rep>(
                      serialKind ? charMicros_ * static_cast<double>(MAX_FRAME_SIZE) : 0.0))) +
                  settleWait_;
    // 전문 1건 송신 처리 상한: 최대 전문 2건 선로 시간(저속 직렬 대응) + 100ms, 최소 250ms
    TransportConfig transport = settings.transport;
    transport.sendTimeoutMs = static_cast<DWORD>((std::max)(
        250.0, std::ceil(2.0 * static_cast<double>(MAX_FRAME_SIZE) * charMicros_ / 1000.0) + 100.0));
    transport_ = makeTransport(transport);
    if (!transport_) { error = L"접속 방식 미지원."; return false; }
    if (!transport_->open(error)) { transport_.reset(); return false; }
    return true;
}

void LoopbackTest::begin(std::size_t lane)
{
    if (!transport_ || worker_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = {};
        state_.running = true;
        state_.duration = settings_.duration;
        // 시험 중 힙 할당 배제용 선확보: 시퀀스 1순환분 송신 전문 상태
        slots_.assign(SEQUENCE_SPACE, Slot{});
        outstanding_ = 0;
        expireCursor_ = 1;
        events_.clear();
        timeline_.fill(0);
        latencySumUs_ = 0;
        loggedFailures_ = 0;
        loggedLate_ = 0;
        filedFailures_ = 0;
        filedLate_ = 0;
        evidenceLogged_ = 0;
        summarizing_ = false;
        summarizedFailures_ = 0;
        summarizedLate_ = 0;
        completed_ = false;
        dropLocalEcho_ = halfDuplex_ && settings_.transport.serial.localEcho == LocalEcho::Present;
        echoDetected_ = false;
        drainStarted_ = false;
        noiseBytes_ = 0;
        const std::wstring notice = transport_->notice();
        if (!notice.empty()) note(notice);
        frameBytes_.clear();
        frameBytes_.reserve(MAX_FRAME_SIZE * 4);
        noiseSample_.clear();
        noiseSample_.reserve(kCompareBytes);
        compareSent_.reserve(MAX_FRAME_SIZE);
        compareGot_.reserve(kCompareBytes);
    }
    rxBuffer_.clear();
    rxBuffer_.reserve(65536);
    txFrame_.reserve(MAX_FRAME_SIZE);
    lane_ = lane;
    startedAt_ = Clock::now();
    cancel_.store(false);
    worker_ = std::thread([this] { run(); });
}

void LoopbackTest::cancel()
{
    cancel_.store(true);
}

void LoopbackTest::stop()
{
    cancel_.store(true);
    if (worker_.joinable()) worker_.join();
    if (transport_) { transport_->close(); transport_.reset(); }
}

void LoopbackTest::interrupt(const std::wstring& reason)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!state_.running) return;
    // 상세 기록 상한과 무관하게 항상 기록
    fail(0, reason, Clock::now(), false);
    note(reason);
}

TestSnapshot LoopbackTest::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    TestSnapshot copy = state_;
    if (copy.running) copy.elapsed = sinceStart(Clock::now());
    // 종료 후 잔여 회신 대기 구간은 표시 시간에서 제외
    if (copy.elapsed > copy.duration) copy.elapsed = copy.duration;
    if (copy.matched) copy.meanLatency = std::chrono::microseconds(
        static_cast<std::chrono::microseconds::rep>(latencySumUs_ / copy.matched));
    copy.timeline.assign(timeline_.begin(), timeline_.end());
    return copy;
}

std::vector<TestEvent> LoopbackTest::takeEvents()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TestEvent> taken(std::make_move_iterator(events_.begin()),
                                 std::make_move_iterator(events_.end()));
    events_.clear();
    return taken;
}

void LoopbackTest::run()
{
    prepareWorkerThread(lane_);
    const auto deadline = startedAt_ + settings_.duration;
    auto nextTx = startedAt_;
    bool draining = false;
    Clock::time_point drainUntil{};
    std::uint64_t number = 0;
    Clock::time_point lastSentAt = startedAt_;
    Frame frame;
    std::wstring abortReason;
    Clock::time_point lastRxAt = startedAt_;  // 최근 수신 시각(정적 구간 판정 기준)

    while (!cancel_.load()) {
        const auto now = Clock::now();
        // 시험 시간 내 예정 주기 송신 완료 후에만 잔여 회신 대기 진입. 종료 직전 정지 시 누락 주기 계수 보장
        if (!draining && now >= deadline && nextTx >= deadline) {
            // 최종 전문 송신 결과 확인. 실패 시 송신 오류로 시험 중단
            std::wstring error;
            if (!transport_->flush(error)) {
                abortReason = L"송신 오류: " + error;
                collectReceived();
                break;
            }
            draining = true;
            // 잔여 회신 대기: 최종 전문 회신 기한(송신 주기 기준 후속 가상 시퀀스 replyWindow건)까지
            drainUntil = lastSentAt + settings_.interval * settings_.replyWindow;
            std::lock_guard<std::mutex> lock(mutex_);
            drainStarted_ = true;
        }
        if (draining) {
            std::lock_guard<std::mutex> lock(mutex_);
            const bool settled = now - lastRxAt >= settleWait_;
            // 잔여 회신 대기 종료: 미회신·미완결 수신 없이 정적 구간 경과, 또는 기한 경과
            // 기한 경과 시점 수신 진행 중 전문은 정적 구간 또는 완결 대기 상한까지 연장
            const bool idle = !outstanding_ && rxBuffer_.empty() && settled;
            const bool expired = now >= drainUntil && (rxBuffer_.empty() || settled || now >= drainUntil + drainGrace_);
            if (idle || expired) {
                completed_ = true;
                break;
            }
        }

        if (!draining && now >= nextTx) {
            ++number;
            frame.sequence = static_cast<std::uint16_t>(number - 1);
            randomData(frame.data);
            buildFrame(frame, txFrame_);
            std::wstring error;
            const auto before = Clock::now();
            if (!transport_->send(txFrame_, error)) {
                abortReason = L"송신 오류: " + error;
                collectReceived();
                break;
            }
            // 송신 시각: OS 송신 호출 직전(직전 송신 완료 대기 이후). 선로 송신 개시 하한, 회신 지연·하한·주기 이탈 기준
            const auto issued = transport_->issuedAt(before);
            lastSentAt = issued;
            std::lock_guard<std::mutex> lock(mutex_);
            // 잡음 계수 단위 상한: 송신 주기 1회. 진행 중 단발 잡음 분할 방지 위해 1주기 이상 지속분만 확정
            if (noiseBytes_ && issued - noiseAt_ >= settings_.interval) flushNoise();
            record(number, frame, txFrame_.size(), issued);
            // 1주기 이상 지연 송신은 누락 주기 실패 계수 (절전·스케줄링 정지·회선 정체·송신 정체)
            // 누락 주기는 시험 시간 내 예정분만 계수, 최소 1주기
            const auto slot = nextTx;
            const auto lateness = issued - slot;
            nextTx += settings_.interval;
            if (lateness >= settings_.interval) {
                const auto skipped = static_cast<std::uint64_t>(lateness / settings_.interval);
                const auto window = slot + settings_.interval < deadline
                                        ? static_cast<std::uint64_t>((deadline - slot - Clock::duration(1)) /
                                                                     settings_.interval)
                                        : std::uint64_t{0};
                const std::uint64_t missed = (std::max)(std::uint64_t{1}, (std::min)(skipped, window));
                fail(0, L"송신 주기 이탈 (" + grouped(millisecondsOf(lateness)) + L" ms 지연, 누락 " +
                            grouped(missed) + L"주기)", issued, true, missed);
                nextTx = issued + settings_.interval;
            }
        }

        // 다음 처리 시각(송신 주기·가상 시퀀스 기한·잔여 회신 대기 종료·정적 확인)까지 수신 대기. 주기 폴링 배제
        auto wake = Clock::now() + idleWait;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto current = Clock::now();
            if (!draining) {
                wake = (std::min)(wake, nextTx);
            } else if (current < drainUntil) {
                const auto elapsedSlots = (current - lastSentAt) / settings_.interval;
                wake = (std::min)({wake, drainUntil, lastSentAt + settings_.interval * (elapsedSlots + 1)});
            } else {
                wake = (std::min)(wake, drainUntil + drainGrace_);
            }
            // 정적 구간 도래 시각: 잡음 1회 확정, 잔여 회신 대기 종료 판정
            const auto quietAt = lastRxAt + settleWait_;
            if (quietAt > current && (noiseBytes_ || (draining && (!outstanding_ || current >= drainUntil))))
                wake = (std::min)(wake, quietAt);
        }
        const auto wait = (std::max)(std::chrono::milliseconds(0),
                                     std::chrono::ceil<std::chrono::milliseconds>(wake - Clock::now()));
        std::wstring error;
        const std::size_t buffered = rxBuffer_.size();
        const bool received = transport_->receive(rxBuffer_, wait, error);
        const LineFault fault = transport_->takeFault();
        const bool sendEcho = transport_->takeSendEcho();
        const auto at = Clock::now();
        if (rxBuffer_.size() != buffered) lastRxAt = at;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // 반향 미발생 설정 중 송신 진행 구간에 송신 전문 동일 바이트 수신: 자기 송신 반향 확정. 해당 전문 사본부터 반향 제거 적용
            if (sendEcho && halfDuplex_ && !dropLocalEcho_ && number) {
                dropLocalEcho_ = true;
                echoDetected_ = true;
                const std::wstring text = L"송신 반향 검출. 송신 반향 설정 '발생' 필요 (송신 중 송신 전문 동일 바이트 수신, 이후 반향 제거 적용)";
                fail(number, text, at, false);
                note(L"#" + std::to_wstring(number) + L" " + text);
            }
            if (fault.count) {
                fail(0, fault.text + (fault.count > 1 ? L" 외 " + grouped(fault.count - 1) + L"건" : std::wstring()),
                     at, true, fault.count);
            }
            // 잔여 회신 대기 중 회신 기한: 최종 송신 이후 송신 주기 경과 수를 가상 시퀀스로 적용. 수신분 해석 전 기한 확정
            if (draining && number)
                expireThrough(number + static_cast<std::uint64_t>((at - lastSentAt) / settings_.interval), at);
            // 치명 오류 시에도 동일 호출에서 수신 완료된 전문은 판정 반영
            for (;;) {
                Frame got;
                const ParseResult parsed = parseFrame(rxBuffer_, got, &frameBytes_, &noiseSample_);
                handleFrame(parsed, got, at);
                if (parsed.status == Parse::NeedMore) break;
            }
            // 정적 구간 경과 시 잡음 1회 발생 확정. 수신 분할 단위와 무관
            if (noiseBytes_ && at - lastRxAt >= settleWait_) flushNoise();
            summarize(at);
        }
        if (!received) {
            abortReason = L"수신 오류: " + error;
            break;
        }
    }

    finish(abortReason);
}

void LoopbackTest::collectReceived()
{
    std::wstring ignored;
    transport_->receive(rxBuffer_, std::chrono::milliseconds(0), ignored);
    const LineFault fault = transport_->takeFault();
    const auto at = Clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    if (fault.count)
        fail(0, fault.text + (fault.count > 1 ? L" 외 " + grouped(fault.count - 1) + L"건" : std::wstring()), at, true,
             fault.count);
    for (;;) {
        Frame got;
        const ParseResult parsed = parseFrame(rxBuffer_, got, &frameBytes_, &noiseSample_);
        handleFrame(parsed, got, at);
        if (parsed.status == Parse::NeedMore) break;
    }
}

void LoopbackTest::finish(const std::wstring& abortReason)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto end = Clock::now();
    flushNoise();
    const bool aborted = !abortReason.empty();
    // 시험 시간 경과 후 중지는 잔여 회신 대기 중 중지(화면 표시 경과 시간 기준과 일치)
    const bool waitStopped = !aborted && !completed_ && (drainStarted_ || end >= startedAt_ + settings_.duration);
    const bool cancelled = !aborted && !completed_ && !waitStopped;
    // 잔여 회신 대기 종료: 전 전문 회신 기한 도래
    if (completed_) expireThrough(state_.sent + settings_.replyWindow, end);
    const std::uint64_t unresolved = outstanding_;
    // 잔여 회신 대기 종료 시점 미완결 수신은 전문 절단 이상
    if (completed_ && !rxBuffer_.empty())
        fail(0, L"미완결 전문 수신 " + grouped(rxBuffer_.size()) + L" Byte (잔여 회신 대기 종료 시점)", end);

    if (aborted) {
        state_.abortReason = abortReason;
        // 상세 기록 상한과 무관하게 항상 기록
        const std::wstring text = L"시험 중단" +
                                  (unresolved ? L" (미회신 " + grouped(unresolved) + L"건)" : std::wstring()) +
                                  L". " + abortReason;
        fail(0, text, end, false);
        note(text);
    }
    // 사용자 중지 시 미회신분은 회신 확인 미완료(판정 보류 사유). 시험 완료·중단 시 미회신분은 이상
    if (!cancelled && !waitStopped && unresolved) {
        // 기한 판정 이전 전문만 대상. 미회신 처리 완료분 중복 계수 배제
        for (std::uint64_t n = expireCursor_; n <= state_.sent; ++n) {
            const Slot& slot = slotOf(n);
            if (slot.number == n && !slot.answered && !slot.lost)
                fail(n, L"미회신 (시험 종료 시점)" + missHint(slot), end, false);
        }
        if (!aborted) note(L"시험 종료 시점 미회신 " + grouped(unresolved) + L"건.");
    }
    if (state_.failed) {
        state_.verdict = Verdict::Fail;
        note(L"판정 FAIL. 이상 " + grouped(state_.failed) + L"건, 정상 회신 " + grouped(state_.matched) + L"건." +
             (state_.late ? L" 기한 경과 후 지연 회신 " + grouped(state_.late) + L"건." : std::wstring()));
    } else if (cancelled || (waitStopped && (unresolved || !rxBuffer_.empty()))) {
        state_.verdict = Verdict::Stopped;
        note((cancelled ? std::wstring(L"판정 보류. 사용자 중지로 시험 시간 미완료.")
                        : std::wstring(L"판정 보류. 사용자 중지로 회신 확인 미완료.")) +
             (unresolved ? L" 회신 대기 " + grouped(unresolved) + L"건." : std::wstring()));
    } else {
        state_.verdict = Verdict::Ok;
        note(L"판정 OK. 전 전문 정상 회신 " + grouped(state_.matched) + L"건." +
             (halfDuplex_ && !dropLocalEcho_ ? L" 송신 반향 미발생 설정 기준." : L""));
    }
    state_.elapsed = sinceStart(end);
    state_.running = false;
}

void LoopbackTest::record(std::uint64_t number, const Frame& frame, std::size_t wireBytes, Clock::time_point at)
{
    ++state_.sent;
    Slot& slot = slotOf(number);
    slot = Slot{};
    slot.number = number;
    slot.data = frame.data;
    slot.sentAt = at;
    slot.wireBytes = static_cast<std::uint8_t>(wireBytes);
    ++outstanding_;
    expireThrough(number, at);
}

void LoopbackTest::handleFrame(const ParseResult& parsed, const Frame& got, Clock::time_point at)
{
    // 연속 전문 외 수신은 1회 발생으로 누적. 수신 분할 단위와 무관한 계수
    if (parsed.discarded) {
        if (!noiseBytes_) noiseAt_ = at;
        noiseBytes_ += parsed.discarded;
    }
    if (parsed.status == Parse::NeedMore) return;
    flushNoise();

    if (parsed.status == Parse::Invalid) {
        ++state_.received;
        // 손상 전문의 시퀀스 해석값(본문 2B 이상 복원 시)은 대응 후보로만 사용
        std::uint64_t hint = 0;
        if (parsed.bodyLength >= DATA_OFFSET)
            hint = slots_[static_cast<std::uint16_t>((parsed.body[SEQUENCE_OFFSET] << 8) |
                                                     parsed.body[SEQUENCE_OFFSET + 1])].number;
        Comparison match = compare(frameBytes_, frameBytes_.size(), hint, false, at);
        markDamaged(match.number);
        fail(match.number,
             std::wstring(L"전문 불량: ") + describe(parsed.error) + L". " +
                 (match.number ? match.summary : L"대응 송신 전문 특정 불가 (수신 " + grouped(frameBytes_.size()) + L" Byte)"),
             at, true, 1, std::move(match.evidence));
        return;
    }

    Slot& slot = slots_[got.sequence];
    if (!slot.number || slot.data != got.data) {
        ++state_.received;
        Comparison match = compare(frameBytes_, frameBytes_.size(), slot.number, true, at);
        markDamaged(match.number);
        std::wstring text;
        if (match.number && match.number != slot.number)
            text = L"시퀀스 불일치 (수신 " + sequenceText(got.sequence) + L", 대응 송신 " +
                   sequenceText(static_cast<std::uint16_t>(match.number - 1)) + L")";
        else if (slot.number)
            text = L"DATA 불일치 (시퀀스 " + sequenceText(got.sequence) + L")";
        else
            text = L"송신 대응 없는 전문 수신 (시퀀스 " + sequenceText(got.sequence) + L")";
        fail(match.number, match.number ? text + L". " + match.summary : text, at, true, 1, std::move(match.evidence));
        return;
    }
    if (slot.answered) {
        ++state_.received;
        fail(slot.number, halfDuplex_ && !dropLocalEcho_ ? L"중복 회신 (송신 반향이면 송신 반향 설정 '발생' 필요)"
                                                         : L"중복 회신", at);
        return;
    }
    // RS-485 2선식 자기 송신 반향: 송신 전문당 최초 1회 판정 제외
    if (dropLocalEcho_ && !slot.localEchoSeen) {
        slot.localEchoSeen = true;
        return;
    }
    const auto latency = at - slot.sentAt;
    // 회신 기한 경과 후 도착: 미회신 이상 처리 완료분. 지연 회신 기록만 수행
    if (slot.lost) {
        ++state_.received;
        ++state_.late;
        slot.answered = true;
        lateDetail(L"#" + std::to_wstring(slot.number) + L" 지연 회신 (" + grouped(millisecondsOf(latency)) +
                       L" ms, 시퀀스 " + grouped(settings_.replyWindow) + L"회 경과 후 도착" +
                       (slot.localEchoSeen ? L", 송신 반향 수신 확인" : L"") + L")", at);
        return;
    }
    // 반이중 선로의 장치 회신은 송신 완료 후 개시. 하한 미달 사본은 장치 회신 불가, 자기 송신 반향 확정
    if (halfDuplex_ && !dropLocalEcho_ && latency < replyFloor(slot.wireBytes)) {
        dropLocalEcho_ = true;
        echoDetected_ = true;
        slot.localEchoSeen = true;
        const std::wstring text = L"송신 반향 검출. 송신 반향 설정 '발생' 필요 (" + msText(latency) +
                                  L" 회신, 장치 회신 하한 " + msText(replyFloor(slot.wireBytes)) +
                                  L", 이후 반향 제거 적용)";
        // 상세 기록 상한과 무관하게 항상 기록
        fail(slot.number, text, at, false);
        note(L"#" + std::to_wstring(slot.number) + L" " + text);
        return;
    }
    ++state_.received;
    slot.answered = true;
    --outstanding_;
    pass(latency, at);
}

void LoopbackTest::flushNoise()
{
    if (!noiseBytes_) return;
    std::wstring text = L"전문 외 수신 " + grouped(noiseBytes_) + L" Byte";
    // 송신 전문 길이 수준 잡음은 STX 손상 전문 여부 대조
    Comparison match;
    if (noiseSample_.size() >= kNoiseCompareMin) {
        match = compare(noiseSample_, noiseBytes_, 0, false, noiseAt_);
    } else {
        match.evidence = L"수신 원문 " + grouped(noiseBytes_) + L" Byte\r\n" + byteRows(noiseSample_, L"수신");
    }
    markDamaged(match.number);
    if (match.number) text += L". " + match.summary;
    fail(match.number, text, noiseAt_, true, 1, std::move(match.evidence));
    noiseBytes_ = 0;
    noiseSample_.clear();
}

LoopbackTest::Comparison LoopbackTest::compare(const Buffer& received, std::uint64_t total, std::uint64_t hint,
                                               bool trustHint, Clock::time_point at)
{
    const std::size_t used = (std::min)(received.size(), kCompareBytes);
    compareGot_.assign(received.begin(), received.begin() + static_cast<std::ptrdiff_t>(used));
    const std::wstring sizes = grouped(total) + L" Byte" + (used < total ? L" 중 앞 " + grouped(used) + L" Byte" : std::wstring());
    const std::uint64_t sent = state_.sent;
    const std::uint64_t span = (std::min)(sent, (std::min)(settings_.replyWindow + kCompareMargin, kCompareSpan));
    const auto rebuild = [this](std::uint64_t number) {
        buildFrame(Frame{static_cast<std::uint16_t>(number - 1), slotOf(number).data}, compareSent_);
    };

    // 후보 선별: 최근 송신 전문 중 위치 대응 일치 상위 후보. 동률 시 최근 전문 우선
    std::array<std::uint64_t, kFinalists> top{};
    std::array<std::size_t, kFinalists> scores{};
    for (std::uint64_t back = 0; back < span; ++back) {
        const std::uint64_t number = sent - back;
        if (slotOf(number).number != number) continue;
        rebuild(number);
        const std::size_t score = wireAffinity(compareSent_, compareGot_);
        for (std::size_t i = 0; i < top.size(); ++i) {
            if (top[i] && score <= scores[i]) continue;
            for (std::size_t k = top.size() - 1; k > i; --k) {
                top[k] = top[k - 1];
                scores[k] = scores[k - 1];
            }
            top[i] = number;
            scores[i] = score;
            break;
        }
    }

    // 정렬 대조: 상이 최소, 동률 시 연속 일치 DATA·CRC 다수
    const bool hintValid = hint && hint <= sent && slotOf(hint).number == hint;
    const bool hintRecent = hintValid && sent - hint < span;
    std::uint64_t bestNumber = 0;
    WireDiff best;
    WireDiff hinted;
    const auto consider = [&](std::uint64_t number) {
        rebuild(number);
        WireDiff diff = alignWire(compareSent_, compareGot_);
        if (number == hint) hinted = diff;
        if (!bestNumber || diff.errors() < best.errors() ||
            (diff.errors() == best.errors() && diff.anchored > best.anchored)) {
            best = std::move(diff);
            bestNumber = number;
        }
    };
    if (hintValid) consider(hint);
    for (const std::uint64_t number : top)
        if (number && number != hint) consider(number);

    // 대응 인정: 연속 일치 구간 내 DATA·CRC 4B 이상. 최근 송신 전문과 시퀀스 일치 시 1B 이상(선두 절단 전문)
    const auto accepted = [&](std::uint64_t number, const WireDiff& diff) {
        return number && (diff.anchored >= kMinAnchored || (number == hint && hintRecent && diff.anchored > 0));
    };
    std::uint64_t chosen = 0;
    const WireDiff* diff = nullptr;
    if (trustHint && hintValid) {
        const bool other = bestNumber != hint && accepted(bestNumber, best) && best.errors() < hinted.errors();
        chosen = other ? bestNumber : hint;
        diff = other ? &best : &hinted;
    } else if (accepted(bestNumber, best)) {
        chosen = bestNumber;
        diff = &best;
    } else if (hintValid && accepted(hint, hinted)) {
        chosen = hint;
        diff = &hinted;
    }

    Comparison result;
    if (!chosen) {
        result.evidence = L"수신 원문 " + sizes + L"\r\n" + byteRows(compareGot_, L"수신");
        return result;
    }
    rebuild(chosen);
    const std::vector<std::wstring> items = diffItems(compareSent_, compareGot_, *diff);
    const auto join = [&items](std::size_t limit, const wchar_t* separator) {
        std::wstring text;
        for (std::size_t i = 0; i < (std::min)(limit, items.size()); ++i) text += (i ? separator : L"") + items[i];
        return text;
    };
    result.number = chosen;
    result.summary = diff->errors() ? L"송신 대비 상이 " + grouped(diff->errors()) + L" Byte: " + join(kSummaryItems, L", ") +
                                          (items.size() > kSummaryItems
                                               ? L" 외 " + grouped(items.size() - kSummaryItems) + L"건"
                                               : std::wstring())
                                    : std::wstring(L"송신 전문과 동일");
    result.evidence = L"송·수신 대조 · 송신 #" + std::to_wstring(chosen) + L" (시퀀스 " +
                      sequenceText(static_cast<std::uint16_t>(chosen - 1)) + L", 송신 후 " +
                      msText(at - slotOf(chosen).sentAt) + L") · 송신 " + grouped(compareSent_.size()) + L" Byte · 수신 " +
                      sizes + L"\r\n" + diffTable(compareSent_, compareGot_, *diff);
    if (items.size() > kSummaryItems) {
        result.evidence += L"\r\n상이 항목 " + grouped(items.size()) + L"건\r\n  " + join(kEvidenceItems, L"\r\n  ");
        if (items.size() > kEvidenceItems) result.evidence += L"\r\n  외 " + grouped(items.size() - kEvidenceItems) + L"건";
    }
    return result;
}

void LoopbackTest::markDamaged(std::uint64_t number)
{
    if (!number) return;
    Slot& slot = slotOf(number);
    if (slot.number == number && !slot.answered && !slot.lost) slot.damaged = true;
}

void LoopbackTest::expireThrough(std::uint64_t latest, Clock::time_point now)
{
    for (; expireCursor_ + settings_.replyWindow <= latest; ++expireCursor_) {
        Slot& slot = slotOf(expireCursor_);
        if (slot.number != expireCursor_ || slot.answered || slot.lost) continue;
        slot.lost = true;
        --outstanding_;
        fail(slot.number, L"미회신 (시퀀스 " + grouped(settings_.replyWindow) + L"회 경과)" + missHint(slot), now);
    }
}

std::wstring LoopbackTest::missHint(const Slot& slot) const
{
    if (slot.damaged) return L". 기한 내 손상 회신 수신";
    if (!slot.localEchoSeen) return {};
    return echoDetected_ ? L". 기한 내 송신 반향만 수신 (장치 무회신 또는 회신 지연)"
                         : L". 기한 내 사본 1건만 수신: 장치 무회신·회신 지연 또는 송신 반향 미발생 어댑터 (송신 반향 설정 확인)";
}

void LoopbackTest::fail(std::uint64_t number, const std::wstring& text, Clock::time_point at, bool log,
                        std::uint64_t count, std::wstring evidence)
{
    state_.failed += count;
    state_.verdict = Verdict::Fail;
    remember(2, at);

    const std::wstring tagged = (number ? L"#" + std::to_wstring(number) + L" " : std::wstring()) + text;
    if (!state_.hasFailure) {
        state_.hasFailure = true;
        state_.firstFailureAt = sinceStart(at);
        state_.firstFailure = firstLine(tagged);
    }
    if (log) detail(tagged, at, std::move(evidence));
}

void LoopbackTest::pass(Clock::duration latency, Clock::time_point at)
{
    ++state_.matched;
    remember(1, at);
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(latency);
    if (us.count() > 0) latencySumUs_ += static_cast<std::uint64_t>(us.count());
    if (us > state_.maxLatency) state_.maxLatency = us;
}

// 화면 상한 초과분은 기록 파일 전용 상세 기록(별도 상한)
void LoopbackTest::detail(const std::wstring& text, Clock::time_point at, std::wstring evidence)
{
    const bool screen = loggedFailures_ < detailLogLimit;
    if (!screen && filedFailures_ >= fileDetailLimit) return;
    note(text, admitEvidence(std::move(evidence)), !screen);
    if (!screen) {
        if (++filedFailures_ == fileDetailLimit)
            note(L"기록 파일 이상 상세 기록 " + grouped(fileDetailLimit) + L"건 도달. 이후 " +
                     std::to_wstring(summaryInterval.count()) + L"분 주기 요약만 기록.",
                 {}, true);
        return;
    }
    if (++loggedFailures_ == detailLogLimit) {
        note(L"이상 상세 기록 " + std::to_wstring(detailLogLimit) + L"건 도달. 이후 화면은 " +
             std::to_wstring(summaryInterval.count()) + L"분 주기 요약 기록.");
        startSummaries(at);
    }
}

void LoopbackTest::lateDetail(const std::wstring& text, Clock::time_point at)
{
    const bool screen = loggedLate_ < detailLogLimit;
    if (!screen && filedLate_ >= fileDetailLimit) return;
    note(text, {}, !screen);
    if (!screen) {
        if (++filedLate_ == fileDetailLimit)
            note(L"기록 파일 지연 상세 기록 " + grouped(fileDetailLimit) + L"건 도달. 이후 " +
                     std::to_wstring(summaryInterval.count()) + L"분 주기 요약만 기록.",
                 {}, true);
        return;
    }
    if (++loggedLate_ == detailLogLimit) {
        note(L"지연 상세 기록 " + std::to_wstring(detailLogLimit) + L"건 도달. 이후 화면은 " +
             std::to_wstring(summaryInterval.count()) + L"분 주기 요약 기록.");
        startSummaries(at);
    }
}

std::wstring LoopbackTest::admitEvidence(std::wstring evidence)
{
    if (evidence.empty()) return evidence;
    if (evidenceLogged_ >= evidenceLogLimit) return {};
    if (++evidenceLogged_ == evidenceLogLimit)
        evidence += L"\r\n송·수신 대조 기록(대조표·수신 원문) " + grouped(evidenceLogLimit) + L"건 도달. 이후 생략.";
    return evidence;
}

void LoopbackTest::startSummaries(Clock::time_point at)
{
    if (summarizing_) return;
    summarizing_ = true;
    summarizedFailures_ = state_.failed;
    summarizedLate_ = state_.late;
    summarizedAt_ = at;
}

void LoopbackTest::summarize(Clock::time_point now)
{
    if (!summarizing_ || now - summarizedAt_ < summaryInterval) return;
    const std::wstring period = L"최근 " + std::to_wstring(summaryInterval.count()) + L"분 ";
    if (loggedFailures_ >= detailLogLimit && state_.failed > summarizedFailures_)
        note(period + L"이상 " + grouped(state_.failed - summarizedFailures_) + L"건 (누적 " + grouped(state_.failed) +
             L"건)");
    if (loggedLate_ >= detailLogLimit && state_.late > summarizedLate_)
        note(period + L"지연 회신 " + grouped(state_.late - summarizedLate_) + L"건 (누적 " + grouped(state_.late) +
             L"건)");
    summarizedFailures_ = state_.failed;
    summarizedLate_ = state_.late;
    summarizedAt_ = now;
}

void LoopbackTest::note(std::wstring text, std::wstring evidence, bool fileOnly)
{
    events_.push_back({std::chrono::system_clock::now(), std::move(text), std::move(evidence), fileOnly});
    if (events_.size() > eventQueueLimit) events_.pop_front();
}

// 발생 시각 기준 구간에 판정 누적. 구간 내 이상 1건이면 해당 구간 이상
void LoopbackTest::remember(std::uint8_t mark, Clock::time_point at)
{
    const double fraction = std::chrono::duration<double>(at - startedAt_) / std::chrono::duration<double>(settings_.duration);
    const auto slot = (std::min)(timelineSlots - 1, static_cast<std::size_t>(std::clamp(fraction, 0.0, 1.0) * timelineSlots));
    if (timeline_[slot] < mark) timeline_[slot] = mark;
}

std::chrono::milliseconds LoopbackTest::sinceStart(Clock::time_point at) const
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(at - startedAt_);
}

LoopbackTest::Clock::duration LoopbackTest::replyFloor(std::size_t wireBytes) const
{
    const double micros = 2.0 * static_cast<double>(wireBytes) * charMicros_ * 0.75;
    return std::chrono::duration_cast<Clock::duration>(
        std::chrono::microseconds(static_cast<std::chrono::microseconds::rep>(micros)));
}

}  // namespace loopback
