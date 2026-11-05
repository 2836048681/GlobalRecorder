#pragma once
#define NOMINMAX
#include <windows.h>
#include <filesystem>
#include <string>

namespace lightrec {
// GDI+ must be initialized by the caller. Caller owns the returned HICON.
HICON makeRecorderIcon(int pixels, bool recording);
bool exportRecorderIcons(const std::filesystem::path& folder);
std::wstring startupCommand(const std::wstring& executable);
LONG readStartup(std::wstring& command, const wchar_t* subkey = nullptr);
LONG writeStartup(bool enabled, const std::wstring& executable, const wchar_t* subkey = nullptr);
bool startupSelfTest(std::wstring& issue);
bool traySelfTest(HINSTANCE instance, std::wstring& issue);
}
