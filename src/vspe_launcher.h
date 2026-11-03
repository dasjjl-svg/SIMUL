#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace simul {

struct VspePair {
    unsigned firstPort = 0;   // COM<firstPort>
    unsigned secondPort = 0;  // COM<secondPort>
};

struct VspeResult {
    bool launched = false;
    std::wstring executable;
    std::wstring message;
};

// 설치 레지스트리·연결 프로그램·기본 Program Files 경로 순차 탐색
std::wstring findVspeExecutable();

// Pair;first;second;0 명령 인수 생성. 입력 오류 시 공백 반환.
std::wstring buildVspeArguments(const std::vector<VspePair>& pairs);

// VSPEmulator.exe 실행 후 즉시 반환. 포트 활성 확인은 상위 UI에서 처리.
VspeResult launchVspe(HWND owner, const std::vector<VspePair>& pairs,
                      const std::wstring& executableOverride = {});

}  // namespace simul
