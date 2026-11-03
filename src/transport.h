#pragma once

#include <winsock2.h>
#include <windows.h>

#include <memory>
#include <string>

#include "protocol.h"

namespace simul {

enum class TransportKind { Serial, Tcp, Udp };

struct SerialConfig {
    std::wstring port = L"COM101";
    DWORD baud = 115200;
    BYTE  dataBits = 8;
    BYTE  parity   = NOPARITY;
    BYTE  stopBits = ONESTOPBIT;
};

struct TcpConfig {
    std::string host = "127.0.0.1";
    unsigned short port = 50000;
};

struct UdpConfig {
    std::string localHost = "0.0.0.0";
    unsigned short localPort = 50001;
    std::string remoteHost = "127.0.0.1";
    unsigned short remotePort = 50000;
};

struct TransportConfig {
    TransportKind kind = TransportKind::Serial;
    SerialConfig serial;
    TcpConfig    tcp;
    UdpConfig    udp;
};

class Transport {
public:
    virtual ~Transport() = default;
    virtual bool open(std::wstring& error) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    virtual bool send(const Buffer& frame, std::wstring& error) = 0;
    virtual bool receive(Buffer& inOut, std::wstring& error) = 0;
};

std::unique_ptr<Transport> makeTransport(const TransportConfig& config);

}  // namespace simul
