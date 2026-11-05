#include "frame_diff.h"

#include <algorithm>
#include <cwchar>
#include <iterator>

namespace loopback {

namespace {

constexpr std::size_t kRowBytes = 16;
constexpr std::size_t kExtraShown = 8;  // 추가 바이트 항목 내 값 표기 상한
constexpr std::size_t kAnchorRun = 3;   // 대응 판별 연속 일치 구간 최소 길이

enum class Area : std::uint8_t { Stx, Body, Etx };

// 송신 회선 오프셋별 전문 영역
struct Place {
    Area area = Area::Body;
    std::size_t body = 0;  // 본문 순번(DLE 복원 기준)
    bool prefix = false;   // DLE 치환 접두 바이트
};

std::vector<Place> layoutOf(const Buffer& sent)
{
    std::vector<Place> places(sent.size());
    if (sent.empty()) return places;
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
    return places;
}

std::wstring areaName(const Place& place)
{
    if (place.area == Area::Stx) return L"STX";
    if (place.area == Area::Etx) return L"ETX";
    const auto at = static_cast<unsigned>(place.body);
    wchar_t text[24]{};
    if (place.body < DATA_OFFSET)
        std::swprintf(text, std::size(text), L"C%u", at);
    else if (place.body < CRC_OFFSET)
        std::swprintf(text, std::size(text), L"DATA[%u]", at - static_cast<unsigned>(DATA_OFFSET));
    else if (place.body < TRAILER_OFFSET)
        std::swprintf(text, std::size(text), L"CRC[%u]", at - static_cast<unsigned>(CRC_OFFSET));
    else if (place.body < PAYLOAD_SIZE)
        std::swprintf(text, std::size(text), L"트레일러[%u]", at - static_cast<unsigned>(TRAILER_OFFSET));
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

// 행 머리: "송신 016 "
std::wstring rowHead(const wchar_t* label, std::size_t at)
{
    wchar_t digits[8]{};
    std::swprintf(digits, std::size(digits), L"%03u", static_cast<unsigned>(at));
    return std::wstring(label) + L" " + digits + L" ";
}

unsigned bitCount(unsigned value)
{
    unsigned count = 0;
    for (; value; value &= value - 1) ++count;
    return count;
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
    const std::size_t columns = got.size() + 1;
    std::vector<std::uint16_t> cost((sent.size() + 1) * columns);
    const auto at = [columns](std::size_t i, std::size_t j) { return i * columns + j; };
    const auto value = [&cost](std::size_t index) { return static_cast<unsigned>(cost[index]); };
    for (std::size_t j = 0; j < columns; ++j) cost[at(0, j)] = static_cast<std::uint16_t>(j);
    for (std::size_t i = 1; i <= sent.size(); ++i) {
        cost[at(i, 0)] = static_cast<std::uint16_t>(i);
        for (std::size_t j = 1; j < columns; ++j) {
            const unsigned changed = value(at(i - 1, j - 1)) + (sent[i - 1] == got[j - 1] ? 0u : 1u);
            const unsigned missing = value(at(i - 1, j)) + 1u;
            const unsigned extra = value(at(i, j - 1)) + 1u;
            cost[at(i, j)] = static_cast<std::uint16_t>((std::min)({changed, missing, extra}));
        }
    }

    // 말미부터 역추적. 동일 비용 시 누락·추가 우선 선택으로 공백을 뒤쪽 배치(절단·말미 추가가 연속 1항목)
    WireDiff diff;
    diff.steps.reserve(sent.size() + got.size());
    std::size_t i = sent.size();
    std::size_t j = got.size();
    while (i || j) {
        const unsigned here = value(at(i, j));
        if (i && here == value(at(i - 1, j)) + 1u) {
            --i;
            diff.steps.push_back({DiffOp::Missing, static_cast<std::uint16_t>(i), static_cast<std::uint16_t>(j)});
        } else if (j && here == value(at(i, j - 1)) + 1u) {
            --j;
            diff.steps.push_back({DiffOp::Extra, static_cast<std::uint16_t>(i), static_cast<std::uint16_t>(j)});
        } else {
            --i;
            --j;
            diff.steps.push_back({sent[i] == got[j] ? DiffOp::Same : DiffOp::Changed, static_cast<std::uint16_t>(i),
                                  static_cast<std::uint16_t>(j)});
        }
    }
    std::reverse(diff.steps.begin(), diff.steps.end());

    // 연속 일치 구간 단위 집계. 짧은 구간의 우연 일치는 대응 근거 제외
    const std::vector<Place> places = layoutOf(sent);
    std::size_t run = 0;
    std::size_t runPayload = 0;
    const auto closeRun = [&diff, &run, &runPayload] {
        if (run >= kAnchorRun) diff.anchored += runPayload;
        run = 0;
        runPayload = 0;
    };
    for (const DiffStep& step : diff.steps) {
        if (step.op == DiffOp::Same) {
            const Place& place = places[step.sentAt];
            ++run;
            if (place.area == Area::Body && !place.prefix && place.body >= DATA_OFFSET && place.body < TRAILER_OFFSET)
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
    const std::vector<Place> places = layoutOf(sent);
    const auto name = [&places](std::size_t at) { return areaName(places[at]); };
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
                       L"~" + name(last) + L" " + std::to_wstring(run) + L" Byte 누락";
            }
            break;
        }
        case DiffOp::Extra: {
            item = step.sentAt < sent.size() ? offsetText(step.sentAt) + L" " + name(step.sentAt) + L" 앞 "
                                             : std::wstring(L"ETX 뒤 ");
            if (run > 1) item += std::to_wstring(run) + L" Byte 추가: ";
            for (std::size_t m = 0; m < (std::min)(run, kExtraShown); ++m) {
                if (m) item.push_back(L' ');
                appendHex(item, got[steps[k + m].gotAt]);
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
    std::wstring table;
    for (std::size_t row = 0; row < steps.size(); row += kRowBytes) {
        std::wstring upper = rowHead(L"송신", steps[row].sentAt);
        std::wstring lower = rowHead(L"수신", steps[row].gotAt);
        for (std::size_t k = row; k < (std::min)(steps.size(), row + kRowBytes); ++k) {
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
    }
    return table;
}

std::wstring byteRows(const Buffer& bytes, const wchar_t* label)
{
    std::wstring rows;
    for (std::size_t row = 0; row < bytes.size(); row += kRowBytes) {
        if (!rows.empty()) rows += L"\r\n";
        rows += rowHead(label, row);
        for (std::size_t k = row; k < (std::min)(bytes.size(), row + kRowBytes); ++k) {
            rows.push_back(L' ');
            appendHex(rows, bytes[k]);
        }
    }
    return rows;
}

}  // namespace loopback
