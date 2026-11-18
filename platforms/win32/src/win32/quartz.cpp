// quartz.dll: DirectShow's Filter Graph Manager (CLSID_FilterGraph) for
// games that play media files the simple way: IGraphBuilder::RenderFile,
// IMediaControl::Run, and EC_COMPLETE through IMediaEventEx's notify
// window. There are no filters here: a graph opens its file with
// libavformat (audio::MakeMediaFileSource: WMA, MP3...) and plays its
// audio stream on the mixer, as the default renderer would.
//
// One Graph holds the state; each interface a game asks for is a facade
// object with its own vtable (com.h: one guest pointer per interface),
// holding the graph. Methods are named "<Interface>::<Method>", so one a
// game calls without a handler here fails by name.
//
// The filters a graph lists (EnumFilters) are its renderers, one per
// stream: the DirectSound renderer (IBasicAudio) and the video renderer
// (IBasicVideo, IVideoWindow), answered with the graph's own interfaces.
// Each reports EC_COMPLETE at the end of its stream (one event for the
// graph unless the application cancels that default handling).
//
// Video: a decoder thread per graph (VideoDecoder) keeps frames ahead of
// the graph's clock (the audio's position, so pictures follow the sound),
// and a present overlay (present_overlay.h) draws the frame due now where
// IVideoWindow put the window, inside its owner: what on Windows is the
// renderer's child window over the game's. IVideoWindow and IBasicVideo
// methods get handlers as games are seen to call them.
//
// Test hook: <ID>_SKIP_VIDEOS=1 ends every file with a video stream as soon
// as its graph runs (both renderers complete at once), so automated runs
// and benchmarks pass movies the way the game handles their end. Audio-
// only files (music, speech) play as usual.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#include "runtime/audio/mixer.h"
#include "runtime/gfx/device.h"
#include "runtime/audio/source.h"
#include "runtime/env.h"
#include "runtime/log.h"
#include "win32/com.h"
#include "win32/hle.h"
#include "win32/kernel32_state.h"
#include "win32/present_overlay.h"
#include "win32/user32_window.h"
#include "win32/win32.h"

namespace recomp::hle {
namespace {

using guest::GUID;

constexpr const char* kDll = "quartz.dll";

constexpr GUID kCLSID_FilterGraph = {0xE436EBB3, 0x524F, 0x11CE, {0x9F, 0x53, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
constexpr GUID kIID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
#define DSHOW_IID(name, d1) \
  constexpr GUID name = {d1, 0x0AD4, 0x11CE, {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}}
DSHOW_IID(kIID_IBaseFilter, 0x56A86895);
DSHOW_IID(kIID_IEnumFilters, 0x56A86893);
DSHOW_IID(kIID_IFilterGraph, 0x56A8689F);
DSHOW_IID(kIID_IGraphBuilder, 0x56A868A9);
DSHOW_IID(kIID_IMediaControl, 0x56A868B1);
DSHOW_IID(kIID_IBasicAudio, 0x56A868B3);
DSHOW_IID(kIID_IVideoWindow, 0x56A868B4);
DSHOW_IID(kIID_IBasicVideo, 0x56A868B5);
DSHOW_IID(kIID_IMediaEvent, 0x56A868B6);
DSHOW_IID(kIID_IMediaEventEx, 0x56A868C0);
#undef DSHOW_IID
constexpr GUID kIID_IMediaSeeking = {0x36B73880, 0xC2C8, 0x11CF, {0x8B, 0x46, 0x00, 0x80, 0x5F, 0x6C, 0xEF, 0x60}};
constexpr GUID kTIME_FORMAT_MEDIA_TIME = {0x7B785574, 0x8C82, 0x11CF, {0xBC, 0x0C, 0x00, 0xAA, 0x00, 0xAC, 0x74, 0xF6}};

constexpr uint32_t S_OK = 0, S_FALSE = 1, E_NOTIMPL = 0x80004001, E_NOINTERFACE = 0x80004002,
                   E_POINTER = 0x80004003, E_ABORT = 0x80004004, E_FAIL = 0x80004005,
                   E_INVALIDARG = 0x80070057, VFW_E_NOT_FOUND = 0x80040216,
                   VFW_E_CANNOT_RENDER = 0x80040218, VFW_E_WRONG_STATE = 0x80040227;
constexpr uint32_t EC_COMPLETE = 0x01;
// OAFilterState
constexpr uint32_t State_Stopped = 0, State_Paused = 1, State_Running = 2;
// One REFERENCE_TIME unit: 100 ns.
constexpr int64_t kUnitsPerSecond = 10000000;

audio::Mixer& M() { return audio::GetMixer(); }

// ---- video decoding ---------------------------------------------------------

// Decodes a file's video stream to BGRA frames on its own thread, a few
// ahead of where playback is. FrameAt picks the frame showing at a time.
class VideoDecoder {
 public:
  ~VideoDecoder() {
    {
      std::lock_guard<std::mutex> l(mu_);
      quit_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    sws_freeContext(sws_);
    av_frame_free(&frame_);
    av_packet_free(&pkt_);
    avcodec_free_context(&ctx_);
    avformat_close_input(&fmt_);
  }

  bool Open(const std::string& path) {
    if (avformat_open_input(&fmt_, path.c_str(), nullptr, nullptr) < 0) return false;
    if (avformat_find_stream_info(fmt_, nullptr) < 0) return false;
    const AVCodec* codec = nullptr;
    stream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (stream_ < 0 || !codec) return false;
    ctx_ = avcodec_alloc_context3(codec);
    if (!ctx_ || avcodec_parameters_to_context(ctx_, fmt_->streams[stream_]->codecpar) < 0 ||
        avcodec_open2(ctx_, codec, nullptr) < 0)
      return false;
    w_ = ctx_->width;
    h_ = ctx_->height;
    if (w_ <= 0 || h_ <= 0) return false;
    pkt_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    thread_ = std::thread([this] { Run(); });
    return true;
  }

  int width() const { return w_; }
  int height() const { return h_; }

  // Restarts decoding at `units` (REFERENCE_TIME).
  void Seek(int64_t units) {
    {
      std::lock_guard<std::mutex> l(mu_);
      seek_to_ = units;
      queue_.clear();
    }
    cv_.notify_all();
  }

  // With the current frame for time `units` (the latest one at or before
  // it) calls fn(pixels, pitch); false if none is decoded yet.
  template <typename Fn>
  bool WithFrameAt(int64_t units, Fn fn) {
    std::lock_guard<std::mutex> l(mu_);
    bool advanced = false;
    while (!queue_.empty() && queue_.front().pts <= units) {
      current_ = std::move(queue_.front());
      queue_.pop_front();
      have_current_ = true;
      advanced = true;
    }
    if (advanced) cv_.notify_all();
    if (!have_current_) return false;
    fn(current_.px.data(), uint32_t(w_ * 4));
    return true;
  }

 private:
  struct Frame {
    int64_t pts = 0;  // REFERENCE_TIME
    std::vector<uint8_t> px;
  };

  void Run() {
    for (;;) {
      int64_t seek = -1;
      {
        std::unique_lock<std::mutex> l(mu_);
        cv_.wait(l, [&] { return quit_ || seek_to_ >= 0 || (queue_.size() < kAhead && !eof_); });
        if (quit_) return;
        seek = seek_to_;
        seek_to_ = -1;
      }
      if (seek >= 0) {
        const AVStream* st = fmt_->streams[stream_];
        int64_t ts = av_rescale_q(seek, AVRational{1, 10000000}, st->time_base);
        av_seek_frame(fmt_, stream_, ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(ctx_);
        std::lock_guard<std::mutex> l(mu_);
        eof_ = false;
        have_current_ = false;
        drop_before_ = seek;
        continue;
      }
      Frame f;
      if (!Decode(&f)) {
        std::lock_guard<std::mutex> l(mu_);
        eof_ = true;
        continue;
      }
      std::lock_guard<std::mutex> l(mu_);
      if (seek_to_ >= 0) continue;  // a seek came in meanwhile
      // Frames before a seek target only matter as the one showing at it.
      if (f.pts < drop_before_ && !queue_.empty()) queue_.clear();
      queue_.push_back(std::move(f));
    }
  }

  // The next frame of the stream, converted. False at the end.
  bool Decode(Frame* out) {
    for (;;) {
      int r = avcodec_receive_frame(ctx_, frame_);
      if (r == 0) {
        const AVStream* st = fmt_->streams[stream_];
        int64_t pts = frame_->best_effort_timestamp;
        out->pts = pts == AV_NOPTS_VALUE ? 0
                                         : av_rescale_q(pts, st->time_base, AVRational{1, 10000000});
        out->px.resize(size_t(w_) * h_ * 4);
        sws_ = sws_getCachedContext(sws_, frame_->width, frame_->height,
                                    AVPixelFormat(frame_->format), w_, h_, AV_PIX_FMT_BGRA,
                                    SWS_BILINEAR, nullptr, nullptr, nullptr);
        uint8_t* dst[4] = {out->px.data()};
        int pitch[4] = {w_ * 4};
        if (sws_) sws_scale(sws_, frame_->data, frame_->linesize, 0, frame_->height, dst, pitch);
        av_frame_unref(frame_);
        return true;
      }
      if (r != AVERROR(EAGAIN)) return false;
      int rr;
      while ((rr = av_read_frame(fmt_, pkt_)) >= 0 && pkt_->stream_index != stream_)
        av_packet_unref(pkt_);
      if (rr < 0) {
        if (draining_) return false;
        draining_ = true;
        avcodec_send_packet(ctx_, nullptr);
        continue;
      }
      draining_ = false;
      avcodec_send_packet(ctx_, pkt_);
      av_packet_unref(pkt_);
    }
  }

  static constexpr size_t kAhead = 4;
  AVFormatContext* fmt_ = nullptr;
  AVCodecContext* ctx_ = nullptr;
  AVPacket* pkt_ = nullptr;
  AVFrame* frame_ = nullptr;
  SwsContext* sws_ = nullptr;
  int stream_ = -1, w_ = 0, h_ = 0;
  bool draining_ = false;

  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Frame> queue_;
  Frame current_;
  bool have_current_ = false;
  bool eof_ = false, quit_ = false;
  int64_t seek_to_ = -1, drop_before_ = -1;
  std::thread thread_;
};

enum Kind { kGraph, kControl, kEvent, kSeeking, kAudio, kVideo, kWindow, kKinds };

struct Event {
  uint32_t code, p1, p2;
};

struct Graph {
  std::mutex mu;  // everything below but the voice (the mixer's lock)
  uint32_t facades[kKinds] = {};
  uint32_t state = State_Stopped;
  // The audio stream, if the file has one.
  std::unique_ptr<audio::Voice> voice;
  bool in_mixer = false;
  int rate = 0;          // its frames per second
  int64_t length = -1;   // its frames
  // The video stream, if the file has one.
  bool has_video = false;
  std::unique_ptr<VideoDecoder> video;
  int video_w = 0, video_h = 0;
  int64_t duration = -1;  // the file's, REFERENCE_TIME
  int64_t clock = 0;      // the video renderer's position, REFERENCE_TIME
  // Renderer filters (IBaseFilter, one reference held here) for the
  // streams of the rendered file. EnumFilters lists them; with
  // EC_COMPLETE's default handling cancelled, each one's completion
  // names it in lParam2.
  uint32_t audio_renderer = 0, video_renderer = 0;
  bool audio_done = false, video_done = false;  // this run, per renderer
  bool all_done = false;  // the graph's one EC_COMPLETE (default handling)
  bool cancel_complete = false;
  std::deque<Event> events;
  uint32_t notify_hwnd = 0, notify_msg = 0, notify_param = 0;
  int32_t volume = 0, balance = 0;  // IBasicAudio: hundredths of a dB
  // IVideoWindow's properties, as the application set them (by name), and
  // IBasicVideo's source and destination rectangles (left, top, w, h;
  // empty: the default, the whole video).
  std::map<std::string, int32_t> window;
  int32_t source[4] = {}, dest[4] = {};

  ~Graph() {
    if (in_mixer) {
      std::lock_guard<std::mutex> l(M().mu());
      M().Remove(voice.get());
    }
    if (audio_renderer) com::Release(audio_renderer);
    if (video_renderer) com::Release(video_renderer);
  }

  // With mu held: queues an event and tells the notify window.
  void Notify(const Event& e) {
    events.push_back(e);
    if (notify_hwnd) PostWindowMessage(notify_hwnd, notify_msg, 0, notify_param);
  }

  // With mu held: the end of a renderer's stream. Without default
  // handling the renderer's own EC_COMPLETE goes out now; with it, the
  // graph sends one once every renderer is done.
  void RendererDone(uint32_t renderer) {
    if (cancel_complete) {
      Notify({EC_COMPLETE, 0, renderer});
      return;
    }
    bool audio_end = !audio_renderer || audio_done;
    bool video_end = !video_renderer || video_done;
    if (audio_end && video_end && !all_done) {
      all_done = true;
      Notify({EC_COMPLETE, 0, 0});
    }
  }
};

struct Facade : com::Object {
  std::shared_ptr<Graph> g;
  Kind kind = kGraph;
};

// Graphs alive, for the completion monitor.
std::mutex g_graphs_mu;
std::vector<std::weak_ptr<Graph>> g_graphs;

// Advances the video clocks of running graphs and reports the ends of
// streams: the audio renderer's when its voice has played out, the video
// renderer's when its clock reaches the file's duration.
void MonitorCompletion() {
  const char* skip_env = Env("SKIP_VIDEOS");
  const bool skip_videos = skip_env && *skip_env && *skip_env != '0';
  auto last = std::chrono::steady_clock::now();
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    auto now = std::chrono::steady_clock::now();
    int64_t dt = std::chrono::duration_cast<std::chrono::nanoseconds>(now - last).count() / 100;
    last = now;
    std::vector<std::shared_ptr<Graph>> live;
    {
      std::lock_guard<std::mutex> l(g_graphs_mu);
      for (auto it = g_graphs.begin(); it != g_graphs.end();) {
        if (auto g = it->lock()) {
          live.push_back(std::move(g));
          ++it;
        } else {
          it = g_graphs.erase(it);
        }
      }
    }
    for (auto& g : live) {
      bool voice_end;
      {
        std::lock_guard<std::mutex> l(M().mu());
        voice_end = g->voice && g->voice->state == audio::Voice::State::Done;
      }
      std::lock_guard<std::mutex> l(g->mu);
      if (g->state != State_Running) continue;
      if (skip_videos && g->has_video) {
        if (g->duration >= 0) g->clock = g->duration;
        voice_end = true;
      }
      if (g->audio_renderer && voice_end && !g->audio_done) {
        g->audio_done = true;
        g->RendererDone(g->audio_renderer);
      }
      if (g->video_renderer && !g->video_done) {
        g->clock += dt;
        if (skip_videos || (g->duration >= 0 && g->clock >= g->duration)) {
          g->clock = g->duration;
          g->video_done = true;
          g->RendererDone(g->video_renderer);
        }
      }
    }
  }
}

uint32_t Vtable(Kind k) {
  static const std::vector<const char*> kIDispatch = {
      "QueryInterface", "AddRef", "Release", "GetTypeInfoCount", "GetTypeInfo",
      "GetIDsOfNames", "Invoke"};
  auto with_dispatch = [](std::vector<const char*> m) {
    std::vector<const char*> v = kIDispatch;
    v.insert(v.end(), m.begin(), m.end());
    return v;
  };
  switch (k) {
    case kGraph:
      return com::Vtable(kDll, "IGraphBuilder",
                         {"QueryInterface", "AddRef", "Release", "AddFilter", "RemoveFilter",
                          "EnumFilters", "FindFilterByName", "ConnectDirect", "Reconnect",
                          "Disconnect", "SetDefaultSyncSource", "Connect", "Render", "RenderFile",
                          "AddSourceFilter", "SetLogFile", "Abort", "ShouldOperationContinue"});
    case kControl:
      return com::Vtable(kDll, "IMediaControl",
                         with_dispatch({"Run", "Pause", "Stop", "GetState", "RenderFile",
                                        "AddSourceFilter", "get_FilterCollection",
                                        "get_RegFilterCollection", "StopWhenReady"}));
    case kEvent:
      return com::Vtable(kDll, "IMediaEventEx",
                         with_dispatch({"GetEventHandle", "GetEvent", "WaitForCompletion",
                                        "CancelDefaultHandling", "RestoreDefaultHandling",
                                        "FreeEventParams", "SetNotifyWindow", "SetNotifyFlags",
                                        "GetNotifyFlags"}));
    case kSeeking:
      return com::Vtable(kDll, "IMediaSeeking",
                         {"QueryInterface", "AddRef", "Release", "GetCapabilities",
                          "CheckCapabilities", "IsFormatSupported", "QueryPreferredFormat",
                          "GetTimeFormat", "IsUsingTimeFormat", "SetTimeFormat", "GetDuration",
                          "GetStopPosition", "GetCurrentPosition", "ConvertTimeFormat",
                          "SetPositions", "GetPositions", "GetAvailable", "SetRate", "GetRate",
                          "GetPreroll"});
    case kAudio:
      return com::Vtable(kDll, "IBasicAudio",
                         with_dispatch({"put_Volume", "get_Volume", "put_Balance", "get_Balance"}));
    case kVideo:
      return com::Vtable(
          kDll, "IBasicVideo",
          with_dispatch({"get_AvgTimePerFrame", "get_BitRate", "get_BitErrorRate",
                         "get_VideoWidth", "get_VideoHeight", "put_SourceLeft", "get_SourceLeft",
                         "put_SourceWidth", "get_SourceWidth", "put_SourceTop", "get_SourceTop",
                         "put_SourceHeight", "get_SourceHeight", "put_DestinationLeft",
                         "get_DestinationLeft", "put_DestinationWidth", "get_DestinationWidth",
                         "put_DestinationTop", "get_DestinationTop", "put_DestinationHeight",
                         "get_DestinationHeight", "SetSourcePosition", "GetSourcePosition",
                         "SetDefaultSourcePosition", "SetDestinationPosition",
                         "GetDestinationPosition", "SetDefaultDestinationPosition", "GetVideoSize",
                         "GetVideoPaletteEntries", "GetCurrentImage", "IsUsingDefaultSource",
                         "IsUsingDefaultDestination"}));
    case kWindow:
      return com::Vtable(
          kDll, "IVideoWindow",
          with_dispatch({"put_Caption", "get_Caption", "put_WindowStyle", "get_WindowStyle",
                         "put_WindowStyleEx", "get_WindowStyleEx", "put_AutoShow", "get_AutoShow",
                         "put_WindowState", "get_WindowState", "put_BackgroundPalette",
                         "get_BackgroundPalette", "put_Visible", "get_Visible", "put_Left",
                         "get_Left", "put_Width", "get_Width", "put_Top", "get_Top", "put_Height",
                         "get_Height", "put_Owner", "get_Owner", "put_MessageDrain",
                         "get_MessageDrain", "get_BorderColor", "put_BorderColor",
                         "get_FullScreenMode", "put_FullScreenMode", "SetWindowForeground",
                         "NotifyOwnerMessage", "SetWindowPosition", "GetWindowPosition",
                         "GetMinIdealImageSize", "GetMaxIdealImageSize", "GetRestorePosition",
                         "HideCursor", "IsCursorHidden"}));
    default:
      return 0;
  }
}

// The facade of `kind` for g: the live one, or a new one.
uint32_t FacadeFor(const std::shared_ptr<Graph>& g, Kind kind) {
  {
    std::lock_guard<std::mutex> l(g->mu);
    if (g->facades[kind] && com::As<Facade>(g->facades[kind])) {
      com::AddRef(g->facades[kind]);
      return g->facades[kind];
    }
  }
  auto f = std::make_shared<Facade>();
  f->g = g;
  f->kind = kind;
  uint32_t p = com::Publish(f, Vtable(kind));
  std::lock_guard<std::mutex> l(g->mu);
  g->facades[kind] = p;
  return p;
}

bool KindOf(uint32_t iid, Kind* k) {
  struct Map {
    const GUID* iid;
    Kind kind;
  };
  static const Map kMap[] = {
      {&kIID_IUnknown, kGraph},      {&kIID_IFilterGraph, kGraph},  {&kIID_IGraphBuilder, kGraph},
      {&kIID_IMediaControl, kControl}, {&kIID_IMediaEvent, kEvent}, {&kIID_IMediaEventEx, kEvent},
      {&kIID_IMediaSeeking, kSeeking}, {&kIID_IBasicAudio, kAudio}, {&kIID_IBasicVideo, kVideo},
      {&kIID_IVideoWindow, kWindow},
  };
  for (const Map& m : kMap)
    if (com::GuidEq(iid, *m.iid)) {
      *k = m.kind;
      return true;
    }
  return false;
}

// The present overlay: each graph's current video frame, where its video
// window is in the window being presented (the window's rectangle in its
// owner's client area, mapped to back-buffer pixels; IBasicVideo's
// destination rectangle within it, the whole window by default).
int64_t Position(Graph& g);
void DrawVideos(const OverlayTarget& t) {
  std::vector<std::shared_ptr<Graph>> live;
  {
    std::lock_guard<std::mutex> l(g_graphs_mu);
    for (auto& w : g_graphs)
      if (auto g = w.lock()) live.push_back(std::move(g));
  }
  int cw, ch;
  ClientAreaSize(t.hwnd, &cw, &ch);
  if (cw <= 0 || ch <= 0) return;
  for (auto& gp : live) {
    Graph& g = *gp;
    std::lock_guard<std::mutex> l(g.mu);
    if (!g.video || g.state == State_Stopped) continue;
    auto get = [&](const char* n, int32_t d) {
      auto it = g.window.find(n);
      return it != g.window.end() ? it->second : d;
    };
    if (uint32_t(get("Owner", 0)) != t.hwnd || get("Visible", 0) == 0) continue;
    int32_t wl = get("Left", 0), wt = get("Top", 0);
    int32_t ww = get("Width", g.video_w), wh = get("Height", g.video_h);
    int32_t dl = 0, dt = 0, dw = ww, dh = wh;
    if (g.dest[2] && g.dest[3]) {
      dl = g.dest[0];
      dt = g.dest[1];
      dw = g.dest[2];
      dh = g.dest[3];
    }
    int32_t sl = 0, st = 0, sw = g.video->width(), sh = g.video->height();
    if (g.source[2] && g.source[3]) {
      sl = g.source[0];
      st = g.source[1];
      sw = g.source[2];
      sh = g.source[3];
    }
    auto bx = [&](int32_t x) { return int32_t(int64_t(x) * t.bb_w / cw); };
    auto by = [&](int32_t y) { return int32_t(int64_t(y) * t.bb_h / ch); };
    const gfx::Rect dst{bx(wl + dl), by(wt + dt), bx(wl + dl + dw), by(wt + dt + dh)};
    const gfx::Rect src{sl, st, sl + sw, st + sh};
    g.video->WithFrameAt(Position(g), [&](const uint8_t* px, uint32_t pitch) {
      t.dev->Blit(px, pitch, gfx::Format::X8R8G8B8, src, dst, false, 0);
    });
  }
}

uint32_t CreateFilterGraph(uint32_t iid, uint32_t* out) {
  Kind k;
  if (!KindOf(iid, &k)) {
    guest::GUID g = com::ReadGuid(iid);
    WarnF("quartz: FilterGraph interface {%08X-...} not provided\n", g.Data1);
    *out = 0;
    return E_NOINTERFACE;
  }
  static std::once_flag monitor;
  std::call_once(monitor, [] {
    std::thread(MonitorCompletion).detach();
    AddPresentOverlay(DrawVideos);
  });
  auto g = std::make_shared<Graph>();
  {
    std::lock_guard<std::mutex> l(g_graphs_mu);
    g_graphs.push_back(g);
  }
  *out = FacadeFor(g, k);
  return S_OK;
}

com::CoClassRegistrar reg_filter_graph{kCLSID_FilterGraph, &CreateFilterGraph};

Facade* Self(Ctx& c) {
  Facade* f = com::As<Facade>(Arg(c, 0));
  if (!f) FatalF("quartz: call on unknown object %08X", uint32_t(Arg(c, 0)));
  return f;
}

// IUnknown, for every facade: (riid, ppv).
uint32_t QueryInterface(Ctx& c) {
  Facade* f = Self(c);
  uint32_t out = Arg(c, 2);
  if (!out) return E_POINTER;
  Kind k;
  if (!KindOf(Arg(c, 1), &k)) {
    guest::GUID g = com::ReadGuid(Arg(c, 1));
    RECOMP_TRACE("  quartz: QueryInterface {%08X-...}: not provided\n", g.Data1);
    Wr32(out, 0);
    return E_NOINTERFACE;
  }
  Wr32(out, FacadeFor(f->g, k));
  return S_OK;
}

// Pause/Run/Stop: the voice follows the graph state.
void SetState(Graph& g, uint32_t s) {
  std::lock_guard<std::mutex> l(M().mu());
  g.state = s;
  if (!g.voice) return;
  if (s == State_Running) {
    if (g.voice->state != audio::Voice::State::Done) g.voice->state = audio::Voice::State::Playing;
    if (!g.in_mixer) {
      M().Add(g.voice.get());
      g.in_mixer = true;
    }
  } else if (g.voice->state != audio::Voice::State::Done) {
    g.voice->state = audio::Voice::State::Paused;
  }
}

// The voice's gains from IBasicAudio's volume and balance (both hundredths
// of a dB, as DirectSound's; the voice is a DirectSound one for that).
void ApplyVolume(Graph& g) {
  if (!g.voice) return;
  std::lock_guard<std::mutex> l(M().mu());
  g.voice->volume = g.volume <= -10000 ? 0.0f : float(std::pow(10.0, g.volume / 2000.0));
  g.voice->pan = float(g.balance);
}

// With mu held: moves playback to `units` (REFERENCE_TIME, clamped to the
// file), as IMediaSeeking does in any state; streams can complete again.
void SeekTo(Graph& g, int64_t units) {
  if (units < 0) units = 0;
  if (g.duration >= 0 && units > g.duration) units = g.duration;
  g.clock = units;
  g.audio_done = g.video_done = g.all_done = false;
  if (g.video) g.video->Seek(units);
  if (!g.voice) return;
  int64_t frame = units * g.rate / kUnitsPerSecond;
  if (g.length >= 0 && frame > g.length) frame = g.length;
  std::lock_guard<std::mutex> l(M().mu());
  g.voice->Seek(frame);
  g.voice->state = g.state == State_Running ? audio::Voice::State::Playing
                                            : audio::Voice::State::Paused;
  g.voice->eos = false;
}

// With mu held: the position now, REFERENCE_TIME: the audio's if the
// file has audio, else the video clock.
int64_t Position(Graph& g) {
  if (!g.voice) return g.clock;
  std::lock_guard<std::mutex> l(M().mu());
  int64_t frame = g.voice->state == audio::Voice::State::Done && g.length >= 0
                      ? g.length
                      : g.voice->Position();
  return frame * kUnitsPerSecond / g.rate;
}

// ---- IUnknown / IDispatch on every facade -----------------------------------

#define QUARTZ_UNKNOWN(Iface, Prefix)                                                   \
  HLE_NAMED(kDll, #Iface "::QueryInterface", Prefix##_QueryInterface, 3) {              \
    return QueryInterface(c);                                                            \
  }                                                                                      \
  HLE_NAMED(kDll, #Iface "::AddRef", Prefix##_AddRef, 1) { return com::AddRef(Arg(c, 0)); } \
  HLE_NAMED(kDll, #Iface "::Release", Prefix##_Release, 1) { return com::Release(Arg(c, 0)); }
#define QUARTZ_DISPATCH(Iface, Prefix)                                                  \
  QUARTZ_UNKNOWN(Iface, Prefix)                                                          \
  HLE_NAMED(kDll, #Iface "::GetTypeInfoCount", Prefix##_GetTypeInfoCount, 2) {          \
    if (Arg(c, 1)) Wr32(Arg(c, 1), 0);                                                   \
    return S_OK;                                                                         \
  }                                                                                      \
  HLE_NAMED(kDll, #Iface "::GetTypeInfo", Prefix##_GetTypeInfo, 4) { return E_NOTIMPL; } \
  HLE_NAMED(kDll, #Iface "::GetIDsOfNames", Prefix##_GetIDsOfNames, 6) { return E_NOTIMPL; } \
  HLE_NAMED(kDll, #Iface "::Invoke", Prefix##_Invoke, 9) { return E_NOTIMPL; }

QUARTZ_UNKNOWN(IGraphBuilder, GB)
QUARTZ_UNKNOWN(IMediaSeeking, MS)
QUARTZ_DISPATCH(IMediaControl, MC)
QUARTZ_DISPATCH(IMediaEventEx, ME)
QUARTZ_DISPATCH(IBasicAudio, BA)
QUARTZ_DISPATCH(IBasicVideo, BV)
QUARTZ_DISPATCH(IVideoWindow, VW)

// ---- filters and their enumeration ----------------------------------------------

// A renderer in the graph: the audio or the video renderer. The graph
// holds it (weakly back: the graph's reference keeps it alive).
struct Filter : com::Object {
  std::weak_ptr<Graph> g;
  bool video = false;
};
struct FilterEnum : com::Object {
  std::shared_ptr<Graph> g;
  size_t next = 0;
};

uint32_t FilterVtable() {
  return com::Vtable(kDll, "IBaseFilter",
                     {"QueryInterface", "AddRef", "Release", "GetClassID", "Stop", "Pause", "Run",
                      "GetState", "SetSyncSource", "GetSyncSource", "EnumPins", "FindPin",
                      "QueryFilterInfo", "JoinFilterGraph", "QueryVendorInfo"});
}
uint32_t FilterEnumVtable() {
  return com::Vtable(kDll, "IEnumFilters",
                     {"QueryInterface", "AddRef", "Release", "Next", "Skip", "Reset", "Clone"});
}

// The graph's filters: its renderers, once a file is rendered.
std::vector<uint32_t> Filters(Graph& g) {
  std::lock_guard<std::mutex> l(g.mu);
  std::vector<uint32_t> v;
  if (g.video_renderer) v.push_back(g.video_renderer);
  if (g.audio_renderer) v.push_back(g.audio_renderer);
  return v;
}

HLE_NAMED(kDll, "IGraphBuilder::EnumFilters", GB_EnumFilters, 2) {
  Facade* f = Self(c);
  if (!Arg(c, 1)) return E_POINTER;
  auto e = std::make_shared<FilterEnum>();
  e->g = f->g;
  Wr32(Arg(c, 1), com::Publish(e, FilterEnumVtable()));
  return S_OK;
}

FilterEnum* SelfEnum(Ctx& c) {
  FilterEnum* e = com::As<FilterEnum>(Arg(c, 0));
  if (!e) FatalF("quartz: IEnumFilters call on unknown object %08X", uint32_t(Arg(c, 0)));
  return e;
}
HLE_NAMED(kDll, "IEnumFilters::QueryInterface", EF_QueryInterface, 3) {
  if (!Arg(c, 2)) return E_POINTER;
  if (com::GuidEq(Arg(c, 1), kIID_IEnumFilters) || com::GuidEq(Arg(c, 1), kIID_IUnknown)) {
    com::AddRef(Arg(c, 0));
    Wr32(Arg(c, 2), Arg(c, 0));
    return S_OK;
  }
  Wr32(Arg(c, 2), 0);
  return E_NOINTERFACE;
}
HLE_NAMED(kDll, "IEnumFilters::AddRef", EF_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED(kDll, "IEnumFilters::Release", EF_Release, 1) { return com::Release(Arg(c, 0)); }
// (cFilters, IBaseFilter** ppFilter, ULONG* pcFetched): S_FALSE when fewer
// than asked remain.
HLE_NAMED(kDll, "IEnumFilters::Next", EF_Next, 4) {
  FilterEnum* e = SelfEnum(c);
  uint32_t want = Arg(c, 1), out = Arg(c, 2), fetched = Arg(c, 3);
  if (!out) return E_POINTER;
  uint32_t n = 0;
  std::vector<uint32_t> filters = Filters(*e->g);
  while (n < want && e->next < filters.size()) {
    uint32_t r = filters[e->next];
    com::AddRef(r);
    Wr32(out + 4 * n, r);
    e->next++;
    n++;
  }
  if (fetched) Wr32(fetched, n);
  return n == want ? S_OK : S_FALSE;
}
HLE_NAMED(kDll, "IEnumFilters::Skip", EF_Skip, 2) {
  FilterEnum* e = SelfEnum(c);
  size_t total = Filters(*e->g).size();
  e->next += Arg(c, 1);
  if (e->next > total) {
    e->next = total;
    return S_FALSE;
  }
  return S_OK;
}
HLE_NAMED(kDll, "IEnumFilters::Reset", EF_Reset, 1) {
  SelfEnum(c)->next = 0;
  return S_OK;
}

Filter* SelfFilter(Ctx& c) {
  Filter* f = com::As<Filter>(Arg(c, 0));
  if (!f) FatalF("quartz: IBaseFilter call on unknown object %08X", uint32_t(Arg(c, 0)));
  return f;
}
// A renderer: IBaseFilter, and the graph's IBasicAudio (the audio
// renderer) or IBasicVideo and IVideoWindow (the video renderer).
HLE_NAMED(kDll, "IBaseFilter::QueryInterface", BF_QueryInterface, 3) {
  Filter* f = SelfFilter(c);
  uint32_t out = Arg(c, 2);
  if (!out) return E_POINTER;
  if (com::GuidEq(Arg(c, 1), kIID_IBaseFilter) || com::GuidEq(Arg(c, 1), kIID_IUnknown)) {
    com::AddRef(Arg(c, 0));
    Wr32(out, Arg(c, 0));
    return S_OK;
  }
  auto g = f->g.lock();
  if (g && !f->video && com::GuidEq(Arg(c, 1), kIID_IBasicAudio)) {
    Wr32(out, FacadeFor(g, kAudio));
    return S_OK;
  }
  if (g && f->video && com::GuidEq(Arg(c, 1), kIID_IBasicVideo)) {
    Wr32(out, FacadeFor(g, kVideo));
    return S_OK;
  }
  if (g && f->video && com::GuidEq(Arg(c, 1), kIID_IVideoWindow)) {
    Wr32(out, FacadeFor(g, kWindow));
    return S_OK;
  }
  Wr32(out, 0);
  return E_NOINTERFACE;
}
HLE_NAMED(kDll, "IBaseFilter::AddRef", BF_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED(kDll, "IBaseFilter::Release", BF_Release, 1) { return com::Release(Arg(c, 0)); }
// (FILTER_INFO*): achName[128] WCHARs, then pGraph (referenced).
HLE_NAMED(kDll, "IBaseFilter::QueryFilterInfo", BF_QueryFilterInfo, 2) {
  Filter* f = SelfFilter(c);
  uint32_t info = Arg(c, 1);
  if (!info) return E_POINTER;
  std::memset(GPtr(info), 0, 256 + 4);
  PutWStr(info, f->video ? "Video Renderer" : "Default DirectSound Device");
  if (auto g = f->g.lock()) Wr32(info + 256, FacadeFor(g, kGraph));
  return S_OK;
}

// ---- IGraphBuilder ------------------------------------------------------------

// (lpcwstrFile, lpcwstrPlayList): builds the playback graph for a file: a
// renderer for its audio stream and one for its video stream. A file with
// neither is VFW_E_CANNOT_RENDER (missing: VFW_E_NOT_FOUND).
HLE_NAMED(kDll, "IGraphBuilder::RenderFile", GB_RenderFile, 3) {
  Facade* f = Self(c);
  if (!Arg(c, 1)) return E_POINTER;
  std::string guest = NarrowPath(GWStr(Arg(c, 1)));
  std::string host = HostPath(guest);
  if (access(host.c_str(), R_OK) != 0) {
    WarnF("quartz: RenderFile(%s): not found\n", guest.c_str());
    return VFW_E_NOT_FOUND;
  }
  audio::MediaInfo info;
  if (!audio::ProbeMediaFile(host, &info) || (!info.audio && !info.video)) {
    WarnF("quartz: RenderFile(%s): no stream to render\n", guest.c_str());
    return VFW_E_CANNOT_RENDER;
  }
  std::unique_ptr<audio::Source> src;
  if (info.audio) {
    std::string err;
    src = audio::MakeMediaFileSource(host, &err);
    if (!src) WarnF("quartz: RenderFile(%s): audio not played: %s\n", guest.c_str(), err.c_str());
  }
  Graph& g = *f->g;
  std::lock_guard<std::mutex> l(g.mu);
  if (g.audio_renderer || g.video_renderer)
    FatalF("quartz: RenderFile into a graph that already has a file");
  g.duration = info.seconds >= 0 ? int64_t(info.seconds * kUnitsPerSecond) : -1;
  if (src) {
    g.rate = src->rate();
    g.length = src->Length();
    if (g.duration < 0 && g.length >= 0) g.duration = g.length * kUnitsPerSecond / g.rate;
    g.voice = std::make_unique<audio::Voice>();
    g.voice->src = std::move(src);
    g.voice->dsound = true;
    g.voice->loops = 1;
    g.voice->state = audio::Voice::State::Paused;
    auto r = std::make_shared<Filter>();
    r->g = f->g;
    g.audio_renderer = com::Publish(r, FilterVtable());
    ApplyVolume(g);
  }
  if (info.video) {
    g.has_video = true;
    g.video_w = info.width;
    g.video_h = info.height;
    auto dec = std::make_unique<VideoDecoder>();
    if (dec->Open(host)) g.video = std::move(dec);
    else WarnF("quartz: RenderFile(%s): video not decoded\n", guest.c_str());
    auto r = std::make_shared<Filter>();
    r->g = f->g;
    r->video = true;
    g.video_renderer = com::Publish(r, FilterVtable());
  }
  RECOMP_TRACE("  quartz: RenderFile(%s): audio %d Hz, video %dx%d, %.2f s\n", guest.c_str(),
               g.voice ? g.rate : 0, g.video_w, g.video_h, double(g.duration) / kUnitsPerSecond);
  return S_OK;
}

// ---- IMediaControl ------------------------------------------------------------

HLE_NAMED(kDll, "IMediaControl::Run", MC_Run, 1) {
  Graph& g = *Self(c)->g;
  std::lock_guard<std::mutex> l(g.mu);
  SetState(g, State_Running);
  // AutoShow (on unless turned off) shows the video window when the graph
  // starts.
  auto as = g.window.find("AutoShow");
  if (g.has_video && (as == g.window.end() || as->second != 0)) g.window["Visible"] = -1;
  return S_OK;
}
HLE_NAMED(kDll, "IMediaControl::Pause", MC_Pause, 1) {
  Graph& g = *Self(c)->g;
  std::lock_guard<std::mutex> l(g.mu);
  SetState(g, State_Paused);
  return S_OK;
}
// Stopping keeps the position (applications rewind with IMediaSeeking).
HLE_NAMED(kDll, "IMediaControl::Stop", MC_Stop, 1) {
  Graph& g = *Self(c)->g;
  std::lock_guard<std::mutex> l(g.mu);
  SetState(g, State_Stopped);
  return S_OK;
}
// (msTimeout, OAFilterState*): transitions here are immediate.
HLE_NAMED(kDll, "IMediaControl::GetState", MC_GetState, 3) {
  Graph& g = *Self(c)->g;
  if (!Arg(c, 2)) return E_POINTER;
  std::lock_guard<std::mutex> l(g.mu);
  Wr32(Arg(c, 2), g.state);
  return S_OK;
}

// ---- IMediaEventEx ------------------------------------------------------------

// (lEventCode*, lParam1*, lParam2*, msTimeout): the next event, or E_ABORT
// if none arrives within the timeout.
HLE_NAMED(kDll, "IMediaEventEx::GetEvent", ME_GetEvent, 5) {
  Graph& g = *Self(c)->g;
  uint32_t timeout = Arg(c, 4);
  auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
  for (;;) {
    {
      std::lock_guard<std::mutex> l(g.mu);
      if (!g.events.empty()) {
        Event e = g.events.front();
        g.events.pop_front();
        if (Arg(c, 1)) Wr32(Arg(c, 1), e.code);
        if (Arg(c, 2)) Wr32(Arg(c, 2), e.p1);
        if (Arg(c, 3)) Wr32(Arg(c, 3), e.p2);
        return S_OK;
      }
    }
    if (timeout != 0xFFFFFFFF && std::chrono::steady_clock::now() >= until) return E_ABORT;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}
// (lEvCode): the graph manager's own handling of an event is off: the
// application gets it as it arrives. For EC_COMPLETE that means one per
// renderer, naming it (see MonitorCompletion); other events are already
// delivered as they arrive.
HLE_NAMED(kDll, "IMediaEventEx::CancelDefaultHandling", ME_CancelDefaultHandling, 2) {
  Graph& g = *Self(c)->g;
  std::lock_guard<std::mutex> l(g.mu);
  if (Arg(c, 1) == EC_COMPLETE) g.cancel_complete = true;
  return S_OK;
}
HLE_NAMED(kDll, "IMediaEventEx::RestoreDefaultHandling", ME_RestoreDefaultHandling, 2) {
  Graph& g = *Self(c)->g;
  std::lock_guard<std::mutex> l(g.mu);
  if (Arg(c, 1) == EC_COMPLETE) g.cancel_complete = false;
  return S_OK;
}
// (lEventCode, lParam1, lParam2): nothing to free (no BSTR parameters).
HLE_NAMED(kDll, "IMediaEventEx::FreeEventParams", ME_FreeEventParams, 4) { return S_OK; }
// (hwnd, lMsg, lInstanceData): events are posted to hwnd as lMsg with
// lParam = lInstanceData; hwnd 0 stops that.
HLE_NAMED(kDll, "IMediaEventEx::SetNotifyWindow", ME_SetNotifyWindow, 4) {
  Graph& g = *Self(c)->g;
  std::lock_guard<std::mutex> l(g.mu);
  g.notify_hwnd = Arg(c, 1);
  g.notify_msg = Arg(c, 2);
  g.notify_param = Arg(c, 3);
  if (g.notify_hwnd && !g.events.empty())
    PostWindowMessage(g.notify_hwnd, g.notify_msg, 0, g.notify_param);
  return S_OK;
}

// ---- IMediaSeeking ------------------------------------------------------------
// Positions in REFERENCE_TIME (100 ns), the only time format here.

HLE_NAMED(kDll, "IMediaSeeking::GetCapabilities", MS_GetCapabilities, 2) {
  if (!Arg(c, 1)) return E_POINTER;
  // CanSeekAbsolute | CanSeekForwards | CanSeekBackwards | CanGetCurrentPos
  // | CanGetStopPos | CanGetDuration
  Wr32(Arg(c, 1), 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20);
  return S_OK;
}
HLE_NAMED(kDll, "IMediaSeeking::SetTimeFormat", MS_SetTimeFormat, 2) {
  return com::GuidEq(Arg(c, 1), kTIME_FORMAT_MEDIA_TIME) ? S_OK : E_INVALIDARG;
}
HLE_NAMED(kDll, "IMediaSeeking::GetDuration", MS_GetDuration, 2) {
  Graph& g = *Self(c)->g;
  if (!Arg(c, 1)) return E_POINTER;
  std::lock_guard<std::mutex> l(g.mu);
  if (g.duration < 0) return E_NOTIMPL;
  Wr64(Arg(c, 1), uint64_t(g.duration));
  return S_OK;
}
HLE_NAMED(kDll, "IMediaSeeking::GetCurrentPosition", MS_GetCurrentPosition, 2) {
  Graph& g = *Self(c)->g;
  if (!Arg(c, 1)) return E_POINTER;
  std::lock_guard<std::mutex> l(g.mu);
  Wr64(Arg(c, 1), uint64_t(Position(g)));
  return S_OK;
}
// (pCurrent, dwCurrentFlags, pStop, dwStopFlags). Positioning (flags & 3):
// none 0, absolute 1, relative 2. A stop position is not supported.
HLE_NAMED(kDll, "IMediaSeeking::SetPositions", MS_SetPositions, 5) {
  Graph& g = *Self(c)->g;
  uint32_t cur_flags = Arg(c, 2), stop_flags = Arg(c, 4);
  if ((stop_flags & 3) != 0)
    FatalF("quartz: IMediaSeeking::SetPositions with a stop position is not supported yet");
  if ((cur_flags & 3) == 0) return S_OK;
  if (!Arg(c, 1)) return E_POINTER;
  int64_t units = int64_t(Rd64(Arg(c, 1)));
  std::lock_guard<std::mutex> l(g.mu);
  if ((cur_flags & 3) == 2) units += Position(g);
  else if ((cur_flags & 3) != 1) return E_INVALIDARG;
  SeekTo(g, units);
  return S_OK;
}

// ---- IBasicAudio --------------------------------------------------------------

// (long volume): -10000 (silence) to 0 (full), hundredths of a dB.
HLE_NAMED(kDll, "IBasicAudio::put_Volume", BA_put_Volume, 2) {
  Graph& g = *Self(c)->g;
  int32_t v = int32_t(Arg(c, 1));
  if (v < -10000 || v > 0) return E_INVALIDARG;
  std::lock_guard<std::mutex> l(g.mu);
  g.volume = v;
  ApplyVolume(g);
  return S_OK;
}
HLE_NAMED(kDll, "IBasicAudio::get_Volume", BA_get_Volume, 2) {
  Graph& g = *Self(c)->g;
  if (!Arg(c, 1)) return E_POINTER;
  std::lock_guard<std::mutex> l(g.mu);
  Wr32(Arg(c, 1), uint32_t(g.volume));
  return S_OK;
}
// (long balance): -10000 (left only) to 10000 (right only).
HLE_NAMED(kDll, "IBasicAudio::put_Balance", BA_put_Balance, 2) {
  Graph& g = *Self(c)->g;
  int32_t b = int32_t(Arg(c, 1));
  if (b < -10000 || b > 10000) return E_INVALIDARG;
  std::lock_guard<std::mutex> l(g.mu);
  g.balance = b;
  ApplyVolume(g);
  return S_OK;
}
HLE_NAMED(kDll, "IBasicAudio::get_Balance", BA_get_Balance, 2) {
  Graph& g = *Self(c)->g;
  if (!Arg(c, 1)) return E_POINTER;
  std::lock_guard<std::mutex> l(g.mu);
  Wr32(Arg(c, 1), uint32_t(g.balance));
  return S_OK;
}

// ---- IVideoWindow ---------------------------------------------------------
// The video renderer's window, owned by (drawn into) the application's
// window. Its properties are kept as set; nothing is drawn yet. A graph
// without video has no video renderer: E_NOINTERFACE, as DirectShow.

Graph* VideoGraph(Ctx& c) {
  Graph* g = Self(c)->g.get();
  std::lock_guard<std::mutex> l(g->mu);
  return g->has_video ? g : nullptr;
}

// The defaults a video renderer's window starts with.
int32_t WindowDefault(const Graph& g, const std::string& name) {
  if (name == "WindowStyle") return int32_t(0x16CF0000);  // WS_OVERLAPPEDWINDOW | WS_VISIBLE...
  if (name == "AutoShow") return -1;                       // OATRUE
  if (name == "WindowState") return 1;                     // SW_SHOWNORMAL
  if (name == "Visible") return 0;                         // OAFALSE until shown
  if (name == "Width") return g.video_w;
  if (name == "Height") return g.video_h;
  return 0;
}

uint32_t PutWindow(Ctx& c, const char* name) {
  Graph* g = VideoGraph(c);
  if (!g) return E_NOINTERFACE;
  std::lock_guard<std::mutex> l(g->mu);
  g->window[name] = int32_t(Arg(c, 1));
  return S_OK;
}
uint32_t GetWindow(Ctx& c, const char* name) {
  Graph* g = VideoGraph(c);
  if (!g) return E_NOINTERFACE;
  if (!Arg(c, 1)) return E_POINTER;
  std::lock_guard<std::mutex> l(g->mu);
  auto it = g->window.find(name);
  Wr32(Arg(c, 1), uint32_t(it != g->window.end() ? it->second : WindowDefault(*g, name)));
  return S_OK;
}

#define QUARTZ_WINDOW_PROP(Name)                                                          \
  HLE_NAMED(kDll, "IVideoWindow::put_" #Name, VW_put_##Name, 2) { return PutWindow(c, #Name); } \
  HLE_NAMED(kDll, "IVideoWindow::get_" #Name, VW_get_##Name, 2) { return GetWindow(c, #Name); }
QUARTZ_WINDOW_PROP(WindowStyle)
QUARTZ_WINDOW_PROP(WindowStyleEx)
QUARTZ_WINDOW_PROP(AutoShow)
QUARTZ_WINDOW_PROP(WindowState)
QUARTZ_WINDOW_PROP(BackgroundPalette)
QUARTZ_WINDOW_PROP(Visible)
QUARTZ_WINDOW_PROP(Left)
QUARTZ_WINDOW_PROP(Width)
QUARTZ_WINDOW_PROP(Top)
QUARTZ_WINDOW_PROP(Height)
QUARTZ_WINDOW_PROP(Owner)
QUARTZ_WINDOW_PROP(MessageDrain)
QUARTZ_WINDOW_PROP(BorderColor)
QUARTZ_WINDOW_PROP(FullScreenMode)
#undef QUARTZ_WINDOW_PROP

// (BSTR caption): the renderer's own window title; not shown here.
HLE_NAMED(kDll, "IVideoWindow::put_Caption", VW_put_Caption, 2) {
  return VideoGraph(c) ? S_OK : E_NOINTERFACE;
}
// (left, top, width, height)
HLE_NAMED(kDll, "IVideoWindow::SetWindowPosition", VW_SetWindowPosition, 5) {
  Graph* g = VideoGraph(c);
  if (!g) return E_NOINTERFACE;
  std::lock_guard<std::mutex> l(g->mu);
  g->window["Left"] = int32_t(Arg(c, 1));
  g->window["Top"] = int32_t(Arg(c, 2));
  g->window["Width"] = int32_t(Arg(c, 3));
  g->window["Height"] = int32_t(Arg(c, 4));
  return S_OK;
}
// (long* left, top, width, height)
HLE_NAMED(kDll, "IVideoWindow::GetWindowPosition", VW_GetWindowPosition, 5) {
  Graph* g = VideoGraph(c);
  if (!g) return E_NOINTERFACE;
  std::lock_guard<std::mutex> l(g->mu);
  const char* names[] = {"Left", "Top", "Width", "Height"};
  for (int i = 0; i < 4; i++) {
    if (!Arg(c, 1 + i)) return E_POINTER;
    auto it = g->window.find(names[i]);
    Wr32(Arg(c, 1 + i),
         uint32_t(it != g->window.end() ? it->second : WindowDefault(*g, names[i])));
  }
  return S_OK;
}
// (long hideCursor): whether the cursor hides over the video window.
HLE_NAMED(kDll, "IVideoWindow::HideCursor", VW_HideCursor, 2) {
  return PutWindow(c, "CursorHidden");
}
HLE_NAMED(kDll, "IVideoWindow::IsCursorHidden", VW_IsCursorHidden, 2) {
  return GetWindow(c, "CursorHidden");
}
HLE_NAMED(kDll, "IVideoWindow::SetWindowForeground", VW_SetWindowForeground, 2) {
  return VideoGraph(c) ? S_OK : E_NOINTERFACE;
}
// (hwnd, uMsg, wParam, lParam): messages the owner forwards to the video
// window (palette and display changes); nothing to do here.
HLE_NAMED(kDll, "IVideoWindow::NotifyOwnerMessage", VW_NotifyOwnerMessage, 5) {
  return VideoGraph(c) ? S_OK : E_NOINTERFACE;
}

// ---- IBasicVideo ----------------------------------------------------------

HLE_NAMED(kDll, "IBasicVideo::get_VideoWidth", BV_get_VideoWidth, 2) {
  Graph* g = VideoGraph(c);
  if (!g) return E_NOINTERFACE;
  if (!Arg(c, 1)) return E_POINTER;
  Wr32(Arg(c, 1), uint32_t(g->video_w));
  return S_OK;
}
HLE_NAMED(kDll, "IBasicVideo::get_VideoHeight", BV_get_VideoHeight, 2) {
  Graph* g = VideoGraph(c);
  if (!g) return E_NOINTERFACE;
  if (!Arg(c, 1)) return E_POINTER;
  Wr32(Arg(c, 1), uint32_t(g->video_h));
  return S_OK;
}
// (long* width, long* height)
HLE_NAMED(kDll, "IBasicVideo::GetVideoSize", BV_GetVideoSize, 3) {
  Graph* g = VideoGraph(c);
  if (!g) return E_NOINTERFACE;
  if (!Arg(c, 1) || !Arg(c, 2)) return E_POINTER;
  Wr32(Arg(c, 1), uint32_t(g->video_w));
  Wr32(Arg(c, 2), uint32_t(g->video_h));
  return S_OK;
}
uint32_t SetRect(Ctx& c, bool dest) {
  Graph* g = VideoGraph(c);
  if (!g) return E_NOINTERFACE;
  std::lock_guard<std::mutex> l(g->mu);
  int32_t* r = dest ? g->dest : g->source;
  for (int i = 0; i < 4; i++) r[i] = int32_t(Arg(c, 1 + i));
  return S_OK;
}
uint32_t GetRect(Ctx& c, bool dest) {
  Graph* g = VideoGraph(c);
  if (!g) return E_NOINTERFACE;
  std::lock_guard<std::mutex> l(g->mu);
  const int32_t* r = dest ? g->dest : g->source;
  bool dflt = !r[2] || !r[3];
  for (int i = 0; i < 4; i++) {
    if (!Arg(c, 1 + i)) return E_POINTER;
    int32_t v = r[i];
    if (dflt) v = i == 2 ? g->video_w : i == 3 ? g->video_h : 0;
    Wr32(Arg(c, 1 + i), uint32_t(v));
  }
  return S_OK;
}
// (left, top, width, height) / (long* ...)
HLE_NAMED(kDll, "IBasicVideo::SetSourcePosition", BV_SetSourcePosition, 5) { return SetRect(c, false); }
HLE_NAMED(kDll, "IBasicVideo::GetSourcePosition", BV_GetSourcePosition, 5) { return GetRect(c, false); }
HLE_NAMED(kDll, "IBasicVideo::SetDestinationPosition", BV_SetDestinationPosition, 5) {
  return SetRect(c, true);
}
HLE_NAMED(kDll, "IBasicVideo::GetDestinationPosition", BV_GetDestinationPosition, 5) {
  return GetRect(c, true);
}
HLE_NAMED(kDll, "IBasicVideo::SetDefaultSourcePosition", BV_SetDefaultSourcePosition, 1) {
  Graph* g = VideoGraph(c);
  if (!g) return E_NOINTERFACE;
  std::lock_guard<std::mutex> l(g->mu);
  std::memset(g->source, 0, sizeof g->source);
  return S_OK;
}
HLE_NAMED(kDll, "IBasicVideo::SetDefaultDestinationPosition", BV_SetDefaultDestinationPosition, 1) {
  Graph* g = VideoGraph(c);
  if (!g) return E_NOINTERFACE;
  std::lock_guard<std::mutex> l(g->mu);
  std::memset(g->dest, 0, sizeof g->dest);
  return S_OK;
}

}  // namespace
}  // namespace recomp::hle
