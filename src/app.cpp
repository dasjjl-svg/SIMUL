#include "app.h"

#include <mmsystem.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <ctime>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <thread>
#include <utility>

#include "format.h"
#include "resource.h"
#include "transport.h"

namespace loopback {

namespace {

constexpr UINT_PTR kRefreshTimer = 1;
constexpr UINT kRefreshMs = 100;
constexpr UINT_PTR kCountTimer = 2;
constexpr UINT kCountDelayMs = 400;  // 채널 수 입력 확정 대기. 중간 입력값의 채널 삭제 방지
constexpr UINT kRefreshFontsMessage = WM_APP + 1;
constexpr UINT kComboSyncMessage = WM_APP + 2;
constexpr UINT kMaximizeMessage = WM_APP + 3;
constexpr int kLogLimit = 2000000;
constexpr SIZE_T kResidentBytes = 64 * 1024 * 1024;  // 시험 중 작업 집합 상주 하한

constexpr const wchar_t* kAppTitle = L"Loopback Tester";
constexpr const wchar_t* kRegistryKey = L"Software\\LoopbackTester";
constexpr wchar_t kFieldSeparator = L'\x1F';
constexpr std::size_t kFieldCount = 15;
constexpr std::size_t kFieldLimit = 64;

constexpr unsigned long long kMinBaud = 50;
constexpr unsigned long long kMaxBaud = 4000000;
constexpr unsigned long long kMinReplyWindow = 1;     // 회신 허용 시퀀스 수 범위
constexpr unsigned long long kMaxReplyWindow = 1000;
constexpr const wchar_t* kDefaultReplyWindow = L"5";
constexpr unsigned long long kMinInterval = 10;       // 송신 주기 범위(ms)
constexpr unsigned long long kMaxInterval = 10000;
constexpr const wchar_t* kDefaultInterval = L"50";
constexpr unsigned long long kSteadyInterval = 20;    // 미만 송신 주기는 PC 부하 시 주기 이탈 판정 가능
constexpr unsigned long long kMaxHours = 72;
constexpr const wchar_t* kDefaultData = L"32";        // DATA 길이 범위: MIN_DATA_SIZE~MAX_DATA_SIZE Byte

// 평균 전문 길이: STX·FF FF·ETX 4B + 시퀀스·DATA·CRC(DATA 길이 + 4B)의 DLE 치환 기대치(바이트당 3/256 확률 1B 추가)
double meanFrameBytes(std::size_t dataSize)
{
    return 4.0 + static_cast<double>(dataSize + 4) * (1.0 + 3.0 / 256.0);
}

enum TransportIndex { kSerial = 0, kTcp = 1, kUdp = 2 };
enum StandardIndex { kRs232 = 0, kRs422 = 1, kRs485TwoWire = 2, kRs485FourWire = 3 };
enum EchoIndex { kEchoUnset = 0, kEchoPresent = 1, kEchoAbsent = 2 };
enum Column {
    kColChannel, kColKind, kColTarget, kColVerdict, kColSent, kColMatched, kColFailed, kColLatency, kColTrend,
    kColumnCount
};

struct ColumnSpec {
    const wchar_t* title;
    int width;     // 최소 창 기준 폭(DLU)
    int grow;      // 목록 확장분 배분 비율
    bool counter;  // 건수 열: 7자리 천 단위 구분 표기 수용 폭 보장
    int format;
};

// 최소 창 기준 합계 384 DLU: 목록 폭 400에서 테두리·세로 스크롤 막대 여유 제외
constexpr ColumnSpec kColumns[kColumnCount] = {
    {L"CH", 18, 0, false, LVCFMT_LEFT},        {L"접속", 42, 1, false, LVCFMT_LEFT},
    {L"대상", 100, 3, false, LVCFMT_LEFT},     {L"판정", 34, 1, false, LVCFMT_CENTER},
    {L"송신", 44, 1, true, LVCFMT_RIGHT},      {L"정상 회신", 44, 1, true, LVCFMT_RIGHT},
    {L"이상", 42, 1, true, LVCFMT_RIGHT},      {L"최대 지연", 36, 1, false, LVCFMT_RIGHT},
    {L"추이", 24, 4, false, LVCFMT_LEFT},
};
constexpr int kCellPadding = 10;    // 목록 셀 좌우 여백 합(DLU)
constexpr int kScrollReserve = 12;  // 세로 스크롤 막대 여유(DLU)
constexpr int kTargetFloor = 72;    // 폭 부족 시 대상 열 축소 하한(DLU)

// 창 확장분 배분 비율(%): 좌·상·우·하 경계별. 표 외 컨트롤은 우측 열 우측 이동, 좌측 열 고정
struct Anchor {
    int id;
    std::uint8_t left;
    std::uint8_t top;
    std::uint8_t right;
    std::uint8_t bottom;
};
constexpr std::uint8_t kListShare = 50;  // 세로 확장분 중 채널 목록 몫. 나머지는 시험 기록
constexpr Anchor kAnchors[] = {
    {IDC_HEADER, 0, 0, 100, 0},
    {IDC_VERDICT, 0, 0, 100, 0},
    {IDC_CHANNEL_SUMMARY, 100, 0, 100, 0},
    {IDC_CHANNEL_LIST, 0, 0, 100, kListShare},
    {IDC_CHANNEL_DETAIL, 100, 0, 100, kListShare},
    {IDC_SECTION_LOG, 0, kListShare, 100, kListShare},
    {IDC_LOG_TEXT, 0, kListShare, 100, 100},
};
constexpr int kRightColumn = 418;  // 우측 열 경계(DLU). 좌측 열 우단 412, 우측 열 좌단 424

constexpr const wchar_t* kKindNames[] = {L"직렬 (RS-232·422·485)", L"TCP 클라이언트", L"UDP"};
constexpr const wchar_t* kStandardNames[] = {L"RS-232", L"RS-422", L"RS-485 2선식", L"RS-485 4선식"};
constexpr const wchar_t* kBaudRates[] = {L"300",   L"600",   L"1200",   L"2400",   L"4800",   L"9600",   L"14400",
                                         L"19200", L"38400", L"57600", L"115200", L"230400", L"460800", L"921600"};
constexpr const wchar_t* kDataBits[] = {L"5", L"6", L"7", L"8"};
constexpr const wchar_t* kParityNames[] = {L"없음 (N)", L"홀수 (O)", L"짝수 (E)", L"마크 (M)", L"스페이스 (S)"};
constexpr const wchar_t* kStopNames[] = {L"1", L"1.5", L"2"};
constexpr const wchar_t* kFlowNames[] = {L"없음", L"RTS/CTS", L"DTR/DSR"};
constexpr const wchar_t* kEchoNames[] = {L"선택 필요", L"발생 (반향 제거)", L"미발생"};

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
constexpr int kInputControls[] = {IDC_CHANNEL_COUNT, IDC_CHANNEL_COUNT_SPIN, IDC_ECHO_TIMEOUT, IDC_SEND_INTERVAL,
                                  IDC_DATA_SIZE, IDC_DURATION_HOURS,
                                  IDC_DURATION_HOURS_SPIN, IDC_DURATION_MINUTES, IDC_DURATION_MINUTES_SPIN,
                                  IDC_TRANSPORT_COMBO, IDC_SERIAL_STANDARD, IDC_SERIAL_PORT, IDC_SERIAL_REFRESH,
                                  IDC_SERIAL_BAUD, IDC_SERIAL_DATA, IDC_SERIAL_PARITY, IDC_SERIAL_STOP,
                                  IDC_SERIAL_FLOW, IDC_SERIAL_ECHO, IDC_TCP_HOST, IDC_TCP_PORT, IDC_UDP_LOCAL_HOST,
                                  IDC_UDP_LOCAL_PORT, IDC_UDP_REMOTE_HOST, IDC_UDP_REMOTE_PORT};
constexpr int kMutedLabels[] = {IDC_CHANNEL_SUMMARY, IDC_SERIAL_BAUD_UNIT, IDC_SERIAL_ECHO_HINT,
                                IDC_DURATION_HOURS_UNIT, IDC_DURATION_MINUTES_UNIT, IDC_ECHO_UNIT, IDC_INTERVAL_UNIT,
                                IDC_DATA_UNIT};
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
    static const wchar_t* const standards[] = {L"", L" · 422", L" · 485 2선", L" · 485 4선"};
    switch (form.kind) {
    case kTcp:
        return trimmed(form.tcpHost) + L":" + trimmed(form.tcpPort);
    case kUdp:
        return trimmed(form.udpRemoteHost) + L":" + trimmed(form.udpRemotePort);
    default:
        return portToken(form.serialPort) + L" · " + trimmed(form.baud) + L" " + std::to_wstring(form.dataBits) +
               parities[std::clamp(form.parity, 0, 4)] + kStopNames[std::clamp(form.stopBits, 0, 2)] +
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

// 기록 시각 표기 "MM-DD hh:mm:ss.mmm" + 구분 공백
std::wstring stampText(std::chrono::system_clock::time_point when)
{
    const std::time_t seconds = std::chrono::system_clock::to_time_t(when);
    std::tm local{};
    localtime_s(&local, &seconds);
    const long long millis = std::chrono::duration_cast<std::chrono::milliseconds>(when.time_since_epoch()).count() % 1000;
    wchar_t stamp[32]{};
    std::swprintf(stamp, std::size(stamp), L"%02d-%02d %02d:%02d:%02d.%03lld   ", local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min, local.tm_sec, millis);
    return stamp;
}

// 상위 폴더 포함 생성. 기존 폴더 허용
bool makeFolder(const std::wstring& path)
{
    if (CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) return true;
    if (GetLastError() != ERROR_PATH_NOT_FOUND) return false;
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos || slash == 0 || !makeFolder(path.substr(0, slash))) return false;
    return CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

// 시스템 오류 코드 표기 "오류 N: 설명"
std::wstring errorText(DWORD code)
{
    const std::wstring detail = systemText(code);
    return L"오류 " + std::to_wstring(code) + (detail.empty() ? std::wstring() : L": " + detail);
}

// UTF-8 변환 후 기록. buffer: 변환 버퍼 재사용
bool writeUtf8(HANDLE file, const std::wstring& text, std::string& buffer)
{
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr,
                                         nullptr);
    if (size <= 0) return true;
    buffer.resize(static_cast<std::size_t>(size));
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), buffer.data(), size, nullptr, nullptr);
    DWORD written = 0;
    return WriteFile(file, buffer.data(), static_cast<DWORD>(size), &written, nullptr) &&
           written == static_cast<DWORD>(size);
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
    closeLogFile();
    if (surfaceBrush_) DeleteObject(surfaceBrush_);
    if (disabledBrush_) DeleteObject(disabledBrush_);
    for (HICON icon : {bigIcon_, smallIcon_, headerIcon_})
        if (icon) DestroyIcon(icon);
}

int App::run(HINSTANCE instance)
{
    instance_ = instance;
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
        // 대화상자 자동 배율 조정 완료 후 글꼴·배치·열 폭 재산정
        PostMessageW(dialog_, kRefreshFontsMessage, 0, 0);
        return FALSE;
    case kRefreshFontsMessage:
        refreshFonts();
        refreshIcons();
        layout();
        InvalidateRect(dialog_, nullptr, TRUE);
        return TRUE;
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) layout();
        return FALSE;
    case WM_SHOWWINDOW:
        // 기본 최대화 시작. 대화상자 관리자의 최초 표시(원래 크기) 완료 후 적용
        if (wParam && !shown_) {
            shown_ = true;
            if (maximized_) PostMessageW(dialog_, kMaximizeMessage, 0, 0);
        }
        return FALSE;
    case kMaximizeMessage:
        // 최소화 시작(바로 가기 실행 옵션)은 유지, 복원 시 최대화
        if (IsIconic(dialog_)) {
            WINDOWPLACEMENT placement{};
            placement.length = sizeof(placement);
            if (GetWindowPlacement(dialog_, &placement)) {
                placement.flags |= WPF_RESTORETOMAXIMIZED;
                SetWindowPlacement(dialog_, &placement);
            }
        } else {
            ShowWindow(dialog_, SW_MAXIMIZE);
        }
        return TRUE;
    case WM_GETMINMAXINFO:
        // 최소 크기: 대화상자 원형(현재 DPI 환산)
        if (baseWindow_.cx) {
            auto& limits = *reinterpret_cast<MINMAXINFO*>(lParam);
            const int dpi = ui::dpiFor(dialog_);
            limits.ptMinTrackSize.x = MulDiv(baseWindow_.cx, dpi, baseDpi_);
            limits.ptMinTrackSize.y = MulDiv(baseWindow_.cy, dpi, baseDpi_);
        }
        return FALSE;
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
    refreshIcons();

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
    SetDlgItemTextW(dialog_, IDC_ECHO_TIMEOUT, kDefaultReplyWindow);
    SetDlgItemTextW(dialog_, IDC_SEND_INTERVAL, kDefaultInterval);
    SetDlgItemTextW(dialog_, IDC_DATA_SIZE, kDefaultData);

    Edit_LimitText(item(IDC_CHANNEL_COUNT), 2);
    Edit_LimitText(item(IDC_ECHO_TIMEOUT), 4);
    Edit_LimitText(item(IDC_SEND_INTERVAL), 5);
    Edit_LimitText(item(IDC_DATA_SIZE), 4);
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
    recordLayout();
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
    case IDC_SEND_INTERVAL:
    case IDC_DATA_SIZE:
        if (code == EN_CHANGE) {
            conditionsChanged();
            InvalidateRect(item(IDC_VERDICT), nullptr, FALSE);
        }
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
    case IDOK:
        // 소유자 그리기 단추는 기본 단추 지정 불가. 초점 단추의 Enter 입력을 해당 동작으로 연결
        if (GetFocus() == item(IDC_START_BUTTON)) startTest();
        else if (GetFocus() == item(IDC_STOP_BUTTON)) stopTest();
        else if (GetFocus() == item(IDC_SERIAL_REFRESH)) refreshPorts();
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
        anyRunning = anyRunning || views_[i].running;
    }
    appendEvents();
    // 최소화 중 화면 갱신 생략. 시험 기록·판정 수집과 제목 표시줄 상태는 계속
    updateTitle();
    if (!IsIconic(dialog_)) {
        refreshRows();
        refreshDetail();
        refreshSummary();
        invalidateVerdict();
    }
    if (!anyRunning) finishTest();
}

void App::onDrawItem(const DRAWITEMSTRUCT& draw)
{
    const int base = ui::basePixels(dialog_);
    switch (draw.CtlID) {
    case IDC_HEADER:
        ui::paintHeader(draw.hDC, draw.rcItem, fonts_, base, headerIcon_, headerIconSize_, kAppTitle,
                        L"인터페이스 장치 반송 시험", L"v1.0");
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
    // 절전 전후 채널 기록을 먼저 반영하여 시각 순서 유지
    if (event == PBT_APMSUSPEND) {
        for (auto& test : tests_)
            if (test) test->interrupt(L"시스템 절전 진입. 절전 구간 송·수신 불가.");
        appendEvents();
    } else if (event == PBT_APMRESUMEAUTOMATIC) {
        appendEvents();
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
        MessageBoxW(dialog_, (L"시험 조건 확인 필요.\n\n" + warnings + L"\n시험 시작?").c_str(), kAppTitle,
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return;
    saveSettings();

    // 송신 주기 정밀도 확보용 1ms 타이머 분해능. 기본 15.6ms 분해능에서는 주기 편차 ±15ms
    timerRaised_ = timeBeginPeriod(1) == TIMERR_NOERROR;
    const HCURSOR previous = SetCursor(LoadCursorW(nullptr, IDC_WAIT));

    // 채널별 전송로 병렬 개방. TCP 접속 대기 상한 3s의 채널 수 누적 방지
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
        appendLog(channelName(entry.channel) + L" · " + errors[entry.channel]);
    }
    if (!failures.empty()) {
        for (auto& test : prepared)
            if (test) test->stop();
        releaseSystemHolds();
        appendLog(L"접속 실패로 시험 미시작.");
        MessageBoxW(dialog_,
                    (L"접속 실패로 시험 미시작.\n\n" + failures +
                     L"\n포트·주소 설정, 장치 전원·결선, 타 프로그램 점유 여부 확인 후 재시작.").c_str(),
                    kAppTitle, MB_ICONERROR | MB_OK);
        return;
    }

    clearResults();
    for (const Plan& entry : plan) {
        active_[entry.channel] = true;
        tests_[entry.channel] = std::move(prepared[entry.channel]);
        views_[entry.channel].running = true;
    }

    duration_ = plan.front().settings.duration;
    running_ = true;
    // 장시간 무인 시험 중 시스템 절전 진입 방지, 시스템 종료 시 차단 사유 표시
    sleepBlocked_ = SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED) != 0;
    holdProcessResources();
    ShutdownBlockReasonCreate(dialog_, L"반송 시험 진행 중. 종료 시 시험 중지.");
    setInputsEnabled(false);

    std::wstring logError;
    logPath_.clear();
    logFailed_ = false;
    const bool logged = openLogFile(logError);
    const TestSettings& first = plan.front().settings;
    appendLog(L"시험 시작. 사용 " + std::to_wstring(plan.size()) + L"채널 · 시험 시간 " +
              clockText(std::chrono::duration_cast<std::chrono::milliseconds>(duration_)) + L" · 송신 주기 " +
              grouped(static_cast<std::uint64_t>(first.interval.count())) + L" ms · DATA " +
              grouped(first.dataSize) + L" Byte · 회신 허용 시퀀스 " +
              grouped(first.replyWindow) + L"회 (" +
              grouped(static_cast<std::uint64_t>((first.interval * first.replyWindow).count())) + L" ms)");
    for (const Plan& entry : plan) {
        const TransportConfig& transport = entry.settings.transport;
        const bool realcom = transport.kind == TransportKind::Serial && realcomPorts_.count(transport.serial.port);
        appendLog(channelName(entry.channel) + L" · " + describe(transport) +
                  (realcom ? L" · Real COM (통신 규격은 NPort 장치 설정 적용)" : L""));
    }
    appendLog(logged ? L"기록 파일 " + logPath_ + L" (상세 기록 및 송·수신 대조표 포함)"
                     : L"기록 파일 생성 실패 (" + logError + L"). 화면 기록만 유지.");
    // 작업 스레드 기동은 시작 기록 이후. 채널별 기록의 시각 순서 유지
    for (const Plan& entry : plan) tests_[entry.channel]->begin(entry.channel);
    onTimer();
    // 비활성화된 시작 단추의 초점을 채널 목록으로 이동. 중지 단추는 초점 대상 제외
    SetFocus(list_);
}

void App::stopTest()
{
    if (!running_) return;
    if (MessageBoxW(dialog_, L"시험 진행 중. 중지 시 시험 종료, 시험 시간 또는 회신 확인 미완료 채널은 판정 보류 (FAIL 채널은 FAIL 유지).\n\n시험 중지?",
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
    }
    appendEvents();
    for (auto& test : tests_) test.reset();
    running_ = false;
    hasResult_ = true;
    releaseSystemHolds();
    ShutdownBlockReasonDestroy(dialog_);
    setInputsEnabled(true);

    const Overall result = overall();
    if (result.failedChannels) {
        appendLog(L"전체 판정 FAIL. FAIL " + std::to_wstring(result.failedChannels) + L"채널 (" + result.failedList + L").");
    } else if (result.stopped) {
        // 시험 시간 경과 후 중지: 잔여 회신 대기 중 중지
        appendLog(result.elapsed >= std::chrono::duration_cast<std::chrono::milliseconds>(duration_)
                      ? L"전체 판정 보류. 사용자 중지로 회신 확인 미완료."
                      : L"전체 판정 보류. 사용자 중지로 시험 시간 미완료.");
    } else {
        appendLog(L"전체 판정 OK. 사용 " + std::to_wstring(result.channels) + L"채널 전 채널 정상 회신.");
    }
    if (logFile_ != INVALID_HANDLE_VALUE) {
        // 기록 파일 말미 채널별 결과
        const auto now = std::chrono::system_clock::now();
        for (std::size_t i = 0; i < kMaxChannels; ++i) {
            if (!active_[i]) continue;
            const TestSnapshot& view = views_[i];
            COLORREF face{};
            COLORREF ink{};
            std::wstring label;
            rowVerdict(i, face, ink, label);
            appendLog(channelName(i) + L" · 판정 " + label + L" · 송신 " + grouped(view.sent) + L"건 · 수신 " +
                          grouped(view.received) + L"건 · 정상 회신 " + grouped(view.matched) + L"건 · 이상 " +
                          grouped(view.failed) + L"건 · 지연 회신 " + grouped(view.late) + L"건 · 최대 지연 " +
                          (view.matched ? msText(view.maxLatency) : std::wstring(L"—")) + L" · 평균 지연 " +
                          (view.matched ? msText(view.meanLatency) : std::wstring(L"—")) + L" · 최대 송신 지연 " +
                          (view.sent ? msText(view.maxSendDelay) : std::wstring(L"—")),
                      now, {}, false);
        }
        if (logFile_ != INVALID_HANDLE_VALUE) appendLog(L"기록 파일 저장. " + logPath_);
        closeLogFile();
    }
    refreshRows();
    refreshDetail();
    refreshSummary();
    updateTitle();
    invalidateVerdict();
    // 판정·추이 열 최종 상태 반영용 목록 무효화
    InvalidateRect(list_, nullptr, FALSE);
    // 결과 확인 중 Enter 1회에 의한 재시작·결과 소멸 방지. 시작 단추 초점 배제
    SetFocus(list_);
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
        const wchar_t* prompt =
            logPath_.empty() ? L"시험 결과 표시 중. 종료 시 판정·시험 기록 미보존.\n\n프로그램 종료?"
            : logFailed_     ? L"시험 결과 표시 중. 기록 파일 쓰기 실패로 기록 일부만 보존, 종료 시 화면 판정·기록 소거.\n\n프로그램 종료?"
                             : L"시험 결과 표시 중. 종료 시 화면 판정·기록 소거 (시험 기록은 기록 파일 보존).\n\n프로그램 종료?";
        if (MessageBoxW(dialog_, prompt, kAppTitle, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
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

void App::recordLayout()
{
    RECT client{};
    RECT window{};
    if (!GetClientRect(dialog_, &client) || !GetWindowRect(dialog_, &window)) return;
    baseClient_ = {client.right, client.bottom};
    baseWindow_ = {window.right - window.left, window.bottom - window.top};
    baseDpi_ = ui::dpiFor(dialog_);
    RECT split{kRightColumn, 0, kRightColumn, 0};
    MapDialogRect(dialog_, &split);
    placements_.clear();
    for (HWND child = GetWindow(dialog_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        Placement place;
        place.window = child;
        GetWindowRect(child, &place.rect);
        MapWindowPoints(nullptr, dialog_, reinterpret_cast<POINT*>(&place.rect), 2);
        if (place.rect.left >= split.left) place.left = place.right = 100;
        const int id = GetDlgCtrlID(child);
        for (const Anchor& anchor : kAnchors) {
            if (anchor.id != id) continue;
            place.left = anchor.left;
            place.top = anchor.top;
            place.right = anchor.right;
            place.bottom = anchor.bottom;
        }
        placements_.push_back(place);
    }
}

// 최소 창 기준 위치(현재 DPI 환산) + 창 확장분 × 배분 비율
void App::layout()
{
    if (!placements_.empty() && !IsIconic(dialog_)) {
        RECT client{};
        GetClientRect(dialog_, &client);
        const int dpi = ui::dpiFor(dialog_);
        const auto scaled = [dpi, this](LONG value) { return static_cast<LONG>(MulDiv(value, dpi, baseDpi_)); };
        const LONG dx = (std::max)(0L, client.right - scaled(baseClient_.cx));
        const LONG dy = (std::max)(0L, client.bottom - scaled(baseClient_.cy));
        HDWP batch = BeginDeferWindowPos(static_cast<int>(placements_.size()));
        for (const Placement& place : placements_) {
            // 확장 대상 외 컨트롤은 위치만 조정. 콤보 상자 목록 높이 등 기존 크기 유지
            const bool stretch = place.left != place.right || place.top != place.bottom;
            const LONG left = scaled(place.rect.left) + dx * place.left / 100;
            const LONG top = scaled(place.rect.top) + dy * place.top / 100;
            const LONG width = scaled(place.rect.right) + dx * place.right / 100 - left;
            const LONG height = scaled(place.rect.bottom) + dy * place.bottom / 100 - top;
            const UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | (stretch ? 0u : static_cast<UINT>(SWP_NOSIZE));
            if (batch) batch = DeferWindowPos(batch, place.window, nullptr, left, top, width, height, flags);
            else SetWindowPos(place.window, nullptr, left, top, width, height, flags);
        }
        if (batch) EndDeferWindowPos(batch);
        // 확장 컨트롤 전체 재표시(자체 그리기 영역의 우측 정렬 요소 잔상 방지)
        for (const Placement& place : placements_)
            if (place.left != place.right || place.top != place.bottom) InvalidateRect(place.window, nullptr, TRUE);
        InvalidateRect(dialog_, nullptr, TRUE);
    }
    sizeColumns();
}

bool App::windowMaximized() const
{
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (!GetWindowPlacement(dialog_, &placement)) return maximized_;
    return placement.showCmd == SW_SHOWMAXIMIZED ||
           (placement.showCmd == SW_SHOWMINIMIZED && (placement.flags & WPF_RESTORETOMAXIMIZED));
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
            error = L"정지 비트 1.5는 데이터 비트 5 전용. 정지 비트 1 또는 2 지정.";
            field = IDC_SERIAL_STOP;
            return false;
        }
        if (form.stopBits == TWOSTOPBITS && form.dataBits == 5) {
            error = L"데이터 비트 5와 정지 비트 2 조합 불가. 정지 비트 1 또는 1.5 지정.";
            field = IDC_SERIAL_STOP;
            return false;
        }
        // 반향 여부 미지정 상태의 자기 송신 반향은 장치 무회신에도 정상 판정 유발. 시작 전 명시 선택 강제
        if (form.standard == kRs485TwoWire && form.echo == kEchoUnset) {
            error = L"RS-485 2선식 송신 반향 선택 필요. 어댑터가 자기 송신 전문을 수신하면 '발생', 아니면 '미발생' 선택.";
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
    unsigned long long window = 0;
    unsigned long long interval = 0;
    unsigned long long dataSize = 0;
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
    if (!parseNumber(trimmed(text(IDC_ECHO_TIMEOUT)), kMaxReplyWindow, window) || window < kMinReplyWindow) {
        error = L"회신 허용 범위 오류. 1~1,000 시퀀스 지정.";
        field = IDC_ECHO_TIMEOUT;
        return false;
    }
    if (!parseNumber(trimmed(text(IDC_SEND_INTERVAL)), kMaxInterval, interval) || interval < kMinInterval) {
        error = L"송신 주기 범위 오류. 10~10,000 ms 지정.";
        field = IDC_SEND_INTERVAL;
        return false;
    }
    if (!parseNumber(trimmed(text(IDC_DATA_SIZE)), MAX_DATA_SIZE, dataSize) || dataSize < MIN_DATA_SIZE) {
        error = L"DATA 길이 범위 오류. " + grouped(MIN_DATA_SIZE) + L"~" + grouped(MAX_DATA_SIZE) + L" Byte 지정.";
        field = IDC_DATA_SIZE;
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
        entry.settings.interval = std::chrono::milliseconds(static_cast<long long>(interval));
        entry.settings.replyWindow = static_cast<std::uint32_t>(window);
        entry.settings.dataSize = static_cast<std::size_t>(dataSize);
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
                                   L". 8비트 전문 바이트(시퀀스·DATA·FF 등) 전송 불가로 판정 FAIL 예상, 데이터 비트 8 지정 필요.");
        // 반향 오설정 자동 검출은 송신 완료 대기형 직렬 포트 또는 저속 조건 한정
        if (serial.standard == SerialStandard::Rs485TwoWire && serial.localEcho == LocalEcho::Absent)
            add(entry.channel, L"RS-485 2선식 송신 반향 미발생 설정. USB·네트워크 변환기는 송신 반향 오설정 자동 검출 불가, "
                               L"송신 반향 발생 시 장치 무회신도 정상 판정.");
        // 소요 속도: 평균 전문 × 문자당 비트 ÷ 송신 주기. RS-485 2선식은 송신·회신이 한 선로 공유
        const double stop = serial.stopBits == ONESTOPBIT ? 1.0 : serial.stopBits == ONE5STOPBITS ? 1.5 : 2.0;
        const double bits = 1.0 + serial.dataBits + (serial.parity != NOPARITY ? 1.0 : 0.0) + stop;
        const bool shared = serial.standard == SerialStandard::Rs485TwoWire;
        const double period = std::chrono::duration<double>(entry.settings.interval).count();
        const auto required =
            static_cast<unsigned long long>(meanFrameBytes(entry.settings.dataSize) * bits / period * (shared ? 2.0 : 1.0)) + 1;
        if (serial.baud < required)
            add(entry.channel, L"통신 속도 " + grouped(serial.baud) + L" bps. " +
                                   grouped(static_cast<std::uint64_t>(entry.settings.interval.count())) +
                                   L" ms 주기 유지 소요 " +
                                   grouped(required) + L" bps 미달" + (shared ? L" (반이중 송·회신 공유)" : L"") +
                                   L", 판정 FAIL 예상.");
    }
    std::wstring warnings;
    const auto interval = plan.empty() ? 0ULL : static_cast<unsigned long long>(plan.front().settings.interval.count());
    if (interval && interval < kSteadyInterval)
        warnings = L"송신 주기 " + grouped(interval) + L" ms. " + std::to_wstring(kSteadyInterval) +
                   L" ms 미만은 PC 부하 시 송신 주기 이탈(이상) 판정 가능.\n";
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
    if (readRegistryText(key, L"ReplyWindow", value) && parseNumber(value, kMaxReplyWindow, number) &&
        number >= kMinReplyWindow)
        SetDlgItemTextW(dialog_, IDC_ECHO_TIMEOUT, value.c_str());
    if (readRegistryText(key, L"SendInterval", value) && parseNumber(value, kMaxInterval, number) &&
        number >= kMinInterval)
        SetDlgItemTextW(dialog_, IDC_SEND_INTERVAL, value.c_str());
    if (readRegistryText(key, L"DataBytes", value) && parseNumber(value, MAX_DATA_SIZE, number) && number >= MIN_DATA_SIZE)
        SetDlgItemTextW(dialog_, IDC_DATA_SIZE, value.c_str());
    if (readRegistryText(key, L"Maximized", value)) maximized_ = value != L"0";
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
    writeRegistryText(key, L"ReplyWindow", trimmed(text(IDC_ECHO_TIMEOUT)));
    writeRegistryText(key, L"SendInterval", trimmed(text(IDC_SEND_INTERVAL)));
    writeRegistryText(key, L"DataBytes", trimmed(text(IDC_DATA_SIZE)));
    writeRegistryText(key, L"Maximized", windowMaximized() ? L"1" : L"0");
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

// 직전 시험 결과 폐기 후 대기 표시 복귀: 판정 패널 시험 시간, 선택 채널 상세, 요약, 제목, 목록 사후 그리기 열 무효화
void App::clearResults()
{
    for (std::size_t i = 0; i < kMaxChannels; ++i) {
        active_[i] = false;
        views_[i] = TestSnapshot{};
    }
    hasResult_ = false;
    previewDuration();
    refreshDetail();
    refreshSummary();
    updateTitle();
    if (list_) InvalidateRect(list_, nullptr, FALSE);
}

// 최소 창 기준 폭 보장. 목록 확장분은 열별 배분 비율로 추가, 폭 부족분은 대상 열에서 차감
// 건수 열은 7자리 천 단위 구분 표기("8,888,888") 수용 폭 이상
void App::sizeColumns()
{
    if (!list_) return;
    const int counter = ListView_GetStringWidth(list_, L"8,888,888") + dluToPixels(kCellPadding);
    std::array<int, kColumnCount> widths{};
    int total = 0;
    int weights = 0;
    for (std::size_t column = 0; column < widths.size(); ++column) {
        const ColumnSpec& spec = kColumns[column];
        widths[column] = spec.counter ? (std::max)(dluToPixels(spec.width), counter) : dluToPixels(spec.width);
        total += widths[column];
        weights += spec.grow;
    }
    RECT client{};
    GetClientRect(list_, &client);
    // 세로 스크롤 막대 미표시 상태도 막대 폭 확보: 행 증가 시 가로 스크롤 막대 발생 방지
    const bool scrolling = (GetWindowLongPtrW(list_, GWL_STYLE) & WS_VSCROLL) != 0;
    const int spare = static_cast<int>(client.right) - (scrolling ? 0 : dluToPixels(kScrollReserve)) - total;
    if (spare < 0) {
        int& target = widths[kColTarget];
        target = (std::max)(dluToPixels(kTargetFloor), target + spare);
    } else if (weights) {
        int given = 0;
        int cumulative = 0;
        for (std::size_t column = 0; column < widths.size(); ++column) {
            cumulative += kColumns[column].grow;
            const int share = spare * cumulative / weights - given;
            widths[column] += share;
            given += share;
        }
    }
    for (std::size_t column = 0; column < widths.size(); ++column)
        ListView_SetColumnWidth(list_, static_cast<int>(column), widths[column]);
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

// 흐름 제어는 RS-232 전용(타 방식 선택 시 없음으로 초기화), 송신 반향 선택은 RS-485 2선식 전용(타 방식 선택 중 선택값 보존)
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
        setCell(row, kColLatency, !shown ? std::wstring() : view.matched ? msText(view.maxLatency) : L"—");

        COLORREF face{};
        COLORREF ink{};
        std::wstring label;
        rowVerdict(row, face, ink, label);
        // 판정·추이 열은 사후 그리기 표시. 판정 변경 또는 시험 중 이력 갱신 시 해당 셀 무효화
        auto& cached = cells_[row][kColVerdict];
        RECT cell{};
        if (cached != label) {
            cached = label;
            if (ListView_GetSubItemRect(list_, static_cast<int>(row), kColVerdict, LVIR_BOUNDS, &cell))
                InvalidateRect(list_, &cell, FALSE);
        }
        if (running_ && shown && ListView_GetSubItemRect(list_, static_cast<int>(row), kColTrend, LVIR_BOUNDS, &cell))
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

// 미시험 채널은 판정 표시 없음(false)
bool App::rowVerdict(std::size_t row, COLORREF& face, COLORREF& ink, std::wstring& label) const
{
    face = ui::palette::Muted;
    ink = ui::palette::Text;
    label.clear();
    if (!active_[row]) return false;
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
        label = L"보류";
    } else {
        face = ui::palette::Muted;
        ink = ui::palette::Text;
        label = L"대기";
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
        if (active_[row] && ListView_GetSubItemRect(list_, static_cast<int>(row), kColTrend, LVIR_BOUNDS, &cell))
            ui::paintTimeline(draw.nmcd.hdc, cell, base, views_[row].timeline);
        return CDRF_DODEFAULT;
    }
    default:
        return CDRF_DODEFAULT;
    }
}

// 목록 밖으로 스크롤된 채널 포함 판정별 채널 수
void App::refreshSummary()
{
    std::wstring text = L"사용 " + std::to_wstring(channelCount_) + L"채널";
    if (running_ || hasResult_) {
        const Overall result = overall();
        text += L" · FAIL " + std::to_wstring(result.failedChannels);
        if (!running_) {
            text += L" · OK " + std::to_wstring(result.passed);
            if (result.stopped) text += L" · 보류 " + std::to_wstring(result.stopped);
        }
    }
    if (text != summaryText_) {
        summaryText_ = text;
        SetDlgItemTextW(dialog_, IDC_CHANNEL_SUMMARY, summaryText_.c_str());
    }
    if (!running_) InvalidateRect(item(IDC_VERDICT), nullptr, FALSE);
}

// 제목 표시줄 판정 상태. 최소화·가림 상태에서도 작업 표시줄로 확인
void App::updateTitle()
{
    std::wstring title = kAppTitle;
    if (running_ || hasResult_) {
        const ui::VerdictView view = verdictView();
        title += L" · " + view.state + (view.percent.empty() ? std::wstring() : L" " + view.percent);
    }
    if (title == titleText_) return;
    titleText_ = title;
    SetWindowTextW(dialog_, titleText_.c_str());
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

void App::appendLog(const std::wstring& line, std::chrono::system_clock::time_point when,
                    const std::wstring& appendix, bool screen)
{
    const std::wstring stamp = stampText(when);
    const std::size_t indent = stamp.size() + 4;
    if (logFile_ != INVALID_HANDLE_VALUE)
        writeLogFile(stamp + indentContinuation(appendix.empty() ? line : line + L"\r\n" + appendix, indent) + L"\r\n");
    if (!screen) return;
    const std::wstring entry = stamp + indentContinuation(line, indent) + L"\r\n";

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

// 전 채널 기록을 발생 시각 순으로 병합 기록
void App::appendEvents()
{
    std::vector<std::pair<std::size_t, TestEvent>> batch;
    for (std::size_t i = 0; i < kMaxChannels; ++i) {
        if (!tests_[i]) continue;
        for (auto& event : tests_[i]->takeEvents()) batch.emplace_back(i, std::move(event));
    }
    std::stable_sort(batch.begin(), batch.end(),
                     [](const auto& a, const auto& b) { return a.second.wall < b.second.wall; });
    for (const auto& entry : batch)
        appendLog(channelName(entry.first) + L" · " + entry.second.text, entry.second.wall, entry.second.evidence,
                  !entry.second.fileOnly);
}

bool App::openLogFile(std::wstring& error)
{
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t name[48]{};
    std::swprintf(name, std::size(name), L"LoopbackTester_%04u%02u%02u_%02u%02u%02u", static_cast<unsigned>(now.wYear),
                  static_cast<unsigned>(now.wMonth), static_cast<unsigned>(now.wDay), static_cast<unsigned>(now.wHour),
                  static_cast<unsigned>(now.wMinute), static_cast<unsigned>(now.wSecond));
    std::vector<std::wstring> folders;
    std::wstring module(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (length && length < module.size()) {
        module.resize(length);
        const auto slash = module.find_last_of(L"\\/");
        if (slash != std::wstring::npos) folders.push_back(module.substr(0, slash) + L"\\logs");
    }
    std::wstring local(32768, L'\0');
    const DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", local.data(), static_cast<DWORD>(local.size()));
    if (size && size < local.size()) folders.push_back(local.substr(0, size) + L"\\LoopbackTester\\logs");

    DWORD code = ERROR_PATH_NOT_FOUND;
    for (const std::wstring& folder : folders) {
        if (!makeFolder(folder)) {
            code = GetLastError();
            continue;
        }
        // 동일 초 재시작 대비 일련번호 부가
        for (int copy = 1; copy < 100; ++copy) {
            const std::wstring path =
                folder + L"\\" + name + (copy > 1 ? L"_" + std::to_wstring(copy) : std::wstring()) + L".log";
            const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                                            FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) {
                code = GetLastError();
                if (code == ERROR_FILE_EXISTS) continue;
                break;
            }
            // 머리 행 기록으로 쓰기 가능 확인(용량 부족 매체 대비). 실패 시 빈 파일 삭제 후 다음 폴더
            // UTF-8 BOM: 메모장 등 문자 집합 자동 판별
            wchar_t opened[64]{};
            std::swprintf(opened, std::size(opened), L"%04u-%02u-%02u %02u:%02u:%02u", static_cast<unsigned>(now.wYear),
                          static_cast<unsigned>(now.wMonth), static_cast<unsigned>(now.wDay),
                          static_cast<unsigned>(now.wHour), static_cast<unsigned>(now.wMinute),
                          static_cast<unsigned>(now.wSecond));
            if (writeUtf8(file, L"\xFEFF" + std::wstring(kAppTitle) + L" v1.0 시험 기록 (" + opened + L" 시작)\r\n\r\n",
                          logBytes_)) {
                logFile_ = file;
                logPath_ = path;
                return true;
            }
            code = GetLastError();
            CloseHandle(file);
            DeleteFileW(path.c_str());
            break;
        }
    }
    error = errorText(code);
    return false;
}

void App::writeLogFile(const std::wstring& text)
{
    if (logFile_ == INVALID_HANDLE_VALUE || text.empty() || writeUtf8(logFile_, text, logBytes_)) return;
    const DWORD code = GetLastError();
    closeLogFile();
    logFailed_ = true;
    appendLog(L"기록 파일 쓰기 실패 (" + errorText(code) + L"). 이후 화면 기록만 유지.");
}

void App::closeLogFile()
{
    if (logFile_ == INVALID_HANDLE_VALUE) return;
    CloseHandle(logFile_);
    logFile_ = INVALID_HANDLE_VALUE;
}

void App::refreshIcons()
{
    // 제목 표시줄 16, Alt+Tab·작업 표시줄 32(현재 DPI 배율), 상단 로고는 기준 글꼴 2배
    const int dpi = ui::dpiFor(dialog_);
    const auto load = [this](int size) {
        HICON icon = nullptr;
        if (FAILED(LoadIconWithScaleDown(instance_, MAKEINTRESOURCEW(IDI_APP), size, size, &icon))) return HICON{};
        return icon;
    };
    const HICON largeIcon = load(MulDiv(32, dpi, 96));
    const HICON titleIcon = load(MulDiv(16, dpi, 96));
    const int markSize = ui::basePixels(dialog_) * 2;
    const HICON markIcon = load(markSize);
    if (largeIcon) SendMessageW(dialog_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(largeIcon));
    if (titleIcon) SendMessageW(dialog_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(titleIcon));
    // 창 보유분 교체 후 이전 아이콘 해제
    const auto replace = [](HICON& slot, HICON fresh) {
        if (!fresh) return;
        if (slot) DestroyIcon(slot);
        slot = fresh;
    };
    replace(bigIcon_, largeIcon);
    replace(smallIcon_, titleIcon);
    replace(headerIcon_, markIcon);
    if (markIcon) headerIconSize_ = markSize;
    InvalidateRect(item(IDC_HEADER), nullptr, FALSE);
}

void App::refreshFonts()
{
    const HFONT font = fonts_.get(ui::basePixels(dialog_), FW_BOLD);
    if (!font) return;
    for (int id : kSections) SendMessageW(item(id), WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void App::invalidateVerdict()
{
    InvalidateRect(item(IDC_VERDICT), nullptr, FALSE);
}

// 시험 중 프로세스 우선순위 상향, 작업 집합 상주 하한 설정. 타 프로그램 부하·메모리 회수에 의한 송신 주기 지연 방지
void App::holdProcessResources()
{
    const HANDLE process = GetCurrentProcess();
    const DWORD priority = GetPriorityClass(process);
    if (priority == NORMAL_PRIORITY_CLASS && SetPriorityClass(process, ABOVE_NORMAL_PRIORITY_CLASS)) {
        savedPriority_ = priority;
        priorityRaised_ = true;
    }
    SIZE_T minimum = 0;
    SIZE_T maximum = 0;
    DWORD flags = 0;
    if (!GetProcessWorkingSetSizeEx(process, &minimum, &maximum, &flags)) return;
    const SIZE_T resident = (std::max)(minimum, kResidentBytes);
    if (SetProcessWorkingSetSizeEx(process, resident, (std::max)(maximum, resident * 4),
                                   QUOTA_LIMITS_HARDWS_MIN_DISABLE | QUOTA_LIMITS_HARDWS_MAX_DISABLE)) {
        savedWorkingSet_ = {minimum, maximum, flags};
        workingSetHeld_ = true;
    }
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
    if (priorityRaised_) {
        SetPriorityClass(GetCurrentProcess(), savedPriority_);
        priorityRaised_ = false;
    }
    if (workingSetHeld_) {
        SetProcessWorkingSetSizeEx(GetCurrentProcess(), savedWorkingSet_.minimum, savedWorkingSet_.maximum,
                                   savedWorkingSet_.flags);
        workingSetHeld_ = false;
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
        unsigned long long interval = 0;
        unsigned long long dataSize = 0;
        const bool timed = parseNumber(trimmed(text(IDC_SEND_INTERVAL)), kMaxInterval, interval) && interval >= kMinInterval;
        const bool sized = parseNumber(trimmed(text(IDC_DATA_SIZE)), MAX_DATA_SIZE, dataSize) && dataSize >= MIN_DATA_SIZE;
        view.detail = L"사용 " + std::to_wstring(channelCount_) + L"채널 · 송신 주기 " +
                      (timed ? grouped(interval) + L" ms" : std::wstring(L"확인 필요")) + L" · DATA " +
                      (sized ? grouped(dataSize) + L" Byte" : std::wstring(L"길이 확인 필요")) +
                      L" · 시험 시간 최대 72시간";
    } else if (result.failedChannels) {
        view.color = ui::palette::Fault;
        view.state = L"FAIL";
        // 진행 여부를 선두 배치. 채널 목록은 3채널 이하만 표기(전체는 목록·기록 참조)
        std::wstring first = result.firstFailure;
        if (!first.empty() && first.back() == L'.') first.pop_back();
        view.detail = (running_ ? L"시험 계속 · FAIL " : L"FAIL ") + std::to_wstring(result.failedChannels) + L"채널" +
                      (result.failedChannels <= 3 ? L" (" + result.failedList + L")" : std::wstring()) + L" · 최초 " +
                      channelName(result.firstFailureChannel) + L" " + clockText(result.firstFailureAt) + L" " + first;
    } else if (running_) {
        view.color = ui::palette::Running;
        view.state = L"시험 중";
        view.detail = L"사용 " + std::to_wstring(result.channels) + L"채널 · 정상 회신 " + grouped(result.matched) +
                      L"건 · " + (elapsed >= total ? std::wstring(L"잔여 회신 대기") : L"잔여 " + clockText(total - elapsed));
    } else if (result.stopped) {
        view.color = ui::palette::Halted;
        view.state = L"보류";
        view.detail = elapsed >= total ? L"사용자 중지 · 회신 확인 미완료" : L"사용자 중지 · 시험 시간 미완료";
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
                                     ? L"최대 지연 " + msText(view.maxLatency) + L" · 평균 " + msText(view.meanLatency)
                                     : std::wstring(L"정상 회신 없음");
    // 회신 기한 경과 후 도착 건수(해당 전문은 미회신 이상)
    const std::wstring lateness = view.late ? L" · 지연 회신 " + grouped(view.late) + L"건" : std::wstring();
    const std::wstring summary = name + L"수신 " + grouped(view.received) + L"건 · " + latency + lateness;
    // 송신 지연: 예정 송신 시각 대비 PC 측 최대 지연. 1행 폭 초과 시 별도 행(항목 중간 줄바꿈 방지)
    const std::wstring sendDelay = L"최대 송신 지연 " + (view.sent ? msText(view.maxSendDelay) : std::wstring(L"—"));
    const std::wstring joined = summary + L" · " + sendDelay;
    const std::wstring failure = !view.abortReason.empty() ? L"시험 중단 · " + view.abortReason
                                 : view.hasFailure ? L"최초 이상 " + clockText(view.firstFailureAt, true) + L" · " + view.firstFailure
                                                   : std::wstring(L"이상 없음");
    return (fitsDetailLine(joined) ? joined : summary + L"\n" + sendDelay) + L"\n" + failure;
}

bool App::fitsDetailLine(const std::wstring& text) const
{
    const HWND control = item(IDC_CHANNEL_DETAIL);
    RECT client{};
    if (!control || !GetClientRect(control, &client)) return true;
    const HDC dc = GetDC(control);
    if (!dc) return true;
    const HGDIOBJ oldFont = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0)));
    SIZE size{};
    const BOOL measured = GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    SelectObject(dc, oldFont);
    ReleaseDC(control, dc);
    return !measured || size.cx <= client.right;
}

int App::dluToPixels(int units) const
{
    RECT rect{0, 0, units, 0};
    MapDialogRect(dialog_, &rect);
    return rect.right;
}

}  // namespace loopback
