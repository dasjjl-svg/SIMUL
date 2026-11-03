#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace simul {

using Byte = std::uint8_t;
using Buffer = std::vector<Byte>;

// 전문 경계 바이트
constexpr Byte STX = 0x02;
constexpr Byte ETX = 0x03;
constexpr Byte DLE = 0x80;

// 전문 머리말·꼬리말
constexpr Byte HEADER_HI  = 0xC0;
constexpr Byte HEADER_LO  = 0xC1;
constexpr Byte TRAILER_HI = 0xFF;
constexpr Byte TRAILER_LO = 0xFF;

// DATA 고정 길이
constexpr std::size_t DATA_SIZE = 32;

// STX·ETX 사이 논리 적재: C0 C1 | DATA(32) | CRC16(2) | FF FF
constexpr std::size_t PAYLOAD_SIZE = 2 + DATA_SIZE + 2 + 2;

using Data = std::array<Byte, DATA_SIZE>;

// CRC-16/CCITT-FALSE: 다항식 0x1021, 초기값 0xFFFF, 반사·최종 XOR 없음
std::uint16_t crc16(const Byte* data, std::size_t size);

// 0x00~0xFF 전범위 균등 분포
void randomData(Data& out);

// DLE 치환 규칙
//   0x02 → 0x80 0x82
//   0x03 → 0x80 0x83
//   0x80 → 0x80 0x80
// 반환 전문: STX | escape(PAYLOAD) | ETX
Buffer buildFrame(const Data& data);

enum class Parse { NeedMore, Ok, Invalid };

// 수신 적치 버퍼에서 하나의 전문을 추출. Ok·Invalid 모두 ETX까지 소비.
Parse parseFrame(Buffer& inOut, Data& dataOut);

}  // namespace simul
