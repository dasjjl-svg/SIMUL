#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace simul {

// Minimal helper that drives the Virtual Serial Ports Emulator (VSPE)
// command line to create one or more virtual COM port pairs.
//
// Install VSPE separately. SIMUL does not bundle, modify, or distribute it.

struct VspePair {
    unsigned firstPort = 0;   // e.g. 101 -> COM101
    unsigned secondPort = 0;
};

struct VspeResult {
    bool launched = false;
    std::wstring executable;
    std::wstring message;
};

// Searches common installation locations and registry entries for
// VSPEmulator.exe. Empty return means no suitable install was found.
std::wstring findVspeExecutable();

// Build the command-line arguments for the requested pairs.
std::wstring buildVspeArguments(const std::vector<VspePair>& pairs);

// Launch VSPE to create the requested pairs. Returns immediately once the
// process has been started (the UI should poll COM port presence
// separately if confirmation matters).
VspeResult launchVspe(HWND owner, const std::vector<VspePair>& pairs,
                      const std::wstring& executableOverride = {});

}  // namespace simul
