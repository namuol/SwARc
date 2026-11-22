// winmm waveOut on an SDL audio stream: one device (the wave mapper and
// device 0 are the default playback device), PCM 8 or 16 bit or IEEE float
// (also as WAVE_FORMAT_EXTENSIBLE), mono or stereo, any rate (SDL
// resamples).
//
// waveOutWrite queues the buffer's samples on the stream at once and marks
// the header WHDR_INQUEUE; a thread marks it WHDR_DONE when the device has
// consumed up to its end (the bytes queued so far minus those still in the
// stream). Games that poll WHDR_DONE (CALLBACK_NULL) see their buffers come
// back at the rate they play, as on Windows. With CALLBACK_FUNCTION the
// thread then calls the game's waveOutProc (WOM_DONE; WOM_OPEN and
// WOM_CLOSE come from waveOutOpen and waveOutClose), as Windows calls it
// from a thread of its own; CALLBACK_EVENT sets the event. Window and
// thread callbacks are not supported yet: opening with one is fatal and
// names it.
//
// Dev aid: <ID>_WAVE_DUMP=file.wav also writes everything played to a WAV
// file (header sizes fixed up as it grows).

#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "runtime/env.h"
#include "runtime/log.h"
#include "win32/hle.h"
#include "win32/kernel32_state.h"

namespace recomp::hle {
namespace {

constexpr const char* kDll = "winmm.dll";

constexpr uint32_t kMmNoError = 0, kMmBadDeviceId = 2, kMmAllocated = 4, kMmInvalHandle = 5,
                   kMmInvalParam = 11, kWavErrBadFormat = 32, kWavErrStillPlaying = 33,
                   kWavErrUnprepared = 34;
constexpr uint32_t kWhdrDone = 1, kWhdrPrepared = 2, kWhdrInQueue = 0x10;
constexpr uint32_t kWaveMapper = 0xFFFFFFFF;
constexpr uint32_t kHandle = 0x00C10001;  // the one open device
constexpr uint32_t kWomOpen = 0x3BB, kWomClose = 0x3BC, kWomDone = 0x3BD;
constexpr uint32_t kCallbackEvent = 0x50000, kCallbackFunction = 0x30000;

// WAVEHDR: lpData, dwBufferLength, dwBytesRecorded, dwUser, dwFlags,
// dwLoops, lpNext, reserved; on x64 the pointers and dwUser are 8 bytes.
struct HdrLayout {
  uint32_t len, flags, loops;
};
HdrLayout HdrFields() { return g_proc->x64 ? HdrLayout{8, 24, 28} : HdrLayout{4, 16, 20}; }

struct Pending {
  gaddr hdr;
  uint64_t end;  // total bytes queued once this buffer is in
};

struct Device {
  std::mutex mu;
  SDL_AudioStream* stream = nullptr;
  uint64_t queued = 0;  // bytes put on the stream since open
  std::deque<Pending> pending;
  std::atomic<bool> stop{false};
  std::atomic<bool> stopped{true};  // the watcher has returned
  uint32_t block = 1;
  uint32_t callback = 0;  // fdwOpen's CALLBACK_* type
  gaddr proc = 0;         // its function or event handle
  uint64_t instance = 0;  // dwInstance
};
Device g_dev;
bool g_open = false;
// The guest thread the watcher runs on (Windows calls waveOutProc from a
// thread of its own); made at the first open.
Thread* g_thread = nullptr;

// <ID>_WAVE_DUMP: the WAV file, its format and the data bytes so far.
std::FILE* g_dump = nullptr;
uint32_t g_dump_bytes = 0;

void DumpOpen(const SDL_AudioSpec& spec, uint32_t block) {
  const char* path = Env("WAVE_DUMP");
  if (!path || !*path || g_dump) return;
  g_dump = std::fopen(path, "wb");
  if (!g_dump) {
    WarnF("WAVE_DUMP: cannot open %s\n", path);
    return;
  }
  // PCM, or IEEE float (3) for float streams.
  uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0,
                   uint8_t(spec.format == SDL_AUDIO_F32LE ? 3 : 1), 0};
  auto p16 = [&](int o, uint16_t v) { std::memcpy(h + o, &v, 2); };
  auto p32 = [&](int o, uint32_t v) { std::memcpy(h + o, &v, 4); };
  p16(22, uint16_t(spec.channels));
  p32(24, uint32_t(spec.freq));
  p32(28, uint32_t(spec.freq) * block);
  p16(32, uint16_t(block));
  p16(34, uint16_t(spec.format == SDL_AUDIO_U8 ? 8 : spec.format == SDL_AUDIO_F32LE ? 32 : 16));
  std::memcpy(h + 36, "data", 4);
  std::fwrite(h, 1, sizeof h, g_dump);
}

void DumpWrite(const void* data, uint32_t len) {
  if (!g_dump) return;
  std::fwrite(data, 1, len, g_dump);
  g_dump_bytes += len;
  uint32_t riff = 36 + g_dump_bytes;
  long at = std::ftell(g_dump);
  std::fseek(g_dump, 4, SEEK_SET);
  std::fwrite(&riff, 4, 1, g_dump);
  std::fseek(g_dump, 40, SEEK_SET);
  std::fwrite(&g_dump_bytes, 4, 1, g_dump);
  std::fseek(g_dump, at, SEEK_SET);
  std::fflush(g_dump);
}

void MarkDone(gaddr hdr) {
  const uint32_t at = HdrFields().flags;
  uint32_t f = Rd32(hdr + at);
  Wr32(hdr + at, (f & ~kWhdrInQueue) | kWhdrDone);
}

// Tells the game about a message: its waveOutProc(hwo, msg, instance,
// param1, param2) on the watcher's guest thread, or its event.
void Notify(Ctx* c, uint32_t msg, gaddr hdr) {
  if (g_dev.callback == kCallbackFunction && c) {
    GuestSyncFence();
    CallGuest(*c, g_dev.proc, {HandleRet(kHandle), msg, g_dev.instance, hdr, 0});
  } else if (g_dev.callback == kCallbackEvent) {
    SignalEventHandle(uint32_t(g_dev.proc), false);
  }
}

// Marks the buffers the device has played through, and tells the game.
void Retire(Ctx* c) {
  std::vector<gaddr> done;
  {
    std::lock_guard<std::mutex> l(g_dev.mu);
    if (!g_dev.stream) return;
    int left = SDL_GetAudioStreamQueued(g_dev.stream);
    uint64_t played = g_dev.queued - uint64_t(left < 0 ? 0 : left);
    while (!g_dev.pending.empty() && g_dev.pending.front().end <= played) {
      MarkDone(g_dev.pending.front().hdr);
      done.push_back(g_dev.pending.front().hdr);
      g_dev.pending.pop_front();
    }
  }
  for (gaddr h : done) Notify(c, kWomDone, h);
}

void Watch(Thread* t) {
  while (!g_dev.stop.load()) {
    Retire(&t->ctx);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  g_dev.stopped = true;
}

// WAVEFORMATEX: wFormatTag, nChannels, nSamplesPerSec, nAvgBytesPerSec,
// nBlockAlign, wBitsPerSample, cbSize. WAVE_FORMAT_EXTENSIBLE (0xFFFE)
// names the sample type by the first dword of its SubFormat GUID (at 24):
// 1 PCM, 3 IEEE float, as the plain tags.
bool SpecOf(gaddr fmt, SDL_AudioSpec* spec, uint32_t* block) {
  uint16_t tag = Rd16(fmt), ch = Rd16(fmt + 2), bits = Rd16(fmt + 14);
  uint32_t rate = Rd32(fmt + 4);
  if (tag == 0xFFFE && Rd16(fmt + 16) >= 22) tag = uint16_t(Rd32(fmt + 24));
  if ((ch != 1 && ch != 2) || rate < 1000 || rate > 192000) return false;
  if (tag == 1 && (bits == 8 || bits == 16)) {  // WAVE_FORMAT_PCM
    spec->format = bits == 8 ? SDL_AUDIO_U8 : SDL_AUDIO_S16LE;
  } else if (tag == 3 && bits == 32) {  // WAVE_FORMAT_IEEE_FLOAT
    spec->format = SDL_AUDIO_F32LE;
  } else {
    return false;
  }
  spec->channels = ch;
  spec->freq = int(rate);
  *block = ch * bits / 8u;
  return true;
}

HLE64(kDll, waveOutGetNumDevs, 0) { return 1; }

// (uDeviceID, WAVEOUTCAPSA*, cbwoc)
HLE64(kDll, waveOutGetDevCapsA, 3) {
  uint32_t id = uint32_t(Arg(c, 0)), size = uint32_t(Arg(c, 2));
  gaddr caps = Arg(c, 1);
  if (id != 0 && id != kWaveMapper) return kMmBadDeviceId;
  if (!caps) return kMmInvalParam;
  uint8_t buf[52] = {};
  auto p16 = [&](int o, uint16_t v) { std::memcpy(buf + o, &v, 2); };
  auto p32 = [&](int o, uint32_t v) { std::memcpy(buf + o, &v, 4); };
  p16(0, 1);           // wMid: MM_MICROSOFT
  p16(2, 100);         // wPid: MM_WAVE_MAPPER
  p32(4, 0x0500);      // vDriverVersion 5.0
  std::strcpy(reinterpret_cast<char*>(buf + 8), "Speakers");
  p32(40, 0x000FFFFF); // dwFormats: every WAVE_FORMAT_* through 96 kHz
  p16(44, 2);          // wChannels
  p32(48, 0x002C);     // dwSupport: VOLUME | LRVOLUME | SAMPLEACCURATE
  std::memcpy(GPtr(caps), buf, std::min<uint32_t>(size, sizeof buf));
  return kMmNoError;
}

// (uDeviceID, WAVEOUTCAPSW*, cbwoc): as the A form, the name in UTF-16
// (WCHAR[32]), so the later fields sit 32 bytes further on.
HLE64(kDll, waveOutGetDevCapsW, 3) {
  uint32_t id = uint32_t(Arg(c, 0)), size = uint32_t(Arg(c, 2));
  gaddr caps = Arg(c, 1);
  if (id != 0 && id != kWaveMapper) return kMmBadDeviceId;
  if (!caps) return kMmInvalParam;
  uint8_t buf[84] = {};
  auto p16 = [&](int o, uint16_t v) { std::memcpy(buf + o, &v, 2); };
  auto p32 = [&](int o, uint32_t v) { std::memcpy(buf + o, &v, 4); };
  p16(0, 1);
  p16(2, 100);
  p32(4, 0x0500);
  const char* name = "Speakers";
  for (int i = 0; name[i]; i++) p16(8 + 2 * i, uint16_t(name[i]));
  p32(72, 0x000FFFFF);
  p16(76, 2);
  p32(80, 0x002C);
  std::memcpy(GPtr(caps), buf, std::min<uint32_t>(size, sizeof buf));
  return kMmNoError;
}

// waveIn: no recording devices. With none to open, no handle exists for
// the per-handle calls either.
HLE64(kDll, waveInGetNumDevs, 0) { return 0; }
HLE64(kDll, waveInGetDevCapsA, 3) { return kMmBadDeviceId; }
HLE64(kDll, waveInGetDevCapsW, 3) { return kMmBadDeviceId; }
HLE64(kDll, waveInOpen, 6) {
  if (Arg(c, 0)) WrPtr(Arg(c, 0), 0);
  return kMmBadDeviceId;
}
#define WAVE_IN_NO_HANDLE(name, n) \
  HLE64(kDll, name, n) { return 5; /* MMSYSERR_INVALHANDLE */ }
WAVE_IN_NO_HANDLE(waveInClose, 1)
WAVE_IN_NO_HANDLE(waveInReset, 1)
WAVE_IN_NO_HANDLE(waveInStart, 1)
WAVE_IN_NO_HANDLE(waveInStop, 1)
WAVE_IN_NO_HANDLE(waveInPrepareHeader, 3)
WAVE_IN_NO_HANDLE(waveInUnprepareHeader, 3)
WAVE_IN_NO_HANDLE(waveInAddBuffer, 3)
#undef WAVE_IN_NO_HANDLE

// (phwo, uDeviceID, pwfx, dwCallback, dwInstance, fdwOpen)
HLE64(kDll, waveOutOpen, 6) {
  gaddr out = Arg(c, 0), fmt = Arg(c, 2);
  uint32_t id = uint32_t(Arg(c, 1)), flags = uint32_t(Arg(c, 5));
  if (id != 0 && id != kWaveMapper) return kMmBadDeviceId;
  SDL_AudioSpec spec;
  uint32_t block;
  if (!fmt || !SpecOf(fmt, &spec, &block)) return kWavErrBadFormat;
  if (flags & 1) return kMmNoError;  // WAVE_FORMAT_QUERY
  const uint32_t callback = flags & 0x70000;
  if (callback && callback != kCallbackFunction && callback != kCallbackEvent)
    FatalF("waveOutOpen: callback type %X not supported (only none, function and event)",
           callback);
  if (g_open) return kMmAllocated;
  if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    WarnF("waveOutOpen: %s\n", SDL_GetError());
    return kMmBadDeviceId;
  }
  SDL_AudioStream* s =
      SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
  if (!s) {
    WarnF("waveOutOpen: %s\n", SDL_GetError());
    return kMmBadDeviceId;
  }
  SDL_ResumeAudioStreamDevice(s);
  {
    std::lock_guard<std::mutex> l(g_dev.mu);
    g_dev.stream = s;
    g_dev.queued = 0;
    g_dev.pending.clear();
    g_dev.block = block;
    g_dev.callback = callback;
    g_dev.proc = Arg(c, 3);
    g_dev.instance = Arg(c, 4);
  }
  DumpOpen(spec, block);
  if (!g_thread) g_thread = CreateGuestThread(0x100000);
  g_dev.stop = false;
  g_dev.stopped = false;
  Thread* t = g_thread;
  StartServiceThread(t, [t] { Watch(t); });
  g_open = true;
  if (out) WrPtr(out, HandleRet(kHandle));
  RECOMP_TRACE("  waveOut: open %d Hz, %d ch, %s\n", spec.freq, spec.channels,
               spec.format == SDL_AUDIO_U8      ? "8 bit"
               : spec.format == SDL_AUDIO_F32LE ? "float"
                                                : "16 bit");
  Notify(&c, kWomOpen, 0);
  return kMmNoError;
}

// Drops what is queued; the buffers come back done (WOM_DONE each, as
// Windows sends them).
void Reset(Ctx& c) {
  std::vector<gaddr> done;
  {
    std::lock_guard<std::mutex> l(g_dev.mu);
    if (g_dev.stream) SDL_ClearAudioStream(g_dev.stream);
    for (const Pending& p : g_dev.pending) {
      MarkDone(p.hdr);
      done.push_back(p.hdr);
    }
    g_dev.pending.clear();
    g_dev.queued = 0;
  }
  for (gaddr h : done) Notify(&c, kWomDone, h);
}

HLE64(kDll, waveOutClose, 1) {
  if (!g_open || uint32_t(Arg(c, 0)) != kHandle) return kMmInvalHandle;
  {
    std::lock_guard<std::mutex> l(g_dev.mu);
    if (!g_dev.pending.empty()) return kWavErrStillPlaying;
  }
  g_dev.stop = true;
  while (!g_dev.stopped) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  {
    std::lock_guard<std::mutex> l(g_dev.mu);
    SDL_DestroyAudioStream(g_dev.stream);
    g_dev.stream = nullptr;
  }
  g_open = false;
  Notify(&c, kWomClose, 0);
  return kMmNoError;
}

HLE64(kDll, waveOutReset, 1) {
  if (!g_open || uint32_t(Arg(c, 0)) != kHandle) return kMmInvalHandle;
  Reset(c);
  return kMmNoError;
}

// (hwo, WAVEHDR*, cbwh); see HdrFields.
HLE64(kDll, waveOutPrepareHeader, 3) {
  gaddr h = Arg(c, 1);
  if (!h) return kMmInvalParam;
  const uint32_t at = HdrFields().flags;
  Wr32(h + at, (Rd32(h + at) & ~kWhdrDone) | kWhdrPrepared);
  return kMmNoError;
}

HLE64(kDll, waveOutUnprepareHeader, 3) {
  gaddr h = Arg(c, 1);
  if (!h) return kMmInvalParam;
  const uint32_t at = HdrFields().flags;
  uint32_t f = Rd32(h + at);
  if (f & kWhdrInQueue) return kWavErrStillPlaying;
  Wr32(h + at, f & ~kWhdrPrepared);
  return kMmNoError;
}

HLE64(kDll, waveOutWrite, 3) {
  gaddr h = Arg(c, 1);
  if (!g_open || uint32_t(Arg(c, 0)) != kHandle) return kMmInvalHandle;
  const HdrLayout hf = HdrFields();
  uint32_t f = Rd32(h + hf.flags);
  if (!(f & kWhdrPrepared)) return kWavErrUnprepared;
  if (Rd32(h + hf.loops) > 1 || (f & 0xC))
    WarnF("waveOutWrite: loops not supported; played once\n");
  uint32_t len = Rd32(h + hf.len);
  const gaddr data = RdPtr(h);
  std::lock_guard<std::mutex> l(g_dev.mu);
  Wr32(h + hf.flags, (f & ~kWhdrDone) | kWhdrInQueue);
  len -= len % g_dev.block;
  if (len) {
    SDL_PutAudioStreamData(g_dev.stream, GPtr(data), int(len));
    DumpWrite(GPtr(data), len);
  }
  g_dev.queued += len;
  g_dev.pending.push_back({h, g_dev.queued});
  return kMmNoError;
}

}  // namespace
}  // namespace recomp::hle
