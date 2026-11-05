#include "protocol.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <random>

namespace loopback {

namespace {

constexpr std::array<std::uint16_t, 256> makeCrcTable()
{
    std::array<std::uint16_t, 256> table{};
    for (std::size_t i = 0; i < table.size(); ++i) {
        auto crc = static_cast<std::uint16_t>(i << 8);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x8000) ? static_cast<std::uint16_t>((crc << 1) ^ 0x1021) : static_cast<std::uint16_t>(crc << 1);
        table[i] = crc;
    }
    return table;
}

constexpr std::array<std::uint16_t, 256> kCrcTable = makeCrcTable();

// splitmix64 혼합 함수: 입력 1비트 변화가 출력 전 비트에 확산
std::uint64_t mix(std::uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

void appendEscaped(Buffer& out, Byte value)
{
    if (value == STX || value == ETX || value == DLE) {
        out.push_back(DLE);
        out.push_back(static_cast<Byte>(value | 0x80));
    } else {
        out.push_back(value);
    }
}

ParseResult verifyBody(const Byte* body, std::size_t length, Frame& frameOut, std::size_t dataSize,
                       std::size_t discarded)
{
    ParseResult result{Parse::Invalid, FrameError::None, discarded, length, 0};
    if (length >= DATA_OFFSET)
        result.sequence = static_cast<std::uint16_t>((body[SEQUENCE_OFFSET] << 8) | body[SEQUENCE_OFFSET + 1]);
    const std::size_t crcAt = crcOffset(dataSize);
    const std::size_t trailerAt = trailerOffset(dataSize);
    if (length != payloadSize(dataSize)) {
        result.error = FrameError::Length;
    } else if (body[trailerAt] != TRAILER_HI || body[trailerAt + 1] != TRAILER_LO) {
        result.error = FrameError::Trailer;
    } else if (static_cast<std::uint16_t>((body[crcAt] << 8) | body[crcAt + 1]) != crc16(body, crcAt)) {
        result.error = FrameError::Crc;
    } else {
        frameOut.sequence = result.sequence;
        frameOut.data.assign(body + DATA_OFFSET, body + crcAt);
        result.status = Parse::Ok;
    }
    return result;
}

}  // namespace

std::uint16_t crc16(const Byte* data, std::size_t size, std::uint16_t crc)
{
    for (std::size_t i = 0; i < size; ++i)
        crc = static_cast<std::uint16_t>((crc << 8) ^ kCrcTable[((crc >> 8) ^ data[i]) & 0xFF]);
    return crc;
}

std::uint64_t randomKey(std::uint64_t salt)
{
    // 결정적 random_device 구현 대비 단조 시계값·구분값 혼합
    std::random_device device;
    const auto tick = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    std::uint64_t key = mix(tick) ^ mix(salt);
    for (int i = 0; i < 4; ++i) key = mix(key ^ static_cast<std::uint64_t>(device()));
    return key;
}

void frameData(std::uint64_t key, std::uint64_t number, Byte* out, std::size_t size)
{
    // 전문별 기준값 + 8바이트 단위 계수 혼합(계수 방식). 임의 전문·위치 독립 재생성
    const std::uint64_t base = mix(key ^ mix(number));
    for (std::size_t i = 0; i < size; i += 8) {
        const std::uint64_t word = mix(base + i);
        const std::size_t count = (std::min)(std::size_t{8}, size - i);
        for (std::size_t k = 0; k < count; ++k) out[i + k] = static_cast<Byte>(word >> (8 * k));
    }
}

void buildFrame(const Frame& frame, Buffer& out)
{
    const std::size_t dataSize = frame.data.size();
    const Byte sequence[2] = {static_cast<Byte>(frame.sequence >> 8), static_cast<Byte>(frame.sequence & 0xFF)};
    const std::uint16_t crc = crc16(frame.data.data(), dataSize, crc16(sequence, 2));

    out.clear();
    out.reserve(maxFrameSize(dataSize));
    out.push_back(STX);
    for (Byte b : sequence) appendEscaped(out, b);
    for (Byte b : frame.data) appendEscaped(out, b);
    appendEscaped(out, static_cast<Byte>(crc >> 8));
    appendEscaped(out, static_cast<Byte>(crc & 0xFF));
    out.push_back(TRAILER_HI);
    out.push_back(TRAILER_LO);
    out.push_back(ETX);
}

ParseResult parseFrame(Buffer& inOut, Frame& frameOut, std::size_t dataSize, Buffer* frameBytes, Buffer* skipped)
{
    const auto start = std::find(inOut.begin(), inOut.end(), STX);
    const auto discarded = static_cast<std::size_t>(start - inOut.begin());
    if (skipped && discarded) {
        const std::size_t room = skipped->capacity() - skipped->size();
        skipped->insert(skipped->end(), inOut.begin(),
                        inOut.begin() + static_cast<std::ptrdiff_t>((std::min)(discarded, room)));
    }
    inOut.erase(inOut.begin(), start);
    if (inOut.empty()) return {Parse::NeedMore, FrameError::None, discarded, 0, 0};

    // 본문 오류 확정 후에도 ETX까지 동일 전문으로 소비하여 잔여 바이트의 중복 계수 방지
    const std::size_t capacity = payloadSize(dataSize);
    const std::size_t maxFrame = maxFrameSize(dataSize);
    std::array<Byte, MAX_PAYLOAD_SIZE> body{};
    std::size_t length = 0;
    const auto consume = [&inOut, frameBytes](std::size_t count) {
        if (frameBytes) frameBytes->assign(inOut.begin(), inOut.begin() + static_cast<std::ptrdiff_t>(count));
        inOut.erase(inOut.begin(), inOut.begin() + static_cast<std::ptrdiff_t>(count));
    };
    const auto invalid = [discarded, &body, &length](FrameError error) {
        ParseResult result{Parse::Invalid, error, discarded, length, 0};
        if (length >= DATA_OFFSET)
            result.sequence = static_cast<std::uint16_t>((body[SEQUENCE_OFFSET] << 8) | body[SEQUENCE_OFFSET + 1]);
        return result;
    };
    const auto append = [&body, &length, capacity](Byte value) {
        if (length == capacity) return false;
        body[length++] = value;
        return true;
    };
    bool escaping = false;
    FrameError fault = FrameError::None;
    for (std::size_t i = 1; i < inOut.size(); ++i) {
        const Byte b = inOut[i];
        if (b == STX) {
            consume(i);
            return invalid(fault != FrameError::None ? fault : FrameError::Resync);
        }
        if (b == ETX) {
            consume(i + 1);
            if (fault == FrameError::None && escaping) fault = FrameError::Escape;
            if (fault != FrameError::None) return invalid(fault);
            return verifyBody(body.data(), length, frameOut, dataSize, discarded);
        }
        // 최대 길이 초과 전문은 ETX·STX까지 동일 전문으로 소비. 경계 없는 바이트열은 4배 길이에서 절단
        if (i + 1 >= maxFrame * 4) {
            consume(i + 1);
            return invalid(fault != FrameError::None ? fault : FrameError::Length);
        }
        if (i + 1 >= maxFrame && fault == FrameError::None) fault = FrameError::Length;
        if (fault != FrameError::None) continue;
        if (escaping) {
            escaping = false;
            if (b != (STX | 0x80) && b != (ETX | 0x80) && b != DLE) fault = FrameError::Escape;
            else if (!append(b == DLE ? DLE : static_cast<Byte>(b & 0x7F))) fault = FrameError::Length;
        } else if (b == DLE) {
            escaping = true;
        } else if (!append(b)) {
            fault = FrameError::Length;
        }
    }
    return {Parse::NeedMore, FrameError::None, discarded, 0, 0};
}

}  // namespace loopback
