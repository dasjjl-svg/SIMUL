#include "frame_diff.h"

#include <algorithm>
#include <cwchar>
#include <iterator>

#include "format.h"

namespace loopback {

namespace {

constexpr std::size_t kRowBytes = 16;
constexpr std::size_t kExtraShown = 8;           // 추가 바이트 항목 내 값 표기 상한
constexpr std::size_t kAnchorRun = 3;            // 대응 판별 연속 일치 구간 최소 길이
constexpr std::size_t kFullCells = 1u << 16;     // 중간 구간 전체 정렬 상한 셀 수
constexpr std::ptrdiff_t kBandSlack = 64;        // 대각 띠 정렬 여유 폭
constexpr std::size_t kBandCells = 1u << 20;     // 띠 정렬 상한 셀 수(2MB). 초과 시 위치 대응 정렬
constexpr std::uint16_t kUnreached = 0xFFFF;

enum class Area : std::uint8_t { Stx, Body, Etx };

// 송신 회선 오프셋별 전문 영역
struct Place {
    Area area = Area::Body;
    std::size_t body = 0;  // 본문 순번(DLE 복원 기준)
    bool prefix = false;   // DLE 치환 접두 바이트
};

struct Layout {
    std::vector<Place> places;
    std::size_t dataSize = 0;  // 본문 길이 - 6
};

Layout layoutOf(const Buffer& sent)
{
    Layout layout;
    auto& places = layout.places;
    places.resize(sent.size());
    if (sent.empty()) return layout;
    places.front().area = Area::Stx;
    if (sent.size() > 1) places.back().area = Area::Etx;
    std::size_t body = 0;
    for (std::size_t i = 1; i + 1 < sent.size(); ++i, ++body) {
        places[i].body = body;
        if (sent[i] == DLE && i + 2 < sent.size()) {
            places[i].prefix = true;
            places[++i].body = body;
        }
    }
    layout.dataSize = body >= payloadSize(0) ? body - payloadSize(0) : 0;
    return layout;
}

std::wstring areaName(const Place& place, std::size_t dataSize)
{
    if (place.area == Area::Stx) return L"STX";
    if (place.area == Area::Etx) return L"ETX";
    const auto at = static_cast<unsigned>(place.body);
    wchar_t text[24]{};
    if (place.body < DATA_OFFSET)
        std::swprintf(text, std::size(text), L"C%u", at);
    else if (place.body < crcOffset(dataSize))
        std::swprintf(text, std::size(text), L"DATA[%u]", at - static_cast<unsigned>(DATA_OFFSET));
    else if (place.body < trailerOffset(dataSize))
        std::swprintf(text, std::size(text), L"CRC[%u]", at - static_cast<unsigned>(crcOffset(dataSize)));
    else if (place.body < payloadSize(dataSize))
        std::swprintf(text, std::size(text), L"트레일러[%u]", at - static_cast<unsigned>(trailerOffset(dataSize)));
    else
        std::swprintf(text, std::size(text), L"본문[%u]", at);
    return std::wstring(text) + (place.prefix ? L" DLE" : L"");
}

void appendHex(std::wstring& out, Byte value)
{
    static const wchar_t digits[] = L"0123456789ABCDEF";
    out.push_back(digits[value >> 4]);
    out.push_back(digits[value & 0x0F]);
}

std::wstring offsetText(std::size_t at)
{
    return L"[" + std::to_wstring(at) + L"]";
}

// 행 머리: "송신 016 ". width: 오프셋 자릿수(1,000 이상 오프셋 포함 시 4)
std::wstring rowHead(const wchar_t* label, std::size_t at, int width)
{
    wchar_t digits[12]{};
    std::swprintf(digits, std::size(digits), L"%0*u", width, static_cast<unsigned>(at));
    return std::wstring(label) + L" " + digits + L" ";
}

unsigned bitCount(unsigned value)
{
    unsigned count = 0;
    for (; value; value &= value - 1) ++count;
    return count;
}

void push(std::vector<DiffStep>& steps, DiffOp op, std::size_t sentAt, std::size_t gotAt)
{
    steps.push_back({op, static_cast<std::uint16_t>(sentAt), static_cast<std::uint16_t>(gotAt)});
}

// 중간 구간 편집 거리 정렬: sent[s0, s0+a) 대 got[g0, g0+b). 대각(d = j - i) 범위 [low, high] 내 경로만 탐색
// 말미부터 역추적. 동일 비용 시 누락·추가 우선 선택으로 공백을 뒤쪽 배치(절단·말미 추가가 연속 1항목)
// 반환: 띠 내 최소 비용
unsigned alignBand(const Buffer& sent, std::size_t s0, std::size_t a, const Buffer& got, std::size_t g0, std::size_t b,
                   std::ptrdiff_t low, std::ptrdiff_t high, std::vector<DiffStep>& steps)
{
    // 띠 폭이 수신 구간 길이 이상이면 행렬 배치(도달 가능 칸만 확보), 미만이면 대각 띠 배치
    const auto width = static_cast<std::size_t>(high - low + 1);
    const bool dense = width > b + 1;
    const std::size_t rowWidth = dense ? b + 1 : width;
    std::vector<std::uint16_t> cost((a + 1) * rowWidth, kUnreached);
    const auto index = [rowWidth, low, dense](std::size_t i, std::size_t j) {
        return i * rowWidth +
               (dense ? j : static_cast<std::size_t>(static_cast<std::ptrdiff_t>(j) - static_cast<std::ptrdiff_t>(i) - low));
    };
    const auto value = [&](std::size_t i, std::size_t j) -> unsigned {
        const std::ptrdiff_t d = static_cast<std::ptrdiff_t>(j) - static_cast<std::ptrdiff_t>(i);
        if (j > b || d < low || d > high) return kUnreached;
        return cost[index(i, j)];
    };
    for (std::size_t i = 0; i <= a; ++i) {
        const std::ptrdiff_t from = (std::max)(std::ptrdiff_t{0}, static_cast<std::ptrdiff_t>(i) + low);
        const std::ptrdiff_t to = (std::min)(static_cast<std::ptrdiff_t>(b), static_cast<std::ptrdiff_t>(i) + high);
        for (std::ptrdiff_t column = from; column <= to; ++column) {
            const auto j = static_cast<std::size_t>(column);
            unsigned best = i == 0 && j == 0 ? 0u : kUnreached;
            if (i && j) {
                const unsigned diagonal = value(i - 1, j - 1);
                if (diagonal != kUnreached)
                    best = (std::min)(best, diagonal + (sent[s0 + i - 1] == got[g0 + j - 1] ? 0u : 1u));
            }
            if (i) {
                const unsigned up = value(i - 1, j);
                if (up != kUnreached) best = (std::min)(best, up + 1u);
            }
            if (j) {
                const unsigned left = value(i, j - 1);
                if (left != kUnreached) best = (std::min)(best, left + 1u);
            }
            cost[index(i, j)] = static_cast<std::uint16_t>(best);
        }
    }

    const unsigned total = value(a, b);
    std::vector<DiffStep> reversed;
    reversed.reserve(a + b);
    std::size_t i = a;
    std::size_t j = b;
    while (i || j) {
        const unsigned here = value(i, j);
        const unsigned up = i ? value(i - 1, j) : kUnreached;
        const unsigned left = j ? value(i, j - 1) : kUnreached;
        if (up != kUnreached && here == up + 1u) {
            --i;
            push(reversed, DiffOp::Missing, s0 + i, g0 + j);
        } else if (left != kUnreached && here == left + 1u) {
            --j;
            push(reversed, DiffOp::Extra, s0 + i, g0 + j);
        } else {
            --i;
            --j;
            push(reversed, sent[s0 + i] == got[g0 + j] ? DiffOp::Same : DiffOp::Changed, s0 + i, g0 + j);
        }
    }
    steps.insert(steps.end(), reversed.rbegin(), reversed.rend());
    return total;
}

// 누락(추가) 구간 사이 우연 일치 1바이트로 분리된 동종 구간 병합: 일치를 구간 끝으로 이동. 비용 동일
bool mergeSplitGaps(const Buffer& sent, const Buffer& got, std::vector<DiffStep>& steps)
{
    bool merged = false;
    for (std::size_t k = 1; k + 1 < steps.size(); ++k) {
        const DiffOp kind = steps[k + 1].op;
        if (steps[k].op != DiffOp::Same || steps[k - 1].op != kind || (kind != DiffOp::Missing && kind != DiffOp::Extra))
            continue;
        std::size_t end = k + 1;
        while (end + 1 < steps.size() && steps[end + 1].op == kind) ++end;
        const DiffStep same = steps[k];
        if (kind == DiffOp::Missing && sent[steps[end].sentAt] == got[same.gotAt]) {
            for (std::size_t i = k; i < end; ++i)
                steps[i] = {DiffOp::Missing, static_cast<std::uint16_t>(same.sentAt + (i - k)), same.gotAt};
            steps[end] = {DiffOp::Same, static_cast<std::uint16_t>(same.sentAt + (end - k)), same.gotAt};
            merged = true;
        } else if (kind == DiffOp::Extra && got[steps[end].gotAt] == sent[same.sentAt]) {
            for (std::size_t i = k; i < end; ++i)
                steps[i] = {DiffOp::Extra, same.sentAt, static_cast<std::uint16_t>(same.gotAt + (i - k))};
            steps[end] = {DiffOp::Same, same.sentAt, static_cast<std::uint16_t>(same.gotAt + (end - k))};
            merged = true;
        }
    }
    return merged;
}

// 반복 바이트 구간의 누락·추가 구간을 통째로 뒤쪽 이동(구간 분리 없음). 비용 동일
bool rotateGapsBack(const Buffer& sent, const Buffer& got, std::vector<DiffStep>& steps)
{
    bool rotated = false;
    for (std::size_t k = 0; k < steps.size();) {
        const DiffOp kind = steps[k].op;
        if (kind != DiffOp::Missing && kind != DiffOp::Extra) {
            ++k;
            continue;
        }
        std::size_t end = k;
        while (end + 1 < steps.size() && steps[end + 1].op == kind) ++end;
        if (end + 1 < steps.size() && steps[end + 1].op == DiffOp::Same) {
            const DiffStep same = steps[end + 1];
            if (kind == DiffOp::Missing && sent[steps[k].sentAt] == sent[same.sentAt]) {
                const std::uint16_t first = steps[k].sentAt;
                steps[k] = {DiffOp::Same, first, same.gotAt};
                for (std::size_t i = k + 1; i <= end + 1; ++i)
                    steps[i] = {DiffOp::Missing, static_cast<std::uint16_t>(first + (i - k)),
                                static_cast<std::uint16_t>(same.gotAt + 1)};
                rotated = true;
                ++k;
                continue;
            }
            if (kind == DiffOp::Extra && got[steps[k].gotAt] == got[same.gotAt]) {
                const std::uint16_t first = steps[k].gotAt;
                steps[k] = {DiffOp::Same, same.sentAt, first};
                for (std::size_t i = k + 1; i <= end + 1; ++i)
                    steps[i] = {DiffOp::Extra, static_cast<std::uint16_t>(same.sentAt + 1),
                                static_cast<std::uint16_t>(first + (i - k))};
                rotated = true;
                ++k;
                continue;
            }
        }
        k = end + 1;
    }
    return rotated;
}

// 누락·추가 표기 정규화: 분리 구간 병합, 반복 바이트 구간 뒤쪽 이동. 구간 수 감소·위치 증가만 발생하여 종료 보장
void shiftGapsBack(const Buffer& sent, const Buffer& got, std::vector<DiffStep>& steps)
{
    for (;;) {
        const bool merged = mergeSplitGaps(sent, got, steps);
        const bool rotated = rotateGapsBack(sent, got, steps);
        if (!merged && !rotated) break;
    }
}

}  // namespace

std::size_t wireAffinity(const Buffer& sent, const Buffer& got)
{
    const std::size_t common = (std::min)(sent.size(), got.size());
    std::size_t score = 0;
    for (std::size_t i = 0; i < common; ++i) {
        score += sent[i] == got[i] ? 1u : 0u;
        score += sent[sent.size() - 1 - i] == got[got.size() - 1 - i] ? 1u : 0u;
    }
    return score;
}

WireDiff alignWire(const Buffer& sent, const Buffer& got)
{
    // 공통 선두 제외 후 나머지 구간 정렬. 최적성 유지, 선두 일치분 정렬 비용 절감
    // 공통 말미는 미제외: 동일 비용 시 공백 뒤쪽 배치 규칙(역추적 우선순위) 유지
    const std::size_t n = sent.size();
    const std::size_t m = got.size();
    std::size_t head = 0;
    while (head < n && head < m && sent[head] == got[head]) ++head;
    const std::size_t a = n - head;
    const std::size_t b = m - head;

    WireDiff diff;
    diff.steps.reserve(n + m);
    for (std::size_t k = 0; k < head; ++k) push(diff.steps, DiffOp::Same, k, k);
    // 띠 범위: 시작(대각 0)·끝(대각 b - a) 포함
    const std::ptrdiff_t shift = static_cast<std::ptrdiff_t>(b) - static_cast<std::ptrdiff_t>(a);
    const std::ptrdiff_t lowEnd = (std::min)(std::ptrdiff_t{0}, shift);
    const std::ptrdiff_t highEnd = (std::max)(std::ptrdiff_t{0}, shift);
    const auto cells = [a, b](std::ptrdiff_t low, std::ptrdiff_t high) {
        return (a + 1) * (std::min)(static_cast<std::size_t>(high - low + 1), b + 1);
    };
    if (!a || !b) {
        for (std::size_t k = 0; k < a; ++k) push(diff.steps, DiffOp::Missing, head + k, head);
        for (std::size_t k = 0; k < b; ++k) push(diff.steps, DiffOp::Extra, head + a, head + k);
    } else if ((a + 1) * (b + 1) <= kFullCells) {
        alignBand(sent, head, a, got, head, b, -static_cast<std::ptrdiff_t>(a), static_cast<std::ptrdiff_t>(b),
                  diff.steps);
    } else if (cells(lowEnd, highEnd) <= kBandCells) {
        // 띠 밖 경로 비용 하한: |끝 대각| + 2 × 여유 + 2. 띠 내 최소 비용이 하한 미만이면 전체 최소 확정
        // 미확정 시 여유 2배 확장 반복(셀 상한 내). 상한 도달 시 마지막 띠 결과 사용
        const std::ptrdiff_t fullLow = -static_cast<std::ptrdiff_t>(a);
        const auto fullHigh = static_cast<std::ptrdiff_t>(b);
        std::vector<DiffStep> middle;
        for (std::ptrdiff_t slack = kBandSlack;; slack *= 2) {
            const std::ptrdiff_t low = (std::max)(lowEnd - slack, fullLow);
            const std::ptrdiff_t high = (std::min)(highEnd + slack, fullHigh);
            if (cells(low, high) > kBandCells) {
                if (middle.empty()) alignBand(sent, head, a, got, head, b, lowEnd, highEnd, middle);
                break;
            }
            middle.clear();
            const unsigned cost = alignBand(sent, head, a, got, head, b, low, high, middle);
            const auto bound = static_cast<unsigned>((highEnd - lowEnd) + 2 * slack + 2);
            if (cost < bound || (low == fullLow && high == fullHigh)) break;
        }
        diff.steps.insert(diff.steps.end(), middle.begin(), middle.end());
    } else {
        // 대량 불일치(잡음 등): 위치 대응 후 나머지 누락·추가
        const std::size_t common = (std::min)(a, b);
        for (std::size_t k = 0; k < common; ++k)
            push(diff.steps, sent[head + k] == got[head + k] ? DiffOp::Same : DiffOp::Changed, head + k, head + k);
        for (std::size_t k = common; k < a; ++k) push(diff.steps, DiffOp::Missing, head + k, head + common);
        for (std::size_t k = common; k < b; ++k) push(diff.steps, DiffOp::Extra, head + a, head + k);
    }
    shiftGapsBack(sent, got, diff.steps);

    // 연속 일치 구간 단위 집계. 짧은 구간의 우연 일치는 대응 근거 제외
    const Layout layout = layoutOf(sent);
    std::size_t run = 0;
    std::size_t runPayload = 0;
    const auto closeRun = [&diff, &run, &runPayload] {
        if (run >= kAnchorRun) diff.anchored += runPayload;
        run = 0;
        runPayload = 0;
    };
    for (const DiffStep& step : diff.steps) {
        if (step.op == DiffOp::Same) {
            const Place& place = layout.places[step.sentAt];
            ++run;
            if (place.area == Area::Body && !place.prefix && place.body >= DATA_OFFSET &&
                place.body < trailerOffset(layout.dataSize))
                ++runPayload;
            continue;
        }
        closeRun();
        if (step.op == DiffOp::Changed) ++diff.changed;
        else if (step.op == DiffOp::Missing) ++diff.missing;
        else ++diff.extra;
    }
    closeRun();
    return diff;
}

std::vector<std::wstring> diffItems(const Buffer& sent, const Buffer& got, const WireDiff& diff)
{
    const Layout layout = layoutOf(sent);
    const auto name = [&layout](std::size_t at) { return areaName(layout.places[at], layout.dataSize); };
    const auto& steps = diff.steps;
    std::vector<std::wstring> items;
    for (std::size_t k = 0; k < steps.size();) {
        const DiffStep& step = steps[k];
        std::size_t end = k + 1;
        if (step.op == DiffOp::Missing || step.op == DiffOp::Extra)
            while (end < steps.size() && steps[end].op == step.op) ++end;
        const std::size_t run = end - k;
        std::wstring item;
        switch (step.op) {
        case DiffOp::Same:
            break;
        case DiffOp::Changed: {
            const Byte was = sent[step.sentAt];
            const Byte now = got[step.gotAt];
            const auto flip = static_cast<Byte>(was ^ now);
            item = offsetText(step.sentAt) + L" " + name(step.sentAt) + L" ";
            appendHex(item, was);
            item += L"→";
            appendHex(item, now);
            item += L" (XOR ";
            appendHex(item, flip);
            item += L", " + std::to_wstring(bitCount(flip)) + L"비트)";
            break;
        }
        case DiffOp::Missing: {
            const std::size_t last = steps[end - 1].sentAt;
            if (run == 1) {
                item = offsetText(step.sentAt) + L" " + name(step.sentAt) + L" ";
                appendHex(item, sent[step.sentAt]);
                item += L" 누락";
            } else {
                item = L"[" + std::to_wstring(step.sentAt) + L"~" + std::to_wstring(last) + L"] " + name(step.sentAt) +
                       L"~" + name(last) + L" " + grouped(run) + L" Byte 누락";
            }
            break;
        }
        case DiffOp::Extra: {
            item = step.sentAt < sent.size() ? offsetText(step.sentAt) + L" " + name(step.sentAt) + L" 앞 "
                                             : std::wstring(L"ETX 뒤 ");
            if (run > 1) item += grouped(run) + L" Byte 추가: ";
            for (std::size_t e = 0; e < (std::min)(run, kExtraShown); ++e) {
                if (e) item.push_back(L' ');
                appendHex(item, got[steps[k + e].gotAt]);
            }
            if (run > kExtraShown) item += L" …";
            if (run == 1) item += L" 추가";
            break;
        }
        }
        if (!item.empty()) items.push_back(std::move(item));
        k = end;
    }
    return items;
}

std::wstring diffTable(const Buffer& sent, const Buffer& got, const WireDiff& diff)
{
    const auto& steps = diff.steps;
    const std::size_t rows = (steps.size() + kRowBytes - 1) / kRowBytes;
    const int width = (std::max)(sent.size(), got.size()) > 1000 ? 4 : 3;
    // 전 바이트 일치 행 판별. 차이 행·첫 행·끝 행과 인접 행은 표시, 그 외 연속 2행 이상은 생략 1행 대체
    std::vector<bool> plain(rows, true);
    for (std::size_t k = 0; k < steps.size(); ++k)
        if (steps[k].op != DiffOp::Same) plain[k / kRowBytes] = false;
    const auto shown = [&plain, rows](std::size_t row) {
        return row == 0 || row + 1 == rows || !plain[row] || !plain[row - 1] || !plain[row + 1];
    };
    std::wstring table;
    for (std::size_t row = 0; row < rows;) {
        std::size_t hidden = row;
        while (hidden < rows && !shown(hidden)) ++hidden;
        if (hidden - row >= 2) {
            const std::size_t first = steps[row * kRowBytes].sentAt;
            const std::size_t last = steps[hidden * kRowBytes - 1].sentAt;
            if (!table.empty()) table += L"\r\n";
            table += L"… 일치 구간 생략: 송신 " + std::to_wstring(first) + L"~" + std::to_wstring(last) + L" (" +
                     grouped(last - first + 1) + L" Byte)";
            row = hidden;
            continue;
        }
        std::wstring upper = rowHead(L"송신", steps[row * kRowBytes].sentAt, width);
        std::wstring lower = rowHead(L"수신", steps[row * kRowBytes].gotAt, width);
        for (std::size_t k = row * kRowBytes; k < (std::min)(steps.size(), (row + 1) * kRowBytes); ++k) {
            const DiffStep& step = steps[k];
            upper.push_back(L' ');
            lower.push_back(L' ');
            if (step.op == DiffOp::Extra) upper += L"--";
            else appendHex(upper, sent[step.sentAt]);
            if (step.op == DiffOp::Missing) lower += L"--";
            else if (step.op == DiffOp::Same) lower += L"..";
            else appendHex(lower, got[step.gotAt]);
        }
        if (!table.empty()) table += L"\r\n";
        table += upper + L"\r\n" + lower;
        ++row;
    }
    return table;
}

std::wstring byteRows(const Byte* bytes, std::size_t size, const wchar_t* label)
{
    const int width = size > 1000 ? 4 : 3;
    std::wstring rows;
    for (std::size_t row = 0; row < size; row += kRowBytes) {
        if (!rows.empty()) rows += L"\r\n";
        rows += rowHead(label, row, width);
        for (std::size_t k = row; k < (std::min)(size, row + kRowBytes); ++k) {
            rows.push_back(L' ');
            appendHex(rows, bytes[k]);
        }
    }
    return rows;
}

}  // namespace loopback
