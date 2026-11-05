#pragma once

#include <winsock2.h>
#include <windows.h>

#include <chrono>
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
    DWORD sendTimeoutMs = 250;  // 전문 1건 송신 처리 상한(직렬·TCP)
};

// 직전 receive 중 검출된 비치명 회선 이상. 건별 실패 계수 대상
struct LineFault {
    std::wstring text;        // 최초 이상 내용
    std::uint64_t count = 0;  // 이상 건수
};

// open·close는 UI 스레드 또는 개방 전용 스레드, send·receive·takeFault·takeSendEcho는 시험 작업 스레드 전용
class Transport {
public:
    virtual ~Transport() = default;
    virtual bool open(std::wstring& error) = 0;
    virtual void close() = 0;
    // false: 시험 중단 대상 치명 오류
    virtual bool send(const Buffer& frame, std::wstring& error) = 0;
    // 수신 데이터 도착 또는 wait 경과 시 반환. 도착분을 inOut 끝에 추가(1회 수용 상한 적용). false: 시험 중단 대상 치명 오류
    virtual bool receive(Buffer& inOut, std::chrono::milliseconds wait, std::wstring& error) = 0;
    virtual LineFault takeFault() { return {}; }
    // 송신 진행 중 수신 바이트의 송신 전문 선두 일치 여부(직전 receive까지). 반이중 선로의 자기 송신 반향 증거
    virtual bool takeSendEcho() { return false; }
    // 직전 send의 OS 송신 호출 직전 시각(선로 송신 개시 하한). 미보유 전송로는 fallback(send 호출 직전 시각)
    virtual std::chrono::steady_clock::time_point issuedAt(std::chrono::steady_clock::time_point fallback) const
    {
        return fallback;
    }
    // 진행 중 송신 완료 대기·결과 확인. false: 송신 실패
    virtual bool flush(std::wstring& error)
    {
        (void)error;
        return true;
    }
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
// 시스템 오류 코드 설명문. 말미 개행·마침표 제거
std::wstring systemText(DWORD code);
// 활성 COM 포트: SERIALCOMM 등록 항목 + 동작 중 장치의 장치 관리자 표시명. 번호순
std::vector<PortInfo> enumeratePorts();

}  // namespace loopback
