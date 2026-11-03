#include "protocol.h"

#include <algorithm>
#include <random>

namespace simul {

std::uint16_t crc16(const Byte* data, std::size_t size)
{
    std::uint16_t crc = 0xFFFF;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= static_cast<std::uint16_t>(data[i]) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000) ? static_cast<std::uint16_t>((crc << 1) ^ 0x1021)
                                 : static_cast<std::uint16_t>(crc << 1);
        }
    }
    return crc;
}

void randomData(Data& out)
{
    thread_local std::random_device seed;
    thread_local std::mt19937 engine(seed());
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

}  // namespace

Buffer buildFrame(const Data& data)
{
    Buffer payload;
    payload.reserve(PAYLOAD_SIZE);
    payload.push_back(HEADER_HI);
    payload.push_back(HEADER_LO);
    payload.insert(payload.end(), data.begin(), data.end());

    // CRC 범위: C0C1 + DATA(32)
    const std::uint16_t crc = crc16(payload.data(), payload.size());
    payload.push_back(static_cast<Byte>((crc >> 8) & 0xFF));
    payload.push_back(static_cast<Byte>(crc & 0xFF));

    payload.push_back(TRAILER_HI);
    payload.push_back(TRAILER_LO);

    Buffer frame;
    frame.reserve(payload.size() * 2 + 2);
    frame.push_back(STX);
    for (Byte b : payload) appendEscaped(frame, b);
    frame.push_back(ETX);
    return frame;
}

Parse parseFrame(Buffer& inOut, Data& dataOut)
{
    auto firstStx = std::find(inOut.begin(), inOut.end(), STX);
    if (firstStx != inOut.begin()) inOut.erase(inOut.begin(), firstStx);
    if (inOut.empty()) return Parse::NeedMore;

    Buffer payload;
    payload.reserve(PAYLOAD_SIZE);
    bool escaping = false;
    std::size_t consumed = 1;
    bool frameClosed = false;
    bool valid = true;

    for (std::size_t i = 1; i < inOut.size(); ++i) {
        const Byte b = inOut[i];
        ++consumed;

        // 중첩 STX: 재동기화를 위해 선행 전문 폐기, 신규 STX 보존
        if (b == STX && !escaping) {
            --consumed;
            inOut.erase(inOut.begin(), inOut.begin() + static_cast<long>(consumed));
            return Parse::Invalid;
        }

        if (b == ETX && !escaping) { frameClosed = true; break; }

        if (escaping) {
            escaping = false;
            if (b == 0x82) payload.push_back(STX);
            else if (b == 0x83) payload.push_back(ETX);
            else if (b == 0x80) payload.push_back(DLE);
            else { valid = false; break; }
            continue;
        }
        if (b == DLE) { escaping = true; continue; }
        payload.push_back(b);

        if (payload.size() > PAYLOAD_SIZE) { valid = false; break; }
    }

    if (!frameClosed && valid) return Parse::NeedMore;

    inOut.erase(inOut.begin(), inOut.begin() + static_cast<long>(consumed));

    if (!frameClosed || !valid) return Parse::Invalid;
    if (payload.size() != PAYLOAD_SIZE) return Parse::Invalid;
    if (payload[0] != HEADER_HI || payload[1] != HEADER_LO) return Parse::Invalid;
    if (payload[PAYLOAD_SIZE - 2] != TRAILER_HI || payload[PAYLOAD_SIZE - 1] != TRAILER_LO)
        return Parse::Invalid;

    const std::uint16_t expected = static_cast<std::uint16_t>(
        (payload[2 + DATA_SIZE] << 8) | payload[2 + DATA_SIZE + 1]);
    const std::uint16_t actual = crc16(payload.data(), 2 + DATA_SIZE);
    if (expected != actual) return Parse::Invalid;

    std::copy(payload.begin() + 2, payload.begin() + 2 + DATA_SIZE, dataOut.begin());
    return Parse::Ok;
}

}  // namespace simul
