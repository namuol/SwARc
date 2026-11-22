#pragma once
// Host objects behind the Direct3D 9 interfaces, shared by d3d9.cpp
// (IDirect3D9, the device's core) and d3d9_resource.cpp (surfaces,
// textures, buffers, vertex declarations, shaders, queries).

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "runtime/gfx/device.h"
#include "runtime/hlsl/hlsl.h"
#include "win32/com.h"
#include "win32/d3d9_types.h"
#include "x86/ctx.h"
#include "x86/runtime.h"

namespace recomp::d3d9 {

constexpr uint32_t D3D_OK = 0;
constexpr uint32_t D3DERR_NOTFOUND = 0x88760866;
constexpr uint32_t D3DERR_NOTAVAILABLE = 0x8876086A;
constexpr uint32_t D3DERR_INVALIDCALL = 0x8876086C;
constexpr uint32_t S_FALSE = 1;

// D3DFORMAT values
enum : uint32_t {
  D3DFMT_UNKNOWN = 0, D3DFMT_R8G8B8 = 20, D3DFMT_A8R8G8B8 = 21,
  D3DFMT_X8R8G8B8 = 22, D3DFMT_R5G6B5 = 23, D3DFMT_X1R5G5B5 = 24,
  D3DFMT_A1R5G5B5 = 25, D3DFMT_A4R4G4B4 = 26, D3DFMT_R3G3B2 = 27,
  D3DFMT_A8 = 28, D3DFMT_X4R4G4B4 = 30, D3DFMT_P8 = 41, D3DFMT_L8 = 50,
  D3DFMT_A8L8 = 51, D3DFMT_D16_LOCKABLE = 70, D3DFMT_D32 = 71,
  D3DFMT_D15S1 = 73, D3DFMT_D24S8 = 75, D3DFMT_D24X8 = 77, D3DFMT_D16 = 80,
  D3DFMT_INDEX16 = 101, D3DFMT_INDEX32 = 102, D3DFMT_R16F = 111,
  D3DFMT_G16R16F = 112, D3DFMT_A16B16G16R16F = 113, D3DFMT_R32F = 114,
  D3DFMT_G32R32F = 115, D3DFMT_A32B32G32R32F = 116,
  D3DFMT_DXT1 = 0x31545844, D3DFMT_DXT2 = 0x32545844,
  D3DFMT_DXT3 = 0x33545844, D3DFMT_DXT4 = 0x34545844,
  D3DFMT_DXT5 = 0x35545844,
};
// D3DRESOURCETYPE
enum : uint32_t {
  D3DRTYPE_SURFACE = 1, D3DRTYPE_VOLUME = 2, D3DRTYPE_TEXTURE = 3,
  D3DRTYPE_VOLUMETEXTURE = 4, D3DRTYPE_CUBETEXTURE = 5,
  D3DRTYPE_VERTEXBUFFER = 6, D3DRTYPE_INDEXBUFFER = 7,
};
// D3DUSAGE
enum : uint32_t {
  D3DUSAGE_RENDERTARGET = 0x1, D3DUSAGE_DEPTHSTENCIL = 0x2,
  D3DUSAGE_WRITEONLY = 0x8, D3DUSAGE_DYNAMIC = 0x200,
  D3DUSAGE_AUTOGENMIPMAP = 0x400,
  D3DUSAGE_QUERY_SRGBREAD = 0x10000, D3DUSAGE_QUERY_FILTER = 0x20000,
  D3DUSAGE_QUERY_SRGBWRITE = 0x40000,
  D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING = 0x80000,
  D3DUSAGE_QUERY_VERTEXTEXTURE = 0x100000,
  D3DUSAGE_QUERY_WRAPANDMIP = 0x200000,
};
// D3DPOOL
enum : uint32_t {
  D3DPOOL_DEFAULT = 0, D3DPOOL_MANAGED = 1, D3DPOOL_SYSTEMMEM = 2,
  D3DPOOL_SCRATCH = 3,
};
// D3DLOCK
enum : uint32_t {
  D3DLOCK_READONLY = 0x10, D3DLOCK_NOOVERWRITE = 0x1000,
  D3DLOCK_DISCARD = 0x2000, D3DLOCK_NO_DIRTY_UPDATE = 0x8000,
};

gfx::Format FormatFromD3D(uint32_t f);
bool IsDepthFormat(gfx::Format f);
// Formats a render target can have (a swap chain: IsBackBufferFormat).
bool IsRenderTargetFormat(uint32_t f);
bool IsBackBufferFormat(uint32_t f);

template <typename T>
T* G(uint32_t va) {
  return reinterpret_cast<T*>(GPtr(va));
}

struct Texture;

struct Surface final : com::Object {
  enum class Kind {
    BackBuffer,    // the swap chain's: gfx render target 0
    DepthStencil,  // gfx depth buffer
    TextureLevel,  // a level of `texture` (its refcount is the texture's)
    RenderTarget,  // CreateRenderTarget: a gfx render-target texture
    Offscreen,     // CreateOffscreenPlainSurface: memory only
  };
  Kind kind = Kind::BackBuffer;
  uint32_t device = 0;  // IDirect3DDevice9 guest pointer
  guest::D3DSURFACE_DESC desc{};
  gfx::DepthId depth = 0;     // DepthStencil
  gfx::TextureId tex = 0;     // RenderTarget
  Texture* texture = nullptr;  // TextureLevel
  uint32_t container = 0;      // TextureLevel: the texture's guest pointer
  int face = 0, level = 0;
  uint32_t mem = 0;  // Offscreen: its pixels
  uint32_t pitch = 0;
  uint64_t vram = 0;  // charged to the device (ChargeVram)
  ~Surface() override;
};

struct Texture final : com::Object {
  uint32_t device = 0;
  bool cube = false;  // IDirect3DCubeTexture9: six faces of levels each
  bool volume = false;  // IDirect3DVolumeTexture9: depth slices per level
  uint32_t width = 0, height = 0, depth = 1, levels = 0, usage = 0, format = 0, pool = 0;
  gfx::Format fmt = gfx::Format::Unknown;
  gfx::TextureId tex = 0;  // none for SYSTEMMEM/SCRATCH
  struct Level {
    uint32_t w = 0, h = 0, pitch = 0, size = 0;
    uint32_t d = 1, slice = 0;  // volume levels: depth and slice pitch (size = slice * d)
    uint32_t locked_front = 0, locked_back = 0;  // volume levels: the locked box's depth
    uint32_t mem = 0;  // lock shadow, allocated on first lock
    bool locked = false;
    gfx::Rect locked_rect{};
    bool lock_readonly = false;
    std::shared_ptr<Surface> surface;  // GetSurfaceLevel, created lazily
  };
  std::vector<Level> lv;  // face * levels + level
  Level& At(uint32_t face, uint32_t level) { return lv[face * levels + level]; }
  uint32_t Faces() const { return cube ? 6 : 1; }
  uint32_t lod = 0, priority = 0, autogen_filter = 2;  // D3DTEXF_LINEAR
  uint64_t vram = 0;  // charged to the device (ChargeVram)
  ~Texture() override;
};

struct Buffer final : com::Object {
  uint32_t device = 0;
  bool index = false;
  uint32_t length = 0, usage = 0, pool = 0;
  uint32_t fvf_or_format = 0;  // FVF (vertex) or D3DFORMAT (index)
  uint32_t mem = 0;            // the data, in guest memory
  uint32_t priority = 0;
  // Changes (nonzero) whenever the data may have: at creation and at each
  // Unlock. Draws pass it as their streams' gfx::FfStream::lock, so gfx
  // copies a range of a buffer once per pass, not once per draw, and keeps
  // a static buffer's ranges on the GPU across frames.
  uint32_t ticket = 0;
  ~Buffer() override;
};

// A fresh buffer ticket (Buffer::ticket): never 0.
inline uint32_t NextTicket() {
  static std::atomic<uint32_t> next{0};
  uint32_t t;
  while (!(t = ++next)) {
  }
  return t;
}

struct VertexDecl final : com::Object {
  uint32_t device = 0;
  std::vector<guest::D3DVERTEXELEMENT9> elements;  // with the END marker
};

struct Shader final : com::Object {
  uint32_t device = 0;
  bool pixel = false;
  std::vector<uint32_t> tokens;  // the function, END token included
  // A function D3DXCompileShader translated (TranslatedFunction), and
  // the gfx shader made from it.
  std::shared_ptr<const hlsl::Program> program;
  gfx::ShaderId id = 0;
  ~Shader() override;
};

struct Query final : com::Object {
  uint32_t device = 0;
  uint32_t type = 0;
};

struct Device : com::Object {
  uint32_t d3d = 0;  // IDirect3D9 guest pointer
  // The gamma ramp (D3DGAMMARAMP: 256 red, green and blue WORDs), as last
  // set; identity until then. Not applied to the picture yet.
  std::array<uint16_t, 3 * 256> gamma = [] {
    std::array<uint16_t, 3 * 256> g{};
    for (int i = 0; i < 3 * 256; i++) g[i] = uint16_t((i % 256) * 257);
    return g;
  }();
  guest::D3DDEVICE_CREATION_PARAMETERS creation{};
  guest::D3DPRESENT_PARAMETERS pp{};
  uint32_t hwnd = 0;
  std::unique_ptr<gfx::Device> gfx;
  std::shared_ptr<Surface> backbuffer, auto_depth;
  // Bound objects: guest pointers the device holds a reference to.
  uint32_t render_targets[4] = {};
  uint32_t depth_stencil = 0;
  uint32_t textures[20] = {};  // samplers 0-15, then vertex samplers 0-3
  struct Stream {
    uint32_t buffer = 0, offset = 0, stride = 0, freq = 1;
  };
  Stream streams[16];
  uint32_t indices = 0;
  uint32_t decl = 0, vs = 0, ps = 0;
  uint32_t rs[256] = {};
  uint32_t ss[20][14] = {};  // samplers 0-15 and the 4 vertex samplers
  uint32_t tss[8][33] = {};
  uint32_t fvf = 0;
  guest::BOOL software_vp = 0;
  std::map<uint32_t, std::array<float, 16>> transforms;
  // The draws' starting state: D3D's defaults with the set transforms in
  // place (SetTransform writes both), so DrawState looks none of them up.
  gfx::FfState ff_base = gfx::DefaultFfState();
  // Fixed-function lighting: D3DMATERIAL9 and D3DLIGHT9 have gfx's (D3D7's)
  // layout. Lights by index, set and enabled independently.
  gfx::Material material = gfx::DefaultFfState().material;
  std::map<uint32_t, gfx::Light> lights;
  std::map<uint32_t, bool> light_on;
  guest::D3DVIEWPORT9 viewport{};
  guest::D3DRECT scissor{};
  float vs_f[256][4] = {}, ps_f[224][4] = {};
  int32_t vs_i[16][4] = {}, ps_i[16][4] = {};
  guest::BOOL vs_b[16] = {}, ps_b[16] = {};
  int scene_depth = 0;
  // Video memory held by D3DPOOL_DEFAULT textures and the render-target
  // and depth-stencil surfaces made for the game (ChargeVram).
  uint64_t vram_used = 0;
  // Pixel shaders standing in for the fixed-function pixel stage behind a
  // vertex shader (FfPixelShader in d3d9_draw.cpp), by their HLSL source.
  std::map<std::string, gfx::ShaderId> ff_pixel_shaders;
  // The D3D cursor (SetCursorProperties / ShowCursor).
  bool cursor_shown = false, cursor_set = false;
  // Its image (SetCursorProperties): A8R8G8B8 rows, and the hot spot.
  std::vector<uint32_t> cursor_px;
  uint32_t cursor_w = 0, cursor_h = 0, cursor_hot_x = 0, cursor_hot_y = 0;
  // Where SetCursorPosition put it (desktop coordinates), once it has.
  bool cursor_positioned = false;
  int cursor_x = 0, cursor_y = 0;
  // Vertex shaders standing in for the fixed-function vertex stage in
  // draws with a pixel shader (d3d9_draw.cpp), by their HLSL source.
  struct FfVertexShader {
    std::shared_ptr<const hlsl::Program> program;
    gfx::ShaderId id = 0;
  };
  std::map<std::string, FfVertexShader> ff_vs;
};

// IUnknown::QueryInterface for an object with one interface `iid` (and
// IUnknown): (this, riid, ppvObject).
uint32_t QueryInterfaceFor(Ctx& c, const guest::GUID& iid);

// The device a method's object belongs to (fatal if `self` is not one).
Device* DeviceOf(uint32_t self);

// SetTransform's state change: transform `type` (D3D9's numbers) is `m`,
// for GetTransform and for the fixed-function draws (Device::ff_base).
void SetTransform(Device& d, uint32_t type, const float* m);
// User clip plane i (< 8, D3DCAPS9::MaxUserClipPlanes): kept in
// Device::ff_base, where fixed-function draws apply it in world space
// (D3DRS_CLIPPLANEENABLE).
void SetClipPlane(Device& d, uint32_t i, const float* p);

// Sampler index: 0-15, or D3DVERTEXTEXTURESAMPLER0..3 (256..259) -> 16..19;
// -1 if neither.
int SamplerIndex(uint32_t s);

// The object holding `obj`'s reference count: a texture level's is its
// texture's, anything else's its own.
uint32_t RefTarget(uint32_t obj);

// Replaces a bound object: references the new one, releases the old.
void Bind(uint32_t* slot, uint32_t obj);

// A standalone surface (back buffer, depth stencil, render target,
// offscreen plain): published with refcount 1, kept in *keep if given.
uint32_t NewSurface(Device& d, uint32_t device, Surface::Kind kind, uint32_t w,
                    uint32_t h, uint32_t fmt, uint32_t usage, uint32_t pool,
                    std::shared_ptr<Surface>* keep);

// Video memory: a card has GameConfig::video_memory_mb of it (what
// GetAvailableTextureMem reports), and D3DPOOL_DEFAULT resources that do
// not fit fail with D3DERR_OUTOFVIDEOMEMORY. Games size themselves by
// allocating until that happens; a host GPU with unified memory would
// otherwise let them run until the host itself gives out. Charges
// `bytes` and returns true, or false (nothing charged) if they do not fit.
bool ChargeVram(Device& d, uint64_t bytes);
void RefundVram(uint32_t device, uint64_t bytes);

// The gfx render targets and depth buffer for the current bindings.
void BindTargets(Device& d);


// CreateTexture / CreateCubeTexture (cube: w = h = edge length): the
// HRESULT; the new texture's guest pointer in *out (0 on failure).
uint32_t NewTexture(Device& d, uint32_t device, bool cube, uint32_t w, uint32_t h,
                    uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool,
                    uint32_t* out);

// D3DXCompileShader's output: a shader function holding the HLSL
// translation instead of shader model bytecode. Version token, one comment
// with kTranslatedTag and the serialised hlsl::Program, END. Create*Shader
// recognise it.
constexpr uint32_t kTranslatedTag = 0x504D4352;  // 'RCMP'
std::vector<uint32_t> TranslatedFunction(const hlsl::Program& p);
// The Program in such a function; null for any other function.
std::shared_ptr<const hlsl::Program> ReadTranslated(const std::vector<uint32_t>& tokens);

// Replaces a whole level's pixels (rows of `pitch` bytes, in the
// texture's format) and uploads them: D3DX loaders.
void FillLevel(Texture& t, uint32_t face, uint32_t level, const uint8_t* src,
               uint32_t pitch);

}  // namespace recomp::d3d9
