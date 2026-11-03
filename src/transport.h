#pragma once

#include <winsock2.h>
#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "protocol.h"

namespace loopback {

enum class TransportKind { Serial, Tcp, Udp };

// RS-422·RS-485 4선식: 전이중, 제어선 흐름 제어 없음
// RS-485 2선식: 반이중, 송신 구간 RTS 토글 방향 제어
enum class SerialStandard { Rs232, Rs422, Rs485TwoWire, Rs485FourWire };
enum class FlowControl { None, RtsCts, DtrDsr };
// RS-485 2선식 어댑터의 자기 송신 수신 여부. Present: 송신 전문당 최초 사본 판정 제외
enum class LocalEcho { Unset, Present, Absent };

struct SerialConfig {
    std::wstring port = L"COM1";
    SerialStandard standard = SerialStandard::Rs232;
    DWORD baud = 115200;
    BYTE  dataBits = 8;
    BYTE  parity   = NOPARITY;    // NOPARITY·ODDPARITY·EVENPARITY·MARKPARITY·SPACEPARITY
    BYTE  stopBits = ONESTOPBIT;  // ONESTOPBIT·ONE5STOPBITS·TWOSTOPBITS
    FlowControl flow = FlowControl::None;     // RS-232 전용
    LocalEcho localEcho = LocalEcho::Unset;   // RS-485 2선식 전용
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

// 직전 receive 중 검출된 비치명 회선 이상. 건별 실패 계수 대상
struct LineFault {
    std::wstring text;        // 최초 이상 내용
    std::uint64_t count = 0;  // 이상 건수
};

// open·close는 UI 스레드 또는 개방 전용 스레드, send·receive·takeFault는 시험 작업 스레드 전용.
class Transport {
public:
    virtual ~Transport() = default;
    virtual bool open(std::wstring& error) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    // false: 시험 중단 대상 치명 오류
    virtual bool send(const Buffer& frame, std::wstring& error) = 0;
    virtual bool receive(Buffer& inOut, std::wstring& error) = 0;
    virtual LineFault takeFault() { return {}; }
    // 개방 시 적용 조건 변경 등 기록 대상 안내
    virtual std::wstring notice() const { return {}; }
};

struct PortInfo {
    std::wstring name;         // COMn
    std::wstring description;  // 장치 관리자 표시명
    bool realcom = false;      // MOXA NPort Real COM 드라이버 포트
};

std::unique_ptr<Transport> makeTransport(const TransportConfig& config);
std::wstring describe(const TransportConfig& config);
// 활성 COM 포트: SERIALCOMM 등록 항목 + 동작 중 장치의 장치 관리자 표시명. 번호순
std::vector<PortInfo> enumeratePorts();

}  // namespace loopback
