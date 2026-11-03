#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <string>

namespace loopback {

inline std::wstring grouped(std::uint64_t value)
{
    std::wstring digits = std::to_wstring(value);
    for (std::size_t pos = digits.size(); pos > 3; pos -= 3) digits.insert(pos - 3, 1, L',');
    return digits;
}

// h:mm:ss 또는 h:mm:ss.mmm
inline std::wstring clockText(std::chrono::milliseconds value, bool millis = false)
{
    const long long total = value.count() < 0 ? 0 : value.count();
    wchar_t buffer[48]{};
    if (millis) {
        std::swprintf(buffer, sizeof(buffer) / sizeof(buffer[0]), L"%lld:%02lld:%02lld.%03lld",
                      total / 3600000, total / 60000 % 60, total / 1000 % 60, total % 1000);
    } else {
        std::swprintf(buffer, sizeof(buffer) / sizeof(buffer[0]), L"%lld:%02lld:%02lld",
                      total / 3600000, total / 60000 % 60, total / 1000 % 60);
    }
    return buffer;
}

// 소수점 1자리 ms 표기
template <typename Rep, typename Period>
std::wstring msText(std::chrono::duration<Rep, Period> value)
{
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(value).count();
    wchar_t buffer[32]{};
    std::swprintf(buffer, sizeof(buffer) / sizeof(buffer[0]), L"%.1f ms", static_cast<double>(micros) / 1000.0);
    return buffer;
}

inline std::wstring hexText(const std::uint8_t* data, std::size_t size)
{
    static const wchar_t digits[] = L"0123456789ABCDEF";
    std::wstring text;
    text.reserve(size * 3);
    for (std::size_t i = 0; i < size; ++i) {
        if (i) text.push_back(L' ');
        text.push_back(digits[data[i] >> 4]);
        text.push_back(digits[data[i] & 0x0F]);
    }
    return text;
}

}  // namespace loopback
