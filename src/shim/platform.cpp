#include "platform.h"

#include <cstdio>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <shlobj.h>
#elif defined(__APPLE__)
#  include <AudioToolbox/AudioServices.h>
#  include <cstdlib>
#else
#  include <cstdlib>
#endif

namespace fp_platform
{

#ifdef _WIN32

void Beep(int hz, int ms)
{
    // Blocks for the length of the tone. The clipping monitor only ever calls
    // this from a thread of its own, never from the audio callback.
    ::Beep((DWORD)hz, (DWORD)ms);
}

std::wstring DocumentsFolder()
{
    std::wstring out;
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &p)) && p)
        out = p;
    if (p) CoTaskMemFree(p);
    if (out.empty())
    {
        wchar_t buf[MAX_PATH] = { 0 };
        if (GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH)) out = buf;
    }
    return out + L"\\FastPlugs";
}

void DebugLog(const std::wstring& msg)
{
    OutputDebugStringW((L"FastPlugs: " + msg + L"\n").c_str());
    std::fwprintf(stderr, L"FastPlugs: %ls\n", msg.c_str());
}

std::filesystem::path ToPath(const std::wstring& w) { return std::filesystem::path(w); }

#else

void Beep(int hz, int ms)
{
    (void)hz; (void)ms;
#ifdef __APPLE__
    // Asynchronous, and the user's own alert sound at the user's own alert
    // volume. Core Audio has no "play this pitch" call short of opening an
    // output unit, which is a great deal of machinery for a warning beep.
    AudioServicesPlayAlertSound(kSystemSoundID_UserPreferredAlert);
#else
    std::fputs("\a", stderr);
#endif
}

static std::string Utf8(const std::wstring& w)
{
    // wchar_t is UTF-32 everywhere but Windows.
    std::string out;
    out.reserve(w.size());
    for (wchar_t wc : w)
    {
        const unsigned long c = (unsigned long)wc;
        if (c < 0x80) out += (char)c;
        else if (c < 0x800)
        {
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3F));
        }
        else if (c < 0x10000)
        {
            out += (char)(0xE0 | (c >> 12));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        }
        else
        {
            out += (char)(0xF0 | (c >> 18));
            out += (char)(0x80 | ((c >> 12) & 0x3F));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        }
    }
    return out;
}

static std::wstring Widen(const char* s)
{
    // UTF-8 to UTF-32. A malformed sequence becomes U+FFFD rather than being
    // guessed at: a home folder is the one path that has to survive the round
    // trip back through Utf8() exactly.
    std::wstring out;
    const unsigned char* p = (const unsigned char*)(s ? s : "");
    while (*p)
    {
        unsigned long c = *p;
        int more = 0;
        if (c < 0x80)              more = 0;
        else if ((c >> 5) == 0x6) { c &= 0x1F; more = 1; }
        else if ((c >> 4) == 0xE) { c &= 0x0F; more = 2; }
        else if ((c >> 3) == 0x1E){ c &= 0x07; more = 3; }
        else                       { out += (wchar_t)0xFFFD; ++p; continue; }
        ++p;
        bool ok = true;
        for (int i = 0; i < more; ++i, ++p)
        {
            if ((*p & 0xC0) != 0x80) { ok = false; break; }
            c = (c << 6) | (*p & 0x3F);
        }
        out += ok ? (wchar_t)c : (wchar_t)0xFFFD;
    }
    return out;
}

std::wstring DocumentsFolder()
{
    const char* home = std::getenv("HOME");
    return Widen(home ? home : "") + L"/Documents/FastPlugs";
}

void DebugLog(const std::wstring& msg)
{
    std::fprintf(stderr, "FastPlugs: %s\n", Utf8(msg).c_str());
}

std::filesystem::path ToPath(const std::wstring& w)
{
    return std::filesystem::path(Utf8(w));
}

#endif

}  // namespace fp_platform
