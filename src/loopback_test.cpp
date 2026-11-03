#include "loopback_test.h"

#include <algorithm>

namespace simul {

using Clock = std::chrono::steady_clock;

LoopbackTest::LoopbackTest() = default;

LoopbackTest::~LoopbackTest() { stop(); }

bool LoopbackTest::start(const TestSettings& settings, std::wstring& error)
{
    if (running_.load()) { error = L"시험 실행 중."; return false; }
    settings_ = settings;
    transport_ = makeTransport(settings.transport);
    if (!transport_) { error = L"접속 방식 미지원."; return false; }
    if (!transport_->open(error)) { transport_.reset(); return false; }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = {};
        state_.running = true;
        state_.verdict = Verdict::Pending;
        state_.duration = settings_.duration;
        pending_.clear();
        rxBuffer_.clear();
    }
    startedAt_ = Clock::now();
    cancel_.store(false);
    running_.store(true);
    worker_ = std::thread([this] { run(); });
    return true;
}

void LoopbackTest::stop()
{
    cancel_.store(true);
    if (worker_.joinable()) worker_.join();
    running_.store(false);
    if (transport_) { transport_->close(); transport_.reset(); }
    std::lock_guard<std::mutex> lock(mutex_);
    state_.running = false;
}

TestSnapshot LoopbackTest::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto copy = state_;
    if (copy.running) {
        copy.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - startedAt_);
    }
    // 종료 후 잔여 회신 대기 구간은 표시 시간에서 제외
    if (copy.elapsed > copy.duration) copy.elapsed = copy.duration;
    return copy;
}

void LoopbackTest::fail(const std::wstring& event)
{
    ++state_.failed;
    state_.verdict = Verdict::Fail;
    state_.lastEvent = event;
}

void LoopbackTest::run()
{
    auto nextTx = Clock::now();
    const auto deadline = startedAt_ + settings_.duration;
    bool draining = false;
    Clock::time_point drainUntil{};

    while (!cancel_.load()) {
        const auto now = Clock::now();

        if (!draining && now >= deadline) {
            draining = true;
            drainUntil = now + echoTimeout;
        }
        if (draining) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pending_.empty() || now >= drainUntil) break;
        }

        if (!draining && now >= nextTx) {
            Data data{};
            randomData(data);
            const Buffer frame = buildFrame(data);
            std::wstring error;
            if (!transport_->send(frame, error)) {
                std::lock_guard<std::mutex> lock(mutex_);
                state_.lastError = error;
                fail(L"송신 오류: " + error);
                break;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++state_.sent;
                pending_.push_back({data, now});
            }
            nextTx += settings_.interval;
            if (nextTx < now) nextTx = now + settings_.interval;
        }

        std::wstring rxError;
        if (!transport_->receive(rxBuffer_, rxError)) {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.lastError = rxError;
            fail(L"수신 오류: " + rxError);
            break;
        }

        for (;;) {
            Data got{};
            const Parse parsed = parseFrame(rxBuffer_, got);
            if (parsed == Parse::NeedMore) break;
            std::lock_guard<std::mutex> lock(mutex_);
            ++state_.received;
            if (parsed == Parse::Invalid) {
                fail(L"비정상 전문 수신 (프레임·CRC 불량).");
                // 송·수신 적재 정합 유지
                if (!pending_.empty()) pending_.pop_front();
                continue;
            }
            if (pending_.empty()) {
                fail(L"미송신 상태에서 전문 수신.");
                continue;
            }
            const Data expected = pending_.front().data;
            pending_.pop_front();
            if (got != expected) fail(L"송·수신 DATA 상이.");
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            while (!pending_.empty() && now - pending_.front().sentAt > echoTimeout) {
                pending_.pop_front();
                fail(L"반송 시간 초과 (1000 ms).");
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!cancel_.load() && !pending_.empty()) {
        state_.failed += pending_.size();
        state_.verdict = Verdict::Fail;
        state_.lastEvent = L"시험 종료 시 미회신 프레임 " + std::to_wstring(pending_.size()) + L"건.";
        pending_.clear();
    }
    if (draining && !cancel_.load() && state_.verdict == Verdict::Pending && state_.failed == 0) {
        state_.verdict = Verdict::Ok;
        state_.lastEvent = L"시험 시간 완료, 전 프레임 일치.";
    }
    if (cancel_.load() && state_.verdict == Verdict::Pending) {
        state_.lastEvent = L"사용자 중지 (완료 전).";
    }
    state_.running = false;
    state_.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - startedAt_);
}

}  // namespace simul
