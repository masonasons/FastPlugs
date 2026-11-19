#pragma once
// The few things that are genuinely different on Windows and the Mac, kept in
// one file that never sees audio.h - so Win32's own Beep() and the portable
// one the effects call cannot meet.
#include <filesystem>
#include <string>

namespace fp_platform
{
// The clipping monitor's tone: the requested pitch on Windows, the user's
// alert sound on the Mac, which has no way to ask for a pitch.
void Beep(int hz, int ms);

// <Documents>/FastPlugs, not yet created.
std::wstring DocumentsFolder();

// To the debugger on Windows; stderr everywhere.
void DebugLog(const std::wstring& msg);

// A wide path as the file system wants it. On the Mac the conversion is
// spelled out as UTF-8 rather than left to whatever locale the host set.
std::filesystem::path ToPath(const std::wstring& w);
}
