#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace loopback {

using Byte = std::uint8_t;
using Buffer = std::vector<Byte>;

constexpr Byte STX = 0x02;
constexpr Byte ETX = 0x03;
constexpr Byte DLE = 0x80;

constexpr Byte HEADER_HI  = 0xC0;
constexpr Byte HEADER_LO  = 0xC1;
constexpr Byte TRAILER_HI = 0xFF;
constexpr Byte TRAILER_LO = 0xFF;

constexpr std::size_t DATA_SIZE = 32;

// 전문 본문(STX·ETX 제외, DLE 치환 전) 오프셋
//   [0..1]   C0 C1
//   [2..33]  DATA 32B
//   [34..35] CRC16, 상위 바이트 우선
//   [36..37] FF FF
constexpr std::size_t DATA_OFFSET    = 2;
constexpr std::size_t CRC_OFFSET     = DATA_OFFSET + DATA_SIZE;
constexpr std::size_t TRAILER_OFFSET = CRC_OFFSET + 2;
constexpr std::size_t PAYLOAD_SIZE   = TRAILER_OFFSET + 2;

// 회선상 최대 전문 길이 74B: DATA·CRC 전 바이트 DLE 치환 시. C0 C1·FF FF는 치환 대상 아님
constexpr std::size_t MAX_FRAME_SIZE = 1 + 2 + (DATA_SIZE + 2) * 2 + 2 + 1;
static_assert(PAYLOAD_SIZE == 38 && MAX_FRAME_SIZE == 74, "전문 규격 불일치");

using Data = std::array<Byte, DATA_SIZE>;

// CRC-16/CCITT-FALSE: 다항식 0x1021, 초기값 0xFFFF, 반사·최종 XOR 없음. 범위: 본문 [0..33]
std::uint16_t crc16(const Byte* data, std::size_t size);

// DATA 바이트별 0x00~0xFF 균등 분포 난수
void randomData(Data& out);

// DLE 치환: 0x02 → 80 82, 0x03 → 80 83, 0x80 → 80 80
// 반환 전문: STX | 치환 본문 | ETX
Buffer buildFrame(const Data& data);

enum class Parse { NeedMore, Ok, Invalid };
enum class FrameError { None, Resync, Escape, Length, Header, Trailer, Crc };

struct ParseResult {
    Parse status = Parse::NeedMore;
    FrameError error = FrameError::None;
    std::size_t discarded = 0;  // STX 이전 폐기 바이트 수
};

// 수신 버퍼 선두의 전문 1건 해석.
//   STX 이전 바이트: discarded 보고 후 폐기
//   Ok·Invalid: ETX까지 소비. 원시 STX 출현 시 직전까지, 경계 없이 296B(최대 길이 4배) 초과 시 해당 지점까지 소비
//   NeedMore: 미완 전문 보존
// 원시 STX·ETX는 본문 내 출현 불가이므로 DLE 직후라도 전문 경계로 판정.
ParseResult parseFrame(Buffer& inOut, Data& dataOut);

}  // namespace loopback
