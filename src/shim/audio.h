#pragma once
// ---------------------------------------------------------------------------
// Stand-in for FastRoute's src/audio/audio.h
//
// FastRoute's effect sources include "audio.h" for two things: the Effect
// interface they implement, and - for the buffer - the recorder they save
// through. The real header also carries the whole WASAPI engine, ASIO and
// windows.h, none of which an effect touches and none of which exists on a
// Mac. So this is the part of it the effects actually use, declared exactly
// as FastRoute declares it, and nothing else.
//
// model.h is not stood in for: it is plain C++, and including FastRoute's own
// means Block, every range and every default are whatever FastRoute says they
// are, with nothing here to fall out of step.
//
// If FastRoute changes the Effect interface, the copied sources stop matching
// this and the build fails - which is the point. Mirror the change here.
// ---------------------------------------------------------------------------
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "model.h"

static const int kChannels    = 2;
static const int kDefaultRate = 48000;

// ---------------------------------------------------------------------------
// Effects
// ---------------------------------------------------------------------------
struct RecordSettings;

class Effect
{
public:
    virtual ~Effect() {}
    virtual bool Start(int rate, std::wstring* err) = 0;
    virtual void Process(float* buf, size_t frames) = 0;
    virtual int  LatencyFrames() const { return 0; }
    virtual void SetMix(float wet) { m_wet = wet; }
    virtual void Configure(const Block& b) { (void)b; }
    virtual void ConfigureRecording(const struct RecordSettings& rs) { (void)rs; }
    virtual void DoAction(int action) { (void)action; }
    virtual std::wstring StatusText() { return std::wstring(); }

protected:
    std::atomic<float> m_wet{ 1.0f };
};

std::unique_ptr<Effect> MakeSpeexDenoise();
std::unique_ptr<Effect> MakeRnnDenoise();
std::unique_ptr<Effect> MakeToneControl();
std::unique_ptr<Effect> MakeEqualiser();
std::unique_ptr<Effect> MakeParametricEq();
std::unique_ptr<Effect> MakeChannelMixer();
std::unique_ptr<Effect> MakeCompressor();
std::unique_ptr<Effect> MakeNormaliser();
std::unique_ptr<Effect> MakeClipMonitor();
std::unique_ptr<Effect> MakeBuffer();
std::unique_ptr<Effect> MakeLimiter();
std::unique_ptr<Effect> MakeNoiseGate();
std::unique_ptr<Effect> MakePitchShift();
// FastRoute's plugin host. Not something a plugin can be: always null here.
std::unique_ptr<Effect> MakePluginEffect();
std::unique_ptr<Effect> MakeEffectFor(const std::wstring& typeId);

// The clipping monitor's tone. FastRoute calls the Win32 Beep(); this is the
// same call made portable, implemented in platform.cpp.
void Beep(int hz, int ms);

// ---------------------------------------------------------------------------
// Recording, as far as the buffer's Save needs it
//
// The buffer saves through the same recorder a Recorder block uses. Here
// that recorder writes WAV and nothing else: FastRoute's FLAC, MP3 and AAC go
// through Media Foundation, which is Windows only.
// ---------------------------------------------------------------------------
enum class RecFormat { Wav = 0, Flac = 1, Mp3 = 2, Aac = 3, Ogg = 4, Opus = 5 };

struct RecordFormat
{
    int inRate = kDefaultRate;
    int rateHz = kDefaultRate;
    int bits   = 16;
    int kbps   = 192;
};

class Recorder
{
public:
    virtual ~Recorder() {}
    virtual bool Start(const std::wstring& path, const RecordFormat& f,
                       std::wstring* err) = 0;
    virtual void Write(const float* buf, size_t frames) = 0;
    virtual void Stop() = 0;
    virtual std::wstring Path() const = 0;
};

RecordFormat RecFileFormatFor(RecFormat fmt, int rateHz, int bits, int kbps,
                              int sessionRate);
std::unique_ptr<Recorder> MakeRecorder(RecFormat fmt);
std::wstring ResolveRecordingPath(const std::wstring& folder,
                                  const std::wstring& nameTemplate,
                                  const std::wstring& sessionName,
                                  RecFormat fmt,
                                  const std::wstring& blockName = std::wstring(),
                                  int blockIndex = 0, int blockCount = 0);

struct RecordSettings
{
    std::wstring folder;
    std::wstring nameTemplate;
    std::wstring sessionName;
    std::wstring blockName;
    int  blockIndex = 0;
    int  blockCount = 0;
    RecFormat fmt = RecFormat::Wav;
    int  kbps = 192;
    int  rateHz = 0;
    int  bits = 0;

    bool skipSilence = false;
    int  silenceDb = -50;
    int  holdSeconds = 2;

    int  splitMode = 0;
    int  splitMinutes = 30;
    int  splitMb = 100;
};

// Where the buffer saves to: a FastPlugs folder in the user's Documents.
std::wstring DefaultSaveFolder();
