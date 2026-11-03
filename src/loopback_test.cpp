#include "loopback_test.h"

#include <algorithm>

namespace simul {

LoopbackTest::LoopbackTest() = default;

LoopbackTest::~LoopbackTest() { stop(); }

bool LoopbackTest::start(const TestSettings& settings, std::wstring& error)
{
    if (running_.load()) { error = L"Test already running."; return false; }
    settings_ = settings;
    transport_ = makeTransport(settings.transport);
    if (!transport_) { error = L"Unknown transport."; return false; }
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
    startedAt_ = std::chrono::steady_clock::now();
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
            std::chrono::steady_clock::now() - startedAt_);
    }
    return copy;
}

void LoopbackTest::run()
{
    auto nextTx = std::chrono::steady_clock::now();
    const auto deadline = startedAt_ + settings_.duration;

    while (!cancel_.load()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_.failed == 0 && state_.verdict == Verdict::Pending) {
                state_.verdict = Verdict::Ok;
                state_.lastEvent = L"Duration reached. All frames matched.";
            }
            state_.running = false;
            state_.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - startedAt_);
            break;
        }

        if (now >= nextTx) {
            Data data{};
            randomData(data);
            const Buffer frame = buildFrame(data);
            std::wstring error;
            if (!transport_->send(frame, error)) {
                std::lock_guard<std::mutex> lock(mutex_);
                state_.lastError = error;
                state_.verdict = Verdict::Fail;
                state_.lastEvent = L"Transport send error: " + error;
                state_.running = false;
                break;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++state_.sent;
                if (pending_.size() >= pendingLimit) {
                    ++state_.failed;
                    state_.verdict = Verdict::Fail;
                    state_.lastEvent = L"Pending queue overflow; no echo received in time.";
                    pending_.pop_front();
                }
                pending_.push_back(data);
            }
            nextTx += settings_.interval;
            if (nextTx < now) nextTx = now + settings_.interval;
        }

        std::wstring rxError;
        if (!transport_->receive(rxBuffer_, rxError)) {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.lastError = rxError;
            state_.verdict = Verdict::Fail;
            state_.lastEvent = L"Transport receive error: " + rxError;
            state_.running = false;
            break;
        }

        for (;;) {
            Data got{};
            const Parse parsed = parseFrame(rxBuffer_, got);
            if (parsed == Parse::NeedMore) break;
            std::lock_guard<std::mutex> lock(mutex_);
            ++state_.received;
            if (parsed == Parse::Invalid) {
                ++state_.failed;
                state_.verdict = Verdict::Fail;
                state_.lastEvent = L"Invalid or corrupt frame received.";
                continue;
            }
            if (pending_.empty()) {
                ++state_.failed;
                state_.verdict = Verdict::Fail;
                state_.lastEvent = L"Received frame with no outstanding transmission.";
                continue;
            }
            const Data expected = pending_.front();
            pending_.pop_front();
            if (got != expected) {
                ++state_.failed;
                state_.verdict = Verdict::Fail;
                state_.lastEvent = L"Received DATA does not match transmitted DATA.";
            }
        }

        if (!cancel_.load()) {
            // Light sleep to keep the loop responsive without burning CPU.
            // We time the next send against the wall clock, so this sleep
            // does not drift the 50 ms cadence.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    state_.running = false;
    state_.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startedAt_);
    if (state_.verdict == Verdict::Pending && state_.failed == 0 && cancel_.load()) {
        state_.lastEvent = L"Stopped by user before duration reached.";
    }
}

}  // namespace simul
