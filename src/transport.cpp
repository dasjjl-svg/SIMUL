#include "transport.h"

#include <cfgmgr32.h>
#include <mstcpip.h>
#include <setupapi.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <cwctype>
#include <map>
#include <string>
#include <utility>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace loopback {

namespace {

constexpr DWORD kConnectTimeoutMs = 3000;  // TCP 접속 대기 상한. UI 스레드 정지 시간 제한
constexpr DWORD kSerialQueueBytes = 65536; // 직렬 드라이버 송·수신 대기열 권고치. 작업 스레드 지연 시 수신 초과 방지
constexpr DWORD kIoGuardMs = 20;         // 대기 상한(COMMTIMEOUTS) 미준수 드라이버 대비 자체 대기 여유. 준수 드라이버 자체 만료 우선
constexpr Byte kUnfilled = 0x5A;         // 수신 버퍼 미기록 판별 표지
constexpr std::size_t kEchoProbeBytes = 8;  // 송신 반향 확정 최소 일치 바이트. STX·시퀀스 외 DATA 5바이트 이상
constexpr int   kSocketBufferBytes = 262144;  // 소켓 수신 버퍼. 작업 스레드 지연 구간 데이터그램 보존
constexpr std::size_t kReceiveBurstBytes = 65536;     // receive 1회 수용 상한. 연속 유입 시 송신 주기 처리 기회 보장
constexpr std::size_t kReceiveBurstDatagrams = 1024;  // UDP receive 1회 수용 데이터그램 상한

struct WinsockBoot {
    int status;
    WinsockBoot() : status(0) { WSADATA data{}; status = WSAStartup(MAKEWORD(2, 2), &data); }
    ~WinsockBoot() { if (!status) WSACleanup(); }
};

int ensureWinsock()
{
    static WinsockBoot boot;
    return boot.status;
}

}  // namespace

std::wstring systemText(DWORD code)
{
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text = length && buffer ? std::wstring(buffer, length) : std::wstring();
    if (buffer) LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' ||
                             text.back() == L' ' || text.back() == L'.'))
        text.pop_back();
    return text;
}

namespace {

// code는 문자열 조립 전 GetLastError·WSAGetLastError 즉시 획득값 전제
std::wstring failure(const std::wstring& what, long code)
{
    std::wstring text = what + L" (오류 " + std::to_wstring(code);
    const std::wstring detail = systemText(static_cast<DWORD>(code));
    if (!detail.empty()) text += L": " + detail;
    return text + L")";
}

long systemError()
{
    return static_cast<long>(GetLastError());
}

long socketError()
{
    return static_cast<long>(WSAGetLastError());
}

std::wstring widen(const std::string& ascii)
{
    return std::wstring(ascii.begin(), ascii.end());
}

std::wstring endpointText(const sockaddr_in& endpoint)
{
    wchar_t address[INET_ADDRSTRLEN]{};
    if (!InetNtopW(AF_INET, &endpoint.sin_addr, address, INET_ADDRSTRLEN)) return L"?";
    return std::wstring(address) + L":" + std::to_wstring(ntohs(endpoint.sin_port));
}

// 지정 주소로 임시 바인드 성공 시 자기 PC 주소
bool isLocalAddress(const in_addr& address)
{
    if ((ntohl(address.s_addr) >> 24) == 127) return true;
    const SOCKET probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (probe == INVALID_SOCKET) return false;
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_addr = address;
    const bool local = bind(probe, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == 0;
    closesocket(probe);
    return local;
}

DWORD waitMilliseconds(std::chrono::milliseconds wait)
{
    if (wait <= std::chrono::milliseconds::zero()) return 0;
    return static_cast<DWORD>((std::min)(wait, std::chrono::milliseconds(60000)).count());
}

// 소켓 수신 가능 또는 wait 경과까지 대기. 수신 버퍼 기존 데이터 존재 시 즉시 반환
bool awaitReadable(SOCKET socket, std::chrono::milliseconds wait, const wchar_t* what, std::wstring& error)
{
    const DWORD waitMs = waitMilliseconds(wait);
    if (!waitMs) return true;
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(socket, &readable);
    timeval limit{static_cast<long>(waitMs / 1000), static_cast<long>(waitMs % 1000 * 1000)};
    if (select(0, &readable, nullptr, nullptr, &limit) == SOCKET_ERROR) {
        const long code = socketError();
        error = failure(what, code);
        return false;
    }
    return true;
}

// 수신 버퍼 확대. 실패 시 기본값 유지(비치명)
void enlargeReceiveBuffer(SOCKET socket)
{
    const int bytes = kSocketBufferBytes;
    setsockopt(socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bytes), sizeof(bytes));
}

class SerialTransport : public Transport {
public:
    SerialTransport(const SerialConfig& config, DWORD sendTimeoutMs) : config_(config), sendTimeoutMs_(sendTimeoutMs)
    {
        const double stop = config.stopBits == ONESTOPBIT ? 1.0 : config.stopBits == ONE5STOPBITS ? 1.5 : 2.0;
        const double bits = 1.0 + config.dataBits + (config.parity != NOPARITY ? 1.0 : 0.0) + stop;
        charMicros_ = config.baud ? bits * 1e6 / static_cast<double>(config.baud) : 0.0;
        txBuffer_.reserve(MAX_FRAME_SIZE);
    }
    ~SerialTransport() override { close(); }

    bool open(std::wstring& error) override
    {
        close();
        const std::wstring path = L"\\\\.\\" + config_.port;
        // 비동기 입출력: 송신 진행 중에도 수신 완료 즉시 처리(동기 핸들은 송·수신 직렬화로 회신 시각 지연)
        handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OVERLAPPED, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            const long code = systemError();
            const wchar_t* hint = code == ERROR_ACCESS_DENIED ? L". 타 프로그램 점유"
                                  : code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND ? L". 포트 미존재"
                                                                                                 : L"";
            error = failure(config_.port + L" 열기 실패" + hint, code);
            return false;
        }
        readEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        writeEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!readEvent_ || !writeEvent_) {
            const long code = systemError();
            error = failure(config_.port + L" 입출력 이벤트 생성 실패", code);
            close();
            return false;
        }

        SetupComm(handle_, kSerialQueueBytes, kSerialQueueBytes);

        DCB dcb{};
        dcb.DCBlength = sizeof(dcb);
        if (!GetCommState(handle_, &dcb)) {
            const long code = systemError();
            error = failure(config_.port + L" 상태 조회 실패", code);
            close();
            return false;
        }
        const bool rs232 = config_.standard == SerialStandard::Rs232;
        const bool halfDuplex = config_.standard == SerialStandard::Rs485TwoWire;
        const FlowControl flow = rs232 ? config_.flow : FlowControl::None;
        dcb.BaudRate = config_.baud;
        dcb.ByteSize = config_.dataBits;
        dcb.Parity   = config_.parity;
        dcb.StopBits = config_.stopBits;
        dcb.fBinary  = TRUE;
        dcb.fParity  = config_.parity != NOPARITY;
        dcb.fOutxCtsFlow = flow == FlowControl::RtsCts;
        dcb.fOutxDsrFlow = flow == FlowControl::DtrDsr;
        dcb.fDsrSensitivity = FALSE;
        dcb.fOutX = FALSE;
        dcb.fInX = FALSE;
        dcb.fNull = FALSE;
        dcb.fErrorChar = FALSE;
        dcb.fAbortOnError = FALSE;
        dcb.fDtrControl = flow == FlowControl::DtrDsr ? DTR_CONTROL_HANDSHAKE : DTR_CONTROL_ENABLE;
        // RS-485 2선식: 송신 구간만 RTS 활성(송신기 방향 전환)
        dcb.fRtsControl = flow == FlowControl::RtsCts ? RTS_CONTROL_HANDSHAKE
                          : halfDuplex                 ? RTS_CONTROL_TOGGLE
                                                       : RTS_CONTROL_ENABLE;
        // 흐름 제어 미사용이어도 XON·XOFF 동일 값은 SetCommState 거부 대상
        dcb.XonChar  = 0x11;
        dcb.XoffChar = 0x13;

        if (!SetCommState(handle_, &dcb)) {
            const long code = systemError();
            // RTS 토글 미지원 드라이버: 장치 자동 방향 제어 전제로 RTS 상시 활성 재시도
            dcb.fRtsControl = RTS_CONTROL_ENABLE;
            if (!halfDuplex || !SetCommState(handle_, &dcb)) {
                error = failure(config_.port + L" 통신 조건 설정 실패", code);
                close();
                return false;
            }
            notice_ = config_.port + L" RTS 토글 방향 제어 미지원 드라이버. 장치 자동 방향 제어 전제로 RTS 상시 활성.";
        }
        // 설정 중 래치된 회선 오류 플래그 해제. 시험 개시 전 이상의 첫 판정 반영 방지
        DWORD stale = 0;
        readWaitMs_ = MAXDWORD;
        if (!applyReadWait(0) || !PurgeComm(handle_, PURGE_RXCLEAR | PURGE_TXCLEAR) ||
            !ClearCommError(handle_, &stale, nullptr)) {
            const long code = systemError();
            error = failure(config_.port + L" 초기화 실패", code);
            close();
            return false;
        }
        return true;
    }

    void close() override
    {
        if (handle_ != INVALID_HANDLE_VALUE) {
            // 진행 중 송신 취소 후 완료 대기. OVERLAPPED·송신 버퍼 해제 전 입출력 종료 보장
            if (writePending_) {
                CancelIoEx(handle_, &writeIo_);
                DWORD ignored = 0;
                GetOverlappedResult(handle_, &writeIo_, &ignored, TRUE);
                writePending_ = false;
            }
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
        if (readEvent_) CloseHandle(readEvent_);
        if (writeEvent_) CloseHandle(writeEvent_);
        readEvent_ = nullptr;
        writeEvent_ = nullptr;
    }

    bool send(const Buffer& frame, std::wstring& error) override
    {
        // 직전 송신 완료 대기. 송신 버퍼·OVERLAPPED 재사용 전제(통상 직전 주기 내 완료)
        if (!finishWrite(error)) return false;
        txBuffer_.assign(frame.begin(), frame.end());
        echoMatched_ = 0;
        echoBroken_ = false;
        ResetEvent(writeEvent_);
        writeIo_ = OVERLAPPED{};
        writeIo_.hEvent = writeEvent_;
        // 반향 대조 구간 기준 시각. 선로 송신 개시는 WriteFile 호출 이후
        writeIssued_ = std::chrono::steady_clock::now();
        if (!WriteFile(handle_, txBuffer_.data(), static_cast<DWORD>(txBuffer_.size()), nullptr, &writeIo_)) {
            const long code = systemError();
            if (code != ERROR_IO_PENDING) {
                error = failure(L"직렬 송신 실패", code);
                return false;
            }
        }
        writePending_ = true;
        return true;
    }

    bool receive(Buffer& inOut, std::chrono::milliseconds wait, std::wstring& error) override
    {
        DWORD queued = 0;
        if (!queuedBytes(queued, error)) return false;
        const DWORD waitMs = queued ? 0 : waitMilliseconds(wait);
        if (!queued && !waitMs) return true;
        if (!applyReadWait(waitMs)) {
            const long code = systemError();
            error = failure(L"직렬 수신 대기 설정 실패", code);
            return false;
        }

        const DWORD toRead = queued ? (std::min)(queued, static_cast<DWORD>(rxChunk_.size()))
                                    : static_cast<DWORD>(rxChunk_.size());
        const auto begun = std::chrono::steady_clock::now();
        DWORD got = 0;
        if (!read(toRead, waitMs, got, error)) return false;
        if (got && config_.standard == SerialStandard::Rs485TwoWire) probeEcho(got);
        inOut.insert(inOut.end(), rxChunk_.begin(), rxChunk_.begin() + got);
        if (waitMs) {
            // 대기 중 도착 바이트의 회선 오류 즉시 수집. 시험 종료 직전 최종 회신의 오류 누락 방지
            if (got && !queuedBytes(queued, error)) return false;
            // 대기 상한 미준수 드라이버(무수신 즉시 반환)의 공회전 방지
            if (!got && std::chrono::steady_clock::now() - begun < std::chrono::milliseconds(waitMs) / 2) Sleep(1);
        }
        return true;
    }

    LineFault takeFault() override
    {
        LineFault taken;
        std::swap(taken, fault_);
        return taken;
    }

    std::chrono::steady_clock::time_point issuedAt(std::chrono::steady_clock::time_point) const override
    {
        return writeIssued_;
    }

    bool flush(std::wstring& error) override { return finishWrite(error); }

    bool takeSendEcho() override
    {
        const bool echoed = sendEcho_;
        sendEcho_ = false;
        return echoed;
    }

    std::wstring notice() const override { return notice_; }

private:
    // 비동기 수신 개시 후 완료 대기. 대기 상한 + 여유 경과 시 자체 취소(취소 전 기록분 유지)
    bool read(DWORD count, DWORD waitMs, DWORD& got, std::wstring& error)
    {
        got = 0;
        ResetEvent(readEvent_);
        readIo_ = OVERLAPPED{};
        readIo_.hEvent = readEvent_;
        // 자체 취소 시 버퍼 기록 여부 판별용 표지. 버퍼형 입출력 드라이버는 취소 완료분 미복사
        std::fill_n(rxChunk_.begin(), count, kUnfilled);
        if (!ReadFile(handle_, rxChunk_.data(), count, nullptr, &readIo_)) {
            const long code = systemError();
            if (code != ERROR_IO_PENDING) {
                error = failure(L"직렬 수신 실패", code);
                return false;
            }
            if (WaitForSingleObject(readEvent_, waitMs + kIoGuardMs) == WAIT_TIMEOUT) CancelIoEx(handle_, &readIo_);
        }
        if (!GetOverlappedResult(handle_, &readIo_, &got, TRUE)) {
            const long code = systemError();
            if (code != ERROR_OPERATION_ABORTED) {
                error = failure(L"직렬 수신 실패", code);
                return false;
            }
            // 취소 완료: 보고 수신분이 버퍼에 미기록(표지 그대로)이면 폐기, 회선 이상 계수
            got = (std::min)(got, count);
            if (got && std::all_of(rxChunk_.begin(), rxChunk_.begin() + got, [](Byte b) { return b == kUnfilled; })) {
                addFault(L"수신 취소로 " + std::to_wstring(got) + L" Byte 미전달 (드라이버 대기 상한 미준수)");
                got = 0;
            }
        }
        return true;
    }

    // 직전 송신 완료 대기·결과 수집. 송신 처리 상한 경과 시 자체 취소 후 송신 시간 초과 판정
    bool finishWrite(std::wstring& error)
    {
        if (!writePending_) return true;
        const bool cancelled = WaitForSingleObject(writeEvent_, sendTimeoutMs_ + kIoGuardMs) == WAIT_TIMEOUT &&
                               CancelIoEx(handle_, &writeIo_);
        DWORD written = 0;
        if (!GetOverlappedResult(handle_, &writeIo_, &written, TRUE)) {
            const long code = systemError();
            writePending_ = false;
            if (cancelled && code == ERROR_OPERATION_ABORTED) {
                error = L"직렬 송신 시간 초과 (" + std::to_wstring(written) + L"/" + std::to_wstring(txBuffer_.size()) +
                        L" Byte, " + std::to_wstring(sendTimeoutMs_) + L" ms)";
                return false;
            }
            error = failure(L"직렬 송신 실패", code);
            return false;
        }
        writePending_ = false;
        if (written != txBuffer_.size()) {
            error = L"직렬 송신 시간 초과 (" + std::to_wstring(written) + L"/" + std::to_wstring(txBuffer_.size()) +
                    L" Byte, " + std::to_wstring(sendTimeoutMs_) + L" ms)";
            return false;
        }
        return true;
    }

    // RS-485 2선식: 송신 진행 중 도착분과 송신 전문 선두 대조. 반이중 선로상 장치 회신 불가 구간
    // 대조 구간: 송신 미완료 + WriteFile 호출 후 전문 선로 시간 이내(송신 완료 통지가 늦은 드라이버의 장치 회신 배제)
    // 구간 경과 후 도착분 또는 불일치 바이트 발생 시 해당 전문 대조 종료. 타 전문 지연·중복 회신 잔여분 배제
    void probeEcho(DWORD got)
    {
        if (echoBroken_) return;
        const auto onWire = std::chrono::duration<double, std::micro>(charMicros_ * static_cast<double>(txBuffer_.size()));
        DWORD written = 0;
        const bool writing = writePending_ && std::chrono::steady_clock::now() - writeIssued_ < onWire &&
                             !GetOverlappedResult(handle_, &writeIo_, &written, FALSE) &&
                             GetLastError() == ERROR_IO_INCOMPLETE;
        if (!writing) {
            echoBroken_ = true;
            return;
        }
        for (DWORD i = 0; i < got; ++i) {
            if (echoMatched_ >= txBuffer_.size() || rxChunk_[i] != txBuffer_[echoMatched_]) {
                echoBroken_ = true;
                return;
            }
            ++echoMatched_;
        }
        if (echoMatched_ >= kEchoProbeBytes) {
            sendEcho_ = true;
            echoBroken_ = true;
        }
    }

    // 수신 대기열 크기 조회. 래치된 회선 오류는 건별 이상으로 기록
    bool queuedBytes(DWORD& bytes, std::wstring& error)
    {
        DWORD errors = 0;
        COMSTAT status{};
        if (!ClearCommError(handle_, &errors, &status)) {
            const long code = systemError();
            error = failure(L"회선 상태 조회 실패", code);
            return false;
        }
        constexpr DWORD lineErrors = CE_FRAME | CE_OVERRUN | CE_RXPARITY | CE_RXOVER | CE_BREAK;
        if (errors & lineErrors) addFault(lineFaultText(errors & lineErrors));
        bytes = status.cbInQue;
        return true;
    }

    // 수신 대기 방식. 0: 즉시 반환. 그 외: 첫 바이트 도착 즉시 또는 대기 상한 경과 시 반환(수신 시각 오차 최소화)
    // 쓰기 상한은 전문 1건 처리 상한 고정. 값 변경 시에만 SetCommTimeouts 호출
    bool applyReadWait(DWORD waitMs)
    {
        if (waitMs == readWaitMs_) return true;
        COMMTIMEOUTS timeouts{};
        timeouts.ReadIntervalTimeout = MAXDWORD;
        timeouts.ReadTotalTimeoutMultiplier = waitMs ? MAXDWORD : 0;
        timeouts.ReadTotalTimeoutConstant = waitMs;
        timeouts.WriteTotalTimeoutConstant = sendTimeoutMs_;
        if (!SetCommTimeouts(handle_, &timeouts)) return false;
        readWaitMs_ = waitMs;
        return true;
    }

    void addFault(std::wstring text)
    {
        if (!fault_.count) fault_.text = std::move(text);
        ++fault_.count;
    }

    static std::wstring lineFaultText(DWORD flags)
    {
        std::wstring text;
        const auto add = [&text](const wchar_t* item) {
            text += text.empty() ? L"회선 이상: " : L"·";
            text += item;
        };
        if (flags & CE_FRAME)    add(L"프레이밍 오류");
        if (flags & CE_RXPARITY) add(L"패리티 오류");
        if (flags & CE_OVERRUN)  add(L"오버런");
        if (flags & CE_RXOVER)   add(L"수신 버퍼 초과");
        if (flags & CE_BREAK)    add(L"브레이크 신호 수신");
        wchar_t code[16]{};
        std::swprintf(code, sizeof(code) / sizeof(code[0]), L" (0x%04lX)", static_cast<unsigned long>(flags));
        return text + code;
    }

    SerialConfig config_;
    DWORD sendTimeoutMs_;
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    HANDLE readEvent_ = nullptr;
    HANDLE writeEvent_ = nullptr;
    OVERLAPPED readIo_{};
    OVERLAPPED writeIo_{};
    bool writePending_ = false;
    std::chrono::steady_clock::time_point writeIssued_{};  // 최근 WriteFile 호출 시각
    double charMicros_ = 0.0;           // 직렬 문자 1개 선로 시간(us)
    Buffer txBuffer_;                   // 진행 중 송신 전문. 송신 완료까지 유지
    std::array<Byte, 4096> rxChunk_{};  // 비동기 수신 버퍼
    DWORD readWaitMs_ = MAXDWORD;
    std::size_t echoMatched_ = 0;       // 송신 진행 중 송신 전문 선두와 일치한 수신 바이트 수
    bool echoBroken_ = false;           // 해당 전문 반향 대조 종료
    bool sendEcho_ = false;
    LineFault fault_;
    std::wstring notice_;
};

class TcpTransport : public Transport {
public:
    TcpTransport(const TcpConfig& config, DWORD sendTimeoutMs) : config_(config), sendTimeoutMs_(sendTimeoutMs) {}
    ~TcpTransport() override { close(); }

    bool open(std::wstring& error) override
    {
        close();
        if (const int boot = ensureWinsock()) {
            error = failure(L"Winsock 초기화 실패", boot);
            return false;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port   = htons(config_.port);
        if (InetPtonA(AF_INET, config_.host.c_str(), &address.sin_addr) != 1) {
            error = L"TCP 상대 주소 형식 오류 (IPv4 필요).";
            return false;
        }
        socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_ == INVALID_SOCKET) {
            const long code = socketError();
            error = failure(L"TCP 소켓 생성 실패", code);
            return false;
        }
        enlargeReceiveBuffer(socket_);
        u_long nonblocking = 1;
        BOOL noDelay = TRUE;  // 소형 전문 주기 송신의 Nagle 지연 방지
        if (ioctlsocket(socket_, FIONBIO, &nonblocking) != 0 ||
            setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay)) != 0) {
            const long code = socketError();
            error = failure(L"TCP 소켓 설정 실패", code);
            close();
            return false;
        }
        if (connect(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 &&
            !awaitConnect(error)) {
            close();
            return false;
        }
        // 미개방 임시 포트 대상 접속 시 자기 자신과 연결되는 TCP 동시 개방 현상 차단
        if (selfConnected()) {
            error = L"TCP 자기 접속 검출. 상대 포트 미개방 상태로 인터페이스 장치 미경유. 상대 주소·포트 확인 필요.";
            close();
            return false;
        }
        return true;
    }

    void close() override
    {
        if (socket_ != INVALID_SOCKET) closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }

    bool send(const Buffer& frame, std::wstring& error) override
    {
        const ULONGLONG deadline = GetTickCount64() + sendTimeoutMs_;
        std::size_t offset = 0;
        while (offset < frame.size()) {
            const int sent = ::send(socket_, reinterpret_cast<const char*>(frame.data() + offset),
                                    static_cast<int>(frame.size() - offset), 0);
            if (sent > 0) {
                offset += static_cast<std::size_t>(sent);
                continue;
            }
            const long code = socketError();
            if (sent == 0 || code != WSAEWOULDBLOCK) {
                error = failure(L"TCP 송신 실패", code);
                return false;
            }
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) {
                error = L"TCP 송신 시간 초과 (" + std::to_wstring(sendTimeoutMs_) + L" ms). 상대 수신 정체.";
                return false;
            }
            fd_set writable;
            FD_ZERO(&writable);
            FD_SET(socket_, &writable);
            timeval wait{0, static_cast<long>((deadline - now) * 1000)};
            if (select(0, nullptr, &writable, nullptr, &wait) < 0) {
                const long waitCode = socketError();
                error = failure(L"TCP 송신 대기 실패", waitCode);
                return false;
            }
        }
        return true;
    }

    bool receive(Buffer& inOut, std::chrono::milliseconds wait, std::wstring& error) override
    {
        if (!awaitReadable(socket_, wait, L"TCP 수신 대기 실패", error)) return false;
        std::array<Byte, 4096> chunk{};
        for (std::size_t taken = 0; taken < kReceiveBurstBytes;) {
            const int got = recv(socket_, reinterpret_cast<char*>(chunk.data()), static_cast<int>(chunk.size()), 0);
            if (got > 0) {
                inOut.insert(inOut.end(), chunk.begin(), chunk.begin() + got);
                taken += static_cast<std::size_t>(got);
                continue;
            }
            if (got == 0) {
                error = L"TCP 상대 접속 종료 (FIN 수신).";
                return false;
            }
            const long code = socketError();
            if (code == WSAEWOULDBLOCK) return true;
            error = failure(L"TCP 수신 실패", code);
            return false;
        }
        return true;
    }

private:
    bool awaitConnect(std::wstring& error)
    {
        const long code = socketError();
        if (code != WSAEWOULDBLOCK) {
            error = failure(L"TCP 접속 실패", code);
            return false;
        }
        fd_set writable;
        fd_set failed;
        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(socket_, &writable);
        FD_SET(socket_, &failed);
        timeval limit{static_cast<long>(kConnectTimeoutMs / 1000), static_cast<long>(kConnectTimeoutMs % 1000 * 1000)};
        const int ready = select(0, nullptr, &writable, &failed, &limit);
        if (ready == 0) {
            error = L"TCP 접속 시간 초과 (" + std::to_wstring(kConnectTimeoutMs) + L" ms). 상대 장치·주소·포트 확인 필요.";
            return false;
        }
        if (ready < 0) {
            const long selectCode = socketError();
            error = failure(L"TCP 접속 대기 실패", selectCode);
            return false;
        }
        int pendingError = 0;
        int length = sizeof(pendingError);
        if (getsockopt(socket_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&pendingError), &length) != 0) {
            const long queryCode = socketError();
            error = failure(L"TCP 접속 상태 조회 실패", queryCode);
            return false;
        }
        if (pendingError != 0 || FD_ISSET(socket_, &failed)) {
            error = failure(L"TCP 접속 실패", pendingError ? pendingError : WSAECONNREFUSED);
            return false;
        }
        return true;
    }

    bool selfConnected() const
    {
        sockaddr_in local{};
        sockaddr_in peer{};
        int localLength = sizeof(local);
        int peerLength = sizeof(peer);
        return getsockname(socket_, reinterpret_cast<sockaddr*>(&local), &localLength) == 0 &&
               getpeername(socket_, reinterpret_cast<sockaddr*>(&peer), &peerLength) == 0 &&
               local.sin_addr.s_addr == peer.sin_addr.s_addr && local.sin_port == peer.sin_port;
    }

    TcpConfig config_;
    DWORD sendTimeoutMs_;
    SOCKET socket_ = INVALID_SOCKET;
};

class UdpTransport : public Transport {
public:
    explicit UdpTransport(const UdpConfig& config) : config_(config) {}
    ~UdpTransport() override { close(); }

    bool open(std::wstring& error) override
    {
        close();
        if (const int boot = ensureWinsock()) {
            error = failure(L"Winsock 초기화 실패", boot);
            return false;
        }
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port   = htons(config_.localPort);
        remote_ = {};
        remote_.sin_family = AF_INET;
        remote_.sin_port   = htons(config_.remotePort);
        if (InetPtonA(AF_INET, config_.localHost.c_str(), &local.sin_addr) != 1) {
            error = L"UDP 수신 주소 형식 오류 (IPv4 필요).";
            return false;
        }
        if (InetPtonA(AF_INET, config_.remoteHost.c_str(), &remote_.sin_addr) != 1) {
            error = L"UDP 상대 주소 형식 오류 (IPv4 필요).";
            return false;
        }
        // 상대 종단이 자기 수신 종단이면 송신 데이터그램이 장치 미경유로 즉시 반송
        if (remote_.sin_port == local.sin_port && isLocalAddress(remote_.sin_addr) &&
            (local.sin_addr.s_addr == htonl(INADDR_ANY) || local.sin_addr.s_addr == remote_.sin_addr.s_addr)) {
            error = L"UDP 수신·상대 종단 동일. 자기 반송으로 인터페이스 장치 미경유. 수신 포트 또는 상대 주소·포트 변경 필요.";
            return false;
        }
        socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == INVALID_SOCKET) {
            const long code = socketError();
            error = failure(L"UDP 소켓 생성 실패", code);
            return false;
        }
        enlargeReceiveBuffer(socket_);
        BOOL exclusive = TRUE;
        if (setsockopt(socket_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
                       sizeof(exclusive)) != 0 ||
            bind(socket_, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
            const long code = socketError();
            const wchar_t* hint = code == WSAEADDRINUSE || code == WSAEACCES ? L". 타 프로그램 점유" : L"";
            error = failure(L"UDP 수신 포트 " + std::to_wstring(config_.localPort) + L" 열기 실패" + hint, code);
            close();
            return false;
        }
        // ICMP 포트 도달 불가 응답에 따른 recvfrom WSAECONNRESET 반복 억제
        BOOL reportReset = FALSE;
        DWORD returned = 0;
        WSAIoctl(socket_, SIO_UDP_CONNRESET, &reportReset, sizeof(reportReset), nullptr, 0, &returned, nullptr, nullptr);
        u_long nonblocking = 1;
        if (ioctlsocket(socket_, FIONBIO, &nonblocking) != 0) {
            const long code = socketError();
            error = failure(L"UDP 소켓 설정 실패", code);
            close();
            return false;
        }
        return true;
    }

    void close() override
    {
        if (socket_ != INVALID_SOCKET) closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }

    bool send(const Buffer& frame, std::wstring& error) override
    {
        const int sent = sendto(socket_, reinterpret_cast<const char*>(frame.data()), static_cast<int>(frame.size()), 0,
                                reinterpret_cast<const sockaddr*>(&remote_), sizeof(remote_));
        if (sent == SOCKET_ERROR) {
            const long code = socketError();
            error = failure(L"UDP 송신 실패", code);
            return false;
        }
        if (static_cast<std::size_t>(sent) != frame.size()) {
            error = L"UDP 분할 송신 발생.";
            return false;
        }
        return true;
    }

    bool receive(Buffer& inOut, std::chrono::milliseconds wait, std::wstring& error) override
    {
        if (!awaitReadable(socket_, wait, L"UDP 수신 대기 실패", error)) return false;
        std::array<Byte, 2048> chunk{};
        for (std::size_t datagrams = 0; datagrams < kReceiveBurstDatagrams; ++datagrams) {
            sockaddr_in from{};
            int fromLength = sizeof(from);
            const int got = recvfrom(socket_, reinterpret_cast<char*>(chunk.data()), static_cast<int>(chunk.size()), 0,
                                     reinterpret_cast<sockaddr*>(&from), &fromLength);
            // 0 Byte 데이터그램도 정상 수신 반환값. 오류 코드는 SOCKET_ERROR일 때만 유효
            if (got != SOCKET_ERROR) {
                // 상대 주소 외 송신원 데이터그램은 판정 대상 제외, 회선 이상으로 계수. 포트는 장치별 임시 포트 회신 고려로 미대조
                if (from.sin_addr.s_addr != remote_.sin_addr.s_addr) {
                    addFault(L"상대 외 송신원 데이터그램 수신 (" + endpointText(from) + L", " + std::to_wstring(got) + L" Byte)");
                    continue;
                }
                if (got == 0) {
                    addFault(L"빈 데이터그램 수신 (" + endpointText(from) + L")");
                    continue;
                }
                inOut.insert(inOut.end(), chunk.begin(), chunk.begin() + got);
                continue;
            }
            const long code = socketError();
            if (code == WSAEWOULDBLOCK || code == WSAECONNRESET) return true;
            if (code == WSAEMSGSIZE) {
                addFault(L"UDP 과대 데이터그램 수신 (" + std::to_wstring(chunk.size()) + L" Byte 초과)");
                continue;
            }
            error = failure(L"UDP 수신 실패", code);
            return false;
        }
        return true;
    }

    LineFault takeFault() override
    {
        LineFault taken;
        std::swap(taken, fault_);
        return taken;
    }

private:
    void addFault(std::wstring text)
    {
        if (!fault_.count) fault_.text = std::move(text);
        ++fault_.count;
    }

    UdpConfig config_;
    SOCKET socket_ = INVALID_SOCKET;
    sockaddr_in remote_{};
    LineFault fault_;
};

}  // namespace

std::unique_ptr<Transport> makeTransport(const TransportConfig& config)
{
    switch (config.kind) {
    case TransportKind::Serial: return std::make_unique<SerialTransport>(config.serial, config.sendTimeoutMs);
    case TransportKind::Tcp:    return std::make_unique<TcpTransport>(config.tcp, config.sendTimeoutMs);
    case TransportKind::Udp:    return std::make_unique<UdpTransport>(config.udp);
    }
    return {};
}

std::wstring describe(const TransportConfig& config)
{
    switch (config.kind) {
    case TransportKind::Serial: {
        const SerialConfig& serial = config.serial;
        static const wchar_t* const standards[] = {L"RS-232", L"RS-422", L"RS-485 2선식", L"RS-485 4선식"};
        static const wchar_t parities[] = {L'N', L'O', L'E', L'M', L'S'};
        static const wchar_t* const stops[] = {L"1", L"1.5", L"2"};
        std::wstring text = L"직렬 " + serial.port + L" · " + standards[static_cast<int>(serial.standard)] + L" · " +
                            std::to_wstring(serial.baud) + L" bps " + std::to_wstring(serial.dataBits) +
                            parities[serial.parity <= SPACEPARITY ? serial.parity : 0] +
                            stops[serial.stopBits <= TWOSTOPBITS ? serial.stopBits : 0];
        if (serial.standard == SerialStandard::Rs232 && serial.flow != FlowControl::None)
            text += serial.flow == FlowControl::RtsCts ? L" · 흐름 제어 RTS/CTS" : L" · 흐름 제어 DTR/DSR";
        if (serial.standard == SerialStandard::Rs485TwoWire)
            text += serial.localEcho == LocalEcho::Present ? L" · 송신 반향 발생 (제거)" : L" · 송신 반향 미발생";
        return text;
    }
    case TransportKind::Tcp:
        return L"TCP " + widen(config.tcp.host) + L":" + std::to_wstring(config.tcp.port);
    case TransportKind::Udp:
        return L"UDP 상대 " + widen(config.udp.remoteHost) + L":" + std::to_wstring(config.udp.remotePort) +
               L" · 수신 " + widen(config.udp.localHost) + L":" + std::to_wstring(config.udp.localPort);
    }
    return {};
}

namespace {

unsigned portNumber(const std::wstring& name)
{
    if (name.size() < 4 || name.size() > 7 || _wcsnicmp(name.c_str(), L"COM", 3) != 0 || name[3] == L'0') return 0;
    unsigned number = 0;
    for (std::size_t i = 3; i < name.size(); ++i) {
        if (name[i] < L'0' || name[i] > L'9') return 0;
        number = number * 10 + static_cast<unsigned>(name[i] - L'0');
    }
    return number <= 4096 ? number : 0;
}

std::wstring lowered(std::wstring text)
{
    for (auto& ch : text) ch = static_cast<wchar_t>(std::towlower(ch));
    return text;
}

bool registryText(HKEY key, const wchar_t* name, std::wstring& out)
{
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS || type != REG_SZ ||
        bytes > 1024 || bytes % sizeof(wchar_t))
        return false;
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buffer.data()), &bytes) != ERROR_SUCCESS)
        return false;
    out.assign(buffer.data());
    return true;
}

// REG_MULTI_SZ 항목은 공백 구분 결합
std::wstring deviceProperty(HDEVINFO devices, SP_DEVINFO_DATA& device, DWORD property)
{
    wchar_t buffer[512]{};
    DWORD type = 0;
    if (!SetupDiGetDeviceRegistryPropertyW(devices, &device, property, &type, reinterpret_cast<BYTE*>(buffer),
                                           sizeof(buffer) - 2 * sizeof(wchar_t), nullptr) ||
        (type != REG_SZ && type != REG_MULTI_SZ))
        return {};
    std::wstring text;
    for (const wchar_t* entry = buffer; *entry; entry += std::wcslen(entry) + 1) {
        if (!text.empty()) text += L' ';
        text += entry;
    }
    return text;
}

// MOXA NPort Real COM 드라이버 포트 판별: 표시명·제조사·하드웨어 ID·커널 장치명 기준
bool looksRealcom(const std::wstring& text)
{
    static const wchar_t* const keys[] = {L"real com", L"realcom", L"nport", L"nptdrv", L"npreal", L"npdrv"};
    const std::wstring lower = lowered(text);
    return std::any_of(std::begin(keys), std::end(keys),
                       [&lower](const wchar_t* key) { return lower.find(key) != std::wstring::npos; });
}

}  // namespace

std::vector<PortInfo> enumeratePorts()
{
    std::map<unsigned, PortInfo> ports;

    HKEY map = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &map) == ERROR_SUCCESS) {
        for (DWORD i = 0; i < 4096; ++i) {
            wchar_t device[256]{};
            DWORD deviceLength = static_cast<DWORD>(std::size(device));
            wchar_t name[32]{};
            DWORD nameBytes = sizeof(name) - sizeof(wchar_t);
            DWORD type = 0;
            const LONG result = RegEnumValueW(map, i, device, &deviceLength, nullptr, &type,
                                              reinterpret_cast<BYTE*>(name), &nameBytes);
            if (result == ERROR_NO_MORE_ITEMS) break;
            if (result != ERROR_SUCCESS || type != REG_SZ) continue;
            const unsigned number = portNumber(name);
            if (!number) continue;
            PortInfo& info = ports[number];
            info.name = L"COM" + std::to_wstring(number);
            info.realcom = info.realcom || looksRealcom(device);
            if (info.description.empty()) {
                const std::wstring path = device;
                info.description = path.substr(path.find_last_of(L'\\') + 1);
            }
        }
        RegCloseKey(map);
    }

    // 포트 클래스 {4d36e978-e325-11ce-bfc1-08002be10318}. SERIALCOMM 등록 포트의 표시명 보강 전용
    // 사용 안 함·오류 상태 장치는 PortName이 잔류하므로 동작 중(DN_STARTED) 장치만 반영
    static const GUID portsClass{0x4d36e978, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
    const HDEVINFO devices = SetupDiGetClassDevsW(&portsClass, nullptr, nullptr, DIGCF_PRESENT);
    if (devices != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA device{};
        device.cbSize = sizeof(device);
        for (DWORD i = 0; SetupDiEnumDeviceInfo(devices, i, &device); ++i) {
            ULONG status = 0;
            ULONG problem = 0;
            if (CM_Get_DevNode_Status(&status, &problem, device.DevInst, 0) != CR_SUCCESS || !(status & DN_STARTED))
                continue;
            const HKEY key = SetupDiOpenDevRegKey(devices, &device, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
            if (key == INVALID_HANDLE_VALUE) continue;
            std::wstring portName;
            const bool named = registryText(key, L"PortName", portName);
            RegCloseKey(key);
            const unsigned number = named ? portNumber(portName) : 0;
            if (!number || !ports.count(number)) continue;
            std::wstring friendly = deviceProperty(devices, device, SPDRP_FRIENDLYNAME);
            if (friendly.empty()) friendly = deviceProperty(devices, device, SPDRP_DEVICEDESC);
            const std::wstring maker = deviceProperty(devices, device, SPDRP_MFG);
            const std::wstring hardware = deviceProperty(devices, device, SPDRP_HARDWAREID);
            const std::wstring suffix = L" (COM" + std::to_wstring(number) + L")";
            if (friendly.size() > suffix.size() && friendly.compare(friendly.size() - suffix.size(), suffix.size(), suffix) == 0)
                friendly.erase(friendly.size() - suffix.size());
            PortInfo& info = ports[number];
            if (!friendly.empty()) info.description = friendly;
            info.realcom = info.realcom || looksRealcom(friendly + L" " + maker + L" " + hardware);
        }
        SetupDiDestroyDeviceInfoList(devices);
    }

    std::vector<PortInfo> result;
    result.reserve(ports.size());
    for (auto& entry : ports) result.push_back(std::move(entry.second));
    return result;
}

}  // namespace loopback
