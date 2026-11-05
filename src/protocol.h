#pragma once

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

constexpr std::size_t DATA_SIZE = 32;        // DATA 기본 길이
constexpr std::size_t MIN_DATA_SIZE = 1;     // DATA 길이 범위
constexpr std::size_t MAX_DATA_SIZE = 1024;

// 전문 본문(STX·ETX 제외, DLE 치환 전) 오프셋. n: DATA 길이
//   [0..1]       시퀀스 C0 C1: C0 상위·C1 하위. 0x0000부터 전문마다 1 증가, 0xFFFF 다음 0x0000
//   [2..n+1]     DATA n Byte
//   [n+2..n+3]   CRC16, 상위 바이트 우선
//   [n+4..n+5]   FF FF
constexpr std::size_t SEQUENCE_OFFSET = 0;
constexpr std::size_t DATA_OFFSET = 2;
constexpr std::size_t crcOffset(std::size_t dataSize) { return DATA_OFFSET + dataSize; }
constexpr std::size_t trailerOffset(std::size_t dataSize) { return crcOffset(dataSize) + 2; }
constexpr std::size_t payloadSize(std::size_t dataSize) { return trailerOffset(dataSize) + 2; }

// 회선상 최대 전문 길이: 시퀀스·DATA·CRC 전 바이트 DLE 치환 시. FF FF는 치환 대상 아님
constexpr std::size_t maxFrameSize(std::size_t dataSize) { return 1 + (2 + dataSize + 2) * 2 + 2 + 1; }
static_assert(payloadSize(DATA_SIZE) == 38 && maxFrameSize(DATA_SIZE) == 76, "전문 규격 불일치");
constexpr std::size_t MAX_PAYLOAD_SIZE = payloadSize(MAX_DATA_SIZE);
constexpr std::size_t MAX_FRAME_SIZE = maxFrameSize(MAX_DATA_SIZE);  // DATA 길이 상한 기준 2,060B

// 시퀀스 1순환 전문 수
constexpr std::size_t SEQUENCE_SPACE = 65536;

struct Frame {
    std::uint16_t sequence = 0;
    Buffer data;  // DATA. 길이 = DATA 길이 설정
};

// CRC-16/CCITT-FALSE: 다항식 0x1021, 초기값 0xFFFF, 반사·최종 XOR 없음. 범위: 본문 시퀀스·DATA
// crc: 이어 계산 시 직전 결과
std::uint16_t crc16(const Byte* data, std::size_t size, std::uint16_t crc = 0xFFFF);

// 시험별 DATA 생성 키. salt: 동시 시작 채널 간 구분값
std::uint64_t randomKey(std::uint64_t salt);

// DATA: 키·전문 번호로 결정되는 바이트별 0x00~0xFF 균등 분포 의사 난수
// 회신 대조 시 동일 키·번호로 재생성. 전문별 DATA 저장 불요
void frameData(std::uint64_t key, std::uint64_t number, Byte* out, std::size_t size);

// DLE 치환: 0x02 → 80 82, 0x03 → 80 83, 0x80 → 80 80
// out: STX | 치환 본문 | ETX. 기존 용량 재사용으로 송신 주기당 힙 할당 없음
void buildFrame(const Frame& frame, Buffer& out);

enum class Parse { NeedMore, Ok, Invalid };
enum class FrameError { None, Resync, Escape, Length, Trailer, Crc };

struct ParseResult {
    Parse status = Parse::NeedMore;
    FrameError error = FrameError::None;
    std::size_t discarded = 0;   // STX 이전 폐기 바이트 수
    std::size_t bodyLength = 0;  // Ok·Invalid: DLE 복원 본문 길이(오류 시 오류 지점까지)
    std::uint16_t sequence = 0;  // 복원 본문 2B 이상 시 시퀀스 해석값. 송·수신 대조 후보용
};

// 수신 버퍼 선두의 전문 1건 해석. dataSize: 기대 DATA 길이
//   STX 이전 바이트: discarded 보고 후 폐기
//   Ok·Invalid: ETX까지 소비. 원시 STX 출현 시 직전까지, 경계 없이 최대 전문 길이 4배 도달 시 해당 길이 소비
//   NeedMore: 미완 전문 보존
// 원시 STX·ETX는 본문 내 출현 불가. DLE 직후라도 전문 경계로 판정
// frameOut.data: Ok 시 DATA 기록. 용량 선확보 시 할당 없음
// frameBytes: Ok·Invalid 시 소비한 전문의 회선 바이트(STX부터) 대체 기록. 최대 전문 길이 4배 용량 선확보 시 할당 없음
// skipped: STX 이전 폐기 바이트 추가 기록. 기존 용량 한도 내 기록, 초과분 생략(할당 없음)
ParseResult parseFrame(Buffer& inOut, Frame& frameOut, std::size_t dataSize, Buffer* frameBytes = nullptr,
                       Buffer* skipped = nullptr);

}  // namespace loopback
