#include "app.h"

#include <commctrl.h>
#include <windowsx.h>

#include <chrono>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

#include "resource.h"
#include "vspe_launcher.h"

namespace simul {

namespace {

constexpr UINT_PTR kStatusTimer = 1;
constexpr UINT     kStatusIntervalMs = 100;

constexpr const wchar_t* kTransportNames[] = {
    L"직렬 (Com0Com / VSPE)",
    L"TCP",
    L"UDP",
};

// 배너 상태 색상
constexpr COLORREF kIdleBg   = RGB( 55,  65,  81);   // slate-700
constexpr COLORREF kRunBg    = RGB(217, 119,   6);   // amber-600
constexpr COLORREF kOkBg     = RGB( 22, 163,  74);   // green-600
constexpr COLORREF kFailBg   = RGB(220,  38,  38);   // red-600

void setText(HWND dialog, int id, const std::wstring& text)
{
    SetDlgItemTextW(dialog, id, text.c_str());
}

std::wstring getText(HWND dialog, int id)
{
    std::vector<wchar_t> buffer(1024);
    const UINT length = GetDlgItemTextW(dialog, id, buffer.data(),
                                        static_cast<int>(buffer.size()));
    return std::wstring(buffer.data(), length);
}

std::wstring trimmed(std::wstring text)
{
    const auto notSpace = [](wchar_t c) { return c != L' ' && c != L'\t'; };
    while (!text.empty() && !notSpace(text.back())) text.pop_back();
    std::size_t start = 0;
    while (start < text.size() && !notSpace(text[start])) ++start;
    return text.substr(start);
}

bool parseUnsigned(const std::wstring& text, unsigned long& out, unsigned long limit)
{
    if (text.empty()) return false;
    out = 0;
    for (wchar_t c : text) {
        if (c < L'0' || c > L'9') return false;
        out = out * 10 + static_cast<unsigned long>(c - L'0');
        if (out > limit) return false;
    }
    return true;
}

// IPv4 문자열 변환 (ASCII 범위 보장)
std::string wideToNarrow(const std::wstring& s)
{
    if (s.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1,
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string narrow(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, narrow.data(), size, nullptr, nullptr);
    return narrow;
}

std::wstring formatDuration(std::chrono::milliseconds elapsed)
{
    auto total = elapsed.count() / 1000;
    const long long seconds = total % 60;
    total /= 60;
    const long long minutes = total % 60;
    total /= 60;
    const long long hours = total;
    wchar_t buffer[64]{};
    std::swprintf(buffer, std::size(buffer), L"%lld:%02lld:%02lld",
                  hours, minutes, seconds);
    return buffer;
}

std::wstring verdictBanner(Verdict verdict, bool running)
{
    if (running) return L"시 험 중";
    switch (verdict) {
    case Verdict::Ok:   return L"O  K";
    case Verdict::Fail: return L"F A I L";
    default:            return L"대 기 중";
    }
}

HFONT makeBannerFont()
{
    LOGFONTW lf{};
    lf.lfHeight  = -28;
    lf.lfWeight  = FW_SEMIBOLD;
    lf.lfCharSet = HANGUL_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcsncpy_s(lf.lfFaceName, L"Segoe UI Semibold", _TRUNCATE);
    return CreateFontIndirectW(&lf);
}

}  // namespace

App::App() = default;

App::~App()
{
    if (test_) test_->stop();
    if (bannerFont_) DeleteObject(bannerFont_);
    if (idleBrush_)  DeleteObject(idleBrush_);
    if (runBrush_)   DeleteObject(runBrush_);
    if (okBrush_)    DeleteObject(okBrush_);
    if (failBrush_)  DeleteObject(failBrush_);
}

int App::run(HINSTANCE instance, int /*showCommand*/)
{
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    return static_cast<int>(DialogBoxParamW(instance,
        MAKEINTRESOURCEW(IDD_MAIN), nullptr, dialogProc,
        reinterpret_cast<LPARAM>(this)));
}

INT_PTR CALLBACK App::dialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    App* self = nullptr;
    if (message == WM_INITDIALOG) {
        self = reinterpret_cast<App*>(lParam);
        SetWindowLongPtrW(dialog, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->dialog_ = dialog;
    } else {
        self = reinterpret_cast<App*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
    }
    return self ? self->handle(dialog, message, wParam, lParam) : FALSE;
}

INT_PTR App::handle(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_INITDIALOG:
        onInit(dialog);
        return TRUE;

    case WM_COMMAND:
        onCommand(dialog, LOWORD(wParam), HIWORD(wParam));
        return TRUE;

    case WM_TIMER:
        onTimer(dialog, static_cast<UINT_PTR>(wParam));
        return TRUE;

    case WM_CTLCOLORSTATIC: {
        HBRUSH brush = onCtlColor(reinterpret_cast<HWND>(lParam));
        if (brush) {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, activeText_);
            SetBkColor(dc, GetBkColor(dc));
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<INT_PTR>(brush);
        }
        return FALSE;
    }

    case WM_CLOSE:
        if (test_) test_->stop();
        KillTimer(dialog, kStatusTimer);
        EndDialog(dialog, 0);
        return TRUE;
    }
    (void)lParam;
    return FALSE;
}

void App::onInit(HWND dialog)
{
    HWND transport = GetDlgItem(dialog, IDC_TRANSPORT_COMBO);
    for (const auto* name : kTransportNames) ComboBox_AddString(transport, name);
    ComboBox_SetCurSel(transport, 0);

    setText(dialog, IDC_SERIAL_PORT,      L"COM101");
    setText(dialog, IDC_SERIAL_BAUD,      L"115200");
    setText(dialog, IDC_TCP_HOST,         L"127.0.0.1");
    setText(dialog, IDC_TCP_PORT,         L"50000");
    setText(dialog, IDC_UDP_LOCAL_HOST,   L"0.0.0.0");
    setText(dialog, IDC_UDP_LOCAL_PORT,   L"50001");
    setText(dialog, IDC_UDP_REMOTE_HOST,  L"127.0.0.1");
    setText(dialog, IDC_UDP_REMOTE_PORT,  L"50000");
    setText(dialog, IDC_DURATION_HOURS,   L"1");
    setText(dialog, IDC_DURATION_MINUTES, L"0");

    setText(dialog, IDC_TX_COUNT,     L"송신 0");
    setText(dialog, IDC_RX_COUNT,     L"수신 0");
    setText(dialog, IDC_FAIL_COUNT,   L"실패 0");
    setText(dialog, IDC_ELAPSED_TEXT, L"경과 0:00:00 / 0:00:00");

    // 배너 전용 큰 글꼴·상태 브러시
    bannerFont_ = makeBannerFont();
    if (bannerFont_)
        SendMessageW(GetDlgItem(dialog, IDC_RESULT_TEXT), WM_SETFONT,
                     reinterpret_cast<WPARAM>(bannerFont_), TRUE);
    idleBrush_ = CreateSolidBrush(kIdleBg);
    runBrush_  = CreateSolidBrush(kRunBg);
    okBrush_   = CreateSolidBrush(kOkBg);
    failBrush_ = CreateSolidBrush(kFailBg);
    applyBannerState(dialog, Verdict::Pending, false);

    setTransportFieldsEnabled(dialog);
    EnableWindow(GetDlgItem(dialog, IDC_STOP_BUTTON), FALSE);

    SetTimer(dialog, kStatusTimer, kStatusIntervalMs, nullptr);
}

HBRUSH App::onCtlColor(HWND control)
{
    if (control == GetDlgItem(dialog_, IDC_RESULT_TEXT) && activeBrush_)
        return activeBrush_;
    return nullptr;
}

void App::applyBannerState(HWND dialog, Verdict verdict, bool running)
{
    if (running)                  activeBrush_ = runBrush_;
    else if (verdict == Verdict::Ok)   activeBrush_ = okBrush_;
    else if (verdict == Verdict::Fail) activeBrush_ = failBrush_;
    else                               activeBrush_ = idleBrush_;
    activeText_ = RGB(255, 255, 255);
    setText(dialog, IDC_RESULT_TEXT, verdictBanner(verdict, running));
    InvalidateRect(GetDlgItem(dialog, IDC_RESULT_TEXT), nullptr, TRUE);
}

void App::onCommand(HWND dialog, int id, int code)
{
    switch (id) {
    case IDC_START_BUTTON:
        startTest(dialog);
        break;
    case IDC_STOP_BUTTON:
        stopTest(dialog, L"사용자 중지.");
        break;
    case IDC_VSPE_BUTTON: {
        const auto portText = trimmed(getText(dialog, IDC_SERIAL_PORT));
        unsigned long base = 0;
        if (portText.size() < 4 || portText.substr(0, 3) != L"COM" ||
            !parseUnsigned(portText.substr(3), base, 4096) || !base) {
            MessageBoxW(dialog, L"유효한 COM 포트 번호 입력 필요.",
                        L"SIMUL", MB_ICONINFORMATION);
            break;
        }
        const unsigned mate = base + 1 <= 4096 ? static_cast<unsigned>(base + 1)
                                               : static_cast<unsigned>(base - 1);
        std::vector<VspePair> pairs;
        pairs.push_back({static_cast<unsigned>(base), mate});
        const auto result = launchVspe(dialog, pairs);
        MessageBoxW(dialog, result.message.c_str(), L"SIMUL",
                    result.launched ? MB_ICONINFORMATION : MB_ICONWARNING);
        break;
    }
    case IDC_TRANSPORT_COMBO:
        if (code == CBN_SELCHANGE) setTransportFieldsEnabled(dialog);
        break;
    case IDCANCEL:
        if (test_) test_->stop();
        KillTimer(dialog, kStatusTimer);
        EndDialog(dialog, 0);
        break;
    }
}

void App::onTimer(HWND dialog, UINT_PTR id)
{
    if (id != kStatusTimer) return;
    updateStatus(dialog);
}

void App::setTransportFieldsEnabled(HWND dialog)
{
    const int index = ComboBox_GetCurSel(GetDlgItem(dialog, IDC_TRANSPORT_COMBO));
    const bool serial = index == 0;
    const bool tcp    = index == 1;
    const bool udp    = index == 2;

    EnableWindow(GetDlgItem(dialog, IDC_SERIAL_PORT), serial);
    EnableWindow(GetDlgItem(dialog, IDC_SERIAL_BAUD), serial);
    EnableWindow(GetDlgItem(dialog, IDC_VSPE_BUTTON), serial);

    EnableWindow(GetDlgItem(dialog, IDC_TCP_HOST), tcp);
    EnableWindow(GetDlgItem(dialog, IDC_TCP_PORT), tcp);

    EnableWindow(GetDlgItem(dialog, IDC_UDP_LOCAL_HOST),  udp);
    EnableWindow(GetDlgItem(dialog, IDC_UDP_LOCAL_PORT),  udp);
    EnableWindow(GetDlgItem(dialog, IDC_UDP_REMOTE_HOST), udp);
    EnableWindow(GetDlgItem(dialog, IDC_UDP_REMOTE_PORT), udp);
}

bool App::readSettings(HWND dialog, TestSettings& settings, std::wstring& error) const
{
    const int index = ComboBox_GetCurSel(GetDlgItem(dialog, IDC_TRANSPORT_COMBO));
    if      (index == 0) settings.transport.kind = TransportKind::Serial;
    else if (index == 1) settings.transport.kind = TransportKind::Tcp;
    else if (index == 2) settings.transport.kind = TransportKind::Udp;
    else { error = L"접속 방식 선택 필요."; return false; }

    if (settings.transport.kind == TransportKind::Serial) {
        settings.transport.serial.port = trimmed(getText(dialog, IDC_SERIAL_PORT));
        if (settings.transport.serial.port.empty()) {
            error = L"직렬 포트명 입력 필요 (예: COM101).";
            return false;
        }
        unsigned long baud = 0;
        if (!parseUnsigned(trimmed(getText(dialog, IDC_SERIAL_BAUD)), baud, 4000000) || !baud) {
            error = L"보/속 1~4000000 범위 입력 필요.";
            return false;
        }
        settings.transport.serial.baud = static_cast<DWORD>(baud);
    } else if (settings.transport.kind == TransportKind::Tcp) {
        settings.transport.tcp.host = wideToNarrow(trimmed(getText(dialog, IDC_TCP_HOST)));
        unsigned long port = 0;
        if (settings.transport.tcp.host.empty() ||
            !parseUnsigned(trimmed(getText(dialog, IDC_TCP_PORT)), port, 65535) || !port) {
            error = L"TCP 상대 주소·포트 입력 필요.";
            return false;
        }
        settings.transport.tcp.port = static_cast<unsigned short>(port);
    } else {
        settings.transport.udp.localHost  = wideToNarrow(trimmed(getText(dialog, IDC_UDP_LOCAL_HOST)));
        settings.transport.udp.remoteHost = wideToNarrow(trimmed(getText(dialog, IDC_UDP_REMOTE_HOST)));
        unsigned long localPort = 0, remotePort = 0;
        if (settings.transport.udp.localHost.empty() ||
            settings.transport.udp.remoteHost.empty() ||
            !parseUnsigned(trimmed(getText(dialog, IDC_UDP_LOCAL_PORT)),  localPort,  65535) || !localPort ||
            !parseUnsigned(trimmed(getText(dialog, IDC_UDP_REMOTE_PORT)), remotePort, 65535) || !remotePort) {
            error = L"UDP 수신·송신 주소·포트 입력 필요.";
            return false;
        }
        settings.transport.udp.localPort  = static_cast<unsigned short>(localPort);
        settings.transport.udp.remotePort = static_cast<unsigned short>(remotePort);
    }

    unsigned long hours = 0, minutes = 0;
    if (!parseUnsigned(trimmed(getText(dialog, IDC_DURATION_HOURS)),   hours,   72) ||
        !parseUnsigned(trimmed(getText(dialog, IDC_DURATION_MINUTES)), minutes, 59)) {
        error = L"시험 시간: 0~72 시간, 0~59 분.";
        return false;
    }
    const auto seconds = hours * 3600ULL + minutes * 60ULL;
    if (!seconds)              { error = L"시험 시간은 최소 1분 이상 지정.";    return false; }
    if (seconds > 72ULL * 3600) { error = L"시험 시간 최대 72 시간.";             return false; }
    settings.duration = std::chrono::seconds(seconds);
    settings.interval = std::chrono::milliseconds(50);
    return true;
}

void App::startTest(HWND dialog)
{
    if (running_) return;
    TestSettings settings;
    std::wstring error;
    if (!readSettings(dialog, settings, error)) {
        MessageBoxW(dialog, error.c_str(), L"SIMUL", MB_ICONWARNING);
        return;
    }
    test_ = std::make_unique<LoopbackTest>();
    if (!test_->start(settings, error)) {
        test_.reset();
        MessageBoxW(dialog, error.c_str(), L"SIMUL", MB_ICONERROR);
        return;
    }
    running_ = true;
    EnableWindow(GetDlgItem(dialog, IDC_START_BUTTON),    FALSE);
    EnableWindow(GetDlgItem(dialog, IDC_STOP_BUTTON),     TRUE);
    EnableWindow(GetDlgItem(dialog, IDC_TRANSPORT_COMBO), FALSE);
    EnableWindow(GetDlgItem(dialog, IDC_SERIAL_PORT),     FALSE);
    EnableWindow(GetDlgItem(dialog, IDC_SERIAL_BAUD),     FALSE);
    EnableWindow(GetDlgItem(dialog, IDC_TCP_HOST),        FALSE);
    EnableWindow(GetDlgItem(dialog, IDC_TCP_PORT),        FALSE);
    EnableWindow(GetDlgItem(dialog, IDC_UDP_LOCAL_HOST),  FALSE);
    EnableWindow(GetDlgItem(dialog, IDC_UDP_LOCAL_PORT),  FALSE);
    EnableWindow(GetDlgItem(dialog, IDC_UDP_REMOTE_HOST), FALSE);
    EnableWindow(GetDlgItem(dialog, IDC_UDP_REMOTE_PORT), FALSE);
    EnableWindow(GetDlgItem(dialog, IDC_DURATION_HOURS),  FALSE);
    EnableWindow(GetDlgItem(dialog, IDC_DURATION_MINUTES),FALSE);
    EnableWindow(GetDlgItem(dialog, IDC_VSPE_BUTTON),     FALSE);
    appendLog(dialog, L"시험 시작.");
    setText(dialog, IDC_STATUS_TEXT, L"송신 주기 50 ms, DATA 32 Byte 반송 검증 중.");
    applyBannerState(dialog, Verdict::Pending, true);
}

void App::stopTest(HWND dialog, const std::wstring& reason)
{
    if (!test_) return;
    test_->stop();
    const auto snap = test_->snapshot();
    test_.reset();
    running_ = false;
    EnableWindow(GetDlgItem(dialog, IDC_START_BUTTON),    TRUE);
    EnableWindow(GetDlgItem(dialog, IDC_STOP_BUTTON),     FALSE);
    EnableWindow(GetDlgItem(dialog, IDC_TRANSPORT_COMBO), TRUE);
    setTransportFieldsEnabled(dialog);
    EnableWindow(GetDlgItem(dialog, IDC_DURATION_HOURS),   TRUE);
    EnableWindow(GetDlgItem(dialog, IDC_DURATION_MINUTES), TRUE);

    const auto verdict = snap.verdict == Verdict::Pending
        ? (snap.failed == 0 ? Verdict::Ok : Verdict::Fail)
        : snap.verdict;
    applyBannerState(dialog, verdict, false);
    setText(dialog, IDC_STATUS_TEXT, reason);
    appendLog(dialog, L"시험 종료: " + reason);
}

void App::updateStatus(HWND dialog)
{
    if (!test_) return;
    const auto snap = test_->snapshot();
    setText(dialog, IDC_TX_COUNT,   L"송신 " + std::to_wstring(snap.sent));
    setText(dialog, IDC_RX_COUNT,   L"수신 " + std::to_wstring(snap.received));
    setText(dialog, IDC_FAIL_COUNT, L"실패 " + std::to_wstring(snap.failed));
    setText(dialog, IDC_ELAPSED_TEXT,
            L"경과 " + formatDuration(snap.elapsed) + L" / " +
            formatDuration(std::chrono::duration_cast<std::chrono::milliseconds>(snap.duration)));

    applyBannerState(dialog, snap.verdict, snap.running);

    if (!snap.lastEvent.empty() && snap.lastEvent != lastEvent_) {
        lastEvent_ = snap.lastEvent;
        appendLog(dialog, snap.lastEvent);
    }

    if (!snap.running && running_) {
        stopTest(dialog, snap.verdict == Verdict::Ok
                 ? std::wstring(L"시험 시간 완료, 전 프레임 일치.")
                 : (snap.lastEvent.empty() ? std::wstring(L"시험 종료.") : snap.lastEvent));
    }
}

void App::appendLog(HWND dialog, const std::wstring& line)
{
    HWND log = GetDlgItem(dialog, IDC_LOG_TEXT);
    const int length = GetWindowTextLengthW(log);
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t stamp[32]{};
    std::swprintf(stamp, std::size(stamp), L"[%02u:%02u:%02u] ",
                  now.wHour, now.wMinute, now.wSecond);
    const std::wstring entry = std::wstring(stamp) + line + L"\r\n";
    SendMessageW(log, EM_SETSEL, static_cast<WPARAM>(length), static_cast<LPARAM>(length));
    SendMessageW(log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(entry.c_str()));
}

}  // namespace simul
