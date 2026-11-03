#include "vspe_launcher.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <array>
#include <cwctype>
#include <set>

namespace simul {

namespace {

struct Key {
    HKEY value = nullptr;
    ~Key() { if (value && value != INVALID_HANDLE_VALUE) RegCloseKey(value); }
};

std::wstring lower(std::wstring value)
{
    for (auto& ch : value) ch = static_cast<wchar_t>(std::towlower(ch));
    return value;
}

LONG readString(HKEY key, const wchar_t* name, std::wstring& out)
{
    out.clear();
    DWORD type = 0, bytes = 0;
    LONG code = RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes);
    if (code != ERROR_SUCCESS) return code;
    if ((type != REG_SZ && type != REG_EXPAND_SZ) || bytes > 65536 || bytes % sizeof(wchar_t))
        return ERROR_INVALID_DATA;
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, 0);
    code = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buffer.data()), &bytes);
    if (code != ERROR_SUCCESS) return code;
    out.assign(buffer.data());
    if (type == REG_EXPAND_SZ) {
        const DWORD length = ExpandEnvironmentStringsW(out.c_str(), nullptr, 0);
        if (!length || length > 32768) return ERROR_INVALID_DATA;
        std::vector<wchar_t> expanded(length);
        if (ExpandEnvironmentStringsW(out.c_str(), expanded.data(), length) != length)
            return ERROR_INVALID_DATA;
        out.assign(expanded.data());
    }
    return ERROR_SUCCESS;
}

void addCandidate(std::vector<std::wstring>& out, std::set<std::wstring>& seen, std::wstring path)
{
    if (path.size() > 1 && path.front() == L'"' && path.back() == L'"')
        path = path.substr(1, path.size() - 2);
    if (path.find(L'"') != std::wstring::npos || path.size() >= 32768) return;
    const bool absolute = path.size() > 2 && path[1] == L':' &&
                          (path[2] == L'\\' || path[2] == L'/');
    const bool unc = path.compare(0, 2, L"\\\\") == 0;
    if (!absolute && !unc) return;
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    if (_wcsicmp(path.c_str() + slash + 1, L"VSPEmulator.exe") != 0) return;
    if (seen.insert(lower(path)).second) out.push_back(std::move(path));
}

bool fileExists(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

}  // namespace

std::wstring findVspeExecutable()
{
    std::vector<std::wstring> candidates;
    std::set<std::wstring> seen;

    for (HKEY hive : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
        for (REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
            Key app;
            if (RegOpenKeyExW(hive,
                L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\VSPEmulator.exe",
                0, KEY_READ | view, &app.value) == ERROR_SUCCESS) {
                std::wstring path;
                if (readString(app.value, nullptr, path) == ERROR_SUCCESS)
                    addCandidate(candidates, seen, path);
            }
        }
    }

    DWORD length = 0;
    AssocQueryStringW(ASSOCF_NOTRUNCATE, ASSOCSTR_EXECUTABLE, L".vspe", L"open",
                      nullptr, &length);
    if (length && length <= 32768) {
        std::vector<wchar_t> path(length);
        if (SUCCEEDED(AssocQueryStringW(ASSOCF_NOTRUNCATE, ASSOCSTR_EXECUTABLE, L".vspe",
                                        L"open", path.data(), &length)))
            addCandidate(candidates, seen, path.data());
    }

    for (int folderId : {CSIDL_PROGRAM_FILES, CSIDL_PROGRAM_FILESX86}) {
        wchar_t folder[MAX_PATH]{};
        if (FAILED(SHGetFolderPathW(nullptr, folderId, nullptr, SHGFP_TYPE_CURRENT, folder)))
            continue;
        for (const auto* suffix : {L" (64 bit)", L" (x64)", L""}) {
            std::wstring path = std::wstring(folder) +
                L"\\Eterlogic.com\\Virtual Serial Ports Emulator" + suffix + L"\\VSPEmulator.exe";
            addCandidate(candidates, seen, path);
        }
    }

    for (const auto& path : candidates) {
        if (fileExists(path)) return path;
    }
    return {};
}

std::wstring buildVspeArguments(const std::vector<VspePair>& pairs)
{
    if (pairs.empty()) return {};
    std::wstring arguments = L"-hide_splash";
    for (const auto& pair : pairs) {
        if (!pair.firstPort || pair.firstPort > 4096 ||
            !pair.secondPort || pair.secondPort > 4096 ||
            pair.firstPort == pair.secondPort) return {};
        arguments += L" -create_device=Pair;" + std::to_wstring(pair.firstPort) +
                     L";" + std::to_wstring(pair.secondPort) + L";0";
    }
    return arguments;
}

VspeResult launchVspe(HWND owner, const std::vector<VspePair>& pairs,
                      const std::wstring& executableOverride)
{
    VspeResult result;
    std::wstring executable = executableOverride.empty() ? findVspeExecutable() : executableOverride;
    if (executable.empty() || !fileExists(executable)) {
        result.message = L"VSPE executable not found. Install VSPE 1.5.8.489 or newer, "
                         L"or configure the virtual COM pair manually.";
        return result;
    }
    const std::wstring arguments = buildVspeArguments(pairs);
    if (arguments.empty()) {
        result.message = L"Nothing to request from VSPE. Pair numbers must be in COM1..COM4096 "
                         L"and must differ.";
        return result;
    }
    const auto slash = executable.find_last_of(L"\\/");
    const std::wstring folder = slash == std::wstring::npos ? L"" : executable.substr(0, slash);

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.hwnd = owner;
    info.lpVerb = L"open";
    info.lpFile = executable.c_str();
    info.lpParameters = arguments.c_str();
    info.lpDirectory = folder.empty() ? nullptr : folder.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) {
        const DWORD code = GetLastError();
        if (code != ERROR_CANCELLED) {
            // Retry elevated: VSPE needs admin to create devices.
            info.fMask |= SEE_MASK_FLAG_NO_UI;
            info.lpVerb = L"runas";
            if (!ShellExecuteExW(&info)) {
                result.message = L"Failed to launch VSPE. Error " +
                                 std::to_wstring(GetLastError());
                return result;
            }
        } else {
            result.message = L"VSPE launch cancelled.";
            return result;
        }
    }
    if (info.hProcess) CloseHandle(info.hProcess);

    result.launched = true;
    result.executable = executable;
    result.message = L"VSPE launched to create the requested pair(s). "
                     L"Check the VSPE window for status.";
    return result;
}

}  // namespace simul
