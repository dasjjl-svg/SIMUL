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

    void startTest(HWND dialog);
    void stopTest(HWND dialog, const std::wstring& reason);
    void updateStatus(HWND dialog);
    void setTransportFieldsEnabled(HWND dialog);
    void appendLog(HWND dialog, const std::wstring& line);

    bool readSettings(HWND dialog, TestSettings& settings, std::wstring& error) const;

    HWND dialog_ = nullptr;
    std::unique_ptr<LoopbackTest> test_;
    bool running_ = false;
    std::wstring lastEvent_;
};

}  // namespace simul
