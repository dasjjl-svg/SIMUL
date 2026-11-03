#include "app.h"

#include <mmsystem.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <cwctype>
#include <iterator>
#include <thread>
#include <utility>

#include "format.h"
#include "resource.h"

namespace loopback {

namespace {

constexpr UINT_PTR kRefreshTimer = 1;
constexpr UINT kRefreshMs = 100;
constexpr UINT_PTR kCountTimer = 2;
constexpr UINT kCountDelayMs = 400;  // 채널 수 입력 확정 대기. 중간 입력값의 채널 삭제 방지
constexpr UINT kRefreshFontsMessage = WM_APP + 1;
constexpr UINT kComboSyncMessage = WM_APP + 2;
constexpr int kLogLimit = 2000000;

constexpr const wchar_t* kAppTitle = L"Loopback Tester";
constexpr const wchar_t* kRegistryKey = L"Software\\LoopbackTester";
constexpr wchar_t kFieldSeparator = L'\x1F';
constexpr std::size_t kFieldCount = 15;
constexpr std::size_t kFieldLimit = 64;

constexpr unsigned long long kMinBaud = 50;
constexpr unsigned long long kMaxBaud = 4000000;
constexpr unsigned long long kMinEchoMs = 50;
constexpr unsigned long long kMaxEchoMs = 10000;
constexpr unsigned long long kMaxHours = 72;
// 평균 전문 길이: 고정 40B + DATA·CRC 34B의 DLE 치환 기대치 0.4B
constexpr double kMeanFrameBytes = 40.4;

enum TransportIndex { kSerial = 0, kTcp = 1, kUdp = 2 };
enum StandardIndex { kRs232 = 0, kRs422 = 1, kRs485TwoWire = 2, kRs485FourWire = 3 };
enum EchoIndex { kEchoUnset = 0, kEchoPresent = 1, kEchoAbsent = 2 };
enum Column {
    kColChannel, kColKind, kColTarget, kColVerdict, kColSent, kColMatched, kColFailed, kColLatency, kColRecent,
    kColumnCount
};

struct ColumnSpec {
    const wchar_t* title;
    int width;
    int format;
};

// 열 폭 단위: 대화상자 단위(DLU). 합계 384, 목록 폭 400에서 테두리·세로 스크롤 막대 여유 제외
constexpr ColumnSpec kColumns[kColumnCount] = {
    {L"CH", 20, LVCFMT_LEFT},     {L"접속", 42, LVCFMT_LEFT},      {L"대상", 106, LVCFMT_LEFT},
    {L"판정", 40, LVCFMT_CENTER}, {L"송신", 38, LVCFMT_RIGHT},     {L"정상 회신", 40, LVCFMT_RIGHT},
    {L"실패", 30, LVCFMT_RIGHT},  {L"최대 지연", 36, LVCFMT_RIGHT}, {L"최근", 32, LVCFMT_LEFT},
};

constexpr const wchar_t* kKindNames[] = {L"직렬 (RS-232·422·485)", L"TCP 클라이언트", L"UDP"};
constexpr const wchar_t* kStandardNames[] = {L"RS-232", L"RS-422", L"RS-485 2선식", L"RS-485 4선식"};
constexpr const wchar_t* kBaudRates[] = {L"300",   L"600",   L"1200",   L"2400",   L"4800",   L"9600",   L"14400",
                                         L"19200", L"38400", L"57600", L"115200", L"230400", L"460800", L"921600"};
constexpr const wchar_t* kDataBits[] = {L"5", L"6", L"7", L"8"};
constexpr const wchar_t* kParityNames[] = {L"없음 (N)", L"홀수 (O)", L"짝수 (E)", L"마크 (M)", L"스페이스 (S)"};
constexpr const wchar_t* kStopNames[] = {L"1", L"1.5", L"2"};
constexpr const wchar_t* kFlowNames[] = {L"없음", L"RTS/CTS", L"DTR/DSR"};
constexpr const wchar_t* kEchoNames[] = {L"선택 필요", L"발생 (자기 송신분 제거)", L"미발생"};

constexpr int kSerialControls[] = {IDC_SERIAL_STANDARD_LABEL, IDC_SERIAL_STANDARD, IDC_SERIAL_PORT_LABEL,
                                   IDC_SERIAL_PORT, IDC_SERIAL_REFRESH, IDC_SERIAL_BAUD_LABEL, IDC_SERIAL_BAUD,
                                   IDC_SERIAL_BAUD_UNIT, IDC_SERIAL_DATA_LABEL, IDC_SERIAL_DATA,
                                   IDC_SERIAL_PARITY_LABEL, IDC_SERIAL_PARITY, IDC_SERIAL_STOP_LABEL, IDC_SERIAL_STOP,
                                   IDC_SERIAL_FLOW_LABEL, IDC_SERIAL_FLOW, IDC_SERIAL_ECHO_LABEL, IDC_SERIAL_ECHO,
                                   IDC_SERIAL_ECHO_HINT};
constexpr int kTcpControls[] = {IDC_TCP_HOST_LABEL, IDC_TCP_HOST, IDC_TCP_PORT_LABEL, IDC_TCP_PORT};
constexpr int kUdpControls[] = {IDC_UDP_LOCAL_LABEL, IDC_UDP_LOCAL_HOST, IDC_UDP_LOCAL_PORT_LABEL, IDC_UDP_LOCAL_PORT,
                                IDC_UDP_REMOTE_LABEL, IDC_UDP_REMOTE_HOST, IDC_UDP_REMOTE_PORT_LABEL,
                                IDC_UDP_REMOTE_PORT};
constexpr int kInputControls[] = {IDC_CHANNEL_COUNT, IDC_CHANNEL_COUNT_SPIN, IDC_ECHO_TIMEOUT, IDC_DURATION_HOURS,
                                  IDC_DURATION_HOURS_SPIN, IDC_DURATION_MINUTES, IDC_DURATION_MINUTES_SPIN,
                                  IDC_TRANSPORT_COMBO, IDC_SERIAL_STANDARD, IDC_SERIAL_PORT, IDC_SERIAL_REFRESH,
                                  IDC_SERIAL_BAUD, IDC_SERIAL_DATA, IDC_SERIAL_PARITY, IDC_SERIAL_STOP,
                                  IDC_SERIAL_FLOW, IDC_SERIAL_ECHO, IDC_TCP_HOST, IDC_TCP_PORT, IDC_UDP_LOCAL_HOST,
                                  IDC_UDP_LOCAL_PORT, IDC_UDP_REMOTE_HOST, IDC_UDP_REMOTE_PORT};
constexpr int kMutedLabels[] = {IDC_CHANNEL_SUMMARY, IDC_CHANNEL_DETAIL, IDC_SERIAL_BAUD_UNIT, IDC_SERIAL_ECHO_HINT,
                                IDC_DURATION_HOURS_UNIT, IDC_DURATION_MINUTES_UNIT, IDC_ECHO_UNIT};
constexpr int kSections[] = {IDC_SECTION_TEST, IDC_SECTION_CHANNELS, IDC_SECTION_EDITOR, IDC_SECTION_LOG};

template <std::size_t N>
bool contains(const int (&ids)[N], int id)
{
    return std::find(std::begin(ids), std::end(ids), id) != std::end(ids);
}

template <typename T>
bool assign(T& field, const T& value)
{
    if (field == value) return false;
    field = value;
    return true;
}

std::wstring trimmed(const std::wstring& value)
{
    const auto first = value.find_first_not_of(L" \t");
    if (first == std::wstring::npos) return {};
    return value.substr(first, value.find_last_not_of(L" \t") - first + 1);
}

bool parseNumber(const std::wstring& value, unsigned long long limit, unsigned long long& out)
{
    if (value.empty() || value.size() > 10) return false;
    out = 0;
    for (wchar_t ch : value) {
        if (ch < L'0' || ch > L'9') return false;
        out = out * 10 + static_cast<unsigned long long>(ch - L'0');
    }
    return out <= limit;
}

bool parseSmall(const std::wstring& value, int low, int high, int& out)
{
    unsigned long long number = 0;
    if (!parseNumber(value, static_cast<unsigned long long>(high), number) || number < static_cast<unsigned long long>(low))
        return false;
    out = static_cast<int>(number);
    return true;
}

// "COM5 · Real COM · 표시명" 형식 목록 항목에서 포트명만 추출
std::wstring portToken(const std::wstring& value)
{
    std::wstring token = trimmed(value);
    const auto space = token.find(L' ');
    if (space != std::wstring::npos) token.erase(space);
    for (auto& ch : token) ch = static_cast<wchar_t>(std::towupper(ch));
    if (token.compare(0, 4, L"\\\\.\\") == 0) token.erase(0, 4);
    return token;
}

bool parseComPort(const std::wstring& value, std::wstring& port)
{
    const std::wstring token = portToken(value);
    unsigned long long number = 0;
    if (token.size() < 4 || token.compare(0, 3, L"COM") != 0 || token[3] == L'0' ||
        !parseNumber(token.substr(3), 4096, number) || !number)
        return false;
    port = L"COM" + std::to_wstring(number);
    return true;
}

bool parseIpv4(const std::wstring& value, bool allowAny, std::string& out)
{
    IN_ADDR address{};
    if (value.empty() || InetPtonW(AF_INET, value.c_str(), &address) != 1) return false;
    if (!allowAny && address.S_un.S_addr == INADDR_ANY) return false;
    out.clear();
    for (wchar_t ch : value) out.push_back(static_cast<char>(ch));
    return true;
}

std::wstring milliseconds(std::chrono::microseconds value)
{
    wchar_t buffer[32]{};
    std::swprintf(buffer, std::size(buffer), L"%.1f ms", static_cast<double>(value.count()) / 1000.0);
    return buffer;
}

std::wstring channelName(std::size_t index)
{
    return L"CH" + std::to_wstring(index + 1);
}

// 3개 이상 연속 채널은 구간 표기: CH1~CH4, CH7, CH8
std::wstring channelRanges(const std::vector<std::size_t>& channels)
{
    std::wstring text;
    for (std::size_t i = 0; i < channels.size();) {
        std::size_t last = i;
        while (last + 1 < channels.size() && channels[last + 1] == channels[last] + 1) ++last;
        const bool range = last - i >= 2;
        const std::size_t end = range ? i + 1 : last + 1;
        for (std::size_t k = i; k < end; ++k) {
            if (!text.empty()) text += L", ";
            text += channelName(channels[k]);
        }
        if (range) text += L"~" + channelName(channels[last]);
        i = last + 1;
    }
    return text;
}

std::wstring targetText(const ChannelForm& form)
{
    static const wchar_t parities[] = {L'N', L'O', L'E', L'M', L'S'};
    static const wchar_t* const stops[] = {L"1", L"1.5", L"2"};
    static const wchar_t* const standards[] = {L"", L" · 422", L" · 485-2W", L" · 485-4W"};
    switch (form.kind) {
    case kTcp:
        return trimmed(form.tcpHost) + L":" + trimmed(form.tcpPort);
    case kUdp:
        return trimmed(form.udpRemoteHost) + L":" + trimmed(form.udpRemotePort) + L" (수신 " +
               trimmed(form.udpLocalPort) + L")";
    default:
        return portToken(form.serialPort) + L" · " + trimmed(form.baud) + L" " + std::to_wstring(form.dataBits) +
               parities[std::clamp(form.parity, 0, 4)] + stops[std::clamp(form.stopBits, 0, 2)] +
               standards[std::clamp(form.standard, 0, 3)];
    }
}

// 기본 배치: 직렬 COM1~, TCP 상대 포트 50001~, UDP 수신 51001~·상대 50001~
ChannelForm defaultForm(std::size_t index)
{
    const unsigned number = static_cast<unsigned>(index + 1);
    ChannelForm form;
    form.kind = kSerial;
    form.serialPort = L"COM" + std::to_wstring(number);
    form.standard = kRs232;
    form.baud = L"115200";
    form.dataBits = 8;
    form.parity = NOPARITY;
    form.stopBits = ONESTOPBIT;
    form.flow = 0;
    form.echo = kEchoUnset;
    form.tcpHost = L"127.0.0.1";
    form.tcpPort = std::to_wstring(50000 + number);
    form.udpLocalHost = L"0.0.0.0";
    form.udpLocalPort = std::to_wstring(51000 + number);
    form.udpRemoteHost = L"127.0.0.1";
    form.udpRemotePort = std::to_wstring(50000 + number);
    return form;
}

std::wstring encodeForm(const ChannelForm& form)
{
    const std::wstring fields[kFieldCount] = {
        std::to_wstring(form.kind),     form.serialPort,       std::to_wstring(form.standard), form.baud,
        std::to_wstring(form.dataBits), std::to_wstring(form.parity), std::to_wstring(form.stopBits),
        std::to_wstring(form.flow),     std::to_wstring(form.echo), form.tcpHost, form.tcpPort, form.udpLocalHost,
        form.udpLocalPort,              form.udpRemoteHost,    form.udpRemotePort};
    std::wstring text;
    for (std::size_t i = 0; i < kFieldCount; ++i) {
        if (i) text.push_back(kFieldSeparator);
        text += fields[i];
    }
    return text;
}

bool decodeForm(const std::wstring& text, ChannelForm& form)
{
    std::vector<std::wstring> fields;
    std::size_t begin = 0;
    for (;;) {
        const auto end = text.find(kFieldSeparator, begin);
        fields.push_back(text.substr(begin, end == std::wstring::npos ? std::wstring::npos : end - begin));
        if (end == std::wstring::npos) break;
        begin = end + 1;
    }
    if (fields.size() != kFieldCount) return false;
    for (const auto& field : fields)
        if (field.size() > kFieldLimit) return false;
    ChannelForm parsed;
    if (!parseSmall(fields[0], kSerial, kUdp, parsed.kind) ||
        !parseSmall(fields[2], kRs232, kRs485FourWire, parsed.standard) ||
        !parseSmall(fields[4], 5, 8, parsed.dataBits) || !parseSmall(fields[5], NOPARITY, SPACEPARITY, parsed.parity) ||
        !parseSmall(fields[6], ONESTOPBIT, TWOSTOPBITS, parsed.stopBits) || !parseSmall(fields[7], 0, 2, parsed.flow) ||
        !parseSmall(fields[8], kEchoUnset, kEchoAbsent, parsed.echo))
        return false;
    parsed.serialPort = fields[1];
    parsed.baud = fields[3];
    parsed.tcpHost = fields[9];
    parsed.tcpPort = fields[10];
    parsed.udpLocalHost = fields[11];
    parsed.udpLocalPort = fields[12];
    parsed.udpRemoteHost = fields[13];
    parsed.udpRemotePort = fields[14];
    form = parsed;
    return true;
}

std::wstring channelValueName(std::size_t index)
{
    return (index + 1 < 10 ? L"Channel0" : L"Channel") + std::to_wstring(index + 1);
}

bool readRegistryText(HKEY key, const wchar_t* name, std::wstring& out)
{
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS || type != REG_SZ ||
        bytes > 4096 || bytes % sizeof(wchar_t))
        return false;
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buffer.data()), &bytes) != ERROR_SUCCESS ||
        type != REG_SZ)
        return false;
    out.assign(buffer.data());
    return true;
}

void writeRegistryText(HKEY key, const wchar_t* name, const std::wstring& value)
{
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

std::wstring indentContinuation(const std::wstring& text, std::size_t width)
{
    std::wstring result;
    const std::wstring pad(width, L' ');
    std::size_t begin = 0;
    for (;;) {
        const std::size_t end = text.find(L"\r\n", begin);
        result += text.substr(begin, end == std::wstring::npos ? std::wstring::npos : end - begin);
        if (end == std::wstring::npos) break;
        result += L"\r\n" + pad;
        begin = end + 2;
    }
    return result;
}

void fillCombo(HWND combo, const wchar_t* const* names, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i) ComboBox_AddString(combo, names[i]);
}

// 다중 행 편집 컨트롤의 Esc 키 → 대화상자 WM_CLOSE 변환 차단
LRESULT CALLBACK swallowEscape(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR)
{
    if ((message == WM_KEYDOWN || message == WM_CHAR) && wParam == VK_ESCAPE) return 0;
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, swallowEscape, id);
    return DefSubclassProc(window, message, wParam, lParam);
}

// Windows 11 최소화·가림 상태의 타이머 분해능 저하 및 EcoQoS 실행 속도 제한 해제
void exemptFromPowerThrottling()
{
    struct ThrottlingState {
        ULONG version;
        ULONG controlMask;
        ULONG stateMask;
    };
    constexpr int kProcessPowerThrottling = 4;
    constexpr ULONG kExecutionSpeed = 0x1;
    constexpr ULONG kIgnoreTimerResolution = 0x4;
    using SetInformation = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
    const auto setInformation = reinterpret_cast<SetInformation>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation")));
    if (!setInformation) return;
    ThrottlingState state{1, kExecutionSpeed | kIgnoreTimerResolution, 0};
    if (!setInformation(GetCurrentProcess(), kProcessPowerThrottling, &state, sizeof(state))) {
        state.controlMask = kExecutionSpeed;
        setInformation(GetCurrentProcess(), kProcessPowerThrottling, &state, sizeof(state));
    }
}

}  // namespace

App::App()
{
    for (std::size_t i = 0; i < kMaxChannels; ++i) forms_[i] = defaultForm(i);
}

App::~App()
{
    for (auto& test : tests_)
        if (test) test->cancel();
    for (auto& test : tests_)
        if (test) test->stop();
    releaseSystemHolds();
    if (sectionFont_) DeleteObject(sectionFont_);
    if (surfaceBrush_) DeleteObject(surfaceBrush_);
    if (disabledBrush_) DeleteObject(disabledBrush_);
}

int App::run(HINSTANCE instance)
{
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES | ICC_UPDOWN_CLASS | ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&controls);
    exemptFromPowerThrottling();
    return static_cast<int>(DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_MAIN), nullptr, dialogProc,
                                            reinterpret_cast<LPARAM>(this)));
}

INT_PTR CALLBACK App::dialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_INITDIALOG) {
        auto* self = reinterpret_cast<App*>(lParam);
        SetWindowLongPtrW(dialog, DWLP_USER, lParam);
        self->dialog_ = dialog;
    }
    auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(dialog, DWLP_USER));
    return self ? self->handle(message, wParam, lParam) : FALSE;
}

INT_PTR App::handle(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_INITDIALOG:
        onInit();
        return FALSE;
    case WM_COMMAND:
        onCommand(LOWORD(wParam), HIWORD(wParam));
        return TRUE;
    case WM_NOTIFY:
        return onNotify(reinterpret_cast<NMHDR*>(lParam));
    case WM_TIMER:
        if (wParam == kRefreshTimer) onTimer();
        else if (wParam == kCountTimer) applyCountText();
        return TRUE;
    case WM_POWERBROADCAST:
        onPower(wParam);
        return FALSE;
    case WM_DRAWITEM:
        onDrawItem(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
        SetWindowLongPtrW(dialog_, DWLP_MSGRESULT, TRUE);
        return TRUE;
    case WM_CTLCOLORDLG:
        return reinterpret_cast<INT_PTR>(surfaceBrush_);
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        return onControlColor(reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam));
    case WM_DPICHANGED:
        // 대화상자 자동 배율 조정 완료 후 글꼴·열 폭 재산정
        PostMessageW(dialog_, kRefreshFontsMessage, 0, 0);
        return FALSE;
    case kRefreshFontsMessage:
        refreshFonts();
        sizeColumns();
        InvalidateRect(dialog_, nullptr, TRUE);
        return TRUE;
    case kComboSyncMessage:
        syncComboText(static_cast<int>(wParam));
        return TRUE;
    case WM_QUERYENDSESSION:
        // 시험 중 종료 요청은 보류 응답(종료 차단 사유 표시). 강제 종료 시 WM_ENDSESSION에서 시험 정리
        if (!running_) saveSettings();
        SetWindowLongPtrW(dialog_, DWLP_MSGRESULT, running_ ? FALSE : TRUE);
        return TRUE;
    case WM_ENDSESSION:
        if (wParam) {
            if (running_) {
                appendLog(L"시스템 종료로 시험 중지.");
                finishTest();
            }
            saveSettings();
        }
        return TRUE;
    case WM_CLOSE:
        requestClose();
        return TRUE;
    default:
        return FALSE;
    }
}

void App::onInit()
{
    loading_ = true;
    surfaceBrush_ = CreateSolidBrush(ui::palette::Surface);
    disabledBrush_ = CreateSolidBrush(ui::palette::Muted);
    refreshFonts();

    fillCombo(item(IDC_TRANSPORT_COMBO), kKindNames, std::size(kKindNames));
    fillCombo(item(IDC_SERIAL_STANDARD), kStandardNames, std::size(kStandardNames));
    fillCombo(item(IDC_SERIAL_BAUD), kBaudRates, std::size(kBaudRates));
    fillCombo(item(IDC_SERIAL_DATA), kDataBits, std::size(kDataBits));
    fillCombo(item(IDC_SERIAL_PARITY), kParityNames, std::size(kParityNames));
    fillCombo(item(IDC_SERIAL_STOP), kStopNames, std::size(kStopNames));
    fillCombo(item(IDC_SERIAL_FLOW), kFlowNames, std::size(kFlowNames));
    fillCombo(item(IDC_SERIAL_ECHO), kEchoNames, std::size(kEchoNames));

    SendDlgItemMessageW(dialog_, IDC_CHANNEL_COUNT_SPIN, UDM_SETRANGE32, 1, static_cast<LPARAM>(kMaxChannels));
    SendDlgItemMessageW(dialog_, IDC_DURATION_HOURS_SPIN, UDM_SETRANGE32, 0, static_cast<LPARAM>(kMaxHours));
    SendDlgItemMessageW(dialog_, IDC_DURATION_HOURS_SPIN, UDM_SETPOS32, 0, 1);
    SendDlgItemMessageW(dialog_, IDC_DURATION_MINUTES_SPIN, UDM_SETRANGE32, 0, 59);
    SendDlgItemMessageW(dialog_, IDC_DURATION_MINUTES_SPIN, UDM_SETPOS32, 0, 0);
    SetDlgItemTextW(dialog_, IDC_ECHO_TIMEOUT, L"1000");

    Edit_LimitText(item(IDC_CHANNEL_COUNT), 2);
    Edit_LimitText(item(IDC_ECHO_TIMEOUT), 5);
    Edit_LimitText(item(IDC_DURATION_HOURS), 2);
    Edit_LimitText(item(IDC_DURATION_MINUTES), 2);
    ComboBox_LimitText(item(IDC_SERIAL_PORT), 11);
    ComboBox_LimitText(item(IDC_SERIAL_BAUD), 7);
    Edit_LimitText(item(IDC_TCP_HOST), 15);
    Edit_LimitText(item(IDC_TCP_PORT), 5);
    Edit_LimitText(item(IDC_UDP_LOCAL_HOST), 15);
    Edit_LimitText(item(IDC_UDP_LOCAL_PORT), 5);
    Edit_LimitText(item(IDC_UDP_REMOTE_HOST), 15);
    Edit_LimitText(item(IDC_UDP_REMOTE_PORT), 5);
    SendDlgItemMessageW(dialog_, IDC_LOG_TEXT, EM_SETLIMITTEXT, kLogLimit, 0);
    SetWindowSubclass(item(IDC_LOG_TEXT), swallowEscape, 1, 0);

    loadSettings();
    SendDlgItemMessageW(dialog_, IDC_CHANNEL_COUNT_SPIN, UDM_SETPOS32, 0, static_cast<LPARAM>(channelCount_));
    setupChannelList();
    loading_ = false;

    refreshPorts();
    selectChannel(0);
    refreshSummary();
    setInputsEnabled(true);
    previewDuration();
    SetTimer(dialog_, kRefreshTimer, kRefreshMs, nullptr);
    fitToWorkArea();
    SetFocus(item(IDC_START_BUTTON));
}

void App::onCommand(int id, int code)
{
    if (loading_) return;
    ChannelForm& form = forms_[selected_];
    bool changed = false;
    switch (id) {
    case IDC_CHANNEL_COUNT:
        if (code == EN_CHANGE && !running_) SetTimer(dialog_, kCountTimer, kCountDelayMs, nullptr);
        else if (code == EN_KILLFOCUS) applyCountText();
        break;
    case IDC_ECHO_TIMEOUT:
        if (code == EN_CHANGE) conditionsChanged();
        break;
    case IDC_DURATION_HOURS:
    case IDC_DURATION_MINUTES:
        if (code == EN_CHANGE) {
            conditionsChanged();
            previewDuration();
        }
        break;
    case IDC_TRANSPORT_COMBO:
        if (code == CBN_SELCHANGE) {
            const int kind = ComboBox_GetCurSel(item(id));
            if (kind >= kSerial && kind <= kUdp && assign(form.kind, kind)) {
                showTransportPanel();
                changed = true;
            }
        }
        break;
    case IDC_SERIAL_STANDARD:
        if (code == CBN_SELCHANGE) {
            const int standard = ComboBox_GetCurSel(item(id));
            if (standard >= kRs232 && standard <= kRs485FourWire && assign(form.standard, standard)) {
                applySerialRules();
                changed = true;
            }
        }
        break;
    case IDC_SERIAL_PORT:
        if (code == CBN_SELCHANGE || code == CBN_SELENDOK) {
            const int index = ComboBox_GetCurSel(item(id));
            if (index >= 0) {
                const int length = (std::max)(ComboBox_GetLBTextLen(item(id), index), 0);
                std::wstring label(static_cast<std::size_t>(length) + 1, L'\0');
                ComboBox_GetLBText(item(id), index, label.data());
                label.resize(static_cast<std::size_t>(length));
                changed = assign(form.serialPort, portToken(label));
                PostMessageW(dialog_, kComboSyncMessage, IDC_SERIAL_PORT, 0);
            }
        } else if (code == CBN_EDITCHANGE) {
            changed = assign(form.serialPort, text(id));
        } else if (code == CBN_CLOSEUP) {
            // 목록 열림 시 자동 선택 항목의 표시명 잔류 방지. 편집 영역을 설정값으로 정리
            PostMessageW(dialog_, kComboSyncMessage, IDC_SERIAL_PORT, 0);
        }
        break;
    case IDC_SERIAL_REFRESH:
        if (code == BN_CLICKED) refreshPorts();
        break;
    case IDC_SERIAL_BAUD:
        if (code == CBN_SELCHANGE || code == CBN_SELENDOK) {
            const int index = ComboBox_GetCurSel(item(id));
            if (index >= 0 && index < static_cast<int>(std::size(kBaudRates)))
                changed = assign(form.baud, std::wstring(kBaudRates[index]));
        } else if (code == CBN_EDITCHANGE) {
            changed = assign(form.baud, text(id));
        } else if (code == CBN_CLOSEUP) {
            PostMessageW(dialog_, kComboSyncMessage, IDC_SERIAL_BAUD, 0);
        }
        break;
    case IDC_SERIAL_DATA:
    case IDC_SERIAL_PARITY:
    case IDC_SERIAL_STOP:
    case IDC_SERIAL_FLOW:
    case IDC_SERIAL_ECHO:
        if (code == CBN_SELCHANGE) {
            const int index = ComboBox_GetCurSel(item(id));
            if (index < 0) break;
            if (id == IDC_SERIAL_DATA) changed = assign(form.dataBits, index + 5);
            else if (id == IDC_SERIAL_PARITY) changed = assign(form.parity, index);
            else if (id == IDC_SERIAL_STOP) changed = assign(form.stopBits, index);
            else if (id == IDC_SERIAL_FLOW) changed = assign(form.flow, index);
            else changed = assign(form.echo, index);
        }
        break;
    case IDC_TCP_HOST:
    case IDC_TCP_PORT:
    case IDC_UDP_LOCAL_HOST:
    case IDC_UDP_LOCAL_PORT:
    case IDC_UDP_REMOTE_HOST:
    case IDC_UDP_REMOTE_PORT:
        if (code == EN_CHANGE) {
            if (std::wstring* field = formField(id)) changed = assign(*field, text(id));
        }
        break;
    case IDC_START_BUTTON:
        if (code == BN_CLICKED) startTest();
        break;
    case IDC_STOP_BUTTON:
        if (code == BN_CLICKED) stopTest();
        break;
    default:
        // IDCANCEL(Esc 키) 무시. 결과 표시 중 의도치 않은 종료 방지, 종료는 WM_CLOSE 경유
        break;
    }
    if (changed) {
        conditionsChanged();
        refreshRows();
    }
}

INT_PTR App::onNotify(NMHDR* header)
{
    if (!header || header->idFrom != IDC_CHANNEL_LIST) return FALSE;
    switch (header->code) {
    case NM_CUSTOMDRAW:
        SetWindowLongPtrW(dialog_, DWLP_MSGRESULT, onListCustomDraw(*reinterpret_cast<NMLVCUSTOMDRAW*>(header)));
        return TRUE;
    case LVN_ITEMCHANGED: {
        const auto& change = *reinterpret_cast<const NMLISTVIEW*>(header);
        if (loading_ || change.iItem < 0 || change.iItem >= static_cast<int>(channelCount_) ||
            !(change.uChanged & LVIF_STATE))
            return FALSE;
        if ((change.uNewState & LVIS_SELECTED) && !(change.uOldState & LVIS_SELECTED))
            selectChannel(static_cast<std::size_t>(change.iItem));
        return FALSE;
    }
    default:
        return FALSE;
    }
}

void App::onTimer()
{
    if (!running_) return;
    bool anyRunning = false;
    for (std::size_t i = 0; i < kMaxChannels; ++i) {
        if (!tests_[i]) continue;
        views_[i] = tests_[i]->snapshot();
        appendChannelEvents(i, tests_[i]->takeEvents());
        anyRunning = anyRunning || views_[i].running;
    }
    refreshRows();
    refreshDetail();
    invalidatePanels();
    if (!anyRunning) finishTest();
}

void App::onDrawItem(const DRAWITEMSTRUCT& draw)
{
    const int base = ui::basePixels(dialog_);
    switch (draw.CtlID) {
    case IDC_HEADER:
        ui::paintHeader(draw.hDC, draw.rcItem, fonts_, base, kAppTitle, L"인터페이스 장치 반송 시험",
                        L"최대 " + std::to_wstring(kMaxChannels) + L"채널 · v1.0");
        break;
    case IDC_VERDICT:
        ui::paintVerdict(draw.hDC, draw.rcItem, fonts_, base, verdictView());
        break;
    case IDC_START_BUTTON:
        ui::paintButton(draw, fonts_, base, true);
        break;
    case IDC_STOP_BUTTON:
    case IDC_SERIAL_REFRESH:
        ui::paintButton(draw, fonts_, base, false);
        break;
    default:
        break;
    }
}

INT_PTR App::onControlColor(HDC dc, HWND control)
{
    const int id = GetDlgCtrlID(control);
    wchar_t className[16]{};
    GetClassNameW(control, className, static_cast<int>(std::size(className)));
    if (lstrcmpiW(className, L"Edit") == 0) {
        const bool plain = id == IDC_LOG_TEXT || IsWindowEnabled(control);
        SetTextColor(dc, plain ? ui::palette::TextStrong : ui::palette::TextMuted);
        SetBkColor(dc, plain ? ui::palette::Surface : ui::palette::Muted);
        return reinterpret_cast<INT_PTR>(plain ? surfaceBrush_ : disabledBrush_);
    }
    COLORREF color = ui::palette::Text;
    if (contains(kSections, id)) color = ui::palette::TextStrong;
    else if (contains(kMutedLabels, id)) color = ui::palette::TextMuted;
    SetTextColor(dc, color);
    SetBkColor(dc, ui::palette::Surface);
    SetBkMode(dc, TRANSPARENT);
    return reinterpret_cast<INT_PTR>(surfaceBrush_);
}

void App::onPower(WPARAM event)
{
    if (!running_) return;
    if (event == PBT_APMSUSPEND) {
        for (auto& test : tests_)
            if (test) test->interrupt(L"시스템 절전 진입. 절전 구간 송·수신 불가.");
    } else if (event == PBT_APMRESUMEAUTOMATIC) {
        appendLog(L"시스템 절전 복귀.");
    }
}

void App::startTest()
{
    if (running_) return;
    applyCountText();
    std::vector<Plan> plan;
    std::wstring error;
    int channel = -1;
    int field = 0;
    if (!readPlan(plan, error, channel, field)) {
        if (channel >= 0) focusChannel(static_cast<std::size_t>(channel), 0);
        MessageBoxW(dialog_, error.c_str(), kAppTitle, MB_ICONWARNING | MB_OK);
        if (field) {
            SetFocus(item(field));
            SendMessageW(item(field), EM_SETSEL, 0, -1);
        }
        return;
    }
    const std::wstring warnings = planWarnings(plan);
    if (!warnings.empty() &&
        MessageBoxW(dialog_, (L"판정 FAIL 예상 조건 포함.\n\n" + warnings + L"\n시험 시작?").c_str(), kAppTitle,
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return;
    saveSettings();

    // 50ms 송신 주기 정밀도 확보용 1ms 타이머 분해능. 기본 15.6ms 분해능에서는 주기 편차 ±15ms
    timerRaised_ = timeBeginPeriod(1) == TIMERR_NOERROR;
    const HCURSOR previous = SetCursor(LoadCursorW(nullptr, IDC_WAIT));

    // 채널별 전송로 병렬 개방. TCP 접속 대기 상한 3s가 채널 수만큼 누적되지 않도록 동시 수행
    std::array<std::unique_ptr<LoopbackTest>, kMaxChannels> prepared{};
    std::array<std::wstring, kMaxChannels> errors{};
    std::array<bool, kMaxChannels> opened{};
    for (const Plan& entry : plan) prepared[entry.channel] = std::make_unique<LoopbackTest>();
    {
        std::vector<std::thread> openers;
        openers.reserve(plan.size());
        for (const Plan& entry : plan) {
            LoopbackTest* test = prepared[entry.channel].get();
            std::wstring* reason = &errors[entry.channel];
            bool* result = &opened[entry.channel];
            const TestSettings* settings = &entry.settings;
            openers.emplace_back([test, reason, result, settings] { *result = test->open(*settings, *reason); });
        }
        for (auto& opener : openers) opener.join();
    }
    SetCursor(previous);

    std::wstring failures;
    for (const Plan& entry : plan) {
        if (opened[entry.channel]) continue;
        failures += channelName(entry.channel) + L": " + errors[entry.channel] + L"\n";
        appendLog(channelName(entry.channel) + L" · 접속 실패: " + errors[entry.channel]);
    }
    if (!failures.empty()) {
        for (auto& test : prepared)
            if (test) test->stop();
        releaseSystemHolds();
        MessageBoxW(dialog_, (L"접속 실패로 시험 미시작.\n\n" + failures).c_str(), kAppTitle, MB_ICONERROR | MB_OK);
        return;
    }

    clearResults();
    for (const Plan& entry : plan) {
        active_[entry.channel] = true;
        tests_[entry.channel] = std::move(prepared[entry.channel]);
        views_[entry.channel].running = true;
        views_[entry.channel].duration = entry.settings.duration;
    }
    for (const Plan& entry : plan) tests_[entry.channel]->begin();

    duration_ = plan.front().settings.duration;
    running_ = true;
    // 장시간 무인 시험 중 시스템 절전 진입 방지, 시스템 종료 시 차단 사유 표시
    sleepBlocked_ = SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED) != 0;
    ShutdownBlockReasonCreate(dialog_, L"반송 시험 진행 중. 종료 시 시험 중단.");
    setInputsEnabled(false);

    appendLog(L"시험 시작. 사용 " + std::to_wstring(plan.size()) + L"채널 · 시험 시간 " +
              clockText(std::chrono::duration_cast<std::chrono::milliseconds>(duration_)) + L" · 회신 제한 " +
              grouped(static_cast<std::uint64_t>(plan.front().settings.echoTimeout.count())) + L" ms");
    for (const Plan& entry : plan) {
        const TransportConfig& transport = entry.settings.transport;
        const bool realcom = transport.kind == TransportKind::Serial && realcomPorts_.count(transport.serial.port);
        appendLog(channelName(entry.channel) + L" · " + describe(transport) +
                  (realcom ? L" · Real COM (인터페이스 방식은 NPort 장치 설정 적용)" : L""));
    }
    onTimer();
    // 시작 후 중지 단추 초점 배제. 키 입력 1회에 의한 장시간 시험 중지 방지
    SetFocus(list_);
}

void App::stopTest()
{
    if (!running_) return;
    if (MessageBoxW(dialog_, L"시험 진행 중. 중지 시 시험 종료, 시험 시간 미완료 채널은 판정 보류 (FAIL 채널은 FAIL 유지).\n\n시험 중지?",
                    kAppTitle, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;
    finishTest();
}

void App::finishTest()
{
    bool any = false;
    for (auto& test : tests_) {
        if (!test) continue;
        test->cancel();
        any = true;
    }
    if (!any) return;
    for (std::size_t i = 0; i < kMaxChannels; ++i) {
        if (!tests_[i]) continue;
        tests_[i]->stop();
        views_[i] = tests_[i]->snapshot();
        appendChannelEvents(i, tests_[i]->takeEvents());
        tests_[i].reset();
    }
    running_ = false;
    hasResult_ = true;
    releaseSystemHolds();
    ShutdownBlockReasonDestroy(dialog_);
    setInputsEnabled(true);

    const Overall result = overall();
    if (result.failedChannels) {
        appendLog(L"전체 판정 FAIL. FAIL " + std::to_wstring(result.failedChannels) + L"채널 (" + result.failedList + L").");
    } else if (result.stopped) {
        appendLog(L"전체 판정 보류. 사용자 중지로 시험 시간 미완료.");
    } else {
        appendLog(L"전체 판정 OK. 사용 " + std::to_wstring(result.channels) + L"채널 전 채널 정상 회신.");
    }
    refreshRows();
    refreshDetail();
    invalidatePanels();
    // 판정·최근 열 최종 상태 재도장
    InvalidateRect(list_, nullptr, FALSE);
    SetFocus(item(IDC_START_BUTTON));
}

void App::requestClose()
{
    if (running_) {
        const bool failed = overall().failedChannels > 0;
        const wchar_t* prompt = failed ? L"시험 진행 중. 종료 시 시험 중지, 판정 FAIL 유지.\n\n프로그램 종료?"
                                       : L"시험 진행 중. 종료 시 시험 중지 및 판정 보류.\n\n프로그램 종료?";
        if (MessageBoxW(dialog_, prompt, kAppTitle, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
        finishTest();
    } else if (hasResult_) {
        if (MessageBoxW(dialog_, L"시험 결과 표시 중. 종료 시 판정·시험 기록 미보존.\n\n프로그램 종료?", kAppTitle,
                        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
            return;
    }
    saveSettings();
    KillTimer(dialog_, kRefreshTimer);
    KillTimer(dialog_, kCountTimer);
    EndDialog(dialog_, 0);
}

void App::applyCountText()
{
    KillTimer(dialog_, kCountTimer);
    if (running_) return;
    unsigned long long count = 0;
    if (parseNumber(trimmed(text(IDC_CHANNEL_COUNT)), kMaxChannels, count) && count >= 1)
        setChannelCount(static_cast<std::size_t>(count));
}

// 시험 후 조건 변경 시 직전 결과 폐기. 표시 결과와 현재 설정의 불일치 방지(시험 기록은 유지)
void App::conditionsChanged()
{
    if (running_ || !hasResult_) return;
    clearResults();
    refreshRows();
}

void App::syncComboText(int id)
{
    const ChannelForm& form = forms_[selected_];
    const bool wasLoading = loading_;
    loading_ = true;
    if (id == IDC_SERIAL_PORT) SetWindowTextW(item(id), form.serialPort.c_str());
    else if (id == IDC_SERIAL_BAUD) SetWindowTextW(item(id), form.baud.c_str());
    loading_ = wasLoading;
}

// 작업 영역 초과 시 작업 영역 안쪽으로 이동, 초과 크기는 좌상단 기준 배치. 제목 표시줄 화면 이탈 방지
void App::fitToWorkArea()
{
    RECT window{};
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetWindowRect(dialog_, &window) ||
        !GetMonitorInfoW(MonitorFromWindow(dialog_, MONITOR_DEFAULTTONEAREST), &monitor))
        return;
    const RECT& area = monitor.rcWork;
    LONG left = window.left;
    LONG top = window.top;
    if (window.right > area.right) left -= window.right - area.right;
    if (window.bottom > area.bottom) top -= window.bottom - area.bottom;
    left = (std::max)(left, area.left);
    top = (std::max)(top, area.top);
    if (left != window.left || top != window.top)
        SetWindowPos(dialog_, nullptr, left, top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

bool App::readChannel(std::size_t index, TransportConfig& config, std::wstring& error, int& field) const
{
    const ChannelForm& form = forms_[index];
    unsigned long long number = 0;
    switch (form.kind) {
    case kSerial: {
        config.kind = TransportKind::Serial;
        SerialConfig& serial = config.serial;
        if (!parseComPort(form.serialPort, serial.port)) {
            error = L"직렬 포트 형식 오류. COM1~COM4096 범위 지정.";
            field = IDC_SERIAL_PORT;
            return false;
        }
        if (!parseNumber(trimmed(form.baud), kMaxBaud, number) || number < kMinBaud) {
            error = L"통신 속도 범위 오류. 50~4,000,000 bps 지정.";
            field = IDC_SERIAL_BAUD;
            return false;
        }
        // Windows 직렬 규칙: 정지 비트 1.5는 데이터 비트 5 전용, 데이터 비트 5는 정지 비트 2 불가
        if (form.stopBits == ONE5STOPBITS && form.dataBits != 5) {
            error = L"정지 비트 1.5는 데이터 비트 5 전용.";
            field = IDC_SERIAL_STOP;
            return false;
        }
        if (form.stopBits == TWOSTOPBITS && form.dataBits == 5) {
            error = L"데이터 비트 5와 정지 비트 2 조합 불가.";
            field = IDC_SERIAL_STOP;
            return false;
        }
        // 반향 여부 미지정 상태의 자기 송신 반향은 장치 무회신에도 정상 판정 유발. 시작 전 명시 선택 강제
        if (form.standard == kRs485TwoWire && form.echo == kEchoUnset) {
            error = L"RS-485 2선식 송신 반향 선택 필요. 어댑터가 자기 송신 데이터를 수신하면 '발생', 아니면 '미발생'.";
            field = IDC_SERIAL_ECHO;
            return false;
        }
        serial.baud = static_cast<DWORD>(number);
        serial.standard = static_cast<SerialStandard>(form.standard);
        serial.dataBits = static_cast<BYTE>(form.dataBits);
        serial.parity = static_cast<BYTE>(form.parity);
        serial.stopBits = static_cast<BYTE>(form.stopBits);
        serial.flow = form.standard == kRs232 ? static_cast<FlowControl>(form.flow) : FlowControl::None;
        serial.localEcho = form.standard != kRs485TwoWire ? LocalEcho::Unset
                           : form.echo == kEchoPresent    ? LocalEcho::Present
                                                          : LocalEcho::Absent;
        return true;
    }
    case kTcp:
        config.kind = TransportKind::Tcp;
        if (!parseIpv4(trimmed(form.tcpHost), false, config.tcp.host)) {
            error = L"TCP 상대 주소 형식 오류. IPv4 주소 지정 (예: 192.168.0.10).";
            field = IDC_TCP_HOST;
            return false;
        }
        if (!parseNumber(trimmed(form.tcpPort), 65535, number) || !number) {
            error = L"TCP 상대 포트 범위 오류. 1~65535 지정.";
            field = IDC_TCP_PORT;
            return false;
        }
        config.tcp.port = static_cast<unsigned short>(number);
        return true;
    case kUdp:
        config.kind = TransportKind::Udp;
        if (!parseIpv4(trimmed(form.udpLocalHost), true, config.udp.localHost)) {
            error = L"UDP 수신 주소 형식 오류. IPv4 주소 또는 0.0.0.0 지정.";
            field = IDC_UDP_LOCAL_HOST;
            return false;
        }
        if (!parseNumber(trimmed(form.udpLocalPort), 65535, number) || !number) {
            error = L"UDP 수신 포트 범위 오류. 1~65535 지정.";
            field = IDC_UDP_LOCAL_PORT;
            return false;
        }
        config.udp.localPort = static_cast<unsigned short>(number);
        if (!parseIpv4(trimmed(form.udpRemoteHost), false, config.udp.remoteHost)) {
            error = L"UDP 상대 주소 형식 오류. IPv4 주소 지정 (예: 192.168.0.10).";
            field = IDC_UDP_REMOTE_HOST;
            return false;
        }
        if (!parseNumber(trimmed(form.udpRemotePort), 65535, number) || !number) {
            error = L"UDP 상대 포트 범위 오류. 1~65535 지정.";
            field = IDC_UDP_REMOTE_PORT;
            return false;
        }
        config.udp.remotePort = static_cast<unsigned short>(number);
        return true;
    default:
        error = L"접속 방식 선택 필요.";
        field = IDC_TRANSPORT_COMBO;
        return false;
    }
}

bool App::readPlan(std::vector<Plan>& plan, std::wstring& error, int& channel, int& field) const
{
    channel = -1;
    field = 0;
    unsigned long long count = 0;
    unsigned long long hours = 0;
    unsigned long long minutes = 0;
    unsigned long long echo = 0;
    if (!parseNumber(trimmed(text(IDC_CHANNEL_COUNT)), kMaxChannels, count) || count != channelCount_) {
        error = L"채널 수 범위 오류. 1~32 지정.";
        field = IDC_CHANNEL_COUNT;
        return false;
    }
    if (!parseNumber(trimmed(text(IDC_DURATION_HOURS)), kMaxHours, hours)) {
        error = L"시험 시간 오류. 0~72시간 지정.";
        field = IDC_DURATION_HOURS;
        return false;
    }
    if (!parseNumber(trimmed(text(IDC_DURATION_MINUTES)), 59, minutes)) {
        error = L"시험 시간 오류. 0~59분 지정.";
        field = IDC_DURATION_MINUTES;
        return false;
    }
    const unsigned long long seconds = hours * 3600 + minutes * 60;
    if (seconds < 60 || seconds > kMaxHours * 3600) {
        error = L"시험 시간 범위 오류. 1분~72시간 지정.";
        field = seconds < 60 ? IDC_DURATION_MINUTES : IDC_DURATION_HOURS;
        return false;
    }
    if (!parseNumber(trimmed(text(IDC_ECHO_TIMEOUT)), kMaxEchoMs, echo) || echo < kMinEchoMs) {
        error = L"회신 제한 범위 오류. 50~10,000 ms 지정.";
        field = IDC_ECHO_TIMEOUT;
        return false;
    }

    plan.clear();
    for (std::size_t i = 0; i < channelCount_; ++i) {
        Plan entry{i, {}};
        if (!readChannel(i, entry.settings.transport, error, field)) {
            error = channelName(i) + L": " + error;
            channel = static_cast<int>(i);
            return false;
        }
        entry.settings.duration = std::chrono::seconds(static_cast<long long>(seconds));
        entry.settings.interval = std::chrono::milliseconds(50);
        entry.settings.echoTimeout = std::chrono::milliseconds(static_cast<long long>(echo));
        plan.push_back(entry);
    }

    // 채널 간 자원 중복: 동일 COM 포트, 동일 UDP 수신 종단
    for (std::size_t a = 0; a < plan.size(); ++a) {
        for (std::size_t b = a + 1; b < plan.size(); ++b) {
            const TransportConfig& first = plan[a].settings.transport;
            const TransportConfig& second = plan[b].settings.transport;
            const std::wstring both = channelName(plan[a].channel) + L"·" + channelName(plan[b].channel);
            if (first.kind == TransportKind::Serial && second.kind == TransportKind::Serial &&
                first.serial.port == second.serial.port) {
                error = both + L" 직렬 포트 중복: " + second.serial.port + L".";
                channel = static_cast<int>(plan[b].channel);
                field = IDC_SERIAL_PORT;
                return false;
            }
            if (first.kind == TransportKind::Udp && second.kind == TransportKind::Udp &&
                first.udp.localPort == second.udp.localPort &&
                (first.udp.localHost == second.udp.localHost || first.udp.localHost == "0.0.0.0" ||
                 second.udp.localHost == "0.0.0.0")) {
                error = both + L" UDP 수신 포트 중복: " + std::to_wstring(second.udp.localPort) + L".";
                channel = static_cast<int>(plan[b].channel);
                field = IDC_UDP_LOCAL_PORT;
                return false;
            }
        }
    }
    return true;
}

// 동일 내용 경고는 채널 묶음 1행 표기
std::wstring App::planWarnings(const std::vector<Plan>& plan) const
{
    std::vector<std::pair<std::wstring, std::vector<std::size_t>>> groups;
    const auto add = [&groups](std::size_t channel, std::wstring text) {
        for (auto& group : groups) {
            if (group.first != text) continue;
            group.second.push_back(channel);
            return;
        }
        groups.emplace_back(std::move(text), std::vector<std::size_t>{channel});
    };
    for (const Plan& entry : plan) {
        if (entry.settings.transport.kind != TransportKind::Serial) continue;
        const SerialConfig& serial = entry.settings.transport.serial;
        if (serial.dataBits < 8)
            add(entry.channel, L"데이터 비트 " + std::to_wstring(serial.dataBits) +
                                   L". C0·C1·FF 등 8비트 전문 바이트 전송 불가.");
        // 소요 속도: 평균 전문 × 문자당 비트 ÷ 50ms. RS-485 2선식은 송신·회신이 한 선로 공유
        const double stop = serial.stopBits == ONESTOPBIT ? 1.0 : serial.stopBits == ONE5STOPBITS ? 1.5 : 2.0;
        const double bits = 1.0 + serial.dataBits + (serial.parity != NOPARITY ? 1.0 : 0.0) + stop;
        const bool shared = serial.standard == SerialStandard::Rs485TwoWire;
        const auto required =
            static_cast<unsigned long long>(kMeanFrameBytes * bits / 0.05 * (shared ? 2.0 : 1.0)) + 1;
        if (serial.baud < required)
            add(entry.channel, L"통신 속도 " + grouped(serial.baud) + L" bps. 50 ms 주기 유지 소요 " +
                                   grouped(required) + L" bps 미달" + (shared ? L" (반이중 송·회신 공유)." : L"."));
    }
    std::wstring warnings;
    for (const auto& group : groups) warnings += channelRanges(group.second) + L": " + group.first + L"\n";
    return warnings;
}

void App::loadSettings()
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return;
    std::wstring value;
    unsigned long long number = 0;
    if (readRegistryText(key, L"ChannelCount", value) && parseNumber(value, kMaxChannels, number) && number >= 1)
        channelCount_ = static_cast<std::size_t>(number);
    for (std::size_t i = 0; i < kMaxChannels; ++i) {
        ChannelForm form = forms_[i];
        if (readRegistryText(key, channelValueName(i).c_str(), value) && decodeForm(value, form)) forms_[i] = form;
    }
    if (readRegistryText(key, L"DurationHours", value) && parseNumber(value, kMaxHours, number))
        SetDlgItemTextW(dialog_, IDC_DURATION_HOURS, value.c_str());
    if (readRegistryText(key, L"DurationMinutes", value) && parseNumber(value, 59, number))
        SetDlgItemTextW(dialog_, IDC_DURATION_MINUTES, value.c_str());
    if (readRegistryText(key, L"EchoTimeoutMs", value) && parseNumber(value, kMaxEchoMs, number) && number >= kMinEchoMs)
        SetDlgItemTextW(dialog_, IDC_ECHO_TIMEOUT, value.c_str());
    RegCloseKey(key);
}

void App::saveSettings() const
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    writeRegistryText(key, L"ChannelCount", std::to_wstring(channelCount_));
    for (std::size_t i = 0; i < kMaxChannels; ++i) writeRegistryText(key, channelValueName(i).c_str(), encodeForm(forms_[i]));
    writeRegistryText(key, L"DurationHours", trimmed(text(IDC_DURATION_HOURS)));
    writeRegistryText(key, L"DurationMinutes", trimmed(text(IDC_DURATION_MINUTES)));
    writeRegistryText(key, L"EchoTimeoutMs", trimmed(text(IDC_ECHO_TIMEOUT)));
    RegCloseKey(key);
}

void App::setupChannelList()
{
    list_ = item(IDC_CHANNEL_LIST);
    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    SetWindowTheme(list_, L"Explorer", nullptr);
    for (int column = 0; column < kColumnCount; ++column) {
        LVCOLUMNW spec{};
        spec.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        spec.fmt = kColumns[column].format;
        spec.cx = dluToPixels(kColumns[column].width);
        spec.pszText = const_cast<wchar_t*>(kColumns[column].title);
        ListView_InsertColumn(list_, column, &spec);
    }
    const std::size_t count = channelCount_;
    channelCount_ = 0;
    setChannelCount(count);
}

void App::setChannelCount(std::size_t count)
{
    count = std::clamp<std::size_t>(count, 1, kMaxChannels);
    if (!list_ || (count == channelCount_ && ListView_GetItemCount(list_) == static_cast<int>(count))) return;
    const bool wasLoading = loading_;
    loading_ = true;
    for (int row = ListView_GetItemCount(list_); row < static_cast<int>(count); ++row) {
        std::wstring label = std::to_wstring(row + 1);
        LVITEMW entry{};
        entry.mask = LVIF_TEXT;
        entry.iItem = row;
        entry.pszText = label.data();
        ListView_InsertItem(list_, &entry);
        cells_[static_cast<std::size_t>(row)] = {};
    }
    for (int row = ListView_GetItemCount(list_) - 1; row >= static_cast<int>(count); --row) {
        ListView_DeleteItem(list_, row);
        cells_[static_cast<std::size_t>(row)] = {};
    }
    channelCount_ = count;
    loading_ = wasLoading;
    if (selected_ >= count) selectChannel(count - 1);
    clearResults();
    refreshRows();
    refreshSummary();
}

// 직전 시험 결과 폐기 후 대기 표시 복귀: 판정 패널 시험 시간, 선택 채널 상세, 목록 사후 그리기 열 재도장
void App::clearResults()
{
    for (std::size_t i = 0; i < kMaxChannels; ++i) {
        active_[i] = false;
        views_[i] = TestSnapshot{};
    }
    hasResult_ = false;
    previewDuration();
    refreshDetail();
    if (list_) InvalidateRect(list_, nullptr, FALSE);
}

void App::sizeColumns()
{
    for (int column = 0; column < kColumnCount; ++column)
        ListView_SetColumnWidth(list_, column, dluToPixels(kColumns[column].width));
}

void App::focusChannel(std::size_t index, int field)
{
    selectChannel(index);
    ListView_EnsureVisible(list_, static_cast<int>(index), FALSE);
    if (field) SetFocus(item(field));
}

void App::selectChannel(std::size_t index)
{
    if (index >= channelCount_) return;
    selected_ = index;
    if (ListView_GetNextItem(list_, -1, LVNI_SELECTED) != static_cast<int>(index)) {
        const bool wasLoading = loading_;
        loading_ = true;
        ListView_SetItemState(list_, -1, 0, LVIS_SELECTED);
        ListView_SetItemState(list_, static_cast<int>(index), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        loading_ = wasLoading;
    }
    showEditor();
    refreshDetail();
}

void App::showEditor()
{
    const ChannelForm& form = forms_[selected_];
    const bool wasLoading = loading_;
    loading_ = true;
    SetDlgItemTextW(dialog_, IDC_SECTION_EDITOR, (L"채널 설정 · " + channelName(selected_)).c_str());
    ComboBox_SetCurSel(item(IDC_TRANSPORT_COMBO), form.kind);
    ComboBox_SetCurSel(item(IDC_SERIAL_STANDARD), form.standard);
    SetWindowTextW(item(IDC_SERIAL_PORT), form.serialPort.c_str());
    SetWindowTextW(item(IDC_SERIAL_BAUD), form.baud.c_str());
    ComboBox_SetCurSel(item(IDC_SERIAL_DATA), form.dataBits - 5);
    ComboBox_SetCurSel(item(IDC_SERIAL_PARITY), form.parity);
    ComboBox_SetCurSel(item(IDC_SERIAL_STOP), form.stopBits);
    ComboBox_SetCurSel(item(IDC_SERIAL_FLOW), form.flow);
    SetDlgItemTextW(dialog_, IDC_TCP_HOST, form.tcpHost.c_str());
    SetDlgItemTextW(dialog_, IDC_TCP_PORT, form.tcpPort.c_str());
    SetDlgItemTextW(dialog_, IDC_UDP_LOCAL_HOST, form.udpLocalHost.c_str());
    SetDlgItemTextW(dialog_, IDC_UDP_LOCAL_PORT, form.udpLocalPort.c_str());
    SetDlgItemTextW(dialog_, IDC_UDP_REMOTE_HOST, form.udpRemoteHost.c_str());
    SetDlgItemTextW(dialog_, IDC_UDP_REMOTE_PORT, form.udpRemotePort.c_str());
    loading_ = wasLoading;
    showTransportPanel();
    applySerialRules();
}

void App::showTransportPanel()
{
    const int kind = forms_[selected_].kind;
    for (int id : kSerialControls) ShowWindow(item(id), kind == kSerial ? SW_SHOW : SW_HIDE);
    for (int id : kTcpControls) ShowWindow(item(id), kind == kTcp ? SW_SHOW : SW_HIDE);
    for (int id : kUdpControls) ShowWindow(item(id), kind == kUdp ? SW_SHOW : SW_HIDE);
}

// 흐름 제어는 RS-232 전용, 송신 반향 선택은 RS-485 2선식 전용(타 방식 선택 중에도 선택값 보존)
void App::applySerialRules()
{
    ChannelForm& form = forms_[selected_];
    if (form.standard != kRs232 && form.flow != 0) {
        form.flow = 0;
        ComboBox_SetCurSel(item(IDC_SERIAL_FLOW), 0);
    }
    const bool halfDuplex = form.standard == kRs485TwoWire;
    ComboBox_SetCurSel(item(IDC_SERIAL_ECHO), halfDuplex ? form.echo : -1);
    EnableWindow(item(IDC_SERIAL_FLOW), !running_ && form.standard == kRs232);
    EnableWindow(item(IDC_SERIAL_ECHO), !running_ && halfDuplex);
}

void App::refreshPorts()
{
    const std::vector<PortInfo> ports = enumeratePorts();
    realcomPorts_.clear();
    const HWND combo = item(IDC_SERIAL_PORT);
    const bool wasLoading = loading_;
    loading_ = true;
    ComboBox_ResetContent(combo);
    const HDC dc = GetDC(combo);
    const HGDIOBJ oldFont = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(combo, WM_GETFONT, 0, 0)));
    LONG widest = 0;
    for (const PortInfo& port : ports) {
        if (port.realcom) realcomPorts_.insert(port.name);
        std::wstring label = port.name;
        if (port.realcom) label += L" · Real COM";
        if (!port.description.empty()) label += L" · " + port.description;
        ComboBox_AddString(combo, label.c_str());
        SIZE size{};
        GetTextExtentPoint32W(dc, label.c_str(), static_cast<int>(label.size()), &size);
        widest = (std::max)(widest, size.cx);
    }
    SelectObject(dc, oldFont);
    ReleaseDC(combo, dc);
    SendMessageW(combo, CB_SETDROPPEDWIDTH, static_cast<WPARAM>(widest + GetSystemMetrics(SM_CXVSCROLL) + 16), 0);
    SetWindowTextW(combo, forms_[selected_].serialPort.c_str());
    loading_ = wasLoading;
    refreshRows();
    appendLog(L"COM 포트 목록 갱신. " + std::to_wstring(ports.size()) + L"개 (Real COM " +
              std::to_wstring(realcomPorts_.size()) + L"개).");
}

std::wstring* App::formField(int id)
{
    ChannelForm& form = forms_[selected_];
    switch (id) {
    case IDC_TCP_HOST:        return &form.tcpHost;
    case IDC_TCP_PORT:        return &form.tcpPort;
    case IDC_UDP_LOCAL_HOST:  return &form.udpLocalHost;
    case IDC_UDP_LOCAL_PORT:  return &form.udpLocalPort;
    case IDC_UDP_REMOTE_HOST: return &form.udpRemoteHost;
    case IDC_UDP_REMOTE_PORT: return &form.udpRemotePort;
    default:                  return nullptr;
    }
}

std::wstring App::kindText(const ChannelForm& form) const
{
    switch (form.kind) {
    case kTcp: return L"TCP";
    case kUdp: return L"UDP";
    default:   return realcomPorts_.count(portToken(form.serialPort)) ? L"Real COM" : L"직렬";
    }
}

void App::refreshRows()
{
    if (!list_) return;
    for (std::size_t row = 0; row < channelCount_; ++row) {
        const ChannelForm& form = forms_[row];
        const TestSnapshot& view = views_[row];
        const bool shown = active_[row];
        setCell(row, kColKind, kindText(form));
        setCell(row, kColTarget, targetText(form));
        setCell(row, kColSent, shown ? grouped(view.sent) : std::wstring());
        setCell(row, kColMatched, shown ? grouped(view.matched) : std::wstring());
        setCell(row, kColFailed, shown ? grouped(view.failed) : std::wstring());
        setCell(row, kColLatency, !shown ? std::wstring() : view.matched ? milliseconds(view.maxLatency) : L"—");

        COLORREF face{};
        COLORREF ink{};
        std::wstring label;
        rowVerdict(row, face, ink, label);
        // 판정·최근 열은 사후 그리기 표시. 판정 변경 또는 시험 중 이력 갱신 시 해당 셀 재도장
        auto& cached = cells_[row][kColVerdict];
        RECT cell{};
        if (cached != label) {
            cached = label;
            if (ListView_GetSubItemRect(list_, static_cast<int>(row), kColVerdict, LVIR_BOUNDS, &cell))
                InvalidateRect(list_, &cell, FALSE);
        }
        if (running_ && shown && ListView_GetSubItemRect(list_, static_cast<int>(row), kColRecent, LVIR_BOUNDS, &cell))
            InvalidateRect(list_, &cell, FALSE);
    }
}

void App::setCell(std::size_t row, int column, const std::wstring& value)
{
    auto& cached = cells_[row][static_cast<std::size_t>(column)];
    if (cached == value) return;
    cached = value;
    ListView_SetItemText(list_, static_cast<int>(row), column, cached.data());
}

bool App::rowVerdict(std::size_t row, COLORREF& face, COLORREF& ink, std::wstring& label) const
{
    face = ui::palette::Muted;
    ink = ui::palette::TextMuted;
    label = L"대기";
    if (!active_[row]) return true;
    const TestSnapshot& view = views_[row];
    ink = ui::palette::White;
    if (view.verdict == Verdict::Fail) {
        face = ui::palette::Fault;
        label = L"FAIL";
    } else if (view.running) {
        face = ui::palette::Running;
        label = L"시험 중";
    } else if (view.verdict == Verdict::Ok) {
        face = ui::palette::Pass;
        label = L"OK";
    } else if (view.verdict == Verdict::Stopped) {
        face = ui::palette::Halted;
        label = L"중지";
    } else {
        face = ui::palette::Muted;
        ink = ui::palette::TextMuted;
    }
    return true;
}

LRESULT App::onListCustomDraw(NMLVCUSTOMDRAW& draw)
{
    switch (draw.nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT:
        draw.clrText = ui::palette::TextStrong;
        return CDRF_NEWFONT | CDRF_NOTIFYPOSTPAINT;
    case CDDS_ITEMPOSTPAINT: {
        const auto row = static_cast<std::size_t>(draw.nmcd.dwItemSpec);
        if (row >= channelCount_) return CDRF_DODEFAULT;
        const int base = ui::basePixels(dialog_);
        COLORREF face{};
        COLORREF ink{};
        std::wstring label;
        RECT cell{};
        if (rowVerdict(row, face, ink, label) &&
            ListView_GetSubItemRect(list_, static_cast<int>(row), kColVerdict, LVIR_BOUNDS, &cell))
            ui::paintPill(draw.nmcd.hdc, cell, fonts_, base, face, ink, label);
        if (active_[row] && ListView_GetSubItemRect(list_, static_cast<int>(row), kColRecent, LVIR_BOUNDS, &cell))
            ui::paintSparkline(draw.nmcd.hdc, cell, base, views_[row].recent);
        return CDRF_DODEFAULT;
    }
    default:
        return CDRF_DODEFAULT;
    }
}

void App::refreshSummary()
{
    SetDlgItemTextW(dialog_, IDC_CHANNEL_SUMMARY,
                    (L"사용 " + std::to_wstring(channelCount_) + L"채널 · 최대 " + std::to_wstring(kMaxChannels) +
                     L"채널").c_str());
    if (!running_) InvalidateRect(item(IDC_VERDICT), nullptr, FALSE);
}

void App::refreshDetail()
{
    const std::wstring detail = channelDetail();
    if (detail == detailText_) return;
    detailText_ = detail;
    SetDlgItemTextW(dialog_, IDC_CHANNEL_DETAIL, detailText_.c_str());
}

void App::previewDuration()
{
    if (running_ || hasResult_) return;
    unsigned long long hours = 0;
    unsigned long long minutes = 0;
    const bool valid = parseNumber(trimmed(text(IDC_DURATION_HOURS)), kMaxHours, hours) &&
                       parseNumber(trimmed(text(IDC_DURATION_MINUTES)), 59, minutes);
    const unsigned long long seconds = valid ? (std::min)(hours * 3600 + minutes * 60, kMaxHours * 3600) : 0;
    duration_ = std::chrono::seconds(static_cast<long long>(seconds));
    InvalidateRect(item(IDC_VERDICT), nullptr, FALSE);
}

void App::setInputsEnabled(bool enabled)
{
    for (int id : kInputControls) EnableWindow(item(id), enabled);
    EnableWindow(item(IDC_START_BUTTON), enabled);
    EnableWindow(item(IDC_STOP_BUTTON), !enabled);
    if (enabled) applySerialRules();
}

std::wstring App::text(int id) const
{
    const HWND control = item(id);
    const int length = GetWindowTextLengthW(control);
    std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(control, value.data(), length + 1);
    value.resize(static_cast<std::size_t>((std::max)(copied, 0)));
    return value;
}

void App::appendLog(const std::wstring& line, std::chrono::system_clock::time_point when)
{
    const std::time_t seconds = std::chrono::system_clock::to_time_t(when);
    std::tm local{};
    localtime_s(&local, &seconds);
    const long long millis = std::chrono::duration_cast<std::chrono::milliseconds>(when.time_since_epoch()).count() % 1000;
    wchar_t stamp[32]{};
    std::swprintf(stamp, std::size(stamp), L"%02d-%02d %02d:%02d:%02d.%03lld   ", local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min, local.tm_sec, millis);
    const std::wstring entry = stamp + indentContinuation(line, std::wcslen(stamp) + 4) + L"\r\n";

    const HWND log = item(IDC_LOG_TEXT);
    int length = GetWindowTextLengthW(log);
    if (length + static_cast<int>(entry.size()) > kLogLimit) {
        const auto line0 = SendMessageW(log, EM_LINEFROMCHAR, static_cast<WPARAM>(length / 2), 0);
        const auto cut = SendMessageW(log, EM_LINEINDEX, static_cast<WPARAM>(line0 + 1), 0);
        SendMessageW(log, EM_SETSEL, 0, cut > 0 ? cut : length / 2);
        SendMessageW(log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
        length = GetWindowTextLengthW(log);
    }
    SendMessageW(log, EM_SETSEL, static_cast<WPARAM>(length), static_cast<LPARAM>(length));
    SendMessageW(log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(entry.c_str()));
}

void App::appendChannelEvents(std::size_t channel, const std::vector<TestEvent>& events)
{
    for (const auto& event : events) appendLog(channelName(channel) + L" · " + event.text, event.wall);
}

void App::refreshFonts()
{
    LOGFONTW lf{};
    lf.lfHeight = -ui::basePixels(dialog_);
    lf.lfWeight = FW_BOLD;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lstrcpynW(lf.lfFaceName, L"Malgun Gothic", LF_FACESIZE);
    const HFONT font = CreateFontIndirectW(&lf);
    if (!font) return;
    for (int id : kSections) SendMessageW(item(id), WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    if (sectionFont_) DeleteObject(sectionFont_);
    sectionFont_ = font;
}

void App::invalidatePanels()
{
    InvalidateRect(item(IDC_VERDICT), nullptr, FALSE);
}

void App::releaseSystemHolds()
{
    if (timerRaised_) {
        timeEndPeriod(1);
        timerRaised_ = false;
    }
    if (sleepBlocked_) {
        SetThreadExecutionState(ES_CONTINUOUS);
        sleepBlocked_ = false;
    }
}

App::Overall App::overall() const
{
    Overall result;
    std::vector<std::size_t> failed;
    for (std::size_t i = 0; i < kMaxChannels; ++i) {
        if (!active_[i]) continue;
        const TestSnapshot& view = views_[i];
        ++result.channels;
        result.matched += view.matched;
        result.elapsed = (std::max)(result.elapsed, view.elapsed);
        result.running = result.running || view.running;
        if (view.verdict == Verdict::Fail) {
            ++result.failedChannels;
            failed.push_back(i);
        } else if (view.verdict == Verdict::Ok) {
            ++result.passed;
        } else if (view.verdict == Verdict::Stopped) {
            ++result.stopped;
        }
        if (view.hasFailure && (!result.hasFailure || view.firstFailureAt < result.firstFailureAt)) {
            result.hasFailure = true;
            result.firstFailureChannel = i;
            result.firstFailureAt = view.firstFailureAt;
            result.firstFailure = view.firstFailure;
        }
    }
    result.failedList = channelRanges(failed);
    return result;
}

ui::VerdictView App::verdictView() const
{
    const Overall result = overall();
    const auto total = std::chrono::duration_cast<std::chrono::milliseconds>(duration_);
    const bool started = running_ || hasResult_;
    const std::chrono::milliseconds elapsed = started ? (std::min)(result.elapsed, total) : std::chrono::milliseconds(0);

    ui::VerdictView view;
    view.progress = total.count() > 0 ? static_cast<double>(elapsed.count()) / static_cast<double>(total.count()) : 0.0;
    view.elapsed = clockText(elapsed) + L" / " + clockText(total);
    if (started) {
        wchar_t percent[16]{};
        std::swprintf(percent, std::size(percent), L"%.1f %%", (std::min)(1.0, view.progress) * 100.0);
        view.percent = percent;
    }

    if (!started) {
        view.color = ui::palette::Idle;
        view.state = L"대기";
        view.detail = L"사용 " + std::to_wstring(channelCount_) + L"채널 · 송신 주기 50 ms · DATA 32 Byte";
    } else if (result.failedChannels) {
        view.color = ui::palette::Fault;
        view.state = L"FAIL";
        view.detail = L"FAIL " + std::to_wstring(result.failedChannels) + L"채널 (" + result.failedList + L") · 최초 " +
                      channelName(result.firstFailureChannel) + L" " + clockText(result.firstFailureAt) + L" " +
                      result.firstFailure + (running_ ? L" · 시험 계속" : L"");
    } else if (running_) {
        view.color = ui::palette::Running;
        view.state = L"시험 중";
        view.detail = L"사용 " + std::to_wstring(result.channels) + L"채널 · 정상 회신 " + grouped(result.matched) +
                      L"건 · " + (elapsed >= total ? std::wstring(L"잔여 회신 대기") : L"잔여 " + clockText(total - elapsed));
    } else if (result.stopped) {
        view.color = ui::palette::Halted;
        view.state = L"중지";
        view.detail = L"시험 시간 미완료 · 판정 보류";
    } else {
        view.color = ui::palette::Pass;
        view.state = L"OK";
        view.detail = L"사용 " + std::to_wstring(result.channels) + L"채널 전 채널 정상 · 정상 회신 " +
                      grouped(result.matched) + L"건";
    }
    return view;
}

std::wstring App::channelDetail() const
{
    const std::wstring name = channelName(selected_) + L" · ";
    if (!active_[selected_]) return name + L"시험 이력 없음.";
    const TestSnapshot& view = views_[selected_];
    const std::wstring latency = view.matched
                                     ? L"최대 지연 " + milliseconds(view.maxLatency) + L" · 평균 " + milliseconds(view.meanLatency)
                                     : std::wstring(L"정상 회신 없음");
    const std::wstring failure = !view.abortReason.empty() ? L"시험 중단: " + view.abortReason
                                 : view.hasFailure ? L"최초 실패 " + clockText(view.firstFailureAt, true) + L" · " + view.firstFailure
                                                   : std::wstring(L"실패 없음");
    return name + latency + L"\n" + failure;
}

int App::dluToPixels(int units) const
{
    RECT rect{0, 0, units, 0};
    MapDialogRect(dialog_, &rect);
    return rect.right;
}

}  // namespace loopback
