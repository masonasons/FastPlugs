#pragma once
// ---------------------------------------------------------------------------
// Stand-in for FastRoute's src/util.h: the three helpers the effects call.
// The real one includes windows.h and is mostly about dialogs and paths.
// ---------------------------------------------------------------------------
#include <string>

// printf into a wide string. FastRoute's format strings use %s for a wide
// argument, which is what MSVC's wide printf means by it; everywhere else %s
// is a narrow string, so the implementation rewrites it to %ls first.
std::wstring FormatW(const wchar_t* fmt, ...);

// "1 minute 30 seconds".
std::wstring DurationText(int seconds);

// Somewhere for an effect to say what went wrong. A plugin has no error log
// window, so this goes to the debugger and stderr, and is kept for the host.
void LogError(const std::wstring& msg);
std::wstring LastLoggedError();
