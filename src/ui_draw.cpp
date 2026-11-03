#include "ui_draw.h"

#include <algorithm>

namespace loopback::ui {

namespace {

constexpr const wchar_t* kFace = L"Malgun Gothic";

COLORREF mix(COLORREF from, COLORREF to, double amount)
{
    const auto channel = [amount](int a, int b) {
        return static_cast<BYTE>(a + static_cast<int>((b - a) * amount + (b >= a ? 0.5 : -0.5)));
    };
    return RGB(channel(GetRValue(from), GetRValue(to)), channel(GetGValue(from), GetGValue(to)),
               channel(GetBValue(from), GetBValue(to)));
}

void fill(HDC dc, const RECT& rc, COLORREF color)
{
    SetDCBrushColor(dc, color);
    FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void rounded(HDC dc, const RECT& rc, int radius, COLORREF face, COLORREF edge)
{
    const HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(DC_BRUSH));
    const HGDIOBJ oldPen = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, face);
    SetDCPenColor(dc, edge);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius * 2, radius * 2);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
}

void ring(HDC dc, const RECT& rc, int radius, COLORREF edge)
{
    const HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    const HGDIOBJ oldPen = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCPenColor(dc, edge);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius * 2, radius * 2);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
}

void label(HDC dc, RECT rc, const std::wstring& text, HFONT font, COLORREF color, UINT format, int spacing = 0)
{
    if (text.empty()) return;
    const HGDIOBJ oldFont = SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    const int oldSpacing = SetTextCharacterExtra(dc, spacing);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rc, format | DT_NOPREFIX);
    SetTextCharacterExtra(dc, oldSpacing);
    SelectObject(dc, oldFont);
}

int textWidth(HDC dc, HFONT font, const std::wstring& text)
{
    const HGDIOBJ oldFont = SelectObject(dc, font);
    SIZE size{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    SelectObject(dc, oldFont);
    return size.cx;
}

// 깜빡임 방지용 이중 버퍼. 생성 실패 시 대상 DC 직접 사용
class Buffered {
public:
    Buffered(HDC target, const RECT& rc)
        : target_(target), rc_(rc), width_(rc.right - rc.left), height_(rc.bottom - rc.top)
    {
        if (width_ <= 0 || height_ <= 0) return;
        memory_ = CreateCompatibleDC(target);
        bitmap_ = memory_ ? CreateCompatibleBitmap(target, width_, height_) : nullptr;
        if (!bitmap_) {
            if (memory_) DeleteDC(memory_);
            memory_ = nullptr;
            return;
        }
        old_ = SelectObject(memory_, bitmap_);
        SetViewportOrgEx(memory_, -rc.left, -rc.top, nullptr);
    }

    ~Buffered()
    {
        if (!memory_) return;
        SetViewportOrgEx(memory_, 0, 0, nullptr);
        BitBlt(target_, rc_.left, rc_.top, width_, height_, memory_, 0, 0, SRCCOPY);
        SelectObject(memory_, old_);
        DeleteObject(bitmap_);
        DeleteDC(memory_);
    }

    Buffered(const Buffered&) = delete;
    Buffered& operator=(const Buffered&) = delete;

    HDC dc() const { return memory_ ? memory_ : target_; }

private:
    HDC target_;
    RECT rc_;
    int width_;
    int height_;
    HDC memory_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ old_ = nullptr;
};

}  // namespace

FontCache::~FontCache()
{
    for (auto& entry : fonts_)
        if (entry.second) DeleteObject(entry.second);
}

HFONT FontCache::get(int pixelHeight, int weight)
{
    pixelHeight = (std::max)(pixelHeight, 6);
    const auto key = std::make_pair(pixelHeight, weight);
    const auto found = fonts_.find(key);
    if (found != fonts_.end()) return found->second;
    LOGFONTW lf{};
    lf.lfHeight = -pixelHeight;
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfOutPrecision = OUT_TT_PRECIS;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lstrcpynW(lf.lfFaceName, kFace, LF_FACESIZE);
    const HFONT font = CreateFontIndirectW(&lf);
    fonts_.emplace(key, font);
    return font;
}

int basePixels(HWND window)
{
    using DpiForWindow = UINT(WINAPI*)(HWND);
    static const auto dpiForWindow = reinterpret_cast<DpiForWindow>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
    UINT dpi = dpiForWindow ? dpiForWindow(window) : 0;
    if (!dpi) {
        const HDC dc = GetDC(window);
        dpi = static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSY));
        ReleaseDC(window, dc);
    }
    return MulDiv(9, static_cast<int>(dpi ? dpi : 96), 72);
}

void paintHeader(HDC target, const RECT& rc, FontCache& fonts, int base, const std::wstring& title,
                 const std::wstring& subtitle, const std::wstring& note)
{
    Buffered buffer(target, rc);
    const HDC dc = buffer.dc();
    fill(dc, rc, palette::Header);
    RECT accent = rc;
    accent.top = rc.bottom - (std::max)(2, base / 6);
    fill(dc, accent, palette::Accent);

    const int pad = base + base / 3;
    RECT area{rc.left + pad, rc.top, rc.right - pad, accent.top};
    const HFONT titleFont = fonts.get(base * 8 / 5, FW_BOLD);
    label(dc, area, title, titleFont, palette::White, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT sub = area;
    sub.left += textWidth(dc, titleFont, title) + base * 3 / 4;
    label(dc, sub, subtitle, fonts.get(base), palette::TextFaint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    label(dc, area, note, fonts.get(base * 11 / 12), palette::TextMuted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

void paintVerdict(HDC target, const RECT& rc, FontCache& fonts, int base, const VerdictView& view)
{
    Buffered buffer(target, rc);
    const HDC dc = buffer.dc();
    fill(dc, rc, palette::Surface);
    rounded(dc, rc, base * 2 / 3, view.color, view.color);

    const COLORREF soft = mix(view.color, palette::White, 0.72);
    const COLORREF track = mix(view.color, palette::White, 0.30);
    const int pad = base + base / 3;
    const int captionH = base * 5 / 4;
    const int stateH = base * 18 / 5;
    const int barGap = base / 2;
    const int barH = (std::max)(4, base * 5 / 12);
    const int detailGap = base * 3 / 5;
    const int detailH = base * 7 / 5;
    const int contentH = captionH + stateH + barGap + barH + detailGap + detailH;
    const int top = rc.top + (std::max)(pad * 3 / 4, (static_cast<int>(rc.bottom - rc.top) - contentH) / 2);
    const int left = rc.left + pad;
    const int right = rc.right - pad;

    const HFONT small = fonts.get(base * 11 / 12);
    RECT caption{left, top, right, top + captionH};
    label(dc, caption, L"판정", small, soft, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    label(dc, caption, L"경과 / 시험 시간", small, soft, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    RECT state{left, caption.bottom, right, caption.bottom + stateH};
    label(dc, state, view.state, fonts.get(base * 16 / 5, FW_BOLD), palette::White,
          DT_LEFT | DT_VCENTER | DT_SINGLELINE, base / 6);
    label(dc, state, view.elapsed, fonts.get(base * 3 / 2, FW_SEMIBOLD), palette::White,
          DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    RECT bar{left, state.bottom + barGap, right, state.bottom + barGap + barH};
    rounded(dc, bar, barH / 2, track, track);
    const double progress = (std::min)(1.0, (std::max)(0.0, view.progress));
    RECT done = bar;
    done.right = bar.left + static_cast<LONG>((bar.right - bar.left) * progress);
    if (done.right - done.left >= barH) rounded(dc, done, barH / 2, palette::White, palette::White);
    else if (done.right > done.left) fill(dc, done, palette::White);

    const HFONT body = fonts.get(base);
    const HFONT strong = fonts.get(base, FW_SEMIBOLD);
    RECT detail{left, bar.bottom + detailGap, right, bar.bottom + detailGap + detailH};
    RECT text = detail;
    text.right -= textWidth(dc, strong, view.percent) + base;
    label(dc, text, view.detail, body, mix(view.color, palette::White, 0.9),
          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    label(dc, detail, view.percent, strong, palette::White, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

void paintSparkline(HDC dc, const RECT& cell, int base, const std::vector<bool>& outcomes)
{
    if (outcomes.empty()) return;
    const int bar = (std::max)(2, base / 5);
    const int gap = 1;
    const int padX = base / 3;
    const int height = (std::max)(4, static_cast<int>(cell.bottom - cell.top) * 5 / 9);
    const LONG top = cell.top + (cell.bottom - cell.top - height) / 2;
    const int width = static_cast<int>(cell.right - cell.left) - padX * 2;
    const auto slots = static_cast<std::size_t>((std::max)(0, (width + gap) / (bar + gap)));
    const std::size_t shown = (std::min)(slots, outcomes.size());
    LONG right = cell.right - padX;
    for (std::size_t i = 0; i < shown; ++i) {
        const bool ok = outcomes[outcomes.size() - 1 - i];
        fill(dc, RECT{right - bar, top, right, top + height}, ok ? palette::CellPass : palette::CellFault);
        right -= bar + gap;
    }
}

void paintPill(HDC dc, const RECT& cell, FontCache& fonts, int base, COLORREF face, COLORREF ink,
               const std::wstring& text)
{
    const HFONT font = fonts.get(base * 11 / 12, FW_SEMIBOLD);
    const int height = (std::min)(static_cast<int>(cell.bottom - cell.top) - 4, base + base / 2);
    const int width = (std::min)(static_cast<int>(cell.right - cell.left) - 6, textWidth(dc, font, text) + base * 3 / 2);
    if (height <= 0 || width <= 0) return;
    const LONG left = cell.left + (cell.right - cell.left - width) / 2;
    const LONG top = cell.top + (cell.bottom - cell.top - height) / 2;
    const RECT pill{left, top, left + width, top + height};
    rounded(dc, pill, height / 2, face, face);
    label(dc, pill, text, font, ink, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void paintButton(const DRAWITEMSTRUCT& item, FontCache& fonts, int base, bool primary)
{
    const RECT& rc = item.rcItem;
    Buffered buffer(item.hDC, rc);
    const HDC dc = buffer.dc();
    fill(dc, rc, palette::Surface);

    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool focused = (item.itemState & ODS_FOCUS) != 0 && (item.itemState & ODS_NOFOCUSRECT) == 0;

    COLORREF face;
    COLORREF edge;
    COLORREF ink;
    if (primary) {
        face = disabled ? palette::BorderStrong : pressed ? palette::AccentDark : palette::Accent;
        edge = face;
        ink = palette::White;
    } else {
        face = !disabled && pressed ? palette::Muted : palette::Surface;
        edge = disabled ? palette::Border : palette::BorderStrong;
        ink = disabled ? palette::TextFaint : palette::TextStrong;
    }
    const int radius = (std::max)(3, base / 3);
    rounded(dc, rc, radius, face, edge);
    if (focused && !disabled) {
        RECT inner = rc;
        InflateRect(&inner, -2, -2);
        ring(dc, inner, (std::max)(2, radius - 2), primary ? mix(palette::Accent, palette::White, 0.6) : palette::Accent);
    }

    wchar_t caption[128]{};
    GetWindowTextW(item.hwndItem, caption, static_cast<int>(sizeof(caption) / sizeof(caption[0])));
    label(dc, rc, caption, fonts.get(base, FW_SEMIBOLD), ink, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

}  // namespace loopback::ui
