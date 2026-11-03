#include "transport.h"

#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <string>

namespace simul {

namespace {

struct WinsockBoot {
    WinsockBoot() { WSADATA data{}; WSAStartup(MAKEWORD(2, 2), &data); }
    ~WinsockBoot() { WSACleanup(); }
};
void ensureWinsock()
{
    static WinsockBoot boot;
    (void)boot;
}

std::wstring systemMessage(const wchar_t* prefix, int code)
{
    wchar_t buffer[256]{};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   static_cast<DWORD>(code), 0, buffer, static_cast<DWORD>(std::size(buffer)), nullptr);
    std::wstring text = prefix;
    text += L" (오류 ";
    text += std::to_wstring(code);
    text += L") ";
    text += buffer;
    return text;
}

class SerialTransport : public Transport {
public:
    explicit SerialTransport(const SerialConfig& config) : config_(config) {}
    ~SerialTransport() override { close(); }

    bool open(std::wstring& error) override
    {
        close();
        const std::wstring name = L"\\\\.\\" + config_.port;
        handle_ = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, 0, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            error = L"직렬 포트 열기 실패: " + config_.port +
                    L" (오류 " + std::to_wstring(GetLastError()) + L")";
            return false;
        }
        DCB dcb{};
        dcb.DCBlength = sizeof(dcb);
        dcb.BaudRate = config_.baud;
        dcb.ByteSize = config_.dataBits;
        dcb.Parity   = config_.parity;
        dcb.StopBits = config_.stopBits;
        dcb.fBinary  = TRUE;
        dcb.fParity  = config_.parity != NOPARITY;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;
        // 동일 XON·XOFF 바이트 거부 회피
        dcb.XonChar  = 0x11;
        dcb.XoffChar = 0x13;

        COMMTIMEOUTS timeouts{};
        timeouts.ReadIntervalTimeout = MAXDWORD;
        timeouts.WriteTotalTimeoutConstant = 250;

        if (!SetCommState(handle_, &dcb) || !SetCommTimeouts(handle_, &timeouts) ||
            !PurgeComm(handle_, PURGE_RXCLEAR | PURGE_TXCLEAR)) {
            error = L"직렬 포트 설정 실패 (오류 " + std::to_wstring(GetLastError()) + L")";
            close();
            return false;
        }
        return true;
    }

    void close() override
    {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }

    bool isOpen() const override { return handle_ != INVALID_HANDLE_VALUE; }

    bool send(const Buffer& frame, std::wstring& error) override
    {
        if (!isOpen()) { error = L"직렬 포트 미연결."; return false; }
        DWORD written = 0;
        if (!WriteFile(handle_, frame.data(), static_cast<DWORD>(frame.size()), &written, nullptr) ||
            written != frame.size()) {
            error = L"직렬 송신 실패 (오류 " + std::to_wstring(GetLastError()) + L")";
            return false;
        }
        return true;
    }

    bool receive(Buffer& inOut, std::wstring& error) override
    {
        if (!isOpen()) { error = L"직렬 포트 미연결."; return false; }
        DWORD errors = 0;
        COMSTAT status{};
        if (!ClearCommError(handle_, &errors, &status)) {
            error = L"회선 상태 조회 실패 (오류 " + std::to_wstring(GetLastError()) + L")";
            return false;
        }
        if (errors & (CE_FRAME | CE_OVERRUN | CE_RXPARITY | CE_RXOVER)) {
            error = L"회선 오류 플래그 0x" + std::to_wstring(errors);
            return false;
        }
        if (!status.cbInQue) return true;
        std::array<Byte, 1024> chunk{};
        const DWORD toRead = (std::min)(status.cbInQue, static_cast<DWORD>(chunk.size()));
        DWORD got = 0;
        if (!ReadFile(handle_, chunk.data(), toRead, &got, nullptr)) {
            error = L"직렬 수신 실패 (오류 " + std::to_wstring(GetLastError()) + L")";
            return false;
        }
        inOut.insert(inOut.end(), chunk.begin(), chunk.begin() + got);
        return true;
    }

private:
    SerialConfig config_;
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

class TcpTransport : public Transport {
public:
    explicit TcpTransport(const TcpConfig& config) : config_(config) {}
    ~TcpTransport() override { close(); }

    bool open(std::wstring& error) override
    {
        close();
        ensureWinsock();
        socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_ == INVALID_SOCKET) {
            error = systemMessage(L"TCP 소켓 생성 실패", WSAGetLastError());
            return false;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port   = htons(config_.port);
        if (InetPtonA(AF_INET, config_.host.c_str(), &address.sin_addr) != 1) {
            error = L"TCP 상대 IPv4 주소 형식 오류.";
            close();
            return false;
        }
        if (connect(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            error = systemMessage(L"TCP 접속 실패", WSAGetLastError());
            close();
            return false;
        }
        u_long nonblocking = 1;
        if (ioctlsocket(socket_, FIONBIO, &nonblocking) != 0) {
            error = systemMessage(L"TCP 비동기 전환 실패", WSAGetLastError());
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

    bool isOpen() const override { return socket_ != INVALID_SOCKET; }

    bool send(const Buffer& frame, std::wstring& error) override
    {
        if (!isOpen()) { error = L"TCP 소켓 미연결."; return false; }
        std::size_t offset = 0;
        // WSAEWOULDBLOCK 재시도 상한 100회. 최대 전문 78B 기준 충분
        for (int retry = 0; offset < frame.size() && retry < 100; ++retry) {
            const int sent = ::send(socket_,
                reinterpret_cast<const char*>(frame.data() + offset),
                static_cast<int>(frame.size() - offset), 0);
            if (sent == SOCKET_ERROR) {
                const int code = WSAGetLastError();
                if (code == WSAEWOULDBLOCK) { Sleep(1); continue; }
                error = systemMessage(L"TCP 송신 실패", code);
                return false;
            }
            offset += static_cast<std::size_t>(sent);
        }
        if (offset != frame.size()) {
            error = L"TCP 송신 재시도 한도 초과.";
            return false;
        }
        return true;
    }

    bool receive(Buffer& inOut, std::wstring& error) override
    {
        if (!isOpen()) { error = L"TCP 소켓 미연결."; return false; }
        std::array<Byte, 2048> chunk{};
        for (;;) {
            const int got = recv(socket_, reinterpret_cast<char*>(chunk.data()),
                                 static_cast<int>(chunk.size()), 0);
            if (got > 0) {
                inOut.insert(inOut.end(), chunk.begin(), chunk.begin() + got);
                continue;
            }
            if (got == 0) { error = L"TCP 상대 종료."; return false; }
            const int code = WSAGetLastError();
            if (code == WSAEWOULDBLOCK) return true;
            error = systemMessage(L"TCP 수신 실패", code);
            return false;
        }
    }

private:
    TcpConfig config_;
    SOCKET socket_ = INVALID_SOCKET;
};

class UdpTransport : public Transport {
public:
    explicit UdpTransport(const UdpConfig& config) : config_(config) {}
    ~UdpTransport() override { close(); }

    bool open(std::wstring& error) override
    {
        close();
        ensureWinsock();
        socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == INVALID_SOCKET) {
            error = systemMessage(L"UDP 소켓 생성 실패", WSAGetLastError());
            return false;
        }
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port   = htons(config_.localPort);
        if (InetPtonA(AF_INET, config_.localHost.c_str(), &local.sin_addr) != 1) {
            error = L"UDP 수신 IPv4 주소 형식 오류.";
            close();
            return false;
        }
        BOOL exclusive = TRUE;
        if (setsockopt(socket_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                       reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) != 0 ||
            bind(socket_, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
            error = systemMessage(L"UDP 바인드 실패", WSAGetLastError());
            close();
            return false;
        }
        remote_ = {};
        remote_.sin_family = AF_INET;
        remote_.sin_port   = htons(config_.remotePort);
        if (InetPtonA(AF_INET, config_.remoteHost.c_str(), &remote_.sin_addr) != 1) {
            error = L"UDP 상대 IPv4 주소 형식 오류.";
            close();
            return false;
        }
        u_long nonblocking = 1;
        if (ioctlsocket(socket_, FIONBIO, &nonblocking) != 0) {
            error = systemMessage(L"UDP 비동기 전환 실패", WSAGetLastError());
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

    bool isOpen() const override { return socket_ != INVALID_SOCKET; }

    bool send(const Buffer& frame, std::wstring& error) override
    {
        if (!isOpen()) { error = L"UDP 소켓 미연결."; return false; }
        const int sent = sendto(socket_, reinterpret_cast<const char*>(frame.data()),
            static_cast<int>(frame.size()), 0,
            reinterpret_cast<const sockaddr*>(&remote_), sizeof(remote_));
        if (sent == SOCKET_ERROR) {
            error = systemMessage(L"UDP 송신 실패", WSAGetLastError());
            return false;
        }
        if (static_cast<std::size_t>(sent) != frame.size()) {
            error = L"UDP 분할 송신 발생.";
            return false;
        }
        return true;
    }

    bool receive(Buffer& inOut, std::wstring& error) override
    {
        if (!isOpen()) { error = L"UDP 소켓 미연결."; return false; }
        for (;;) {
            std::array<Byte, 2048> chunk{};
            sockaddr_in from{};
            int fromLen = sizeof(from);
            const int got = recvfrom(socket_, reinterpret_cast<char*>(chunk.data()),
                static_cast<int>(chunk.size()), 0,
                reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (got > 0) {
                inOut.insert(inOut.end(), chunk.begin(), chunk.begin() + got);
                continue;
            }
            const int code = WSAGetLastError();
            // WSAECONNRESET: 상대 포트 미개방 ICMP 응답 — 비치명 처리
            if (code == WSAEWOULDBLOCK || code == WSAEMSGSIZE || code == WSAECONNRESET) return true;
            error = systemMessage(L"UDP 수신 실패", code);
            return false;
        }
    }

private:
    UdpConfig config_;
    SOCKET socket_ = INVALID_SOCKET;
    sockaddr_in remote_{};
};

}  // namespace

std::unique_ptr<Transport> makeTransport(const TransportConfig& config)
{
    switch (config.kind) {
    case TransportKind::Serial: return std::make_unique<SerialTransport>(config.serial);
    case TransportKind::Tcp:    return std::make_unique<TcpTransport>(config.tcp);
    case TransportKind::Udp:    return std::make_unique<UdpTransport>(config.udp);
    }
    return {};
}

}  // namespace simul
