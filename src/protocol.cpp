#include "protocol.h"

#include <algorithm>
#include <chrono>
#include <random>

namespace loopback {

std::uint16_t crc16(const Byte* data, std::size_t size)
{
    std::uint16_t crc = 0xFFFF;
    for (std::size_t i = 0; i < size; ++i) {
        crc = static_cast<std::uint16_t>(crc ^ (data[i] << 8));
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000) ? static_cast<std::uint16_t>((crc << 1) ^ 0x1021)
                                 : static_cast<std::uint16_t>(crc << 1);
        }
    }
    return crc;
}

void randomData(Data& out)
{
    // 결정적 random_device 구현 대비 단조 시계값 혼합
    thread_local std::mt19937 engine = [] {
        std::random_device device;
        const auto tick = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        std::seed_seq seed{device(), device(), device(), device(),
                           static_cast<unsigned>(tick & 0xFFFFFFFFu),
                           static_cast<unsigned>(tick >> 32)};
        return std::mt19937(seed);
    }();
    std::uniform_int_distribution<unsigned> dist(0x00, 0xFF);
    for (auto& byte : out) byte = static_cast<Byte>(dist(engine));
}

namespace {

void appendEscaped(Buffer& out, Byte value)
{
    if (value == STX || value == ETX || value == DLE) {
        out.push_back(DLE);
        out.push_back(static_cast<Byte>(value | 0x80));
    } else {
        out.push_back(value);
    }
}

ParseResult verifyBody(const Byte* body, std::size_t length, Data& dataOut, std::size_t discarded)
{
    ParseResult result{Parse::Invalid, FrameError::None, discarded};
    if (length != PAYLOAD_SIZE) {
        result.error = FrameError::Length;
    } else if (body[0] != HEADER_HI || body[1] != HEADER_LO) {
        result.error = FrameError::Header;
    } else if (body[TRAILER_OFFSET] != TRAILER_HI || body[TRAILER_OFFSET + 1] != TRAILER_LO) {
        result.error = FrameError::Trailer;
    } else if (static_cast<std::uint16_t>((body[CRC_OFFSET] << 8) | body[CRC_OFFSET + 1]) !=
               crc16(body, CRC_OFFSET)) {
        result.error = FrameError::Crc;
    } else {
        std::copy_n(body + DATA_OFFSET, DATA_SIZE, dataOut.begin());
        result.status = Parse::Ok;
    }
    return result;
}

}  // namespace

void buildFrame(const Data& data, Buffer& out)
{
    std::array<Byte, PAYLOAD_SIZE> body{};
    body[0] = HEADER_HI;
    body[1] = HEADER_LO;
    std::copy(data.begin(), data.end(), body.begin() + DATA_OFFSET);
    const std::uint16_t crc = crc16(body.data(), CRC_OFFSET);
    body[CRC_OFFSET] = static_cast<Byte>(crc >> 8);
    body[CRC_OFFSET + 1] = static_cast<Byte>(crc & 0xFF);
    body[TRAILER_OFFSET] = TRAILER_HI;
    body[TRAILER_OFFSET + 1] = TRAILER_LO;

    out.clear();
    out.reserve(MAX_FRAME_SIZE);
    out.push_back(STX);
    for (Byte b : body) appendEscaped(out, b);
    out.push_back(ETX);
}

ParseResult parseFrame(Buffer& inOut, Data& dataOut)
{
    const auto start = std::find(inOut.begin(), inOut.end(), STX);
    const auto discarded = static_cast<std::size_t>(start - inOut.begin());
    inOut.erase(inOut.begin(), start);
    if (inOut.empty()) return {Parse::NeedMore, FrameError::None, discarded};

    const auto consume = [&inOut](std::size_t count) {
        inOut.erase(inOut.begin(), inOut.begin() + static_cast<std::ptrdiff_t>(count));
    };
    const auto invalid = [discarded](FrameError error) {
        return ParseResult{Parse::Invalid, error, discarded};
    };

    // 본문 오류 확정 후에도 ETX까지 동일 전문으로 소비하여 잔여 바이트의 중복 계수 방지
    std::array<Byte, PAYLOAD_SIZE> body{};
    std::size_t length = 0;
    const auto append = [&body, &length](Byte value) {
        if (length == PAYLOAD_SIZE) return false;
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
            return verifyBody(body.data(), length, dataOut, discarded);
        }
        // 최대 길이 초과 전문은 ETX·STX까지 동일 전문으로 소비. 경계 없는 바이트열은 4배 길이에서 절단
        if (i + 1 >= MAX_FRAME_SIZE * 4) {
            consume(i + 1);
            return invalid(fault != FrameError::None ? fault : FrameError::Length);
        }
        if (i + 1 >= MAX_FRAME_SIZE && fault == FrameError::None) fault = FrameError::Length;
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
    return {Parse::NeedMore, FrameError::None, discarded};
}

}  // namespace loopback
