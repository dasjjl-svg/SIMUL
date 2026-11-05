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

constexpr Byte TRAILER_HI = 0xFF;
constexpr Byte TRAILER_LO = 0xFF;

constexpr std::size_t DATA_SIZE = 32;

// 전문 본문(STX·ETX 제외, DLE 치환 전) 오프셋
//   [0..1]   시퀀스 C0 C1: C0 상위·C1 하위. 0x0000부터 전문마다 1 증가, 0xFFFF 다음 0x0000
//   [2..33]  DATA 32B
//   [34..35] CRC16, 상위 바이트 우선
//   [36..37] FF FF
constexpr std::size_t SEQUENCE_OFFSET = 0;
constexpr std::size_t DATA_OFFSET    = 2;
constexpr std::size_t CRC_OFFSET     = DATA_OFFSET + DATA_SIZE;
constexpr std::size_t TRAILER_OFFSET = CRC_OFFSET + 2;
constexpr std::size_t PAYLOAD_SIZE   = TRAILER_OFFSET + 2;

// 회선상 최대 전문 길이 76B: 시퀀스·DATA·CRC 전 바이트 DLE 치환 시. FF FF는 치환 대상 아님
constexpr std::size_t MAX_FRAME_SIZE = 1 + (2 + DATA_SIZE + 2) * 2 + 2 + 1;
static_assert(PAYLOAD_SIZE == 38 && MAX_FRAME_SIZE == 76, "전문 규격 불일치");

// 시퀀스 1순환 전문 수
constexpr std::size_t SEQUENCE_SPACE = 65536;

using Data = std::array<Byte, DATA_SIZE>;

struct Frame {
    std::uint16_t sequence = 0;
    Data data{};
};

// CRC-16/CCITT-FALSE: 다항식 0x1021, 초기값 0xFFFF, 반사·최종 XOR 없음. 범위: 본문 [0..33](시퀀스·DATA)
std::uint16_t crc16(const Byte* data, std::size_t size);

// DATA 바이트별 0x00~0xFF 균등 분포 난수
void randomData(Data& out);

// DLE 치환: 0x02 → 80 82, 0x03 → 80 83, 0x80 → 80 80
// out: STX | 치환 본문 | ETX. 기존 용량 재사용으로 송신 주기당 힙 할당 없음
void buildFrame(const Frame& frame, Buffer& out);

enum class Parse { NeedMore, Ok, Invalid };
enum class FrameError { None, Resync, Escape, Length, Trailer, Crc };

struct ParseResult {
    Parse status = Parse::NeedMore;
    FrameError error = FrameError::None;
    std::size_t discarded = 0;  // STX 이전 폐기 바이트 수
    // Ok·Invalid: DLE 복원 본문(오류 시 오류 지점까지). 송·수신 대조용
    std::array<Byte, PAYLOAD_SIZE> body{};
    std::size_t bodyLength = 0;
};

// 수신 버퍼 선두의 전문 1건 해석
//   STX 이전 바이트: discarded 보고 후 폐기
//   Ok·Invalid: ETX까지 소비. 원시 STX 출현 시 직전까지, 경계 없이 304B(최대 길이 4배) 도달 시 304B 소비
//   NeedMore: 미완 전문 보존
// 원시 STX·ETX는 본문 내 출현 불가. DLE 직후라도 전문 경계로 판정
// frameBytes: Ok·Invalid 시 소비한 전문의 회선 바이트(STX부터) 대체 기록. 304B 용량 선확보 시 할당 없음
// skipped: STX 이전 폐기 바이트 추가 기록. 기존 용량 한도 내 기록, 초과분 생략(할당 없음)
ParseResult parseFrame(Buffer& inOut, Frame& frameOut, Buffer* frameBytes = nullptr, Buffer* skipped = nullptr);

}  // namespace loopback
