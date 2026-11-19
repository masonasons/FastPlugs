// ---------------------------------------------------------------------------
// What FastRoute's effects call that lives outside the effects themselves.
//
// Two of these are definitions from FastRoute's model.cpp (EqBandHz and
// AutoMakeupDb), repeated here because model.cpp is the rest of the
// application and cannot come with them. They are short, they are arithmetic,
// and they are the only duplicated logic in FastPlugs. If either changes in
// FastRoute it wants changing here too.
// ---------------------------------------------------------------------------
#include "audio.h"
#include "util.h"
#include "platform.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <cwchar>
#include <fstream>
#include <mutex>
#include <system_error>

// --- model.cpp -------------------------------------------------------------

const int* EqBandHz()
{
    static const int hz[kEqBands] = { 31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };
    return hz;
}

double AutoMakeupDb(double threshDb, double ratio)
{
    if (ratio <= 1.0 || threshDb >= 0.0) return 0.0;
    double db = (1.0 - 1.0 / ratio) * -threshDb;
    if (db > (double)kCompMakeupMax) db = (double)kCompMakeupMax;
    return db;
}

// --- the plugin host, which a plugin is not ----------------------------------

std::unique_ptr<Effect> MakePluginEffect() { return nullptr; }

// --- util.cpp --------------------------------------------------------------

std::wstring FormatW(const wchar_t* fmt, ...)
{
    std::wstring f = fmt ? fmt : L"";
#ifndef _WIN32
    // MSVC reads %s in a wide format as a wide string; C and every other
    // library read it as a narrow one. FastRoute is written for the first,
    // so any %s without a length modifier becomes %ls.
    std::wstring fixed;
    for (size_t i = 0; i < f.size(); ++i)
    {
        fixed += f[i];
        if (f[i] != L'%') continue;
        if (i + 1 < f.size() && f[i + 1] == L'%') { fixed += f[++i]; continue; }
        size_t j = i + 1;
        while (j < f.size() && std::wcschr(L"-+ #0123456789.*", f[j])) ++j;
        const bool hasLength = j < f.size() && std::wcschr(L"hlLqjzt", f[j]);
        fixed.append(f, i + 1, j - i - 1);
        if (j < f.size() && f[j] == L's' && !hasLength) fixed += L'l';
        i = j - 1;
    }
    f = fixed;
#endif

    std::vector<wchar_t> buf(256);
    for (;;)
    {
        va_list ap;
        va_start(ap, fmt);
        const int n = std::vswprintf(buf.data(), buf.size(), f.c_str(), ap);
        va_end(ap);
        if (n >= 0 && (size_t)n < buf.size()) return std::wstring(buf.data(), (size_t)n);
        // vswprintf reports neither the length it needed nor anything but
        // failure, so the buffer grows until it fits or is absurd.
        if (buf.size() >= 65536) return f;
        buf.resize(buf.size() * 4);
    }
}

std::wstring DurationText(int seconds)
{
    if (seconds < 0) seconds = 0;
    const int h = seconds / 3600;
    const int m = (seconds / 60) % 60;
    const int s = seconds % 60;
    if (h > 0) return FormatW(L"%d:%02d:%02d", h, m, s);
    return FormatW(L"%d:%02d", m, s);
}

namespace {
std::mutex g_logLock;
std::wstring g_lastError;
}

void LogError(const std::wstring& msg)
{
    {
        std::lock_guard<std::mutex> g(g_logLock);
        g_lastError = msg;
    }
    fp_platform::DebugLog(msg);
}

std::wstring LastLoggedError()
{
    std::lock_guard<std::mutex> g(g_logLock);
    return g_lastError;
}

// --- recorder.cpp, as far as the buffer needs it -----------------------------

std::wstring DefaultSaveFolder() { return fp_platform::DocumentsFolder(); }

RecordFormat RecFileFormatFor(RecFormat fmt, int rateHz, int bits, int kbps, int sessionRate)
{
    (void)fmt;   // WAV is the only format here
    RecordFormat f;
    f.inRate = sessionRate > 0 ? sessionRate : kDefaultRate;
    // There is no converter here, so the file is at the rate that arrives.
    (void)rateHz;
    f.rateHz = f.inRate;
    f.bits = (bits == 16 || bits == 24 || bits == 32) ? bits : 16;
    f.kbps = kbps;
    return f;
}

namespace {

std::wstring SanitisePiece(const std::wstring& s)
{
    std::wstring out;
    for (wchar_t c : s)
        out += (std::wcschr(L"\\/:*?\"<>|", c) || c < 32) ? L'_' : c;
    return out;
}

void ReplaceAll(std::wstring& s, const std::wstring& from, const std::wstring& to)
{
    for (size_t at = 0; (at = s.find(from, at)) != std::wstring::npos; at += to.size())
        s.replace(at, from.size(), to);
}

}  // namespace

std::wstring ResolveRecordingPath(const std::wstring& folder,
                                  const std::wstring& nameTemplate,
                                  const std::wstring& sessionName,
                                  RecFormat fmt,
                                  const std::wstring& blockName,
                                  int blockIndex, int blockCount)
{
    (void)fmt; (void)blockCount;
    const std::time_t now = std::time(nullptr);
    std::tm t{};
#ifdef _WIN32
    localtime_s(&t, &now);
#else
    localtime_r(&now, &t);
#endif
    const std::wstring date = FormatW(L"%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    const std::wstring time = FormatW(L"%02d-%02d-%02d", t.tm_hour, t.tm_min, t.tm_sec);

    std::wstring name = nameTemplate.empty() ? std::wstring(L"{session} {date} {time}")
                                             : nameTemplate;
    ReplaceAll(name, L"{session}",  SanitisePiece(sessionName));
    ReplaceAll(name, L"{block}",    SanitisePiece(blockName));
    ReplaceAll(name, L"{index}",    blockIndex > 0 ? FormatW(L"%d", blockIndex) : L"");
    ReplaceAll(name, L"{date}",     date);
    ReplaceAll(name, L"{time}",     time);
    ReplaceAll(name, L"{datetime}", date + L" " + time);
    name = SanitisePiece(name);
    if (name.empty()) name = L"Buffer";

    std::error_code ec;
    std::filesystem::create_directories(fp_platform::ToPath(folder), ec);

#ifdef _WIN32
    const wchar_t sep = L'\\';
#else
    const wchar_t sep = L'/';
#endif
    const std::wstring base = folder + sep + name;
    std::wstring path = base + L".wav";
    for (int n = 2; std::filesystem::exists(fp_platform::ToPath(path), ec) && n < 10000; ++n)
        path = base + FormatW(L" (%d).wav", n);
    return path;
}

namespace {

// WAV, at 16 or 24 bit integer or 32 bit float. The header is written with
// placeholder sizes and put right on Stop, so a save that is interrupted
// leaves a file that is short rather than one that claims audio it lacks.
class WavRecorder : public Recorder
{
public:
    ~WavRecorder() override { Stop(); }

    bool Start(const std::wstring& path, const RecordFormat& f, std::wstring* err) override
    {
        m_path = path;
        m_bits = f.bits;
        m_rate = f.rateHz;
        m_file.open(fp_platform::ToPath(path), std::ios::binary | std::ios::trunc);
        if (!m_file)
        {
            if (err) *err = L"Could not create " + path;
            return false;
        }
        WriteHeader(0);
        return (bool)m_file;
    }

    void Write(const float* buf, size_t frames) override
    {
        if (!m_file) return;
        const size_t n = frames * kChannels;
        m_scratch.clear();
        for (size_t i = 0; i < n; ++i)
        {
            float v = buf[i];
            if (m_bits == 32) { Put(&v, 4); continue; }
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            if (m_bits == 24)
            {
                const int32_t s = (int32_t)std::lrint(v * 8388607.0f);
                const uint8_t b[3] = { (uint8_t)s, (uint8_t)(s >> 8), (uint8_t)(s >> 16) };
                Put(b, 3);
            }
            else
            {
                const int16_t s = (int16_t)std::lrint(v * 32767.0f);
                const uint8_t b[2] = { (uint8_t)s, (uint8_t)(s >> 8) };
                Put(b, 2);
            }
        }
        m_file.write(m_scratch.data(), (std::streamsize)m_scratch.size());
        m_dataBytes += m_scratch.size();
    }

    void Stop() override
    {
        if (!m_file.is_open()) return;
        m_file.seekp(0);
        WriteHeader((uint32_t)m_dataBytes);
        m_file.close();
    }

    std::wstring Path() const override { return m_path; }

private:
    void Put(const void* p, size_t n)
    {
        const char* c = (const char*)p;
        m_scratch.insert(m_scratch.end(), c, c + n);
    }

    void U32(uint32_t v)
    {
        const char b[4] = { (char)v, (char)(v >> 8), (char)(v >> 16), (char)(v >> 24) };
        m_file.write(b, 4);
    }
    void U16(uint16_t v)
    {
        const char b[2] = { (char)v, (char)(v >> 8) };
        m_file.write(b, 2);
    }

    void WriteHeader(uint32_t dataBytes)
    {
        const uint16_t format = (m_bits == 32) ? 3 : 1;   // IEEE float or PCM
        const uint16_t block = (uint16_t)(kChannels * (m_bits / 8));
        m_file.write("RIFF", 4);
        U32(36 + dataBytes);
        m_file.write("WAVEfmt ", 8);
        U32(16);
        U16(format);
        U16((uint16_t)kChannels);
        U32((uint32_t)m_rate);
        U32((uint32_t)m_rate * block);
        U16(block);
        U16((uint16_t)m_bits);
        m_file.write("data", 4);
        U32(dataBytes);
    }

    std::wstring m_path;
    std::ofstream m_file;
    std::vector<char> m_scratch;
    unsigned long long m_dataBytes = 0;
    int m_bits = 16;
    int m_rate = kDefaultRate;
};

}  // namespace

std::unique_ptr<Recorder> MakeRecorder(RecFormat fmt)
{
    (void)fmt;
    return std::unique_ptr<Recorder>(new WavRecorder());
}

// --- the clipping monitor's tone ---------------------------------------------

void Beep(int hz, int ms) { fp_platform::Beep(hz, ms); }
