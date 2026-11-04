#pragma once

#include <windows.h>
#include <commctrl.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "loopback_test.h"
#include "ui_draw.h"

namespace loopback {

constexpr std::size_t kMaxChannels = 32;

// 채널별 입력 원문. 시험 시작 시 검증 후 TransportConfig로 변환
struct ChannelForm {
    int kind = 0;
    std::wstring serialPort;
    int standard = 0;
    std::wstring baud;
    int dataBits = 8;
    int parity = NOPARITY;
    int stopBits = ONESTOPBIT;
    int flow = 0;
    int echo = 0;  // RS-485 2선식 송신 반향: 0 미선택, 1 발생(제거), 2 미발생
    std::wstring tcpHost;
    std::wstring tcpPort;
    std::wstring udpLocalHost;
    std::wstring udpLocalPort;
    std::wstring udpRemoteHost;
    std::wstring udpRemotePort;
};

class App {
public:
    App();
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    int run(HINSTANCE instance);

private:
    struct Plan {
        std::size_t channel;
        TestSettings settings;
    };

    struct Overall {
        std::size_t channels = 0;
        std::size_t passed = 0;
        std::size_t failedChannels = 0;
        std::size_t stopped = 0;
        std::uint64_t matched = 0;
        std::chrono::milliseconds elapsed{0};
        std::wstring failedList;
        bool hasFailure = false;
        std::size_t firstFailureChannel = 0;
        std::chrono::milliseconds firstFailureAt{0};
        std::wstring firstFailure;
    };

    static INT_PTR CALLBACK dialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam);
    INT_PTR handle(UINT message, WPARAM wParam, LPARAM lParam);

    void onInit();
    void onCommand(int id, int code);
    INT_PTR onNotify(NMHDR* header);
    void onTimer();
    void onDrawItem(const DRAWITEMSTRUCT& draw);
    INT_PTR onControlColor(HDC dc, HWND control);
    void onPower(WPARAM event);

    void startTest();
    void stopTest();
    void finishTest();
    void requestClose();
    void applyCountText();
    void conditionsChanged();
    void syncComboText(int id);
    void fitToWorkArea();

    bool readPlan(std::vector<Plan>& plan, std::wstring& error, int& channel, int& field) const;
    bool readChannel(std::size_t index, TransportConfig& config, std::wstring& error, int& field) const;
    std::wstring planWarnings(const std::vector<Plan>& plan) const;
    void loadSettings();
    void saveSettings() const;

    void setupChannelList();
    void setChannelCount(std::size_t count);
    void clearResults();
    void sizeColumns();
    void focusChannel(std::size_t index, int field);
    void selectChannel(std::size_t index);
    void showEditor();
    void showTransportPanel();
    void applySerialRules();
    void refreshPorts();
    std::wstring* formField(int id);
    void refreshRows();
    void setCell(std::size_t row, int column, const std::wstring& value);
    bool rowVerdict(std::size_t row, COLORREF& face, COLORREF& ink, std::wstring& label) const;
    std::wstring kindText(const ChannelForm& form) const;
    void refreshSummary();
    void refreshDetail();
    LRESULT onListCustomDraw(NMLVCUSTOMDRAW& draw);

    void previewDuration();
    void setInputsEnabled(bool enabled);
    void appendLog(const std::wstring& line,
                   std::chrono::system_clock::time_point when = std::chrono::system_clock::now());
    void appendEvents();
    void updateTitle();
    void refreshFonts();
    // 창·작업 표시줄 아이콘과 상단 로고를 현재 DPI 크기로 재적재
    void refreshIcons();
    void invalidateVerdict();
    void holdProcessResources();
    void releaseSystemHolds();

    Overall overall() const;
    ui::VerdictView verdictView() const;
    std::wstring channelDetail() const;
    int dluToPixels(int units) const;
    HWND item(int id) const { return GetDlgItem(dialog_, id); }
    std::wstring text(int id) const;

    HWND dialog_ = nullptr;
    HWND list_ = nullptr;
    ui::FontCache fonts_;
    HBRUSH surfaceBrush_ = nullptr;
    HBRUSH disabledBrush_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HICON bigIcon_ = nullptr;
    HICON smallIcon_ = nullptr;
    HICON headerIcon_ = nullptr;
    int headerIconSize_ = 0;

    std::array<ChannelForm, kMaxChannels> forms_{};
    std::array<std::unique_ptr<LoopbackTest>, kMaxChannels> tests_{};
    std::array<TestSnapshot, kMaxChannels> views_{};
    std::array<bool, kMaxChannels> active_{};
    std::array<std::array<std::wstring, 9>, kMaxChannels> cells_{};
    std::set<std::wstring> realcomPorts_;
    std::wstring detailText_;
    std::wstring summaryText_;
    std::wstring titleText_;
    std::size_t channelCount_ = 1;
    std::size_t selected_ = 0;
    bool loading_ = false;

    std::chrono::seconds duration_{3600};
    bool running_ = false;
    bool hasResult_ = false;
    bool timerRaised_ = false;
    bool sleepBlocked_ = false;

    struct WorkingSet {
        SIZE_T minimum = 0;
        SIZE_T maximum = 0;
        DWORD flags = 0;
    };
    bool priorityRaised_ = false;
    DWORD savedPriority_ = NORMAL_PRIORITY_CLASS;
    bool workingSetHeld_ = false;
    WorkingSet savedWorkingSet_{};
};

}  // namespace loopback
