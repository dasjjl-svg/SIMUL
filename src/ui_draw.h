#pragma once

#include <windows.h>

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace loopback::ui {

namespace palette {
constexpr COLORREF Surface      = RGB(255, 255, 255);
constexpr COLORREF Muted        = RGB(241, 245, 249);
constexpr COLORREF Border       = RGB(226, 232, 240);
constexpr COLORREF BorderStrong = RGB(203, 213, 225);
constexpr COLORREF TextStrong   = RGB( 15,  23,  42);
constexpr COLORREF Text         = RGB( 51,  65,  85);
constexpr COLORREF TextMuted    = RGB(100, 116, 139);
constexpr COLORREF TextFaint    = RGB(148, 163, 184);
constexpr COLORREF Header       = RGB( 15,  23,  42);
constexpr COLORREF Accent       = RGB( 37,  99, 235);
constexpr COLORREF AccentDark   = RGB( 29,  78, 216);
constexpr COLORREF Idle         = RGB( 71,  85, 105);
constexpr COLORREF Running      = RGB( 37,  99, 235);
constexpr COLORREF Pass         = RGB( 21, 128,  61);  // 백색 문자 대비 5:1 이상
constexpr COLORREF Fault        = RGB(220,  38,  38);
constexpr COLORREF Halted       = RGB(180,  83,   9);  // 백색 문자 대비 5:1 이상
constexpr COLORREF CellPass     = RGB( 34, 197,  94);
constexpr COLORREF CellFault    = RGB(239,  68,  68);
constexpr COLORREF CellEmpty    = RGB(203, 213, 225);
constexpr COLORREF White        = RGB(255, 255, 255);
}  // namespace palette

class FontCache {
public:
    FontCache() = default;
    ~FontCache();
    FontCache(const FontCache&) = delete;
    FontCache& operator=(const FontCache&) = delete;
    HFONT get(int pixelHeight, int weight = FW_NORMAL);

private:
    std::map<std::pair<int, int>, HFONT> fonts_;
};

struct VerdictView {
    COLORREF color = palette::Idle;
    std::wstring state;
    std::wstring elapsed;
    double progress = 0.0;
    std::wstring detail;
    std::wstring percent;
};

// 창의 현재 DPI. 미지원 환경은 화면 DC 기준
int dpiFor(HWND window);
// 대화상자 기본 글꼴 9pt의 현재 DPI 기준 픽셀 높이
int basePixels(HWND window);

// mark: 제목 앞 로고(markSize 정사각). nullptr이면 생략
void paintHeader(HDC dc, const RECT& rc, FontCache& fonts, int base, HICON mark, int markSize,
                 const std::wstring& title, const std::wstring& subtitle, const std::wstring& note);
void paintVerdict(HDC dc, const RECT& rc, FontCache& fonts, int base, const VerdictView& view);
// 목록 셀 내부 표시. 이중 버퍼 미사용, 호출측 DC에 직접 출력
// slots: 시험 시간 균등 분할 구간별 판정(0 미도래, 1 정상, 2 이상). 셀 폭에 맞춰 구간 병합, 이상 우선
// 이상은 전체 높이·정상은 절반 높이 막대로 색상 외 높이로도 구분
void paintTimeline(HDC dc, const RECT& cell, int base, const std::vector<std::uint8_t>& slots);
void paintPill(HDC dc, const RECT& cell, FontCache& fonts, int base, COLORREF face, COLORREF ink,
               const std::wstring& text);
void paintButton(const DRAWITEMSTRUCT& item, FontCache& fonts, int base, bool primary);

}  // namespace loopback::ui
