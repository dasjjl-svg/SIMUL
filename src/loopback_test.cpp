#include "loopback_test.h"

#include <algorithm>
#include <cstdio>
#include <iterator>

#include "format.h"

namespace loopback {

namespace {

const wchar_t* describe(FrameError error)
{
    switch (error) {
    case FrameError::Resync:  return L"STX 중복 (전문 절단)";
    case FrameError::Escape:  return L"DLE 치환 오류";
    case FrameError::Length:  return L"본문 길이 오류";
    case FrameError::Header:  return L"헤더 C0 C1 불일치";
    case FrameError::Trailer: return L"트레일러 FF FF 불일치";
    case FrameError::Crc:     return L"CRC 불일치";
    case FrameError::None:    break;
    }
    return L"형식 오류";
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

// 소수점 1자리 ms 표기
std::wstring preciseMs(std::chrono::steady_clock::duration value)
{
    wchar_t buffer[32]{};
    std::swprintf(buffer, std::size(buffer), L"%.1f ms",
                  static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(value).count()) / 1000.0);
    return buffer;
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
    transport_ = makeTransport(settings.transport);
    if (!transport_) { error = L"접속 방식 미지원."; return false; }
    if (!transport_->open(error)) { transport_.reset(); return false; }
    return true;
}

bool LoopbackTest::start(const TestSettings& settings, std::wstring& error)
{
    if (!open(settings, error)) return false;
    begin();
    return true;
}

void LoopbackTest::begin()
{
    if (!transport_ || worker_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = {};
        state_.running = true;
        state_.duration = settings_.duration;
        state_.echoTimeout = settings_.echoTimeout;
        pending_.clear();
        expired_.clear();
        answered_.clear();
        events_.clear();
        recentCount_ = 0;
        recentNext_ = 0;
        latencySumUs_ = 0;
        loggedFailures_ = 0;
        summarizedFailures_ = 0;
        completed_ = false;
        dropLocalEcho_ = halfDuplex_ && settings_.transport.serial.localEcho == LocalEcho::Present;
        echoDetected_ = false;
        drainStarted_ = false;
        const std::wstring notice = transport_->notice();
        if (!notice.empty()) note(notice);
    }
    rxBuffer_.clear();
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
    copy.recent.reserve(recentCount_);
    const std::size_t first = (recentNext_ + recentCapacity - recentCount_) % recentCapacity;
    for (std::size_t i = 0; i < recentCount_; ++i)
        copy.recent.push_back(recent_[(first + i) % recentCapacity]);
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
    const auto deadline = startedAt_ + settings_.duration;
    auto nextTx = startedAt_;
    bool draining = false;
    Clock::time_point drainUntil{};
    std::uint64_t seq = 0;
    std::wstring abortReason;

    while (!cancel_.load()) {
        const auto now = Clock::now();
        // 시험 시간 내 예정 주기 송신 완료 후에만 잔여 회신 대기 진입. 종료 직전 정지 시 누락 주기 계수 보장
        if (!draining && now >= deadline && nextTx >= deadline) {
            draining = true;
            drainUntil = now + settings_.echoTimeout;
            std::lock_guard<std::mutex> lock(mutex_);
            drainStarted_ = true;
        }
        if (draining) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pending_.empty() || now >= drainUntil) {
                completed_ = true;
                break;
            }
        }

        if (!draining && now >= nextTx) {
            Outstanding item;
            item.seq = ++seq;
            item.sentAt = now;
            randomData(item.data);
            const Buffer frame = buildFrame(item.data);
            item.wireBytes = frame.size();
            std::wstring error;
            if (!transport_->send(frame, error)) {
                abortReason = L"송신 오류: " + error;
                break;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            ++state_.sent;
            pending_.push_back(item);
            // 1주기 이상 지연 송신은 누락 주기 실패 계수 (절전·스케줄링 정지·회선 정체)
            // 누락 주기는 시험 시간 내 예정분만 계수, 최소 1주기
            const auto slot = nextTx;
            const auto lateness = now - slot;
            nextTx += settings_.interval;
            if (lateness >= settings_.interval) {
                const auto skipped = static_cast<std::uint64_t>(lateness / settings_.interval);
                const auto window = slot + settings_.interval < deadline
                                        ? static_cast<std::uint64_t>((deadline - slot - Clock::duration(1)) /
                                                                     settings_.interval)
                                        : std::uint64_t{0};
                const std::uint64_t missed = (std::max)(std::uint64_t{1}, (std::min)(skipped, window));
                fail(0, L"송신 주기 이탈 (" + grouped(millisecondsOf(lateness)) + L" ms 지연, 누락 " +
                            grouped(missed) + L"주기)", now, true, missed);
                nextTx = now + settings_.interval;
            }
        }

        std::wstring error;
        const bool received = transport_->receive(rxBuffer_, error);
        const LineFault fault = transport_->takeFault();
        const auto at = Clock::now();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (fault.count) {
                fail(0, fault.text + (fault.count > 1 ? L" 외 " + grouped(fault.count - 1) + L"건" : std::wstring()),
                     at, true, fault.count);
            }
            // 치명 오류 시에도 동일 호출에서 수신 완료된 전문은 판정 반영
            for (;;) {
                Data got{};
                const ParseResult parsed = parseFrame(rxBuffer_, got);
                handleFrame(parsed, got, at);
                if (parsed.status == Parse::NeedMore) break;
            }
            if (received) expire(at);
            summarize(at);
        }
        if (!received) {
            abortReason = L"수신 오류: " + error;
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    finish(abortReason);
}

void LoopbackTest::finish(const std::wstring& abortReason)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto end = Clock::now();
    const bool aborted = !abortReason.empty();
    // 시험 시간 경과 후 잔여 회신 대기 중 중지는 시험 완료로 판정. 미회신분은 실패
    const bool waitStopped = !aborted && !completed_ && drainStarted_;
    const bool cancelled = !aborted && !completed_ && !drainStarted_;
    const std::size_t unanswered = cancelled ? 0 : pending_.size();

    if (aborted) {
        state_.abortReason = abortReason;
        // 상세 기록 상한과 무관하게 항상 기록
        const std::wstring text = L"시험 중단" +
                                  (unanswered ? L" (미회신 " + grouped(unanswered) + L"건)" : std::wstring()) +
                                  L". " + abortReason;
        fail(0, text, end, false);
        note(text);
    }
    const wchar_t* reason = waitStopped ? L"미회신 (잔여 회신 대기 중 중지)" : L"미회신 (시험 종료 시점)";
    for (std::size_t i = 0; i < unanswered; ++i) fail(pending_[i].seq, reason, end, false);
    if (!aborted && unanswered)
        note((waitStopped ? L"잔여 회신 대기 중 중지. 미회신 " : L"시험 종료 시점 미회신 ") + grouped(unanswered) + L"건.");
    pending_.clear();

    if (state_.failed) {
        state_.verdict = Verdict::Fail;
        note(L"시험 실패. 실패 " + grouped(state_.failed) + L"건, 정상 회신 " + grouped(state_.matched) + L"건.");
    } else if (cancelled) {
        state_.verdict = Verdict::Stopped;
        note(L"사용자 중지. 시험 시간 미완료로 판정 보류.");
    } else {
        state_.verdict = Verdict::Ok;
        note(L"시험 성공. 전 전문 정상 회신 " + grouped(state_.matched) + L"건." +
             (halfDuplex_ && !dropLocalEcho_ ? L" 송신 반향 미발생 설정 기준." : L""));
    }
    state_.elapsed = sinceStart(end);
    state_.running = false;
}

void LoopbackTest::handleFrame(const ParseResult& parsed, const Data& got, Clock::time_point at)
{
    if (parsed.discarded) fail(0, L"전문 외 수신 " + grouped(parsed.discarded) + L" Byte", at);
    if (parsed.status == Parse::NeedMore) return;

    if (parsed.status == Parse::Invalid) {
        ++state_.received;
        fail(0, std::wstring(L"전문 불량: ") + describe(parsed.error), at);
        return;
    }

    const auto sameData = [&got](const Outstanding& item) { return item.data == got; };
    const auto match = std::find_if(pending_.begin(), pending_.end(), sameData);
    if (match != pending_.end()) {
        // RS-485 2선식 자기 송신 반향: 송신 전문당 최초 1회 판정 제외
        if (dropLocalEcho_ && !match->localEchoSeen) {
            match->localEchoSeen = true;
            return;
        }
        const auto latency = at - match->sentAt;
        // 반이중 선로의 장치 회신은 송신 완료 후 개시. 하한 미달 사본은 장치 회신 불가, 자기 송신 반향 확정
        if (halfDuplex_ && !dropLocalEcho_ && latency < replyFloor(match->wireBytes)) {
            dropLocalEcho_ = true;
            echoDetected_ = true;
            match->localEchoSeen = true;
            const std::wstring text = L"송신 반향 검출 (" + preciseMs(latency) + L" 회신, 장치 회신 하한 " +
                                      preciseMs(replyFloor(match->wireBytes)) +
                                      L"). 이후 반향 제거 적용, 송신 반향 설정 '발생' 필요";
            // 상세 기록 상한과 무관하게 항상 기록
            fail(match->seq, text, at, false);
            note(L"#" + std::to_wstring(match->seq) + L" " + text);
            return;
        }
        ++state_.received;
        if (latency > settings_.echoTimeout)
            fail(match->seq, L"회신 시간 초과 (" + grouped(millisecondsOf(latency)) + L" ms 회신)", at);
        else
            pass(latency);
        answered_.push_back(*match);
        if (answered_.size() > answeredCapacity) answered_.pop_front();
        pending_.erase(match);
        return;
    }

    const auto late = std::find_if(expired_.begin(), expired_.end(), sameData);
    if (late != expired_.end()) {
        // 시간 초과 판정 전문의 자기 송신 반향 사본은 판정 제외
        if (dropLocalEcho_ && !late->localEchoSeen) {
            late->localEchoSeen = true;
            return;
        }
        ++state_.received;
        detail(L"#" + std::to_wstring(late->seq) + L" 지연 회신 (" + grouped(millisecondsOf(at - late->sentAt)) +
                   L" ms, 시간 초과 판정 완료분)", at);
        answered_.push_back(*late);
        if (answered_.size() > answeredCapacity) answered_.pop_front();
        expired_.erase(late);
        return;
    }

    ++state_.received;
    const auto again = std::find_if(answered_.rbegin(), answered_.rend(), sameData);
    if (again != answered_.rend()) {
        fail(again->seq, halfDuplex_ && !dropLocalEcho_ ? L"중복 회신 (자기 송신 반향 시 송신 반향 설정 '발생' 필요)"
                                                       : L"중복 회신", at);
        return;
    }
    if (pending_.empty()) {
        fail(0, L"송신 대응 없는 전문 수신", at);
        return;
    }

    const Outstanding& oldest = pending_.front();
    std::size_t differing = 0;
    for (std::size_t i = 0; i < DATA_SIZE; ++i) differing += oldest.data[i] != got[i] ? 1u : 0u;
    fail(0, L"DATA 불일치 (#" + std::to_wstring(oldest.seq) + L" 대비 " + std::to_wstring(differing) +
                L" Byte 상이)\r\n송신 " + hexText(oldest.data.data(), DATA_SIZE) +
                L"\r\n수신 " + hexText(got.data(), DATA_SIZE), at);
}

void LoopbackTest::expire(Clock::time_point now)
{
    while (!pending_.empty() && now - pending_.front().sentAt > settings_.echoTimeout) {
        const Outstanding& item = pending_.front();
        std::wstring text = L"회신 시간 초과 (" + grouped(static_cast<std::uint64_t>(settings_.echoTimeout.count())) +
                            L" ms)";
        if (item.localEchoSeen)
            text += echoDetected_ ? L". 송신 반향만 수신, 장치 무회신"
                                  : L". 사본 1건만 수신: 장치 무회신 또는 반향 미발생 어댑터 (송신 반향 설정 확인)";
        fail(item.seq, text, now);
        expired_.push_back(pending_.front());
        if (expired_.size() > expiredCapacity) expired_.pop_front();
        pending_.pop_front();
    }
}

void LoopbackTest::fail(std::uint64_t seq, const std::wstring& text, Clock::time_point at, bool log,
                        std::uint64_t count)
{
    state_.failed += count;
    state_.verdict = Verdict::Fail;
    remember(false);

    const std::wstring tagged = (seq ? L"#" + std::to_wstring(seq) + L" " : std::wstring()) + text;
    if (!state_.hasFailure) {
        state_.hasFailure = true;
        state_.firstFailureAt = sinceStart(at);
        state_.firstFailure = firstLine(tagged);
    }
    if (log) detail(tagged, at);
}

void LoopbackTest::pass(Clock::duration latency)
{
    ++state_.matched;
    remember(true);
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(latency);
    if (us.count() > 0) latencySumUs_ += static_cast<std::uint64_t>(us.count());
    if (us > state_.maxLatency) state_.maxLatency = us;
}

void LoopbackTest::detail(const std::wstring& text, Clock::time_point at)
{
    if (loggedFailures_ >= failureLogLimit) return;
    note(text);
    if (++loggedFailures_ == failureLogLimit) {
        note(L"이상 상세 기록 " + std::to_wstring(failureLogLimit) + L"건 도달. 이후 " +
             std::to_wstring(summaryInterval.count()) + L"분 주기 요약 기록.");
        summarizedFailures_ = state_.failed;
        summarizedAt_ = at;
    }
}

void LoopbackTest::summarize(Clock::time_point now)
{
    if (loggedFailures_ < failureLogLimit || now - summarizedAt_ < summaryInterval) return;
    if (state_.failed > summarizedFailures_) {
        note(L"최근 " + std::to_wstring(summaryInterval.count()) + L"분 실패 " +
             grouped(state_.failed - summarizedFailures_) + L"건 (누적 " + grouped(state_.failed) + L"건)");
    }
    summarizedFailures_ = state_.failed;
    summarizedAt_ = now;
}

void LoopbackTest::note(std::wstring text)
{
    events_.push_back({std::chrono::system_clock::now(), std::move(text)});
    if (events_.size() > eventQueueLimit) events_.pop_front();
}

void LoopbackTest::remember(bool ok)
{
    recent_[recentNext_] = ok;
    recentNext_ = (recentNext_ + 1) % recentCapacity;
    if (recentCount_ < recentCapacity) ++recentCount_;
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
