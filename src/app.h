#pragma once

#include <windows.h>

#include <memory>
#include <string>

#include "loopback_test.h"

namespace simul {

class App {
public:
    App();
    ~App();
    int run(HINSTANCE instance, int showCommand);

private:
    static INT_PTR CALLBACK dialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam);
    INT_PTR handle(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam);

    void onInit(HWND dialog);
    void onCommand(HWND dialog, int id, int code);
    void onTimer(HWND dialog, UINT_PTR id);
    HBRUSH onCtlColor(HWND control);

    void startTest(HWND dialog);
    void stopTest(HWND dialog, const std::wstring& reason);
    void updateStatus(HWND dialog);
    void setTransportFieldsEnabled(HWND dialog);
    void appendLog(HWND dialog, const std::wstring& line);
    void applyBannerState(HWND dialog, Verdict verdict, bool running);

    bool readSettings(HWND dialog, TestSettings& settings, std::wstring& error) const;

    HWND  dialog_       = nullptr;
    HFONT bannerFont_   = nullptr;
    HBRUSH idleBrush_   = nullptr;
    HBRUSH runBrush_    = nullptr;
    HBRUSH okBrush_     = nullptr;
    HBRUSH failBrush_   = nullptr;
    HBRUSH activeBrush_ = nullptr;
    COLORREF activeText_ = RGB(255, 255, 255);

    std::unique_ptr<LoopbackTest> test_;
    bool running_ = false;
    std::wstring lastEvent_;
};

}  // namespace simul
