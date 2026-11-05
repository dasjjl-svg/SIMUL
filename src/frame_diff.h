#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "protocol.h"

namespace loopback {

// 송신 전문 대비 수신 회선 바이트 정렬 단위
enum class DiffOp : std::uint8_t { Same, Changed, Missing, Extra };

struct DiffStep {
    DiffOp op = DiffOp::Same;
    std::uint16_t sentAt = 0;  // 송신 회선 오프셋(STX=0). Extra는 후속 송신 바이트 위치
    std::uint16_t gotAt = 0;   // 수신 회선 오프셋. Missing은 후속 수신 바이트 위치
};

struct WireDiff {
    std::vector<DiffStep> steps;
    std::size_t changed = 0;
    std::size_t missing = 0;
    std::size_t extra = 0;
    // 연속 3바이트 이상 일치 구간 내 DATA·CRC 값 바이트 수. 대응 전문 판별 근거(산발적 우연 일치 배제)
    std::size_t anchored = 0;
    std::size_t errors() const { return changed + missing + extra; }
};

// 위치 대응 일치 바이트 수(선두 정렬·말미 정렬 합). 대응 전문 후보 선별용 근사 척도
std::size_t wireAffinity(const Buffer& sent, const Buffer& got);

// 최소 편집 정렬(변경·누락·추가 비용 각 1). sent: buildFrame 산출 전문
// 동일 비용 정렬 중 누락·추가를 뒤쪽 배치. 우연 일치 1바이트로 분리된 동종 구간은 1구간 병합(연속 손실 1건 표기)
// 공통 선두 제외 후 나머지 구간 정렬. 구간이 크면 최소 확정까지 대각 띠 확장 정렬
// 셀 상한(1M) 도달 시 마지막 띠 결과, 띠 불가 규모는 위치 대응 정렬(근사, 대량 잡음 대상)
WireDiff alignWire(const Buffer& sent, const Buffer& got);

// 상이 항목. 변경은 바이트별, 연속 누락·추가는 1항목. 오프셋은 송신 회선 기준
// 예: "[6] DATA[3] 9C→9D (XOR 01, 1비트)", "[21~39] DATA[18]~ETX 19 Byte 누락", "[8] DATA[5] 앞 0A 추가"
std::vector<std::wstring> diffItems(const Buffer& sent, const Buffer& got, const WireDiff& diff);

// 송·수신 대조표. 16열 단위 행, 행 머리 회선 오프셋
// 수신 행: 일치 "..", 누락 "--". 송신 행: 추가 위치 "--"
// 차이 행·첫 행·끝 행과 인접 행 외의 연속 일치 행(2행 이상)은 생략 1행으로 대체
std::wstring diffTable(const Buffer& sent, const Buffer& got, const WireDiff& diff);

// 바이트열 16 Byte 단위 행. 행 머리: label·오프셋
std::wstring byteRows(const Byte* bytes, std::size_t size, const wchar_t* label);

}  // namespace loopback
