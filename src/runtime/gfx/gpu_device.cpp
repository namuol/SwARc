// gfx::Device on SDL_GPU: native Metal on macOS, Vulkan elsewhere.
//
// The D3D fixed-function pipeline is two uber-shaders (shaders/ff.vert
// and ff.frag; SPIR-V for Vulkan, MSL from SPIRV-Cross for Metal) driven
// by uniforms, so nothing is compiled per state. What a pipeline object
// must fix (blend, depth/stencil, rasterizer, topology, depth format) is
// cached by state.
//
// D3D draws arrive one at a time with their data in guest memory; SDL_GPU
// uploads only in copy passes, outside render passes. So draws are
// recorded: vertex/index data and uniforms go to CPU arenas, the draw to a
// command list for the current render target. A flush (render target
// change, texture update of a texture in use, readback, present) uploads
// everything in one copy pass and replays the list in one render pass.
// Texture uploads queue the same way and land before the next pass.
//
// Colour textures and render targets are all B8G8R8A8 (D3D's A8R8G8B8
// memory layout): other D3D formats are converted on upload and on
// readback, as the d3d9 backend does for its X8R8G8B8 back buffer. DXT
// stays compressed (BC1-3).

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "runtime/gfx/device.h"
#include "runtime/gfx/present.h"
#include "runtime/env.h"
#include "runtime/gfx/shader_compile.h"
#include "runtime/log.h"

// kFfVert and kFfFrag, built from shaders/ff.*: MSL source on macOS,
// SPIR-V words elsewhere.
#include "ff_frag.h"
#include "ff_frag_at.h"
#include "ff_frag_glsl.h"
#include "ff_vert.h"

namespace recomp::gfx {

namespace {

// ---- uniform blocks (layouts mirror the shaders')

struct GpuLight {
  float diffuse[4], specular[4], ambient[4], position[4], direction[4], atten[4], spot[4];
};

struct VsUniforms {
  float world_view[4][16];
  float normal[4][16];
  float proj[16];
  float tex[8][16];
  float viewport[4];
  float target[4];
  float ambient[4];
  float mat_diffuse[4], mat_ambient[4], mat_specular[4], mat_emissive[4];
  float mat_power[4];
  float fog[4];
  float clip[8][4];
  GpuLight lights[8];
  int32_t mode[4], mode2[4], mode3[4], mode4[4], mode5[4];
  int32_t tci[8];
  int32_t ttf[8];
  float point[4];
};

// Where the vertex shader reads a draw's vertices (uniform slot 1 beside
// VsUniforms, which then stays the same from draw to draw).
struct VsLayout {
  int32_t layout[4];
  int32_t layout2[4];
  int32_t strides[4];
  int32_t tex_offset[8];
  int32_t tex_stride[8];
  int32_t tex_size[8];
};

struct FsUniforms {
  int32_t color[8][4];
  int32_t alpha[8][4];
  int32_t stage[8][4];
  float tfactor[4];
  float fog_color[4];
  float fog[4];
  int32_t mode[4];
  int32_t mode2[4];
};

// The integer state of FsUniforms that a specialised ff.frag compiles in
// (k_color, k_alpha, k_stage, k_mode, k_mode2), plus whether it has the
// alpha test: the stages up to the one that ends the cascade, the alpha
// function (when tested), fog mode and table mode, flat shading and
// specular. Stages after the end and unused fields stay zero, so states
// that draw alike share a variant.
struct FsVariantKey {
  int8_t color[8][4], alpha[8][4], stage[8][4];
  int8_t func, fog, table, flat, specular, discard;

  bool operator==(const FsVariantKey& o) const { return std::memcmp(this, &o, sizeof *this) == 0; }
};
static_assert(sizeof(FsVariantKey) == 102, "no padding: compared and hashed as bytes");

// ---- small maths on D3D row-major matrices (row vectors: v * M)

using Mat = std::array<float, 16>;

Mat Identity() {
  Mat m{};
  m[0] = m[5] = m[10] = m[15] = 1;
  return m;
}

Mat Mul(const Mat& a, const Mat& b) {
  Mat r{};
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++) {
      float s = 0;
      for (int k = 0; k < 4; k++) s += a[i * 4 + k] * b[k * 4 + j];
      r[i * 4 + j] = s;
    }
  return r;
}

// General 4x4 inverse (cofactors); identity if singular.
Mat Inverse(const Mat& m) {
  Mat inv;
  inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
           m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
  inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
           m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
  inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
           m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
  inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
            m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
  inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
           m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
  inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
           m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
  inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
           m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
  inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
            m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
  inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] +
           m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
  inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] -
           m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
  inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] +
            m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
  inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
            m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
  inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
           m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
  inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] +
           m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
  inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] -
            m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
  inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] +
            m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
  float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
  if (det == 0 || !std::isfinite(det)) return Identity();
  for (float& v : inv) v /= det;
  return inv;
}

// The normal matrix of `m` (row vectors): inverse transpose of its 3x3.
Mat NormalMatrix(const Mat& m) {
  Mat a = Identity();
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 3; c++) a[r * 4 + c] = m[r * 4 + c];
  Mat inv = Inverse(a);
  Mat t = Identity();
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 3; c++) t[r * 4 + c] = inv[c * 4 + r];
  return t;
}

void Vec4(float* out, const Color& c) {
  out[0] = c.r;
  out[1] = c.g;
  out[2] = c.b;
  out[3] = c.a;
}

void Argb(float* out, uint32_t c) {
  out[0] = float((c >> 16) & 0xFF) / 255.0f;
  out[1] = float((c >> 8) & 0xFF) / 255.0f;
  out[2] = float(c & 0xFF) / 255.0f;
  out[3] = float(c >> 24) / 255.0f;
}

float AsFloat(uint32_t v) {
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

uint32_t FloatBits(float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  return v;
}

// ---- D3D enums -> SDL_GPU

SDL_GPUBlendFactor D3DBlendFactor(uint32_t d3d, bool target_alpha) {
  switch (d3d) {
    case 1: return SDL_GPU_BLENDFACTOR_ZERO;
    case 2: return SDL_GPU_BLENDFACTOR_ONE;
    case 3: return SDL_GPU_BLENDFACTOR_SRC_COLOR;
    case 4: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    case 6: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    // A target without alpha (X8R8G8B8, R5G6B5) reads alpha as 1.
    case 7: return target_alpha ? SDL_GPU_BLENDFACTOR_DST_ALPHA : SDL_GPU_BLENDFACTOR_ONE;
    case 8: return target_alpha ? SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA : SDL_GPU_BLENDFACTOR_ZERO;
    case 9: return SDL_GPU_BLENDFACTOR_DST_COLOR;
    case 10: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
    case 11: return SDL_GPU_BLENDFACTOR_SRC_ALPHA_SATURATE;
    case 14: return SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
    case 15: return SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR;
    default: return SDL_GPU_BLENDFACTOR_ONE;
  }
}

SDL_GPUBlendOp D3DBlendOp(uint32_t d3d) {
  switch (d3d) {
    case 2: return SDL_GPU_BLENDOP_SUBTRACT;
    case 3: return SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
    case 4: return SDL_GPU_BLENDOP_MIN;
    case 5: return SDL_GPU_BLENDOP_MAX;
    default: return SDL_GPU_BLENDOP_ADD;
  }
}

// D3DCMPFUNC and D3DSTENCILOP use SDL_GPU's numbering (1..8).
SDL_GPUCompareOp D3DCompareOp(uint32_t d3d) {
  return d3d >= 1 && d3d <= 8 ? SDL_GPUCompareOp(d3d) : SDL_GPU_COMPAREOP_ALWAYS;
}
SDL_GPUStencilOp D3DStencilOp(uint32_t d3d) {
  return d3d >= 1 && d3d <= 8 ? SDL_GPUStencilOp(d3d) : SDL_GPU_STENCILOP_KEEP;
}

SDL_GPUSamplerAddressMode D3DAddressMode(uint32_t d3d) {
  switch (d3d) {
    case 2: case 5: return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
    case 3: case 4: return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;  // no border
    default: return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
  }
}

// IEEE half from a float in [0, 1] (fills): round to nearest, no NaN/inf input.
uint16_t HalfFromFloat(float v) {
  uint32_t b;
  std::memcpy(&b, &v, 4);
  const uint32_t sign = (b >> 16) & 0x8000;
  int e = int((b >> 23) & 0xFF) - 127 + 15;
  uint32_t m = b & 0x7FFFFF;
  if (e <= 0) return uint16_t(sign);  // tiny: zero
  if (e >= 31) return uint16_t(sign | 0x7C00);
  uint32_t h = sign | uint32_t(e) << 10 | (m >> 13);
  if (m & 0x1000) h++;  // round half up
  return uint16_t(h);
}

bool HasAlpha(Format f) {
  switch (f) {
    case Format::A8R8G8B8: case Format::A1R5G5B5: case Format::A4R4G4B4:
    case Format::A8: case Format::A8L8: case Format::DXT1: case Format::DXT2:
    case Format::DXT3: case Format::DXT4: case Format::DXT5:
    case Format::A16B16G16R16F: case Format::A32B32G32R32F:
      return true;
    default:
      return false;
  }
}

// D3D's FVF code, decoded to byte offsets for the vertex shader.
struct FvfLayout {
  int stride = 0;
  int pos_kind = 0;  // 0 xyz, 1 xyzrhw, 2 xyzw
  int weights = 0;
  int normal = -1, psize = -1, diffuse = -1, specular = -1;
  int tex_offset[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
  int tex_size[8] = {};
};

FvfLayout DecodeFvf(uint32_t fvf, uint32_t stride) {
  FvfLayout l;
  int off = 0;
  switch (fvf & 0x400E) {
    case 0x002: off = 12; break;
    case 0x004: l.pos_kind = 1; off = 16; break;
    case 0x006: case 0x008: case 0x00A: case 0x00C: case 0x00E:
      l.weights = int(((fvf & 0xE) - 4) / 2);
      off = 12 + 4 * l.weights;
      break;
    case 0x4002: l.pos_kind = 2; off = 16; break;
    default: off = 12; break;
  }
  if (fvf & 0x010) { l.normal = off; off += 12; }
  if (fvf & 0x020) { l.psize = off; off += 4; }
  if (fvf & 0x040) { l.diffuse = off; off += 4; }
  if (fvf & 0x080) { l.specular = off; off += 4; }
  int ntex = int((fvf >> 8) & 0xF);
  for (int i = 0; i < ntex && i < 8; i++) {
    static constexpr int kSizes[4] = {2, 3, 4, 1};
    int n = kSizes[(fvf >> (16 + 2 * i)) & 3];
    l.tex_offset[i] = off;
    l.tex_size[i] = n;
    off += 4 * n;
  }
  l.stride = int(stride);
  return l;
}

// ---- pipelines

struct PipelineKey {
  uint8_t prim, fill, cull, front;
  uint8_t blend, src, dst, op;
  uint8_t write_mask, depth_test, depth_write, depth_func;
  uint8_t stencil, s_fail, s_zfail, s_pass;
  uint8_t s_func, s_read, s_write, depth_format;
  float bias_const, bias_slope;
  // The pass's colour targets: count, SDL formats, write masks of 1-3
  // (target 0's is write_mask).
  uint8_t targets, fmt[4], mask[3];

  bool operator==(const PipelineKey& o) const { return std::memcmp(this, &o, sizeof *this) == 0; }
};
static_assert(sizeof(PipelineKey) == 36, "no padding: compared and hashed as bytes");

// A pipeline for translated shaders: the fixed state, the shaders and
// D3D9's separate alpha blend.
struct ShaderKey {
  PipelineKey k;
  uint32_t vs, ps;
  uint8_t src_a, dst_a, op_a, pad;

  bool operator==(const ShaderKey& o) const { return std::memcmp(this, &o, sizeof *this) == 0; }
};
static_assert(sizeof(ShaderKey) == 48, "no padding: compared and hashed as bytes");

template <typename K>
struct BytesHash {
  size_t operator()(const K& k) const {
    uint64_t h = 0xCBF29CE484222325ull;
    const auto* b = reinterpret_cast<const uint8_t*>(&k);
    for (size_t i = 0; i < sizeof k; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return size_t(h);
  }
};

struct SamplerKey {
  uint8_t min, mag, mip, u, v, aniso;
  uint8_t w, pad;  // pad: 1 for a shadow map's comparison sampler
  float lod_bias, min_lod;
  bool operator<(const SamplerKey& o) const { return std::memcmp(this, &o, sizeof *this) < 0; }
};

// A pipeline for a program: the program, its fixed state and the pass's
// target formats.
struct ProgramKey {
  uint32_t program;
  uint8_t topology, cull, front_ccw, depth_test, depth_write, depth_clip, depth_compare, stencil;
  uint8_t s_ops[2][4];  // fail, depth fail, pass, compare: front, back
  uint8_t s_read, s_write, targets, depth_format;
  uint8_t fmt[4];
  uint8_t blend[4][8];  // enable, src, dst, op, src alpha, dst alpha, alpha op, mask
  float bias, bias_slope, bias_clamp;

  bool operator==(const ProgramKey& o) const { return std::memcmp(this, &o, sizeof *this) == 0; }
};
static_assert(sizeof(ProgramKey) == 72, "no padding: compared and hashed as bytes");

struct ProgramSamplerKey {
  uint8_t min, mag, mip, u, v, w, compare, compare_op;
  float lod_bias, min_lod, max_lod, aniso;
  bool operator<(const ProgramSamplerKey& o) const {
    return std::memcmp(this, &o, sizeof *this) < 0;
  }
};

// A byte vector that grows without zeroing (AppendWords).
template <typename T>
struct NoZero : std::allocator<T> {
  template <typename U>
  struct rebind {
    using other = NoZero<U>;
  };
  using std::allocator<T>::allocator;
  template <typename U>
  void construct(U* p) noexcept {
    ::new (static_cast<void*>(p)) U;
  }
  template <typename U, typename... A>
  void construct(U* p, A&&... a) {
    ::new (static_cast<void*>(p)) U(std::forward<A>(a)...);
  }
};
using Arena = std::vector<uint8_t, NoZero<uint8_t>>;

class GpuDevice final : public Device {
 public:
  explicit GpuDevice(SDL_Window* w) : window_(w) {}

  bool Init() {
#if defined(__APPLE__)
    const SDL_GPUShaderFormat format = SDL_GPU_SHADERFORMAT_MSL;
    const char* entry = "main0";  // SPIRV-Cross's name for main
    size_t vs_size = sizeof kFfVert - 1, fs_size = sizeof kFfFrag - 1;
#else
    const SDL_GPUShaderFormat format = SDL_GPU_SHADERFORMAT_SPIRV;
    const char* entry = "main";
    size_t vs_size = sizeof kFfVert, fs_size = sizeof kFfFrag;
#endif
    // Vulkan 1.1 rather than SDL's 1.0: shaders use subgroup operations.
    SDL_GPUVulkanOptions vk{};
    vk.vulkan_api_version = (1u << 22) | (1u << 12);  // VK_API_VERSION_1_1
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetBooleanProperty(props, format == SDL_GPU_SHADERFORMAT_MSL ? SDL_PROP_GPU_DEVICE_CREATE_SHADERS_MSL_BOOLEAN
                                                                     : SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN,
                           true);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, false);
    SDL_SetPointerProperty(props, SDL_PROP_GPU_DEVICE_CREATE_VULKAN_OPTIONS_POINTER, &vk);
    dev_ = SDL_CreateGPUDeviceWithProperties(props);
    SDL_DestroyProperties(props);
    if (!dev_) {
      WarnF("gfx: SDL_CreateGPUDeviceWithProperties: %s\n", SDL_GetError());
      return false;
    }
    if (window_ && !SDL_ClaimWindowForGPUDevice(dev_, window_)) {
      WarnF("gfx: SDL_ClaimWindowForGPUDevice: %s\n", SDL_GetError());
      return false;
    }
    SDL_GPUShaderCreateInfo vs{};
    vs.code = reinterpret_cast<const Uint8*>(kFfVert);
    vs.code_size = vs_size;
    vs.entrypoint = entry;
    vs.format = format;
    vs.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    vs.num_uniform_buffers = 2;
    vs.num_storage_buffers = 1;
    vs_ = SDL_CreateGPUShader(dev_, &vs);
    SDL_GPUShaderCreateInfo fs = vs;
    fs.num_uniform_buffers = 1;
    fs.code = reinterpret_cast<const Uint8*>(kFfFrag);
    fs.code_size = fs_size;
    fs.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fs.num_storage_buffers = 0;
    fs.num_samplers = 16;  // ff.frag: eight 2D, then eight cube
    fs_ = SDL_CreateGPUShader(dev_, &fs);
    fs.code = reinterpret_cast<const Uint8*>(kFfFragAlphaTest);
#if defined(__APPLE__)
    fs.code_size = sizeof kFfFragAlphaTest - 1;
#else
    fs.code_size = sizeof kFfFragAlphaTest;
#endif
    fs_alpha_test_ = SDL_CreateGPUShader(dev_, &fs);
    if (const char* e = Env("FF_VCACHE")) {
      if (!std::strcmp(e, "off")) pv_mode_ = PvMode::Off;
      if (!std::strcmp(e, "check")) pv_mode_ = PvMode::Check;
    }
    if (const char* e = Env("FF_VARIANTS")) {
      if (!std::strcmp(e, "off")) variant_mode_ = VariantMode::Off;
      if (!std::strcmp(e, "sync")) variant_mode_ = VariantMode::Sync;
    }
    if (!vs_ || !fs_ || !fs_alpha_test_) FatalF("gfx: compiling the fixed-function shaders: %s", SDL_GetError());

    for (auto [d3d, sdl] : {std::pair{Format::DXT1, SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM},
                            {Format::DXT3, SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM},
                            {Format::DXT5, SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM}})
      if (!SDL_GPUTextureSupportsFormat(dev_, sdl, SDL_GPU_TEXTURETYPE_2D,
                                        SDL_GPU_TEXTUREUSAGE_SAMPLER))
        WarnF("gfx: this GPU cannot sample format %u (DXT)\n", unsigned(d3d));
    depth24_ = SupportsDepth(SDL_GPU_TEXTUREFORMAT_D24_UNORM)
                   ? SDL_GPU_TEXTUREFORMAT_D24_UNORM : SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    depth24s8_ = SupportsDepth(SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT)
                     ? SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT
                     : SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;

    // Stages without a texture still need something bound.
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = ti.height = 1;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    dummy_ = SDL_CreateGPUTexture(dev_, &ti);
    uint32_t white = 0xFFFFFFFFu;
    QueueUpload(dummy_, 0, 0, 0, 1, 1, &white, 4, 1, 4);
    // Shaders' samplers without a texture read (0, 0, 0, 1), as in D3D9.
    uint32_t black = 0xFF000000u;
    for (auto [t, type, layers] : {std::tuple{&dummy_2d_, SDL_GPU_TEXTURETYPE_2D, 1},
                                   {&dummy_cube_, SDL_GPU_TEXTURETYPE_CUBE, 6},
                                   {&dummy_3d_, SDL_GPU_TEXTURETYPE_3D, 1},
                                   {&dummy_array_, SDL_GPU_TEXTURETYPE_2D_ARRAY, 1}}) {
      ti.type = type;
      ti.layer_count_or_depth = Uint32(layers);
      *t = SDL_CreateGPUTexture(dev_, &ti);
      if (!*t) FatalF("gfx: dummy texture: %s", SDL_GetError());
      for (int l = 0; l < layers; l++) QueueUpload(*t, 0, 0, 0, 1, 1, &black, 4, 1, 4, false, false, l);
    }

    std::fprintf(stderr, "gfx: SDL_GPU (%s)\n", SDL_GetGPUDeviceDriver(dev_));
    return true;
  }

  ~GpuDevice() override {
    {
      std::lock_guard<std::mutex> l(variant_mu_);
      variant_quit_ = true;
    }
    variant_cv_.notify_all();
    if (variant_worker_.joinable()) variant_worker_.join();
    if (!dev_) return;
    if (open_) SDL_SubmitGPUCommandBuffer(open_);
    SDL_WaitForGPUIdle(dev_);
    for (const InFlight& f : in_flight_) SDL_ReleaseGPUFence(dev_, f.fence);
    for (const Landing& w : landings_) SDL_ReleaseGPUTransferBuffer(dev_, w.tb);
    for (const Landing& w : download_pool_) SDL_ReleaseGPUTransferBuffer(dev_, w.tb);
    for (auto& [t, rd] : reads_) FinishRead(rd, nullptr, 0, Format::Unknown);
    for (auto& [k, p] : pipelines_) SDL_ReleaseGPUGraphicsPipeline(dev_, p);
    for (auto& [k, p] : pipelines_alpha_test_) SDL_ReleaseGPUGraphicsPipeline(dev_, p);
    for (auto& [k, v] : variants_) {
      for (auto& [pk, p] : v.pipelines) SDL_ReleaseGPUGraphicsPipeline(dev_, p);
      if (SDL_GPUShader* sh = v.shader.load()) SDL_ReleaseGPUShader(dev_, sh);
    }
    for (auto& [k, p] : shader_pipelines_) SDL_ReleaseGPUGraphicsPipeline(dev_, p);
    for (auto& [k, p] : program_pipelines_) SDL_ReleaseGPUGraphicsPipeline(dev_, p);
    for (auto& [id, pr] : programs_) ReleaseProgram(pr);
    if (present_pipeline_) SDL_ReleaseGPUGraphicsPipeline(dev_, present_pipeline_);
    if (gamma_pipeline_) SDL_ReleaseGPUGraphicsPipeline(dev_, gamma_pipeline_);
    if (gamma_tex_) SDL_ReleaseGPUTexture(dev_, gamma_tex_);
    if (gamma_tb_) SDL_ReleaseGPUTransferBuffer(dev_, gamma_tb_);
    for (auto& [k, s] : program_samplers_) SDL_ReleaseGPUSampler(dev_, s);
    {
      std::lock_guard<std::mutex> l(compile_mu_);
      compile_quit_ = true;
    }
    compile_cv_.notify_all();
    for (std::thread& t : compile_threads_) t.join();
    for (auto& [id, sh] : shaders_) SDL_ReleaseGPUShader(dev_, Gpu(sh));
    for (auto& [k, s] : samplers_) SDL_ReleaseGPUSampler(dev_, s);
    for (auto& [id, t] : textures_) SDL_ReleaseGPUTexture(dev_, t.tex);
    for (auto& [id, d] : depths_) SDL_ReleaseGPUTexture(dev_, d.tex);
    for (SDL_GPUTexture* t : {backbuffer_, front_, dummy_, dummy_2d_, dummy_cube_, dummy_3d_,
                              dummy_array_})
      if (t) SDL_ReleaseGPUTexture(dev_, t);
    for (auto& b : blit_pool_) SDL_ReleaseGPUTexture(dev_, b.tex);
    for (SDL_GPUBuffer* b : {vbuf_, ibuf_, pbuf_, cro_, crw_, heap_, pvbuf_})
      if (b) SDL_ReleaseGPUBuffer(dev_, b);
    for (SDL_GPUTransferBuffer* b : {upload_tb_, download_tb_, occl_zeros_})
      if (b) SDL_ReleaseGPUTransferBuffer(dev_, b);
    if (occl_buf_) SDL_ReleaseGPUBuffer(dev_, occl_buf_);
    for (const OcclRead& r : occl_reads_) SDL_ReleaseGPUTransferBuffer(dev_, r.tb);
    for (SDL_GPUTransferBuffer* b : occl_free_) SDL_ReleaseGPUTransferBuffer(dev_, b);
    if (vs_) SDL_ReleaseGPUShader(dev_, vs_);
    if (fs_) SDL_ReleaseGPUShader(dev_, fs_);
    if (fs_alpha_test_) SDL_ReleaseGPUShader(dev_, fs_alpha_test_);
    if (window_) SDL_ReleaseWindowFromGPUDevice(dev_, window_);
    SDL_DestroyGPUDevice(dev_);
  }

  // ---- swap chain

  bool SetWindow(SDL_Window* w) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    if (w == window_) return true;
    Flush();
    if (window_) SDL_ReleaseWindowFromGPUDevice(dev_, window_);
    window_ = w;
    if (window_ && !SDL_ClaimWindowForGPUDevice(dev_, window_)) {
      WarnF("gfx: SDL_ClaimWindowForGPUDevice: %s\n", SDL_GetError());
      window_ = nullptr;
      return false;
    }
    return true;
  }

  void ResetSwapChain(int w, int h, bool vsync) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    Flush();
    pass_color_ = pass_depth_ = nullptr;
    pass_colors_.fill(nullptr);
    for (SDL_GPUTexture** t : {&backbuffer_, &front_}) {
      if (*t) SDL_ReleaseGPUTexture(dev_, *t);
      *t = CreateColor(w, h, 1);
    }
    bb_w_ = w;
    bb_h_ = h;
    // Without vsync, mailbox (no tearing, never blocks), as the d3d9
    // backend has DXVK do. Where there is none (Metal), it is emulated on
    // vsync: Show() presents at most one frame per refresh (immediate
    // presentation tears in fullscreen).
    SDL_GPUPresentMode mode = SDL_GPU_PRESENTMODE_VSYNC;
    mailbox_emulated_ = false;
    if (window_ && !vsync) {
      if (SDL_WindowSupportsGPUPresentMode(dev_, window_, SDL_GPU_PRESENTMODE_MAILBOX)) {
        mode = SDL_GPU_PRESENTMODE_MAILBOX;
      } else {
        mailbox_emulated_ = true;
        const SDL_DisplayMode* dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window_));
        const double hz = dm && dm->refresh_rate > 0 ? double(dm->refresh_rate) : 60.0;
        refresh_ns_ = uint64_t(1e9 / hz + 0.5);
        next_show_ns_ = 0;
      }
    }
    if (window_) SDL_SetGPUSwapchainParameters(dev_, window_, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode);
    // Both start black, as a fresh D3D swap chain does.
    for (SDL_GPUTexture* t : {backbuffer_, front_}) {
      SetPass(t, nullptr);
      pass_clear_color_ = true;
      clear_color_ = {0, 0, 0, 1};
      Flush();
    }
    BindTarget();
    std::fprintf(stderr, "gfx: back buffer %dx%d%s\n", w, h,
                 vsync ? " (vsync)" : mailbox_emulated_ ? " (mailbox on vsync)" : "");
  }

  // ---- resources

  TextureId CreateTexture(int w, int h, int levels, Format fmt, bool rt) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    if (w <= 0 || h <= 0) return 0;
    if (levels <= 0) {
      levels = 1;
      for (int s = std::max(w, h); s > 1; s >>= 1) levels++;
    }
    Tex t;
    t.w = w;
    t.h = h;
    t.levels = levels;
    t.fmt = fmt;
    t.rt = rt;
    t.alpha_tracked = !rt && !IsFloat(fmt) && levels <= 32;
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = GpuFormat(fmt);
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | (rt ? SDL_GPU_TEXTUREUSAGE_COLOR_TARGET : 0);
    ti.width = Uint32(w);
    ti.height = Uint32(h);
    ti.layer_count_or_depth = 1;
    ti.num_levels = Uint32(levels);
    t.tex = SDL_CreateGPUTexture(dev_, &ti);
    if (!t.tex) {
      WarnF("gfx: CreateTexture %dx%d x%d fmt %u failed: %s\n", w, h, levels,
            unsigned(fmt), SDL_GetError());
      return 0;
    }
    if (fmt == Format::P8) WarnF("gfx: P8 texture (no palette support): drawn as magenta\n");
    TextureId id = next_id_++;
    textures_[id] = t;
    tex_gen_++;
    return id;
  }

  void DestroyTexture(TextureId id) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto it = textures_.find(id);
    if (it == textures_.end()) return;
    if (Pending(it->second.tex)) Flush();
    // The pass recorded last may name it without it being bound (a
    // program's targets): forget the pass, its work is flushed.
    if (std::find(pass_colors_.begin(), pass_colors_.end(), it->second.tex) != pass_colors_.end()) {
      pass_color_ = pass_depth_ = nullptr;
      pass_colors_.fill(nullptr);
    }
    bool bound = current_color_ == id;
    if (current_color_ == id) current_color_ = 0, current_level_ = 0;
    for (ColorTarget& e : current_extra_)
      if (e.tex == id) e = {}, bound = true;
    if (bound) BindTarget();
    SDL_ReleaseGPUTexture(dev_, it->second.tex);
    textures_.erase(it);
    tex_gen_++;
    fs_dirty_ = true;
  }

  void FillTexture(TextureId id, const Rect& r, uint32_t argb) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto it = textures_.find(id);
    if (it == textures_.end()) return;
    Rect c = Clip(r, it->second.w, it->second.h);
    int w = c.right - c.left, h = c.bottom - c.top;
    if (w <= 0 || h <= 0) return;
    if (Pending(it->second.tex)) Flush();
    NoteAlpha(it->second, 0, c, uint8_t(argb >> 24));
    const Format f = it->second.fmt;
    if (IsFloat(f)) {
      // The colour converted to the target's own format (channels in the
      // order R, G, B, A of the format's GPU layout), as D3D9 does.
      const float ch[4] = {float((argb >> 16) & 0xFF) / 255.0f, float((argb >> 8) & 0xFF) / 255.0f,
                           float(argb & 0xFF) / 255.0f, float(argb >> 24) / 255.0f};
      const int n = f == Format::R16F || f == Format::R32F ? 1
                    : f == Format::G16R16F || f == Format::G32R32F ? 2 : 4;
      const bool half = f == Format::R16F || f == Format::G16R16F || f == Format::A16B16G16R16F;
      const uint32_t bpp = uint32_t(n) * (half ? 2u : 4u);
      std::vector<uint8_t> px(size_t(w) * h * bpp);
      uint8_t one[16];
      for (int i = 0; i < n; i++) {
        if (half) {
          const uint16_t hv = HalfFromFloat(ch[i]);
          std::memcpy(one + i * 2, &hv, 2);
        } else {
          std::memcpy(one + i * 4, &ch[i], 4);
        }
      }
      for (size_t i = 0; i < size_t(w) * h; i++) std::memcpy(px.data() + i * bpp, one, bpp);
      QueueUpload(it->second.tex, 0, c.left, c.top, w, h, px.data(), uint32_t(w) * bpp,
                  uint32_t(h), uint32_t(w) * bpp, false, false);
      return;
    }
    std::vector<uint32_t> px(size_t(w) * h, argb);
    QueueUpload(it->second.tex, 0, c.left, c.top, w, h, px.data(), uint32_t(w) * 4,
                uint32_t(h), uint32_t(w) * 4);
  }

  // UpdateTexture / UpdateCubeTexture: `layer` is the cube face.
  void Update(TextureId id, int layer, int level, const Rect& r, const void* src,
              uint32_t pitch) {
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto it = textures_.find(id);
    if (it == textures_.end() || level < 0 || level >= it->second.levels) return;
    const Tex& t = it->second;
    int lw = std::max(1, t.w >> level), lh = std::max(1, t.h >> level);
    Rect c = Clip(r, lw, lh);
    int w = c.right - c.left, h = c.bottom - c.top;
    if (w <= 0 || h <= 0) return;
    // Draws recorded before the update must see the old contents.
    if (Pending(t.tex)) Flush();
    const uint8_t* s = static_cast<const uint8_t*>(src);
    if (IsCompressed(t.fmt)) {
      uint32_t row = RowPitch(t.fmt, uint32_t(w)), rows = RowCount(t.fmt, uint32_t(h));
      if (layer == 0 && t.alpha_tracked)
        NoteAlpha(it->second, level, c, BlockMinAlpha(t.fmt, s, pitch, row, rows));
      QueueUpload(t.tex, level, c.left, c.top, w, h, s, pitch, rows, row, false, true, layer);
      return;
    }
    if (IsFloat(t.fmt) || t.fmt == Format::R16G16B16A16) {  // same layout as the GPU format
      QueueUpload(t.tex, level, c.left, c.top, w, h, s, pitch, uint32_t(h),
                  RowPitch(t.fmt, uint32_t(w)), false, false, layer);
      return;
    }
    std::vector<uint32_t> px(size_t(w) * h);
    ConvertToArgb(s, pitch, t.fmt, w, h, px.data());
    if (layer == 0 && t.alpha_tracked) {
      uint32_t lo = 0xFF000000u;
      for (uint32_t v : px) lo = std::min(lo, v & 0xFF000000u);
      NoteAlpha(it->second, level, c, uint8_t(lo >> 24));
    }
    QueueUpload(t.tex, level, c.left, c.top, w, h, px.data(), uint32_t(w) * 4, uint32_t(h),
                uint32_t(w) * 4, false, false, layer);
  }

  // Texel alpha `lo` written to `r` of `level` (Tex::alpha_min).
  void NoteAlpha(auto& t, int level, const Rect& r, uint8_t lo) {  // t: a Tex
    if (!t.alpha_tracked) return;
    if (lo < t.alpha_min) {
      t.alpha_min = lo;
      fs_dirty_ = true;
    }
    const uint32_t bit = 1u << level;
    if (!(t.alpha_levels & bit) && r.left == 0 && r.top == 0 &&
        r.right == LevelSize(t.w, level) && r.bottom == LevelSize(t.h, level)) {
      t.alpha_levels |= bit;
      fs_dirty_ = true;
    }
  }

  // The lowest alpha a block-compressed rectangle can decode to: `rows`
  // rows of `row` bytes of blocks. DXT1: 0 if a block in three-colour
  // mode uses index 3 (transparent). DXT2/3: the lowest 4-bit alpha.
  // DXT4/5: the lower endpoint, or 0 if a block in six-alpha mode uses
  // index 6 (alpha 0); interpolated values lie between the endpoints.
  static uint8_t BlockMinAlpha(Format f, const uint8_t* src, uint32_t pitch, uint32_t row,
                               uint32_t rows) {
    uint8_t lo = 255;
    const bool bc1 = f == Format::DXT1;
    const bool bc2 = f == Format::DXT2 || f == Format::DXT3;
    const bool bc3 = f == Format::DXT4 || f == Format::DXT5;
    if (!bc1 && !bc2 && !bc3) return 0;
    for (uint32_t y = 0; y < rows && lo; y++) {
      const uint8_t* b = src + size_t(y) * pitch;
      for (uint32_t x = 0; x + (bc1 ? 8 : 16) <= row && lo; x += bc1 ? 8 : 16, b += bc1 ? 8 : 16) {
        if (bc1) {
          const uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
          if (c0 > c1) continue;
          const uint32_t idx = uint32_t(b[4] | b[5] << 8 | b[6] << 16 | uint32_t(b[7]) << 24);
          for (int i = 0; i < 16; i++)
            if (((idx >> (2 * i)) & 3) == 3) lo = 0;
        } else if (bc2) {
          for (int i = 0; i < 8; i++)
            lo = std::min<uint8_t>(lo, uint8_t(std::min(b[i] & 0xF, b[i] >> 4) * 17));
        } else {
          const uint8_t a0 = b[0], a1 = b[1];
          lo = std::min(lo, std::min(a0, a1));
          if (a0 > a1) continue;
          uint64_t idx = 0;
          for (int i = 0; i < 6; i++) idx |= uint64_t(b[2 + i]) << (8 * i);
          for (int i = 0; i < 16; i++)
            if (((idx >> (3 * i)) & 7) == 6) lo = 0;
        }
      }
    }
    return lo;
  }
  void UpdateTexture(TextureId id, int level, const Rect& r, const void* src,
                     uint32_t pitch) override {
    Update(id, 0, level, r, src, pitch);
  }

  TextureId CreateCubeTexture(int size, int levels, Format fmt) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    if (size <= 0) return 0;
    if (levels <= 0) {
      levels = 1;
      for (int s = size; s > 1; s >>= 1) levels++;
    }
    Tex t;
    t.w = t.h = size;
    t.levels = levels;
    t.fmt = fmt;
    t.cube = true;
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_CUBE;
    ti.format = GpuFormat(fmt);
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = ti.height = Uint32(size);
    ti.layer_count_or_depth = 6;
    ti.num_levels = Uint32(levels);
    t.tex = SDL_CreateGPUTexture(dev_, &ti);
    if (!t.tex) {
      WarnF("gfx: CreateCubeTexture %d x%d fmt %u failed: %s\n", size, levels,
            unsigned(fmt), SDL_GetError());
      return 0;
    }
    TextureId id = next_id_++;
    textures_[id] = t;
    tex_gen_++;
    return id;
  }

  void UpdateCubeTexture(TextureId id, int face, int level, const Rect& r, const void* src,
                         uint32_t pitch) override {
    if (face < 0 || face > 5) return;
    Update(id, face, level, r, src, pitch);
  }


  DepthId CreateDepth(int w, int h, Format fmt) override { return NewDepth(w, h, 0, fmt); }
  DepthId CreateDepthArray(int w, int h, int layers, Format fmt) override {
    return layers > 0 ? NewDepth(w, h, layers, fmt) : 0;
  }
  BufferId CreateBuffer(uint32_t size) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    if (!size) return 0;
    const uint32_t n = (size + 3) & ~3u;
    const BufferId id = next_id_++;
    gpu_buffers_[id] = {HeapAlloc(n), n};
    return id;
  }

  void DestroyBuffer(BufferId id) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto it = gpu_buffers_.find(id);
    if (it == gpu_buffers_.end()) return;
    Flush();  // draws recorded with it
    HeapFree(it->second.at, it->second.size);
    gpu_buffers_.erase(it);
  }

  void UpdateBuffer(BufferId id, uint32_t at, const void* src, uint32_t size) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto it = gpu_buffers_.find(id);
    if (it == gpu_buffers_.end() || !size) return;
    if (at > it->second.size || size > it->second.size - at)
      FatalF("gfx: an upload past buffer %u's end", id);
    if (std::find(draw_read_.begin(), draw_read_.end(), id) != draw_read_.end())
      Flush();  // draws recorded with it
    std::memcpy(MapUpload(size), src, size);
    SDL_UnmapGPUTransferBuffer(dev_, upload_tb_);
    SDL_GPUCommandBuffer* cmd = Begin();  // after MapUpload, which may submit it
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation from{upload_tb_, upload_at_};
    SDL_GPUBufferRegion to{heap_, it->second.at + at, size};
    SDL_UploadToGPUBuffer(cp, &from, &to, false);
    SDL_EndGPUCopyPass(cp);
    Submit(cmd);
  }

  // A 2D depth target (layers 0) or a 2D array of `layers`.
  DepthId NewDepth(int w, int h, int layers, Format fmt) {
    std::lock_guard<std::recursive_mutex> l(mu_);
    Depth d;
    d.w = w;
    d.h = h;
    d.layers = std::max(layers, 1);
    d.fmt = fmt;
    switch (fmt) {
      case Format::D16: d.gpu_format = SDL_GPU_TEXTUREFORMAT_D16_UNORM; d.bias_scale = 65535.0f; break;
      case Format::D24X8: d.gpu_format = depth24_; break;
      case Format::D32: d.gpu_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT; break;
      case Format::D32FS8: d.gpu_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT; d.stencil = true; break;
      default: d.gpu_format = depth24s8_; d.stencil = true; break;  // D24S8, D15S1
    }
    if (d.gpu_format == SDL_GPU_TEXTUREFORMAT_D24_UNORM ||
        d.gpu_format == SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT)
      d.bias_scale = 16777215.0f;
    SDL_GPUTextureCreateInfo ti{};
    ti.type = layers ? SDL_GPU_TEXTURETYPE_2D_ARRAY : SDL_GPU_TEXTURETYPE_2D;
    ti.format = d.gpu_format;
    // Sampled too where the GPU can (programs read depth targets).
    ti.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    if (SDL_GPUTextureSupportsFormat(dev_, d.gpu_format, ti.type,
                                     SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET |
                                         SDL_GPU_TEXTUREUSAGE_SAMPLER))
      ti.usage |= SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = Uint32(w);
    ti.height = Uint32(h);
    ti.layer_count_or_depth = Uint32(d.layers);
    ti.num_levels = 1;
    d.tex = SDL_CreateGPUTexture(dev_, &ti);
    if (!d.tex) {
      WarnF("gfx: CreateDepth %dx%d fmt %u failed: %s\n", w, h, unsigned(fmt), SDL_GetError());
      return 0;
    }
    DepthId id = next_id_++;
    depths_[id] = d;
    return id;
  }

  void DestroyDepth(DepthId id) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto it = depths_.find(id);
    if (it == depths_.end()) return;
    if (pass_depth_ == it->second.tex) {
      Flush();
      pass_color_ = pass_depth_ = nullptr;  // as DestroyTexture
      pass_colors_.fill(nullptr);
    }
    if (current_depth_ == id) {
      current_depth_ = 0;
      BindTarget();
    }
    SDL_ReleaseGPUTexture(dev_, it->second.tex);
    depths_.erase(it);
  }

  void SetRenderTarget(TextureId color, DepthId depth) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    current_color_ = color;
    current_level_ = 0;
    current_extra_.fill({});
    current_depth_ = depth;
    BindTarget();
  }

  void SetRenderTargets(const ColorTarget color[4], DepthId depth) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    current_color_ = color[0].tex;
    current_level_ = color[0].tex ? color[0].level : 0;
    for (int i = 0; i < 3; i++) current_extra_[i] = color[i + 1];
    current_depth_ = depth;
    BindTarget();
  }

  void StretchRect(ColorTarget src, const Rect& sr, ColorTarget dst, const Rect& dr,
                   bool linear) override {
    Stretch(src, sr, dst, dr, linear, false);
  }
  void StretchRectFlipped(ColorTarget src, const Rect& sr, ColorTarget dst, const Rect& dr,
                          bool linear) override {
    Stretch(src, sr, dst, dr, linear, true);
  }
  void Stretch(ColorTarget src, const Rect& sr, ColorTarget dst, const Rect& dr, bool linear,
               bool flip) {
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto target = [&](ColorTarget t, int* w, int* h) -> SDL_GPUTexture* {
      if (!t.tex) {
        *w = bb_w_;
        *h = bb_h_;
        return backbuffer_;
      }
      auto it = textures_.find(t.tex);
      if (it == textures_.end() || !it->second.rt || t.level < 0 || t.level >= it->second.levels)
        return nullptr;
      *w = LevelSize(it->second.w, t.level);
      *h = LevelSize(it->second.h, t.level);
      return it->second.tex;
    };
    int sw, sh, dw, dh;
    SDL_GPUTexture* s = target(src, &sw, &sh);
    SDL_GPUTexture* d = target(dst, &dw, &dh);
    if (!s || !d) return;
    Rect a = Clip(sr, sw, sh), b = Clip(dr, dw, dh);
    if (a.right <= a.left || a.bottom <= a.top || b.right <= b.left || b.bottom <= b.top) return;
    // Draws recorded so far come first (into either texture).
    Flush();
    SDL_GPUCommandBuffer* cmd = Begin();
    SDL_GPUBlitInfo bi{};
    bi.source.texture = s;
    bi.source.mip_level = Uint32(src.tex ? src.level : 0);
    bi.source.x = Uint32(a.left);
    bi.source.y = Uint32(a.top);
    bi.source.w = Uint32(a.right - a.left);
    bi.source.h = Uint32(a.bottom - a.top);
    bi.destination.texture = d;
    bi.destination.mip_level = Uint32(dst.tex ? dst.level : 0);
    bi.destination.x = Uint32(b.left);
    bi.destination.y = Uint32(b.top);
    bi.destination.w = Uint32(b.right - b.left);
    bi.destination.h = Uint32(b.bottom - b.top);
    bi.load_op = SDL_GPU_LOADOP_LOAD;
    bi.filter = linear ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    if (flip) bi.flip_mode = SDL_FLIP_VERTICAL;
    SDL_BlitGPUTexture(cmd, &bi);
    Submit(cmd);
  }

  // ---- 2D

  void Blit(const void* src, uint32_t pitch, Format fmt, const Rect& sr, const Rect& dr,
            bool colorkey, uint32_t key) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    DrawImage(src, pitch, fmt, sr, dr, colorkey, key);
  }

  void BlitBlend(const void* src, uint32_t pitch, const Rect& sr, const Rect& dr) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    DrawImage(src, pitch, Format::A8R8G8B8, sr, dr, false, 0, true);
  }

  void ReadBack(const Rect& r, void* dst, uint32_t pitch, Format fmt) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    ReadTexture(pass_color_, SDL_GPUTextureFormat(pass_fmts_[0]), pass_w_, pass_h_, r, dst, pitch,
                fmt, pass_levels_[0]);
  }

  uint32_t StartReadBack(const Rect& r) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    uint32_t t = next_read_++;
    if (!t) t = next_read_++;
    reads_[t] = StartRead(pass_color_, SDL_GPUTextureFormat(pass_fmts_[0]), pass_w_, pass_h_, r,
                          false, pass_levels_[0]);
    return t;
  }

  void FinishReadBack(uint32_t ticket, void* dst, uint32_t pitch, Format fmt) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto it = reads_.find(ticket);
    if (it == reads_.end()) return;
    FinishRead(it->second, dst, pitch, fmt);
    reads_.erase(it);
  }

  void BlitFront(const void* src, uint32_t pitch, Format fmt, const Rect& sr,
                 const Rect& dr) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    SetPass(front_, nullptr);
    DrawImage(src, pitch, fmt, sr, dr, false, 0);
    BindTarget();
  }

  void ReadBackFront(const Rect& r, void* dst, uint32_t pitch, Format fmt) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    ReadTexture(front_, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, bb_w_, bb_h_, r, dst, pitch, fmt);
  }

  void PresentFront() override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    Flush();
    SDL_GPUCommandBuffer* cmd = Begin();
    Show(cmd);
    SubmitOpen();
    PersistentFrame();
  }

  // ---- drawing

  void Clear(uint32_t count, const Rect* rects, uint32_t flags, uint32_t color, float z,
             uint32_t stencil) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    if (!pass_depth_) flags &= ~6u;
    if (!pass_stencil_) flags &= ~4u;
    if (!(flags & 7)) return;
    const std::vector<Rect> todo = ClipRects(count, rects, {0, 0, pass_w_, pass_h_});
    if (todo.empty()) return;
    // A clear of the whole target before anything was drawn is the
    // render pass's load operation.
    if (cmds_.empty() && todo.size() == 1 && todo[0].left == 0 && todo[0].top == 0 &&
        todo[0].right == pass_w_ && todo[0].bottom == pass_h_) {
      if (flags & 1) {
        pass_clear_color_ = true;
        Argb(&clear_color_.r, color);
      }
      if (flags & 2) {
        pass_clear_depth_ = true;
        clear_depth_ = z;
      }
      if (flags & 4) {
        pass_clear_stencil_ = true;
        clear_stencil_ = uint8_t(stencil);
      }
      return;
    }
    // Otherwise quads that write exactly what the flags say.
    if ((flags & 1) && pass_targets_ > 1)
      FatalF("gfx: partial Clear of %d render targets is not supported", pass_targets_);
    PipelineKey k = BaseKey(SDL_GPU_PRIMITIVETYPE_TRIANGLELIST);
    k.write_mask = (flags & 1) ? 0xF : 0;
    if (flags & 2) {
      k.depth_test = k.depth_write = 1;
      k.depth_func = SDL_GPU_COMPAREOP_ALWAYS;
    }
    if (flags & 4) {
      k.stencil = 1;
      k.s_fail = k.s_zfail = k.s_pass = SDL_GPU_STENCILOP_REPLACE;
      k.s_func = SDL_GPU_COMPAREOP_ALWAYS;
      k.s_read = k.s_write = 0xFF;
    }
    struct V {
      float x, y, z, rhw;
      uint32_t c;
    };
    for (const Rect& r : todo) {
      float x0 = float(r.left) - 0.5f, y0 = float(r.top) - 0.5f;
      float x1 = float(r.right) - 0.5f, y1 = float(r.bottom) - 0.5f;
      V q[6] = {{x0, y0, z, 1, color}, {x1, y0, z, 1, color}, {x0, y1, z, 1, color},
                {x1, y0, z, 1, color}, {x1, y1, z, 1, color}, {x0, y1, z, 1, color}};
      Cmd c = InternalCmd(k, 0x004 | 0x040, false);
      c.stencil_ref = uint8_t(stencil);
      c.scissor = {r.left, r.top, r.right - r.left, r.bottom - r.top};
      AddVertices(c, q, 6, sizeof(V));
      cmds_.push_back(c);
    }
  }

  void BeginScene() override {}
  void EndScene() override {}

  // The streams as they lie (PlaceStreams), or interleaved when one is
  // not aligned for the shader's reads.
  void DrawFf(const FfState& s, const FfDraw& d) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    uint32_t stride = FvfStride(d.fvf);
    const uint32_t nv = d.vertex_count, first = d.first_vertex;
    if (!nv || !stride) return;
    auto place = [&](VsLayout& vl) {
      if (PlaceStreams(d, vl)) return 0u;
      uint32_t at;
      InterleaveFf(d, ReserveVertices(nv, stride, &at));
      PatchLayout(vl, DecodeFvf(d.fvf, stride));
      return at;
    };
    auto draw = [&](auto index) {
      DrawWith(s, d.prim, d.fvf, nv, d.indices != nullptr, d.index_count, place, index);
    };
    // Indices relative to first_vertex, clamped to the vertices (FfIndex).
    auto clamp = [nv](uint32_t v) { return v < nv ? v : nv - 1; };
    if (!d.indices)
      draw([&](uint32_t i) { return clamp(i); });
    else if (d.index_size == 4)
      draw([&, p = static_cast<const uint32_t*>(d.indices)](uint32_t i) { return clamp(p[i] - first); });
    else
      draw([&, p = static_cast<const uint16_t*>(d.indices)](uint32_t i) { return clamp(p[i] - first); });
  }

  // A fixed-function draw with state `s` of `nv` vertices laid out by the
  // FVF: place(vl) puts them in the arena, points the layout uniforms at
  // them and returns the vertex number the draw's vertex 0 has; index(i)
  // is index i < ni of an indexed draw (clamped to the vertices), i
  // itself otherwise.
  template <class Place, class Index>
  void DrawWith(const FfState& s, uint32_t prim, uint32_t fvf, uint32_t nv, bool indexed,
                uint32_t ni, Place place, Index index) {
    uint32_t count = PrimitiveCountFromVertices(prim, indexed ? ni : nv);
    if (!count) return;
    SDL_GPUPrimitiveType type;
    uint32_t n;  // vertices or indices used
    switch (prim) {
      case 1: type = SDL_GPU_PRIMITIVETYPE_POINTLIST; n = count; break;
      case 2: type = SDL_GPU_PRIMITIVETYPE_LINELIST; n = count * 2; break;
      case 3: type = SDL_GPU_PRIMITIVETYPE_LINESTRIP; n = count + 1; break;
      case 4: type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST; n = count * 3; break;
      case 5: type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP; n = count + 2; break;
      case 6: type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST; n = count + 2; break;  // fan
      default: return;
    }
    const int pos_kind = DecodeFvf(fvf, 0).pos_kind;
    const bool rhw = pos_kind == 1;

    Cmd c{};
    c.key = DrawKey(s, type);
    if (!SetupScissor(s, c, rhw)) return;
    BuildUniforms(s);
    VsUniforms vu = vs_cur_;
    vu.mode[0] = pos_kind;
    VsLayout vl;
    placed_pv_ = false;
    const uint32_t first_vertex = place(vl);
    c.pv = placed_pv_;
    c.vs = PushVs(vu);
    c.vl = PushVl(vl);
    c.fs = PushFs(fs_cur_);
    BindStageTextures(s, c);
    c.stencil_ref = uint8_t(s.rs[57]);
    c.viewport = rhw ? FullViewport() : D3DViewport(s);
    // Indices as 32-bit, fans as lists, out-of-range ones clamped (D3D7
    // tolerates them; the shader would read other draws' vertices).
    if (indexed || prim == 6) {
      c.indexed = true;
      c.first = uint32_t(indices_.size());
      if (prim == 6) {
        for (uint32_t t = 0; t < count; t++) {
          indices_.push_back(index(0));
          indices_.push_back(index(t + 1));
          indices_.push_back(index(t + 2));
        }
        c.count = count * 3;
      } else {
        indices_.resize(c.first + n);
        uint32_t* to = indices_.data() + c.first;
        for (uint32_t i = 0; i < n; i++) to[i] = index(i);
        c.count = n;
      }
      c.base = int32_t(first_vertex);
    } else {
      c.first = first_vertex;
      c.count = std::min(n, nv);
    }
    cmds_.push_back(c);
  }


  // ---- translated shaders (runtime/hlsl). The prelude completes the
  // translation (hlsl.h): bindings in SDL_GPU's layout (SPIR-V sets, or
  // Metal indices with uniform buffers before storage buffers), vertex
  // pulling by declaration type, D3D9's pixel centres, the alpha test.
  //
  // Uniform buffers, both stages: 0 float registers, 1 int and bool
  // registers, 2 the prelude's state.

  struct VsState {
    // Per input slot: byte offset of vertex 0, stride, D3DDECLTYPE (-1:
    // none), and the instance divisor (0: per vertex).
    int32_t in[16][4];
    float pixel[4];     // 1 / viewport width, 1 / viewport height
    float clip[8][4];   // user clip planes, in clip space (shaders)
    int32_t clip_mask[4];  // CLIPPLANEENABLE, -, -, -
  };
  struct PsState {
    int32_t alpha[4];  // D3DCMPFUNC (ALWAYS when the test is off), ALPHAREF
    float bias[16];    // per sampler register: MIPMAPLODBIAS (R_BIAS on Metal)
    float fog_color[4];
    float fog[4];          // FOGSTART, FOGEND, FOGDENSITY
    int32_t fog_mode[4];   // 0 off, 1 vertex (the FOG varying), 2 table; FOGTABLEMODE
    // Bit per sampler register: SRGBTEXTURE (texels decoded from sRGB);
    // SRGBWRITEENABLE (the colour output encoded to sRGB).
    int32_t srgb[4];
    int32_t occl[4];  // the occlusion query's counter (r_oq), in a counting variant
  };

  // A pixel shader's variant (Variant below): occl, occlusion counting
  // (early depth and stencil tests, and R_OCCL_COUNT, which the translated
  // shader calls last, adds 1 to the query's counter); shadow, a bit per
  // sampler register with a depth texture bound (R_SHADOW_n: a shadow map
  // comparison sampler).
  static std::string Prelude(hlsl::Stage stage, uint32_t samplers, bool occl = false,
                             uint32_t shadow = 0) {
#if defined(__APPLE__)
    const bool metal = true;
#else
    const bool metal = false;
#endif
    std::string p;
    auto binding = [&](int set, int b) {
      return "set = " + std::to_string(metal ? 0 : set) + ", binding = " + std::to_string(b);
    };
    if (stage == hlsl::Stage::Vertex) {
      p += "#define R_FLOAT_BINDING " + binding(1, 0) + "\n";
      p += "#define R_INT_BINDING " + binding(1, 1) + "\n";
      p += std::string("#define R_SAMPLER_BINDING(n) set = ") + (metal ? "0" : "0") + ", binding = n\n";
      p += "layout(std140, " + binding(1, 2) +
           ") uniform r_vs_state { ivec4 r_in[16]; vec4 r_pixel; vec4 r_clip[8]; ivec4 r_clipmask; };\n";
      p += "layout(std430, " + binding(0, metal ? 3 : int(samplers)) +
           ") readonly buffer r_vertices { uint r_vb[]; };\n";
      p += R"(uint r_word(int b) {
  int w = b >> 2, s = (b & 3) * 8;
  return s == 0 ? r_vb[w] : (r_vb[w] >> s) | (r_vb[w + 1] << (32 - s));
}
float r_f(int b) { return uintBitsToFloat(r_word(b)); }
int r_lo(uint w) { return int(w << 16) >> 16; }
int r_hi(uint w) { return int(w) >> 16; }
// The input register as D3D9 fills it from the declaration's type;
// components the type lacks are 0 (w 1), inputs it lacks (0, 0, 0, 1).
vec4 r_fetch(int slot) {
  ivec4 d = r_in[slot];
  if (d.z < 0) return vec4(0.0, 0.0, 0.0, 1.0);
  int b = d.x + (d.w == 0 ? gl_VertexIndex : gl_InstanceIndex / d.w) * d.y;
  uint w = r_word(b);
  switch (d.z) {
    case 0: return vec4(uintBitsToFloat(w), 0.0, 0.0, 1.0);
    case 1: return vec4(uintBitsToFloat(w), r_f(b + 4), 0.0, 1.0);
    case 2: return vec4(uintBitsToFloat(w), r_f(b + 4), r_f(b + 8), 1.0);
    case 3: return vec4(uintBitsToFloat(w), r_f(b + 4), r_f(b + 8), r_f(b + 12));
    case 4: return vec4(uvec4(w >> 16, w >> 8, w, w >> 24) & 0xFFu) / 255.0;
    case 5: return vec4(uvec4(w, w >> 8, w >> 16, w >> 24) & 0xFFu);
    case 6: return vec4(r_lo(w), r_hi(w), 0.0, 1.0);
    case 7: { uint v = r_word(b + 4); return vec4(r_lo(w), r_hi(w), r_lo(v), r_hi(v)); }
    case 8: return vec4(uvec4(w, w >> 8, w >> 16, w >> 24) & 0xFFu) / 255.0;
    case 9: return vec4(max(vec2(r_lo(w), r_hi(w)) / 32767.0, -1.0), 0.0, 1.0);
    case 10: {
      uint v = r_word(b + 4);
      return max(vec4(r_lo(w), r_hi(w), r_lo(v), r_hi(v)) / 32767.0, -1.0);
    }
    case 11: return vec4(vec2(w & 0xFFFFu, w >> 16) / 65535.0, 0.0, 1.0);
    case 12: {
      uint v = r_word(b + 4);
      return vec4(w & 0xFFFFu, w >> 16, v & 0xFFFFu, v >> 16) / 65535.0;
    }
    case 13: return vec4(uvec3(w, w >> 10, w >> 20) & 0x3FFu, 1.0);
    case 14: {
      ivec3 v = ivec3(int(w << 22), int(w << 12), int(w << 2)) >> 22;
      return vec4(max(vec3(v) / 511.0, -1.0), 1.0);
    }
    case 15: return vec4(unpackHalf2x16(w), 0.0, 1.0);
    case 16: return vec4(unpackHalf2x16(w), unpackHalf2x16(r_word(b + 4)));
  }
  return vec4(0.0, 0.0, 0.0, 1.0);
}
// D3D9 puts pixel centres on integers, SDL_GPU on halves: half a pixel
// right and down (clip space is y-up).
// User clip planes (CLIPPLANEENABLE): with shaders, D3D9's are in clip
// space, against the position the shader writes.
out float gl_ClipDistance[8];
vec4 r_position(vec4 p) {
  for (int i = 0; i < 8; i++)
    gl_ClipDistance[i] = (r_clipmask.x & (1 << i)) != 0 ? dot(r_clip[i], p) : 1.0;
  return vec4(p.x + p.w * r_pixel.x, p.y - p.w * r_pixel.y, p.zw);
}
)";
    } else {
      p += "#define R_FLOAT_BINDING " + binding(3, 0) + "\n";
      p += "#define R_INT_BINDING " + binding(3, 1) + "\n";
      p += std::string("#define R_SAMPLER_BINDING(n) set = ") + (metal ? "0" : "2") + ", binding = n\n";
      p += "layout(std140, " + binding(3, 2) +
           ") uniform r_ps_state { ivec4 r_alpha; vec4 r_bias[4]; vec4 r_fogc; vec4 r_fogp;"
           " ivec4 r_fogm; ivec4 r_srgb; ivec4 r_occl; };\n";
      for (uint32_t r = 0; r < 16; r++)
        if (shadow & (1u << r)) p += "#define R_SHADOW_" + std::to_string(r) + "\n";
      if (occl) {
        p += "layout(early_fragment_tests) in;\n";
        p += "layout(std430, " + binding(2, metal ? 3 : int(samplers)) +
             ") buffer r_occl_counts { uint r_oq[]; };\n";
        p += "#define R_OCCL_COUNT() atomicAdd(r_oq[r_occl.x], 1u)\n";
      }
      p += R"(// sRGB reads (SRGBTEXTURE) and writes (SRGBWRITEENABLE): the texel
// decoded after filtering, the output encoded before blending, as
// Direct3D 9 hardware did them.
vec3 r_to_linear(vec3 c) {
  return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}
vec3 r_to_srgb(vec3 c) {
  c = clamp(c, 0.0, 1.0);
  return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}
vec4 r_srgb_in(int n, vec4 c) {
  return (r_srgb.x & (1 << n)) != 0 ? vec4(r_to_linear(c.rgb), c.a) : c;
}
#define R_SRGB(n, c) r_srgb_in(n, c)
)";
      // Metal samplers have no LOD bias (SDL_GPU drops it there): the
      // shader adds it where the level is implicit. Vulkan's apply it.
      // Dev aid: <ID>_NAN_PAINT=1 paints pixels whose colour is NaN or
      // infinite magenta.
      {
        static const bool paint = [] { const char* e = Env("NAN_PAINT"); return e && *e && *e != '0'; }();
        p += paint ? "#define R_NAN_PAINT 1\n" : "#define R_NAN_PAINT 0\n";
      }
      p += metal ? "#define R_BIAS(n) r_bias[(n) >> 2][(n) & 3]\n" : "#define R_BIAS(n) 0.0\n";
      p += R"(// Fog after a pixel shader (before shader model 3): the factor is the
// vertex stage's FOG output (vertex fog), or from the depth by FOGTABLEMODE
// (table fog); 1 keeps the colour, 0 is the fog colour.
void r_fog(inout vec4 c, float vf) {
  if (r_fogm.x == 0) return;
  float f = vf;
  if (r_fogm.x == 2) {
    float d = gl_FragCoord.z;
    if (r_fogm.y == 1) f = exp(-d * r_fogp.z);
    else if (r_fogm.y == 2) f = exp(-(d * r_fogp.z) * (d * r_fogp.z));
    else f = r_fogp.y == r_fogp.x ? (d < r_fogp.y ? 1.0 : 0.0) : (r_fogp.y - d) / (r_fogp.y - r_fogp.x);
  }
  c.rgb = mix(r_fogc.rgb, c.rgb, clamp(f, 0.0, 1.0));
}
vec2 r_vpos() { return gl_FragCoord.xy - 0.5; }
float r_vface() { return gl_FrontFacing ? 1.0 : -1.0; }
// The alpha test, on the alpha at 8-bit precision.
bool r_nonfinite(vec4 c) {  // by the bits: fast math may fold isnan away
  uvec4 e = (floatBitsToUint(c) >> 23) & 0xFFu;
  return any(equal(e, uvec4(0xFFu)));
}
void r_color0(inout vec4 c) {
  if (R_NAN_PAINT != 0 && r_nonfinite(c)) { c = vec4(1.0, 0.0, 1.0, 1.0); return; }
  int a = int(round(clamp(c.a, 0.0, 1.0) * 255.0)), ref = r_alpha.y;
  bool pass = true;
  switch (r_alpha.x) {
    case 1: pass = false; break;
    case 2: pass = a < ref; break;
    case 3: pass = a == ref; break;
    case 4: pass = a <= ref; break;
    case 5: pass = a > ref; break;
    case 6: pass = a != ref; break;
    case 7: pass = a >= ref; break;
  }
  if (!pass) discard;
  if (r_srgb.y != 0) c.rgb = r_to_srgb(c.rgb);
}
)";
    }
    return p;
  }

  ShaderId CreateShader(const hlsl::Program& prog) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    Shader sh;
    sh.stage = prog.stage;
    sh.inputs = prog.inputs.size();
    sh.float_regs = prog.float_regs;
    sh.sampler_type.fill(hlsl::ParamType::Void);
    for (const hlsl::Sampler& smp : prog.samplers) {
      if (smp.reg >= 16) FatalF("gfx: shader sampler register %u", smp.reg);
      sh.sampler_type[smp.reg] = smp.type;
      sh.samplers = std::max(sh.samplers, smp.reg + 1);
    }
    bool vertex = prog.stage == hlsl::Stage::Vertex;
    if (!vertex) sh.glsl = prog.glsl;  // for its variants
    // Compiled on the pool (CompileAsync): games create their shaders well
    // before drawing with them (Source: some 2000 before its menu), and
    // Metal's compiler takes tens of milliseconds for each.
    DumpShader(prog.stage, sh.samplers, prog.glsl);
    sh.pending = CompileAsync(sh, prog.glsl);
    ShaderId id = next_id_++;
    shaders_[id] = std::move(sh);
    return id;
  }


  SDL_GPUShader* CompileShader(hlsl::Stage stage, uint32_t samplers, const std::string& body,
                               bool occl, uint32_t shadow) {
    bool vertex = stage == hlsl::Stage::Vertex;
    // Positions invariant across programs (D3D9 multipass draws the same mesh
    // with different shaders and relies on equal depths).
    std::string glsl = "#version 450\n" + std::string(vertex ? "invariant gl_Position;\n" : "") +
                       Prelude(stage, samplers, occl, shadow) + body;
    std::vector<uint32_t> spirv;
    std::string log;
    if (!GlslToSpirv(glsl, vertex ? ShaderStage::Vertex : ShaderStage::Fragment, &spirv, &log)) {
      std::fprintf(stderr, "%s\n", glsl.c_str());
      FatalF("gfx: compiling a translated shader (source above): %s", log.c_str());
    }
    SDL_GPUShaderCreateInfo si{};
#if defined(__APPLE__)
    std::string msl;
    if (!SpirvToMsl(spirv, &msl, &log)) FatalF("gfx: SPIR-V to MSL: %s", log.c_str());
    si.code = reinterpret_cast<const Uint8*>(msl.c_str());
    si.code_size = msl.size();
    si.entrypoint = "main0";
    si.format = SDL_GPU_SHADERFORMAT_MSL;
#else
    si.code = reinterpret_cast<const Uint8*>(spirv.data());
    si.code_size = spirv.size() * 4;
    si.entrypoint = "main";
    si.format = SDL_GPU_SHADERFORMAT_SPIRV;
#endif
    si.stage = vertex ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT;
    si.num_samplers = samplers;
    si.num_storage_buffers = vertex || occl ? 1 : 0;
    si.num_uniform_buffers = 3;
    SDL_GPUShader* sh = SDL_CreateGPUShader(dev_, &si);
    if (!sh) FatalF("gfx: SDL_CreateGPUShader (translated): %s", SDL_GetError());
    return sh;
  }

  // A pixel shader's variant (Prelude's occl and shadow), made on first
  // use; the shader itself for neither.
  ShaderId Variant(ShaderId ps, bool occl, uint32_t shadow) {
    if (!occl && !shadow) return ps;
    Shader& sh = shaders_.at(ps);
    const uint32_t key = shadow << 1 | (occl ? 1 : 0);
    auto it = sh.variants.find(key);
    if (it != sh.variants.end()) return it->second;
    Shader v = sh;
    v.glsl.clear();
    v.variants.clear();
    v.pending = {};
    const Uint64 t0 = SDL_GetTicksNS();
    v.sh = CompileShader(sh.stage, sh.samplers, sh.glsl, occl, shadow);
    AddStall(Stall::ShaderCompile, SDL_GetTicksNS() - t0);
    ShaderId id = next_id_++;
    sh.variants[key] = id;
    shaders_[id] = std::move(v);
    return id;
  }

  void DestroyShader(ShaderId id) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto it = shaders_.find(id);
    if (it == shaders_.end()) return;
    // Recorded draws and cached pipelines may use it.
    Flush();
    for (auto p = shader_pipelines_.begin(); p != shader_pipelines_.end();) {
      if (p->first.vs == id || p->first.ps == id) {
        SDL_ReleaseGPUGraphicsPipeline(dev_, p->second);
        p = shader_pipelines_.erase(p);
      } else {
        ++p;
      }
    }
    std::vector<ShaderId> variants;
    for (const auto& [k, v] : it->second.variants) variants.push_back(v);
    SDL_ReleaseGPUShader(dev_, Gpu(it->second));
    shaders_.erase(it);
    for (ShaderId v : variants) DestroyShader(v);
  }

  void DrawShader(const ShaderDraw& d) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto vit = shaders_.find(d.vs), pit = shaders_.find(d.ps);
    if (vit == shaders_.end() || pit == shaders_.end()) return;
    const Shader& vs = vit->second;
    const Shader& ps = pit->second;
    uint32_t count = d.primitives;
    SDL_GPUPrimitiveType type;
    uint32_t n;  // vertices or indices used
    switch (d.prim) {
      case 1: type = SDL_GPU_PRIMITIVETYPE_POINTLIST; n = count; break;
      case 2: type = SDL_GPU_PRIMITIVETYPE_LINELIST; n = count * 2; break;
      case 3: type = SDL_GPU_PRIMITIVETYPE_LINESTRIP; n = count + 1; break;
      case 4: type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST; n = count * 3; break;
      case 5: type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP; n = count + 2; break;
      case 6: type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST; n = count + 2; break;  // fan
      default: return;
    }
    if (!count) return;
    if (!d.state) FatalF("gfx: DrawShader without its state");
    const FfState& state = *d.state;
    WarnUnsupportedShaderState(state);

    Cmd c{};
    if (!SetupScissor(state, c, false)) return;
    c.viewport = D3DViewport(state);
    c.stencil_ref = uint8_t(state.rs[57]);
    ShaderCmd sc;
    sc.key.k = DrawKey(state, type);
    // Front faces are D3D's (clockwise) whatever is culled, for VFACE.
    PipelineKey& k = sc.key.k;
    if (k.cull != SDL_GPU_CULLMODE_NONE)
      k.cull = state.rs[22] == 2 ? SDL_GPU_CULLMODE_FRONT : SDL_GPU_CULLMODE_BACK;
    k.front = SDL_GPU_FRONTFACE_CLOCKWISE;
    for (int i = 1; i < 4; i++) k.mask[i - 1] = i < pass_targets_ ? uint8_t(state.rs[189 + i] & 0xF) : 0;
    sc.key.vs = d.vs;
    sc.key.ps = d.ps;  // or its variant (below, after the samplers)
    if (occl_open_) {
      sc.occl = true;
      occl_draws_ = true;
    }
    if (k.blend && state.rs[206]) {  // SEPARATEALPHABLENDENABLE
      sc.key.src_a = uint8_t(D3DBlendFactor(state.rs[207], pass_alpha_));
      sc.key.dst_a = uint8_t(D3DBlendFactor(state.rs[208], pass_alpha_));
      sc.key.op_a = uint8_t(D3DBlendOp(state.rs[209]));
    } else {
      sc.key.src_a = k.src;
      sc.key.dst_a = k.dst;
      sc.key.op_a = k.op;
    }
    Argb(&sc.blend_constant.r, state.rs[193]);

    // Vertices: each stream's range copied to the arena once, or, when
    // every stream the shader reads is a locked per-vertex one (a static
    // vertex buffer's) drawn in an earlier frame too, read from pvbuf_
    // (PlacePersistent), copied there once.
    bool indexed = d.indices != nullptr;
    int64_t first = int64_t(d.base_vertex) + (indexed ? d.min_index : 0);
    uint32_t nverts = indexed ? d.vertex_count : n;
    const uint32_t instances = std::max(d.instances, 1u);
    VsState st{};
    int32_t stream_base[16];
    bool copied[16] = {};
    struct PvRun {
      uintptr_t lo, hi;
      uint32_t stride;
      int32_t at;
    };
    PvRun runs[13];
    uint32_t run_lock[13];
    int nruns = 0, run_of[16];
    bool listed[16] = {};
    bool persistent = pv_mode_ != PvMode::Off && nverts > 0;
    for (size_t i = 0; i < vs.inputs && persistent; i++) {
      VertexInput in = i < d.input_count ? d.inputs[i] : VertexInput{};
      if (in.type >= 17 || in.stream >= 16 || !d.streams[in.stream].data) continue;
      const VertexStream& vsm = d.streams[in.stream];
      if (listed[in.stream]) continue;
      listed[in.stream] = true;
      const int64_t start = first * vsm.stride, bytes = int64_t(nverts) * vsm.stride;
      if (!vsm.lock || vsm.divisor || !vsm.stride || start < 0 || start + bytes > vsm.size || nruns == 13) {
        persistent = false;
        break;
      }
      runs[nruns] = {uintptr_t(vsm.data + start), uintptr_t(vsm.data + start) + vsm.stride, vsm.stride, 0};
      run_lock[nruns] = vsm.lock;
      run_of[in.stream] = nruns++;
    }
    if (persistent) persistent = nruns > 0 && PlacePersistent(runs, nruns, run_lock, nverts);
    c.pv = persistent;
    for (size_t i = 0; i < vs.inputs; i++) {
      VertexInput in = i < d.input_count ? d.inputs[i] : VertexInput{};
      if (in.type >= 17 || in.stream >= 16 || !d.streams[in.stream].data) {
        st.in[i][2] = -1;
        continue;
      }
      const VertexStream& vsm = d.streams[in.stream];
      if (!copied[in.stream] && persistent) {
        stream_base[in.stream] =
            runs[run_of[in.stream]].at - int32_t(indexed ? d.min_index : 0) * int32_t(vsm.stride);
        copied[in.stream] = true;
      }
      if (!copied[in.stream]) {
        // Per-instance streams: their elements for every instance, from
        // the first.
        const uint32_t count = vsm.divisor ? (instances - 1) / vsm.divisor + 1 : nverts;
        int64_t start = vsm.divisor ? 0 : first * vsm.stride;
        int64_t end = std::min<int64_t>(start + int64_t(count) * vsm.stride, vsm.size);
        size_t pos = (verts_.size() + 3) & ~size_t(3);
        size_t bytes = start >= 0 && end > start ? size_t(end - start) : 0;
        // Room for a whole last vertex and the shader's unaligned reads:
        // what is not copied is zeroed (the arena grows without zeroing).
        const size_t room = size_t(count) * vsm.stride + 8;
        verts_.resize(pos + room);
        if (bytes) std::memcpy(verts_.data() + pos, vsm.data + start, bytes);
        std::memset(verts_.data() + pos + bytes, 0, room - bytes);
        stream_base[in.stream] =
            int32_t(pos) - (vsm.divisor ? 0 : int32_t(indexed ? d.min_index : 0) * int32_t(vsm.stride));
        copied[in.stream] = true;
      }
      st.in[i][0] = stream_base[in.stream] + int32_t(in.offset);
      st.in[i][1] = int32_t(vsm.stride);
      st.in[i][2] = int32_t(in.type);
      st.in[i][3] = int32_t(vsm.divisor);
    }
    std::memcpy(st.clip, state.clip, sizeof st.clip);
    st.clip_mask[0] = int32_t(state.rs[152] & 0xFF);
    st.pixel[0] = 1.0f / float(std::max(state.viewport.width, 1u));
    st.pixel[1] = 1.0f / float(std::max(state.viewport.height, 1u));

    int64_t pv_indices = -1;  // the indices' place in pvbuf_ (PlaceIndices)
    if (indexed && d.prim != 6 && d.index_lock && pv_mode_ != PvMode::Off) {
      const uint32_t lo = d.min_index, hi = lo + std::max(nverts, 1u) - 1;
      pv_indices = PlaceIndices(d.indices, d.index_size, n, d.index_lock, lo, hi);
    }
    if (pv_indices >= 0) {
      c.indexed = true;
      c.pv_indices = true;
      c.first = uint32_t(pv_indices / 4);
      c.count = n;
      c.base = 0;
    } else if (indexed || d.prim == 6) {
      c.indexed = true;
      c.first = uint32_t(indices_.size());
      uint32_t lo = indexed ? d.min_index : 0, hi = lo + std::max(nverts, 1u) - 1;
      auto at = [&](uint32_t i) -> uint32_t {
        uint32_t v = !indexed ? i
                     : d.index_size == 4 ? static_cast<const uint32_t*>(d.indices)[i]
                                         : static_cast<const uint16_t*>(d.indices)[i];
        return std::clamp(v, lo, hi);
      };
      if (d.prim == 6) {
        for (uint32_t t = 0; t < count; t++) {
          indices_.push_back(at(0));
          indices_.push_back(at(t + 1));
          indices_.push_back(at(t + 2));
        }
        c.count = count * 3;
      } else {
        // The common case, one pass per index size (at() per index, through
        // push_back, cost more than the copy).
        indices_.resize(size_t(c.first) + n);
        uint32_t* out = indices_.data() + c.first;
        if (!indexed) {
          for (uint32_t i = 0; i < n; i++) out[i] = i;
        } else if (d.index_size == 4) {
          const uint32_t* in = static_cast<const uint32_t*>(d.indices);
          for (uint32_t i = 0; i < n; i++) out[i] = std::clamp(in[i], lo, hi);
        } else {
          const uint16_t* in = static_cast<const uint16_t*>(d.indices);
          for (uint32_t i = 0; i < n; i++) out[i] = std::clamp(uint32_t(in[i]), lo, hi);
        }
        c.count = n;
      }
      c.base = 0;
    } else {
      c.first = 0;
      c.count = n;
    }

    c.instances = instances;

    // Uniforms.
    auto ints = [](const int32_t (*i)[4], const uint32_t* b) {
      std::array<int32_t, 80> u{};
      if (i) std::memcpy(u.data(), i, 16 * 16);
      for (int k = 0; b && k < 16; k++) u[64 + k] = b[k] ? 1 : 0;
      return u;
    };
    PushUniform(sc, 0, d.vs_float, vs.float_regs * 16);
    auto vi = ints(d.vs_int, d.vs_bool);
    PushUniform(sc, 1, vi.data(), sizeof vi);
    PushUniform(sc, 2, &st, sizeof st);
    PushUniform(sc, 3, d.ps_float, ps.float_regs * 16);
    auto pi = ints(d.ps_int, d.ps_bool);
    PushUniform(sc, 4, pi.data(), sizeof pi);
    PsState pst{};
    pst.alpha[0] = state.rs[15] ? int32_t(state.rs[25]) : 8;
    pst.alpha[1] = int32_t(state.rs[24] & 0xFF);
    for (uint32_t r = 0; r < 16 && d.sampler_states; r++) pst.bias[r] = AsFloat(d.sampler_states[r][8]);
    if (state.rs[28]) {  // FOGENABLE: table fog when FOGTABLEMODE is set
      pst.fog_mode[0] = state.rs[35] ? 2 : 1;
      pst.fog_mode[1] = int32_t(state.rs[35]);
    }
    Argb(pst.fog_color, state.rs[34]);
    for (int i = 0; i < 3; i++) pst.fog[i] = AsFloat(state.rs[36 + i]);
    for (uint32_t r = 0; r < 16 && d.sampler_states; r++)
      if (d.sampler_states[r][11]) pst.srgb[0] |= 1 << r;  // SRGBTEXTURE
    pst.srgb[1] = state.rs[194] ? 1 : 0;                     // SRGBWRITEENABLE
    if (occl_open_) pst.occl[0] = int32_t(occl_.at(occl_open_).slot);
    PushUniform(sc, 5, &pst, sizeof pst);

    // Samplers: pixel 0-15 at 0-15, vertex 0-3 at 16-19.
    uint32_t shadow = 0;  // pixel sampler registers reading a depth buffer
    sc.nps = ps.samplers;
    sc.nvs = vs.samplers;
    for (uint32_t r = 0; r < 20; r++) {
      bool vertex = r >= 16;
      const Shader& sh = vertex ? vs : ps;
      uint32_t reg = vertex ? r - 16 : r;
      if (reg >= sh.samplers) continue;
      hlsl::ParamType want = sh.sampler_type[reg];
      SDL_GPUTexture* dummy = want == hlsl::ParamType::SamplerCube ? dummy_cube_
                              : want == hlsl::ParamType::Sampler3D ? dummy_3d_
                                                                   : dummy_2d_;
      sc.tex[r] = dummy;
      // A texture of the kind the sampler declares (2D, cube or 3D); any
      // other reads as the dummy, as an unbound one. A depth buffer in a
      // pixel shader's 2D sampler: a shadow map (Variant's shadow).
      auto it = textures_.find(d.textures[r]);
      auto dit = it == textures_.end() && !vertex && want == hlsl::ParamType::Sampler2D
                     ? depths_.find(d.textures[r])
                     : depths_.end();
      if (dit != depths_.end()) {
        sc.tex[r] = dit->second.tex;
        shadow |= 1u << reg;
        SamplerKey sk = D3D9Sampler(d.sampler_states ? d.sampler_states[r] : nullptr);
        sk.pad = 1;  // comparing
        sk.mip = 0xFF;
#if defined(__APPLE__)
        sk.lod_bias = 0;
#endif
        sc.smp[r] = Sampler(sk);
        continue;
      }
      if (it != textures_.end()) {
        const TextureKind k = it->second.cube ? TextureKind::Cube : it->second.kind;
        const TextureKind w = want == hlsl::ParamType::SamplerCube ? TextureKind::Cube
                              : want == hlsl::ParamType::Sampler3D ? TextureKind::Tex3D
                                                                   : TextureKind::Tex2D;
        if (k == w) sc.tex[r] = it->second.tex;
      }
      SamplerKey sk = D3D9Sampler(d.sampler_states ? d.sampler_states[r] : nullptr);
#if defined(__APPLE__)
      sk.lod_bias = 0;  // in the shader (R_BIAS)
#endif
      sc.smp[r] = Sampler(sk);
    }
    sc.key.ps = Variant(d.ps, sc.occl, shadow);
    ShaderPipeline(sc.key);
    c.shader = int(shader_cmds_.size());
    shader_cmds_.push_back(sc);
    cmds_.push_back(c);
  }

  // ---- programs (console GPUs)

  ProgramId CreateProgram(const ProgramDesc& d) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    if (!heap_) GrowHeap(0);  // bound beside the arenas
    // What a draw or dispatch binds: ProgramStageCmd's textures and
    // DispatchProgram's storage images.
    for (const ProgramStage* st : {&d.vs, &d.fs, &d.cs})
      if (st->textures > 16 || st->rw_images > 8)
        FatalF("gfx: a program stage with %u textures and %u storage images (at most 16 and 8)",
               st->textures, st->rw_images);
    Program pr;
    pr.vs = d.vs;
    pr.fs = d.fs;
    pr.cs = d.cs;
    if (!d.cs.glsl.empty()) {
      pr.compute = true;
      const ProgramCode code(CompileProgramStage(d.cs, ShaderStage::Compute, d.local_size));
      SDL_GPUComputePipelineCreateInfo ci{};
      ci.code = code.code;
      ci.code_size = code.size;
      ci.entrypoint = code.entry;
      ci.format = code.format;
      ci.num_samplers = d.cs.textures;
      ci.num_readonly_storage_buffers = d.cs.buffers ? 2 : 0;
      ci.num_readwrite_storage_textures = d.cs.rw_images;
      ci.num_readwrite_storage_buffers = d.cs.rw_buffers ? 1 : 0;
      ci.num_uniform_buffers = ProgramUniformBlocks(d.cs);
      ci.threadcount_x = d.local_size[0];
      ci.threadcount_y = d.local_size[1];
      ci.threadcount_z = d.local_size[2];
      pr.cp = SDL_CreateGPUComputePipeline(dev_, &ci);
      if (!pr.cp) FatalF("gfx: SDL_CreateGPUComputePipeline: %s", SDL_GetError());
    } else {
      for (bool vertex : {true, false}) {
        const ProgramStage& st = vertex ? d.vs : d.fs;
        const ProgramCode code(
            CompileProgramStage(st, vertex ? ShaderStage::Vertex : ShaderStage::Fragment, nullptr));
        SDL_GPUShaderCreateInfo si{};
        si.code = code.code;
        si.code_size = code.size;
        si.entrypoint = code.entry;
        si.format = code.format;
        si.stage = vertex ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT;
        si.num_samplers = st.textures;
        si.num_storage_buffers = st.buffers ? 2 : 0;
        si.num_uniform_buffers = ProgramUniformBlocks(st);
        SDL_GPUShader* sh = SDL_CreateGPUShader(dev_, &si);
        if (!sh) FatalF("gfx: SDL_CreateGPUShader (program): %s", SDL_GetError());
        (vertex ? pr.vsh : pr.fsh) = sh;
      }
    }
    pr.vs.glsl.clear();
    pr.fs.glsl.clear();
    pr.cs.glsl.clear();
    const ProgramId id = next_id_++;
    programs_[id] = pr;
    return id;
  }

  void DestroyProgram(ProgramId id) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto it = programs_.find(id);
    if (it == programs_.end()) return;
    Flush();
    for (auto p = program_pipelines_.begin(); p != program_pipelines_.end();) {
      if (p->first.program == id) {
        SDL_ReleaseGPUGraphicsPipeline(dev_, p->second);
        p = program_pipelines_.erase(p);
      } else {
        ++p;
      }
    }
    ReleaseProgram(it->second);
    programs_.erase(it);
  }

  TextureId CreateImage(const ImageDesc& d) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    if (d.w <= 0 || d.h <= 0 || d.layers <= 0 || d.levels <= 0) return 0;
    Tex t;
    t.w = d.w;
    t.h = d.h;
    t.levels = d.levels;
    t.layers = d.layers;
    t.fmt = d.fmt;
    t.rt = d.color_target;
    t.kind = d.kind;
    t.cube = d.kind == TextureKind::Cube;
    t.storage = d.storage;
    SDL_GPUTextureCreateInfo ti{};
    ti.type = d.kind == TextureKind::Cube         ? SDL_GPU_TEXTURETYPE_CUBE
              : d.kind == TextureKind::Tex2DArray ? SDL_GPU_TEXTURETYPE_2D_ARRAY
              : d.kind == TextureKind::Tex3D      ? SDL_GPU_TEXTURETYPE_3D
                                                  : SDL_GPU_TEXTURETYPE_2D;
    ti.format = GpuFormat(d.fmt);
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER |
               (d.color_target ? SDL_GPU_TEXTUREUSAGE_COLOR_TARGET : 0) |
               (d.storage ? SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE : 0);
    ti.width = Uint32(d.w);
    ti.height = Uint32(d.h);
    ti.layer_count_or_depth = Uint32(d.layers);
    ti.num_levels = Uint32(d.levels);
    t.tex = SDL_CreateGPUTexture(dev_, &ti);
    if (!t.tex) {
      WarnF("gfx: CreateImage %dx%dx%d x%d fmt %u failed: %s\n", d.w, d.h, d.layers, d.levels,
            unsigned(d.fmt), SDL_GetError());
      return 0;
    }
    const TextureId id = next_id_++;
    textures_[id] = t;
    tex_gen_++;
    return id;
  }

  void UpdateImage(TextureId id, int level, int layer, const Rect& r, const void* src,
                   uint32_t pitch) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto it = textures_.find(id);
    if (it == textures_.end() || level < 0 || level >= it->second.levels) return;
    const Tex& t = it->second;
    // A 3D image's layers are its depth, halved per level.
    const bool volume = t.kind == TextureKind::Tex3D;
    if (layer < 0 || layer >= (volume ? LevelSize(t.layers, level) : t.layers)) return;
    const Rect c = Clip(r, LevelSize(t.w, level), LevelSize(t.h, level));
    const int w = c.right - c.left, h = c.bottom - c.top;
    if (w <= 0 || h <= 0) return;
    if (Pending(t.tex)) Flush();
    if (IsRaw(t.fmt) || IsCompressed(t.fmt)) {
      QueueUpload(t.tex, level, c.left, c.top, w, h, src, pitch, RowCount(t.fmt, uint32_t(h)),
                  RowPitch(t.fmt, uint32_t(w)), false, IsCompressed(t.fmt), volume ? 0 : layer);
    } else {  // the Direct3D colour formats: A8R8G8B8 on the GPU (GpuFormat), as UpdateTexture
      std::vector<uint32_t> px(size_t(w) * h);
      ConvertToArgb(static_cast<const uint8_t*>(src), pitch, t.fmt, w, h, px.data());
      QueueUpload(t.tex, level, c.left, c.top, w, h, px.data(), uint32_t(w) * 4, uint32_t(h),
                  uint32_t(w) * 4, false, false, volume ? 0 : layer);
    }
    if (volume) uploads_.back().z = uint32_t(layer);
  }

  void ReadImage(TextureId id, int level, int layer, const Rect& r, void* dst,
                 uint32_t pitch) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto it = textures_.find(id);
    if (it == textures_.end() || level < 0 || level >= it->second.levels) return;
    const Tex& t = it->second;
    const Rect c = Clip(r, LevelSize(t.w, level), LevelSize(t.h, level));
    const uint32_t w = uint32_t(c.right - c.left), h = uint32_t(c.bottom - c.top);
    if (!w || !h || IsCompressed(t.fmt)) return;
    Flush();
    // The Direct3D colour formats are A8R8G8B8 on the GPU (GpuFormat):
    // read as that and converted back.
    const bool convert = !IsRaw(t.fmt);
    const uint32_t row = convert ? w * 4 : RowPitch(t.fmt, w), size = row * h;
    SDL_GPUTransferBufferCreateInfo tbi{};
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tbi.size = size;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev_, &tbi);
    if (!tb) FatalF("gfx: transfer buffer: %s", SDL_GetError());
    SDL_GPUCommandBuffer* cmd = Begin();
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion src{};
    src.texture = t.tex;
    src.mip_level = Uint32(level);
    // A 3D image's layers are its depth.
    if (t.kind == TextureKind::Tex3D) src.z = Uint32(layer);
    else src.layer = Uint32(layer);
    src.x = Uint32(c.left);
    src.y = Uint32(c.top);
    src.w = w;
    src.h = h;
    src.d = 1;
    SDL_GPUTextureTransferInfo to{};
    to.transfer_buffer = tb;
    to.pixels_per_row = w;
    to.rows_per_layer = h;
    SDL_DownloadFromGPUTexture(cp, &src, &to);
    SDL_EndGPUCopyPass(cp);
    SubmitOpen();
    Drain();
    const auto* p = static_cast<const uint8_t*>(SDL_MapGPUTransferBuffer(dev_, tb, false));
    const uint32_t bpp = BitsPerPixel(t.fmt) / 8;
    for (uint32_t y = 0; y < h; y++) {
      uint8_t* d = static_cast<uint8_t*>(dst) + size_t(y) * pitch;
      if (!convert) {
        std::memcpy(d, p + size_t(y) * row, row);
        continue;
      }
      for (uint32_t x = 0; x < w; x++) {
        uint32_t argb;
        std::memcpy(&argb, p + size_t(y) * row + size_t(x) * 4, 4);
        EncodePixel(d + size_t(x) * bpp, t.fmt, argb);
      }
    }
    SDL_UnmapGPUTransferBuffer(dev_, tb);
    SDL_ReleaseGPUTransferBuffer(dev_, tb);
  }

  void ClearImage(TextureId id, int level, const Color& c) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto it = textures_.find(id);
    if (it == textures_.end() || !it->second.rt || level < 0 || level >= it->second.levels) return;
    Flush();
    SDL_GPUCommandBuffer* cmd = Begin();
    for (int layer = 0; layer < it->second.layers; layer++) {
      SDL_GPUColorTargetInfo ct{};
      ct.texture = it->second.tex;
      ct.mip_level = Uint32(level);
      ct.layer_or_depth_plane = Uint32(layer);
      ct.load_op = SDL_GPU_LOADOP_CLEAR;
      ct.clear_color = {c.r, c.g, c.b, c.a};
      ct.store_op = SDL_GPU_STOREOP_STORE;
      SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr));
    }
    Submit(cmd);
  }

  void ClearDepth(DepthId id, float z, int stencil, int layer) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto it = depths_.find(id);
    if (it == depths_.end() || layer < 0 || layer >= it->second.layers) return;
    Flush();
    SDL_GPUCommandBuffer* cmd = Begin();
    SDL_GPUDepthStencilTargetInfo dt{};
    dt.texture = it->second.tex;
    dt.load_op = SDL_GPU_LOADOP_CLEAR;
    dt.clear_depth = z;
    dt.store_op = SDL_GPU_STOREOP_STORE;
    dt.stencil_load_op = stencil >= 0 ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    dt.stencil_store_op = SDL_GPU_STOREOP_STORE;
    dt.clear_stencil = Uint8(stencil >= 0 ? stencil : 0);
    dt.layer = Uint8(layer);
    SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd, nullptr, 0, &dt));
    Submit(cmd);
  }

  void CopyDepth(DepthId src, DepthId dst, const Rect& r) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto s = depths_.find(src), d = depths_.find(dst);
    if (s == depths_.end() || d == depths_.end() || s->second.gpu_format != d->second.gpu_format) return;
    const Rect a = Clip(Clip(r, s->second.w, s->second.h), d->second.w, d->second.h);
    if (a.right <= a.left || a.bottom <= a.top) return;
    Flush();
    SDL_GPUCommandBuffer* cmd = Begin();
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureLocation from{}, to{};
    from.texture = s->second.tex;
    from.x = to.x = Uint32(a.left);
    from.y = to.y = Uint32(a.top);
    to.texture = d->second.tex;
    for (int i = 0; i < std::min(s->second.layers, d->second.layers); i++) {
      from.layer = to.layer = Uint32(i);
      SDL_CopyGPUTextureToTexture(cp, &from, &to, Uint32(a.right - a.left), Uint32(a.bottom - a.top), 1,
                                  false);
    }
    SDL_EndGPUCopyPass(cp);
    Submit(cmd);
  }

  void DrawProgram(const ProgramDraw& d) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto pit = programs_.find(d.program);
    if (pit == programs_.end() || pit->second.compute) return;
    if (!d.indirect && (!d.vertex_count || !d.instance_count)) return;
    const Program& pr = pit->second;

    // The pass: the colour targets (from target 0 on) and the depth.
    std::array<SDL_GPUTexture*, 4> colors{};
    std::array<uint8_t, 4> fmts{};
    std::array<int, 4> levels{};
    int w = 0, h = 0, n = 0;
    for (int i = 0; i < 4 && d.targets[i].tex; i++, n++) {
      auto it = textures_.find(d.targets[i].tex);
      if (it == textures_.end() || !it->second.rt || d.targets[i].level >= it->second.levels)
        FatalF("gfx: DrawProgram: target %d is no colour target", i);
      colors[size_t(i)] = it->second.tex;
      levels[size_t(i)] = d.targets[i].level;
      fmts[size_t(i)] = uint8_t(GpuFormat(it->second.fmt));
      if (!i) {
        w = LevelSize(it->second.w, d.targets[i].level);
        h = LevelSize(it->second.h, d.targets[i].level);
      }
    }
    const Depth* depth = nullptr;
    if (d.depth) {
      auto dit = depths_.find(d.depth);
      if (dit == depths_.end() || d.depth_layer < 0 || d.depth_layer >= dit->second.layers)
        FatalF("gfx: DrawProgram: no depth target %u (layer %d)", d.depth, d.depth_layer);
      depth = &dit->second;
      if (!n) w = depth->w, h = depth->h;
    }
    if (!n && !depth) return;
    SetPass(colors, fmts, depth, w, h, true, levels, d.depth_layer);

    Cmd c{};
    const Rect sc = Intersect(Clip(d.scissor, w, h), Rect{0, 0, w, h});
    if (sc.right <= sc.left || sc.bottom <= sc.top) return;
    c.scissor = {sc.left, sc.top, sc.right - sc.left, sc.bottom - sc.top};
    c.viewport = {float(d.viewport.x), float(d.viewport.y), float(d.viewport.width),
                  float(d.viewport.height), d.viewport.min_z, d.viewport.max_z};
    c.stencil_ref = d.state.stencil_ref;
    if (d.indirect) {
      auto bit = gpu_buffers_.find(d.indirect);
      if (bit == gpu_buffers_.end()) FatalF("gfx: DrawProgram: no buffer %u", d.indirect);
      c.indexed = true;
      c.indirect = heap_;
      c.buffer_at = bit->second.at;
      DrawRead(d.indirect);
    } else if (d.index_buffer) {
      auto bit = gpu_buffers_.find(d.index_buffer);
      if (bit == gpu_buffers_.end()) FatalF("gfx: DrawProgram: no buffer %u", d.index_buffer);
      c.indexed = true;
      c.indices = heap_;
      c.buffer_at = bit->second.at;
      c.short_indices = d.index_size == 2;
      c.first = d.first_index;
      c.base = d.base_vertex;
      DrawRead(d.index_buffer);
    } else if (d.indices) {
      c.indexed = true;
      c.first = uint32_t(indices_.size());
      const uint32_t n = d.vertex_count;  // a local: the stores below may alias d
      indices_.resize(indices_.size() + n);
      uint32_t* to = indices_.data() + c.first;
      if (d.index_size == 4) {
        std::memcpy(to, d.indices, size_t(n) * 4);
      } else {
        const auto* from = static_cast<const uint16_t*>(d.indices);
        for (uint32_t i = 0; i < n; i++) to[i] = from[i];
      }
      c.base = d.base_vertex;
    } else {
      c.first = uint32_t(d.base_vertex);
    }
    c.count = d.vertex_count;

    ProgramCmd pc;
    pc.pipeline = ProgramPipeline(d.program, d.state, n, fmts, depth);
    pc.instances = d.instance_count;
    pc.blend_constant = {d.state.blend_constant.r, d.state.blend_constant.g,
                         d.state.blend_constant.b, d.state.blend_constant.a};
    draw_spans_.clear();
    RecordStage(pc.stage[0], pr.vs, d.vs, true);
    RecordStage(pc.stage[1], pr.fs, d.fs, true);
    c.program = int(program_cmds_.size());
    program_cmds_.push_back(pc);
    cmds_.push_back(c);
  }

  void DispatchProgram(const ProgramDispatch& d) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto pit = programs_.find(d.program);
    if (pit == programs_.end() || !pit->second.compute) return;
    const Program& pr = pit->second;
    const ProgramStage& st = pr.cs;
    if (!d.groups[0] || !d.groups[1] || !d.groups[2]) return;
    // What was drawn before comes first.
    Flush();

    // The read-only and read-write buffers, each in its own arena, and
    // their places in the uniform block. Read-only GPU buffers are read
    // in the heap; read-write ones are copied into their arena, and their
    // results back out, on the GPU.
    Arena ro, rw;
    std::vector<uint32_t> table(4 * size_t(st.buffers + st.rw_buffers), 0);
    std::vector<BufferCopy> rw_in, rw_out;
    bool host_rw = false;
    for (uint32_t b = 0; b < st.buffers; b++) {
      const BufferData& bd = d.cs.buffers[b];
      if (bd.buffer) {
        HeapEntry(bd, &table[4 * b]);
        continue;
      }
      table[4 * b] = uint32_t(ro.size() / 4);
      table[4 * b + 1] = bd.size;
      AppendWords(ro, bd.data, bd.size);
    }
    std::vector<uint32_t> rw_at(st.rw_buffers);
    for (uint32_t b = 0; b < st.rw_buffers; b++) {
      const RwBufferData& bd = d.rw_buffers[b];
      rw_at[b] = uint32_t(rw.size());
      table[4 * (st.buffers + b)] = uint32_t(rw.size() / 4);
      table[4 * (st.buffers + b) + 1] = bd.size;
      if (bd.buffer) {
        BufferCopy c = GpuBufferCopy(bd.buffer, rw.size(), bd.size);
        c.held = (bd.size + 3) & ~3u;
        if (!bd.data) rw_in.push_back(c);
        rw_out.push_back(c);
        host_rw |= bd.land && bd.data;
      } else {
        host_rw = true;
      }
      AppendWords(rw, bd.data, bd.size);
    }
    if (st.buffers && ro.empty()) ro.resize(4, 0);
    if (st.rw_buffers && rw.empty()) rw.resize(4, 0);

    if (!ro.empty() || !rw.empty()) {
      auto* p = MapUpload(ro.size() + rw.size());
      SDL_GPUTransferBuffer* tb = upload_tb_;
      if (!ro.empty()) std::memcpy(p, ro.data(), ro.size());
      HostSpans(rw_in, rw.size(), [&](size_t at, size_t n) { std::memcpy(p + ro.size() + at, rw.data() + at, n); });
      SDL_UnmapGPUTransferBuffer(dev_, tb);
      SDL_GPUCommandBuffer* cmd = Begin();  // after MapUpload, which may submit it
      SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
      if (!ro.empty()) {
        EnsureBuffer(cro_, cro_cap_, ro.size(), SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ);
        SDL_GPUTransferBufferLocation src{tb, upload_at_};
        SDL_GPUBufferRegion dst{cro_, 0, Uint32(ro.size())};
        SDL_UploadToGPUBuffer(cp, &src, &dst, true);
      }
      if (!rw.empty()) {
        EnsureBuffer(crw_, crw_cap_, rw.size(),
                     SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ |
                         SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE);
        HostSpans(rw_in, rw.size(), [&](size_t at, size_t n) {
          SDL_GPUTransferBufferLocation src{tb, Uint32(upload_at_ + ro.size() + at)};
          SDL_GPUBufferRegion dst{crw_, Uint32(at), Uint32(n)};
          SDL_UploadToGPUBuffer(cp, &src, &dst, false);
        });
        CopyBuffers(cp, rw_in, crw_, true);
      }
      SDL_EndGPUCopyPass(cp);
    }

    SDL_GPUCommandBuffer* cmd = Begin();
    SDL_GPUStorageTextureReadWriteBinding images[8]{};
    for (uint32_t i = 0; i < st.rw_images; i++) {
      auto it = textures_.find(d.rw_images[i].tex);
      if (it == textures_.end() || !it->second.storage)
        FatalF("gfx: DispatchProgram: image %u is no storage image", i);
      images[i].texture = it->second.tex;
      images[i].mip_level = Uint32(d.rw_images[i].level);
    }
    SDL_GPUStorageBufferReadWriteBinding rwb{};
    rwb.buffer = crw_;
    SDL_GPUComputePass* cp = SDL_BeginGPUComputePass(cmd, images, st.rw_images, &rwb,
                                                     rw.empty() ? 0 : 1);
    SDL_BindGPUComputePipeline(cp, pr.cp);
    ProgramStageCmd sc;
    RecordStage(sc, st, d.cs);
    if (sc.ntex) {
      SDL_GPUTextureSamplerBinding tbs[16];
      for (uint32_t i = 0; i < sc.ntex; i++) tbs[i] = {sc.tex[i], sc.smp[i]};
      SDL_BindGPUComputeSamplers(cp, 0, tbs, sc.ntex);
    }
    SDL_GPUBuffer* bufs[2] = {cro_, heap_};
    if (!ro.empty()) SDL_BindGPUComputeStorageBuffers(cp, 0, bufs, 2);
    Uint32 slot = 0;
    if (!table.empty()) SDL_PushGPUComputeUniformData(cmd, slot++, table.data(), Uint32(table.size() * 4));
    if (st.uniform_bytes)
      SDL_PushGPUComputeUniformData(cmd, slot, d.cs.uniforms, st.uniform_bytes);
    SDL_DispatchGPUCompute(cp, d.groups[0], d.groups[1], d.groups[2]);
    SDL_EndGPUComputePass(cp);

    if (!rw_out.empty()) {
      SDL_GPUCopyPass* out = SDL_BeginGPUCopyPass(cmd);
      CopyBuffers(out, rw_out, crw_, false);
      SDL_EndGPUCopyPass(out);
    }
    if (!host_rw) {
      Submit(cmd);
      return;
    }
    Landing w;
    w.cap = Uint32(rw.size());
    for (size_t i = 0; i < download_pool_.size(); i++) {
      if (download_pool_[i].cap < w.cap) continue;
      w = download_pool_[i];
      download_pool_.erase(download_pool_.begin() + i);
      break;
    }
    if (!w.tb) w.tb = NewDownload(w.cap);
    SDL_GPUCopyPass* down = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUBufferRegion src{crw_, 0, Uint32(rw.size())};
    SDL_GPUTransferBufferLocation dst{w.tb, 0};
    SDL_DownloadFromGPUBuffer(down, &src, &dst);
    SDL_EndGPUCopyPass(down);
    w.submission = submitted_ + 1;  // the open command buffer's
    Submit(cmd);
    for (uint32_t b = 0; b < st.rw_buffers; b++)
      if (d.rw_buffers[b].data && (!d.rw_buffers[b].buffer || d.rw_buffers[b].land))
        w.out.push_back({d.rw_buffers[b].data, rw_at[b], d.rw_buffers[b].size});
    w.later = d.land_later;
    writes_queued_ += w.later;
    landings_.push_back(std::move(w));
    if (!d.land_later) Land(landings_.size(), true);
  }

  uint64_t WritesQueued() override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    return writes_queued_;
  }

  uint64_t LandWrites(bool wait) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    Land(landings_.size(), wait);
    return writes_landed_;
  }

  // Writes back the first n landings_ (or those of them the GPU has run).
  void Land(size_t n, bool wait) {
    for (; n && !landings_.empty(); n--) {
      Landing& w = landings_.front();
      while (retired_ < w.submission) {
        if (submitted_ < w.submission) SubmitOpen();  // it is still recording
        if (!wait && !in_flight_.empty() && !SDL_QueryGPUFence(dev_, in_flight_.front().fence))
          return;
        Retire();
      }
      const auto* p = static_cast<const uint8_t*>(SDL_MapGPUTransferBuffer(dev_, w.tb, false));
      for (const Landing::Out& o : w.out) std::memcpy(o.data, p + o.at, o.size);
      SDL_UnmapGPUTransferBuffer(dev_, w.tb);
      writes_landed_ += w.later;
      w.out.clear();
      download_pool_.push_back(std::move(w));
      landings_.pop_front();
    }
  }

  // ---- occlusion queries (see occl_buf_). Metal only: SDL_GPU gives
  // graphics shaders read-only storage buffers; Metal lets them write
  // (Vulkan would need fragmentStoresAndAtomics, which SDL may not enable).
  bool SupportsInstancing() override { return true; }
  bool SupportsOcclusion() override {
#if defined(__APPLE__)
    return true;
#else
    return false;
#endif
  }

  uint32_t BeginOcclusion() override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    if (!SupportsOcclusion()) return 0;
    if (!occl_buf_) {
      SDL_GPUBufferCreateInfo bi{};
      bi.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
      bi.size = kOcclSlots * 4;
      occl_buf_ = SDL_CreateGPUBuffer(dev_, &bi);
      SDL_GPUTransferBufferCreateInfo ti{};
      ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
      ti.size = 4;
      occl_zeros_ = SDL_CreateGPUTransferBuffer(dev_, &ti);
      if (!occl_buf_ || !occl_zeros_) FatalF("gfx: occlusion counters: %s", SDL_GetError());
      std::memset(SDL_MapGPUTransferBuffer(dev_, occl_zeros_, false), 0, 4);
      SDL_UnmapGPUTransferBuffer(dev_, occl_zeros_);
    }
    if (occl_open_) EndOcclusion(occl_open_);
    uint32_t id = occl_next_id_++;
    if (!id) id = occl_next_id_++;  // never 0
    const uint32_t slot = occl_next_slot_++ % kOcclSlots;
    occl_[id] = {slot, 0};
    occl_zero_.push_back(slot);
    occl_open_ = id;
    return id;
  }

  void EndOcclusion(uint32_t id) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto it = occl_.find(id);
    if (it == occl_.end() || it->second.end_flush) return;
    it->second.end_flush = flushes_ + 1;  // the next flush draws what it counted
    if (occl_open_ == id) occl_open_ = 0;
  }

  bool OcclusionResult(uint32_t id, uint64_t* count, bool wait) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    *count = 0;
    auto it = occl_.find(id);
    if (it == occl_.end()) return true;
    if (!it->second.end_flush) EndOcclusion(id);
    const OcclQuery q = it->second;
    // Its draws are recorded but not flushed: flush them (D3D9's
    // GetData flushes too), so that the GPU starts on them.
    if (q.end_flush > flushes_) Flush();
    const OcclRead* r = nullptr;
    for (const OcclRead& o : occl_reads_)
      if (o.flush >= q.end_flush) {
        r = &o;
        break;
      }
    if (!r) return true;  // nothing was flushed for it (cannot happen): 0
    if (r->submission > submitted_) {  // still in the open command buffer
      if (!wait) {
        SubmitOpen();  // started, not waited for
        return false;
      }
      SubmitOpen();
    }
    while (!in_flight_.empty() && in_flight_.front().submission <= r->submission &&
           (wait || SDL_QueryGPUFence(dev_, in_flight_.front().fence)))
      Retire();
    if (r->submission > retired_) return false;
    const auto* counts = static_cast<const uint32_t*>(SDL_MapGPUTransferBuffer(dev_, r->tb, false));
    *count = counts ? counts[q.slot] : 0;
    SDL_UnmapGPUTransferBuffer(dev_, r->tb);
    occl_.erase(it);
    // Reads no query still needs (all before the oldest one waiting's
    // flush, keeping the newest) go back to the pool.
    uint64_t oldest = ~0ull;
    for (const auto& [qid, oq] : occl_) oldest = std::min(oldest, oq.end_flush ? oq.end_flush : ~0ull);
    while (occl_reads_.size() > 1 && occl_reads_.front().flush < oldest &&
           occl_reads_.front().submission <= retired_) {
      occl_free_.push_back(occl_reads_.front().tb);
      occl_reads_.pop_front();
    }
    return true;
  }

  // A download transfer buffer for the counters, from the pool.
  SDL_GPUTransferBuffer* OcclTransfer() {
    if (!occl_free_.empty()) {
      SDL_GPUTransferBuffer* tb = occl_free_.back();
      occl_free_.pop_back();
      return tb;
    }
    SDL_GPUTransferBufferCreateInfo ti{};
    ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    ti.size = kOcclSlots * 4;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev_, &ti);
    if (!tb) FatalF("gfx: occlusion read-back: %s", SDL_GetError());
    return tb;
  }

  void SetGammaRamp(const uint16_t ramp[3 * 256]) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    bool identity = true;
    for (int i = 0; i < 3 * 256; i++) identity = identity && ramp[i] / 257 == i % 256;
    gamma_on_ = !identity;
    NoteShownGamma(ramp);
    // As a 256 x 1 RGBA16 texture: texel i holds entry i of each channel.
    for (int i = 0; i < 256; i++) {
      gamma_lut_[i * 4 + 0] = ramp[i];
      gamma_lut_[i * 4 + 1] = ramp[256 + i];
      gamma_lut_[i * 4 + 2] = ramp[512 + i];
      gamma_lut_[i * 4 + 3] = 65535;
    }
    gamma_dirty_ = true;
  }

  void DrawImage(TextureId id, const Rect& r) override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto it = textures_.find(id);
    if (it == textures_.end()) return;
    const Tex& t = it->second;
    Flush();
    if (!present_pipeline_) CreatePresentPipeline();
    SDL_GPUCommandBuffer* cmd = Begin();
    SDL_GPUColorTargetInfo ct{};
    ct.texture = backbuffer_;
    ct.load_op = SDL_GPU_LOADOP_CLEAR;
    ct.clear_color = {0, 0, 0, 1};
    ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(rp, present_pipeline_);
    SDL_GPUTextureSamplerBinding tb{t.tex, Sampler(SamplerKey{})};
    SDL_BindGPUFragmentSamplers(rp, 0, &tb, 1);
    // The region's corner, its far corner, and whether to encode as sRGB.
    const int32_t u[4] = {r.left, r.top, std::min(r.right, t.w) - 1,
                          t.fmt == Format::R8G8B8A8Srgb || t.fmt == Format::B8G8R8A8Srgb ||
                                  t.fmt == Format::Bc1Srgb || t.fmt == Format::Bc2Srgb ||
                                  t.fmt == Format::Bc3Srgb || t.fmt == Format::Bc7Srgb
                              ? 1
                              : 0};
    const int32_t bottom = std::min(r.bottom, t.h) - 1;
    const int32_t ub[8] = {u[0], u[1], u[2], bottom, u[3], 0, 0, 0};
    SDL_PushGPUFragmentUniformData(cmd, 0, ub, sizeof ub);
    SDL_GPUViewport vp{0, 0, float(bb_w_), float(bb_h_), 0, 1};
    SDL_SetGPUViewport(rp, &vp);
    SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
    SDL_EndGPURenderPass(rp);
    Submit(cmd);
  }

  void Present() override {
    std::lock_guard<std::recursive_mutex> l(mu_);
    Flush();
    SDL_GPUCommandBuffer* cmd = Begin();
    if (Labels()) SDL_PushGPUDebugGroup(cmd, "pass present-copy");
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureLocation s{}, d{};
    s.texture = backbuffer_;
    d.texture = front_;
    SDL_CopyGPUTextureToTexture(cp, &s, &d, Uint32(bb_w_), Uint32(bb_h_), 1, false);
    SDL_EndGPUCopyPass(cp);
    if (Labels()) SDL_PopGPUDebugGroup(cmd);
    if (Labels()) SDL_PushGPUDebugGroup(cmd, "pass show");
    Show(cmd);
    if (Labels()) SDL_PopGPUDebugGroup(cmd);
    SubmitOpen();
    PersistentFrame();
  }

 private:
  struct Tex {
    SDL_GPUTexture* tex = nullptr;
    int w = 0, h = 0, levels = 1, layers = 1;
    Format fmt = Format::Unknown;
    bool rt = false;
    bool cube = false;
    TextureKind kind = TextureKind::Tex2D;
    bool storage = false;  // a compute stage's read-write image
    // The lowest texel alpha written to it (AlphaLowerBound): tracked
    // for 2D textures written only from the CPU (Update, FillTexture),
    // and known once every level has been written whole.
    bool alpha_tracked = false;
    uint8_t alpha_min = 255;
    uint32_t alpha_levels = 0;  // bit per level written whole
  };
  struct Depth {
    SDL_GPUTexture* tex = nullptr;
    int w = 0, h = 0, layers = 1;
    Format fmt = Format::Unknown;
    SDL_GPUTextureFormat gpu_format = SDL_GPU_TEXTUREFORMAT_INVALID;
    bool stencil = false;
    float bias_scale = 8388608.0f;  // float depth: 2^23 per unit of depth range
  };
  struct Upload {
    SDL_GPUTexture* tex;
    int layer;
    int level;
    uint32_t x, y, w, h;
    uint32_t offset, pixels_per_row, rows;
    bool cycle;
    uint32_t z = 0;  // a 3D image's slice
  };
  struct Cmd {
    PipelineKey key;
    int vs = 0, fs = 0, vl = 0;  // vs_list_, fs_list_, vl_list_
    SDL_GPUTexture* tex[8] = {};
    bool cube[8] = {};  // tex[i] is a cube texture (ff.frag's cube samplers)
    SDL_GPUSampler* smp[8] = {};
    SDL_GPUViewport viewport{};
    SDL_Rect scissor{};
    uint8_t stencil_ref = 0;
    bool indexed = false;
    uint32_t first = 0, count = 0;
    int32_t base = 0;
    uint32_t instances = 1;  // a DrawShader's
    SDL_GPUBuffer* indirect = nullptr;  // a GPU buffer's draw (ProgramDraw)
    SDL_GPUBuffer* indices = nullptr;   // a GPU buffer's indices (ProgramDraw)
    uint32_t buffer_at = 0;             // that buffer's place in it
    bool short_indices = false;         // its indices are 16-bit
    int shader = -1;   // shader_cmds_ index for a DrawShader
    int program = -1;  // program_cmds_ index for a DrawProgram
    bool pv = false;   // a fixed-function draw whose vertices are all in pvbuf_
    bool pv_indices = false;  // a shader draw whose indices are in pvbuf_ (PlaceIndices)
  };
  // What a DrawShader adds to its Cmd: samplers (vertex 0-3, pixel 0-15)
  // and the uniform blocks in ubytes_ (vertex then pixel: float
  // registers, int and bool registers, the prelude's state).
  struct ShaderCmd {
    SDL_GPUTexture* tex[20] = {};
    SDL_GPUSampler* smp[20] = {};
    uint32_t nvs = 0, nps = 0;  // samplers bound per stage
    uint32_t uoff[6] = {}, ulen[6] = {};
    SDL_FColor blend_constant{};
    ShaderKey key{};
    bool occl = false;  // counts for an occlusion query (binds occl_buf_)
  };
  // A shader's compile: run once, by a pool thread or by Gpu.
  struct CompileJob {
    std::atomic<bool> claimed{false};
    std::packaged_task<SDL_GPUShader*()> task;
  };
  struct Shader {
    SDL_GPUShader* sh = nullptr;
    hlsl::Stage stage = hlsl::Stage::Vertex;
    size_t inputs = 0;
    std::array<hlsl::ParamType, 16> sampler_type{};  // per register; Void if unused
    uint32_t samplers = 0;  // highest register used + 1
    uint32_t float_regs = 0;
    std::string glsl;    // a pixel shader's translation (for its variants)
    std::map<uint32_t, ShaderId> variants;  // Variant's, by shadow << 1 | occl
    std::shared_future<SDL_GPUShader*> pending;  // sh while compiling (Gpu)
    std::shared_ptr<CompileJob> job;             // its compile, until done
  };

  // A shader's GPU shader, waiting for its compilation if still pending.
  // One not started yet is compiled here and now (taken off the pool's
  // queue): a draw waits for its own shaders only.
  static SDL_GPUShader* Gpu(Shader& s) {
    if (!s.sh && s.pending.valid()) {
      const Uint64 t0 = SDL_GetTicksNS();
      bool here = s.job && !s.job->claimed.exchange(true);
      if (here) s.job->task();
      s.sh = s.pending.get();
      s.job.reset();
      AddStall(here ? Stall::ShaderCompile : Stall::ShaderWait, SDL_GetTicksNS() - t0);
    }
    return s.sh;
  }

  // Dev aid: <ID>_SHADER_DUMP=DIR writes each translated shader's GLSL
  // there, as vs_NNNNN.glsl / ps_NNNNN.glsl in creation order.
  void DumpShader(hlsl::Stage stage, uint32_t samplers, const std::string& body) {
    const char* dir = Env("SHADER_DUMP");
    if (!dir) return;
    static int n = 0;
    const bool vertex = stage == hlsl::Stage::Vertex;
    const std::string glsl = "#version 450\n" + Prelude(stage, samplers) + body;
    char path[1024];
    std::snprintf(path, sizeof path, "%s/%s_%05d.glsl", dir, vertex ? "vs" : "ps", n++);
    if (std::FILE* f = std::fopen(path, "w")) {
      std::fwrite(glsl.data(), 1, glsl.size(), f);
      std::fclose(f);
    }
  }

  // CompileShader on the compile pool (whoever claims the job runs it):
  // the result once done.
  std::shared_future<SDL_GPUShader*> CompileAsync(Shader& sh, const std::string& body) {
    const hlsl::Stage stage = sh.stage;
    const uint32_t samplers = sh.samplers;
    auto job = std::make_shared<CompileJob>();
    job->task = std::packaged_task<SDL_GPUShader*()>(
        [this, stage, samplers, body] { return CompileShader(stage, samplers, body, false, 0); });
    std::shared_future<SDL_GPUShader*> f = job->task.get_future().share();
    sh.job = job;
    {
      std::lock_guard<std::mutex> l(compile_mu_);
      if (compile_threads_.empty()) {
        const unsigned hw = std::thread::hardware_concurrency();
        const unsigned n = std::clamp(hw > 2 ? hw - 2 : 1u, 1u, 8u);
        for (unsigned i = 0; i < n; i++) compile_threads_.emplace_back([this] { CompileWorker(); });
      }
      compile_jobs_.push_back(job);
    }
    compile_cv_.notify_one();
    return f;
  }

  void CompileWorker() {
    for (;;) {
      std::shared_ptr<CompileJob> job;
      {
        std::unique_lock<std::mutex> l(compile_mu_);
        compile_cv_.wait(l, [&] { return compile_quit_ || !compile_jobs_.empty(); });
        if (compile_jobs_.empty()) return;  // quitting, nothing left
        job = std::move(compile_jobs_.front());
        compile_jobs_.pop_front();
      }
      if (!job->claimed.exchange(true)) job->task();
    }
  }
  struct BlitTex {
    SDL_GPUTexture* tex;
    int w, h;
  };
  // A readback in flight: the copy of `c` (`r` clipped to the texture)
  // into `tb` (download_tb_ unless `own`), done when `fence` signals.
  struct Read {
    Rect r{}, c{};
    SDL_GPUTextureFormat fmt = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;  // the target's
    SDL_GPUTransferBuffer* tb = nullptr;
    SDL_GPUFence* fence = nullptr;
    bool own = false;
  };

  // ---- render targets and passes

  SDL_GPUTexture* CreateColor(int w, int h, int levels) {
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = Uint32(w);
    ti.height = Uint32(h);
    ti.layer_count_or_depth = 1;
    ti.num_levels = Uint32(levels);
    SDL_GPUTexture* t = SDL_CreateGPUTexture(dev_, &ti);
    if (!t) FatalF("gfx: cannot create a %dx%d render target: %s", w, h, SDL_GetError());
    return t;
  }

  bool SupportsDepth(SDL_GPUTextureFormat f) {
    return SDL_GPUTextureSupportsFormat(dev_, f, SDL_GPU_TEXTURETYPE_2D,
                                        SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
  }

  SDL_GPUTextureFormat GpuFormat(Format f) const {
    switch (f) {
      case Format::DXT1: return SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM;
      case Format::DXT2: case Format::DXT3: return SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM;
      case Format::DXT4: case Format::DXT5: return SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM;
      case Format::R16F: return SDL_GPU_TEXTUREFORMAT_R16_FLOAT;
      case Format::G16R16F: return SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT;
      case Format::A16B16G16R16F: return SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
      case Format::R32F: return SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
      case Format::G32R32F: return SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT;
      case Format::A32B32G32R32F: return SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
      case Format::R8: return SDL_GPU_TEXTUREFORMAT_R8_UNORM;
      case Format::R8G8: return SDL_GPU_TEXTUREFORMAT_R8G8_UNORM;
      case Format::R8G8B8A8: return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
      case Format::R8G8B8A8Srgb: return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
      case Format::B8G8R8A8Srgb: return SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
      case Format::R16: return SDL_GPU_TEXTUREFORMAT_R16_UNORM;
      case Format::R16G16: return SDL_GPU_TEXTUREFORMAT_R16G16_UNORM;
      case Format::R16G16B16A16: return SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UNORM;
      case Format::R10G10B10A2: return SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM;
      case Format::R11G11B10F: return SDL_GPU_TEXTUREFORMAT_R11G11B10_UFLOAT;
      case Format::R8G8B8A8Snorm: return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_SNORM;
      case Format::R16G16Snorm: return SDL_GPU_TEXTUREFORMAT_R16G16_SNORM;
      case Format::R16G16B16A16Snorm: return SDL_GPU_TEXTUREFORMAT_R16G16B16A16_SNORM;
      case Format::R8Uint: return SDL_GPU_TEXTUREFORMAT_R8_UINT;
      case Format::R8G8Uint: return SDL_GPU_TEXTUREFORMAT_R8G8_UINT;
      case Format::R8G8B8A8Uint: return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UINT;
      case Format::R16Uint: return SDL_GPU_TEXTUREFORMAT_R16_UINT;
      case Format::R16G16Uint: return SDL_GPU_TEXTUREFORMAT_R16G16_UINT;
      case Format::R16G16B16A16Uint: return SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UINT;
      case Format::R32Uint: return SDL_GPU_TEXTUREFORMAT_R32_UINT;
      case Format::R32G32Uint: return SDL_GPU_TEXTUREFORMAT_R32G32_UINT;
      case Format::R32G32B32A32Uint: return SDL_GPU_TEXTUREFORMAT_R32G32B32A32_UINT;
      case Format::R32Sint: return SDL_GPU_TEXTUREFORMAT_R32_INT;
      case Format::R32G32Sint: return SDL_GPU_TEXTUREFORMAT_R32G32_INT;
      case Format::R32G32B32A32Sint: return SDL_GPU_TEXTUREFORMAT_R32G32B32A32_INT;
      case Format::Bc1: return SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM;
      case Format::Bc1Srgb: return SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM_SRGB;
      case Format::Bc2: return SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM;
      case Format::Bc2Srgb: return SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM_SRGB;
      case Format::Bc3: return SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM;
      case Format::Bc3Srgb: return SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM_SRGB;
      case Format::Bc4: return SDL_GPU_TEXTUREFORMAT_BC4_R_UNORM;
      case Format::Bc5: return SDL_GPU_TEXTUREFORMAT_BC5_RG_UNORM;
      case Format::Bc6hUfloat: return SDL_GPU_TEXTUREFORMAT_BC6H_RGB_UFLOAT;
      case Format::Bc6hSfloat: return SDL_GPU_TEXTUREFORMAT_BC6H_RGB_FLOAT;
      case Format::Bc7: return SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM;
      case Format::Bc7Srgb: return SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM_SRGB;
      default: return SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    }
  }

  static int LevelSize(int size, int level) { return std::max(1, size >> level); }

  // The pass for the D3D render targets (current_color_ at current_level_,
  // current_extra_, current_depth_).
  void BindTarget() {
    SDL_GPUTexture* color = backbuffer_;
    int w = bb_w_, h = bb_h_;
    bool alpha = false;
    std::array<SDL_GPUTexture*, 4> targets{};
    std::array<uint8_t, 4> fmts{};
    std::array<int, 4> levels{};
    fmts[0] = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    if (current_color_) {
      auto it = textures_.find(current_color_);
      if (it != textures_.end() && it->second.rt && current_level_ < it->second.levels) {
        color = it->second.tex;
        levels[0] = current_level_;
        w = LevelSize(it->second.w, current_level_);
        h = LevelSize(it->second.h, current_level_);
        alpha = HasAlpha(it->second.fmt);
        fmts[0] = uint8_t(GpuFormat(it->second.fmt));
      }
    }
    targets[0] = color;
    for (int i = 0; i < 3 && targets[i]; i++) {
      const ColorTarget& e = current_extra_[i];
      auto it = textures_.find(e.tex);
      if (it == textures_.end() || !it->second.rt || e.level >= it->second.levels) break;
      targets[i + 1] = it->second.tex;
      levels[i + 1] = e.level;
      fmts[i + 1] = uint8_t(GpuFormat(it->second.fmt));
    }
    const Depth* d = nullptr;
    auto dit = depths_.find(current_depth_);
    if (dit != depths_.end()) d = &dit->second;
    SetPass(targets, fmts, d, w, h, alpha, levels);
  }

  // A pass with the back buffer's format (back and front buffer).
  void SetPass(SDL_GPUTexture* color, const Depth* d) {
    std::array<uint8_t, 4> fmts{};
    fmts[0] = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    SetPass({color, nullptr, nullptr, nullptr}, fmts, d, bb_w_, bb_h_, false);
  }

  void SetPass(const std::array<SDL_GPUTexture*, 4>& colors, const std::array<uint8_t, 4>& fmts,
               const Depth* d, int w, int h, bool alpha, const std::array<int, 4>& levels = {},
               int depth_layer = 0) {
    SDL_GPUTexture* depth = d ? d->tex : nullptr;
    if (colors == pass_colors_ && levels == pass_levels_ && depth == pass_depth_ &&
        depth_layer == pass_depth_layer_)
      return;
    Flush();
    pass_colors_ = colors;
    pass_levels_ = levels;
    pass_depth_layer_ = depth_layer;
    pass_fmts_ = fmts;
    pass_color_ = colors[0];
    pass_targets_ = 0;
    while (pass_targets_ < 4 && colors[pass_targets_]) pass_targets_++;
    pass_depth_ = depth;
    pass_w_ = w;
    pass_h_ = h;
    pass_alpha_ = alpha;
    pass_stencil_ = d && d->stencil;
    pass_depth_format_ = d ? d->gpu_format : SDL_GPU_TEXTUREFORMAT_INVALID;
    pass_bias_scale_ = d ? d->bias_scale : 1.0f;
  }

  // Whether recorded draws or queued uploads use `t`.
  bool Pending(SDL_GPUTexture* t) const {
    if (!cmds_.empty() || pass_clear_color_ || pass_clear_depth_ || pass_clear_stencil_)
      for (SDL_GPUTexture* x : pass_colors_)
        if (x && x == t) return true;
    for (const Upload& u : uploads_)
      if (u.tex == t) return true;
    for (const Cmd& c : cmds_)
      for (SDL_GPUTexture* x : c.tex)
        if (x == t) return true;
    for (const ShaderCmd& c : shader_cmds_)
      for (SDL_GPUTexture* x : c.tex)
        if (x == t) return true;
    for (const ProgramCmd& c : program_cmds_)
      for (const ProgramStageCmd& sc : c.stage)
        for (SDL_GPUTexture* x : sc.tex)
          if (x == t) return true;
    return false;
  }

  // ---- uploads

  // Queues `rows` rows of `row_bytes` (pixel rows, or rows of 4x4 blocks
  // if `compressed`) for the texels at x, y, w, h of `level`.
  void QueueUpload(SDL_GPUTexture* tex, int level, int x, int y, int w, int h, const void* src,
                   uint32_t src_pitch, uint32_t rows, uint32_t row_bytes, bool cycle = false,
                   bool compressed = false, int layer = 0) {
    size_t off = (upload_bytes_.size() + 15) & ~size_t(15);
    upload_bytes_.resize(off + size_t(rows) * row_bytes);
    for (uint32_t r = 0; r < rows; r++)
      std::memcpy(upload_bytes_.data() + off + size_t(r) * row_bytes,
                  static_cast<const uint8_t*>(src) + size_t(r) * src_pitch, row_bytes);
    Upload u{tex, layer, level, uint32_t(x), uint32_t(y), uint32_t(w), uint32_t(h),
             uint32_t(off), uint32_t(w), uint32_t(h), cycle};
    if (compressed) {
      u.pixels_per_row = (uint32_t(w) + 3) & ~3u;
      u.rows = (uint32_t(h) + 3) & ~3u;
    }
    uploads_.push_back(u);
  }

  uint32_t AppendVertices(const void* v, uint32_t n, uint32_t stride) {
    uint32_t first;
    std::memcpy(ReserveVertices(n, stride, &first), v, size_t(n) * stride);
    return first;
  }

  // Room for n vertices in the arena, the first of them vertex *first.
  uint8_t* ReserveVertices(uint32_t n, uint32_t stride, uint32_t* first) {
    size_t pos = (verts_.size() + stride - 1) / stride * stride;
    verts_.resize(pos + size_t(n) * stride);
    *first = uint32_t(pos / stride);
    return verts_.data() + pos;
  }

  // Room for `bytes` in the vertex arena, 4-byte aligned: its offset.
  size_t ReserveBytes(size_t bytes) {
    size_t pos = (verts_.size() + 3) & ~size_t(3);
    verts_.resize(pos + bytes);
    return pos;
  }

  SDL_GPUBuffer* EnsureBuffer(SDL_GPUBuffer*& b, uint32_t& cap, size_t need,
                              SDL_GPUBufferUsageFlags usage) {
    if (b && cap >= need) return b;
    if (b) SDL_ReleaseGPUBuffer(dev_, b);
    cap = std::max<uint32_t>(cap ? cap : 1u << 20, 1u << 20);
    while (cap < need) cap *= 2;
    SDL_GPUBufferCreateInfo bi{};
    bi.usage = usage;
    bi.size = cap;
    b = SDL_CreateGPUBuffer(dev_, &bi);
    if (!b) FatalF("gfx: cannot create a %u-byte buffer: %s", cap, SDL_GetError());
    return b;
  }

  // ---- flush: one copy pass, then one render pass for the target

  // Room for `bytes` in upload_tb_, a ring: its offset is upload_at_.
  // Submissions take its space in order and each one's end is kept with
  // its fence (Submit), so writing waits only for the submissions whose
  // uploads the new ones would overwrite. The ring grows (once everything
  // is finished) when one upload needs more than half of it.
  uint8_t* MapUpload(size_t bytes) {
    bytes = (bytes + 255) & ~size_t(255);
    if (!upload_tb_ || upload_tb_cap_ < 2 * bytes) {
      Drain();
      if (upload_tb_) SDL_ReleaseGPUTransferBuffer(dev_, upload_tb_);
      upload_tb_cap_ = std::max<uint32_t>(upload_tb_cap_, 32u << 20);
      while (upload_tb_cap_ < 2 * bytes) upload_tb_cap_ *= 2;
      SDL_GPUTransferBufferCreateInfo tbi{};
      tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
      tbi.size = upload_tb_cap_;
      upload_tb_ = SDL_CreateGPUTransferBuffer(dev_, &tbi);
      if (!upload_tb_) FatalF("gfx: transfer buffer: %s", SDL_GetError());
      upload_head_ = upload_tail_ = 0;
    }
    if (upload_head_ % upload_tb_cap_ + bytes > upload_tb_cap_)
      upload_head_ += upload_tb_cap_ - upload_head_ % upload_tb_cap_;
    while (upload_head_ + bytes - upload_tail_ > upload_tb_cap_) Retire();
    upload_at_ = Uint32(upload_head_ % upload_tb_cap_);
    upload_head_ += bytes;
    auto* p = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(dev_, upload_tb_, false));
    return p + upload_at_;
  }

  // Waits for the oldest unfinished submission (submitting the open
  // command buffer if it is the only one); its uploads' room in the ring
  // is free again.
  void Retire() {
    if (in_flight_.empty() && open_) SubmitOpen();
    if (in_flight_.empty()) {
      upload_tail_ = upload_head_;
      retired_ = submitted_;
      return;
    }
    SDL_WaitForGPUFences(dev_, true, &in_flight_.front().fence, 1);
    SDL_ReleaseGPUFence(dev_, in_flight_.front().fence);
    upload_tail_ = in_flight_.front().upload_end;
    retired_ = in_flight_.front().submission;
    in_flight_.pop_front();
  }

  void Drain() {
    SubmitOpen();
    while (!in_flight_.empty()) Retire();
    upload_tail_ = upload_head_;
    retired_ = submitted_;
  }

  // The command buffer work is recorded into, until it is submitted: one
  // for many flushes and dispatches, since each submission costs the
  // driver far more than the passes in it (Submit).
  SDL_GPUCommandBuffer* Begin() {
    if (!open_) {
      open_ = SDL_AcquireGPUCommandBuffer(dev_);
      if (!open_) FatalF("gfx: SDL_AcquireGPUCommandBuffer: %s", SDL_GetError());
    }
    return open_;
  }

  // The end of a unit of work recorded into Begin()'s command buffer (a
  // flush, a dispatch, a copy): every kBatch of them it is submitted.
  // Waits for the GPU (Retire, Drain, Land) submit it first.
  void Submit(SDL_GPUCommandBuffer* cmd) {
    if (cmd == open_ && ++open_units_ >= kBatch) SubmitOpen();
  }

  // Submits the open command buffer, keeping at most kMaxInFlight
  // submissions unfinished: resources uploaded cycling (vertex, index and
  // arena buffers, textures) get a fresh copy from SDL per submission
  // still using the last one.
  void SubmitOpen() {
    if (!open_) return;
    SDL_GPUFence* f = SDL_SubmitGPUCommandBufferAndAcquireFence(open_);
    open_ = nullptr;
    open_units_ = 0;
    if (!f) FatalF("gfx: SDL_SubmitGPUCommandBuffer: %s", SDL_GetError());
    in_flight_.push_back({f, upload_head_, ++submitted_});
    while (!in_flight_.empty() &&
           (in_flight_.size() > kMaxInFlight || SDL_QueryGPUFence(dev_, in_flight_.front().fence)))
      Retire();
  }

  static bool Labels() {
    static const bool on = [] { const char* e = Env("GFX_LABELS"); return e && *e && *e != '0'; }();
    return on;
  }

  void Flush() {
    bool draw = !cmds_.empty() || pass_clear_color_ || pass_clear_depth_ || pass_clear_stencil_;
    if (!draw && uploads_.empty() && occl_zero_.empty()) return;
    const bool occl = !occl_zero_.empty() || occl_draws_;
    SDL_GPUCommandBuffer* cmd = nullptr;
    size_t vbytes = verts_.size(), ibytes = indices_.size() * 4, pbytes = pmem_.size();
    size_t ubytes = (upload_bytes_.size() + 15) & ~size_t(15);
    size_t pvbytes = pv_staging_.size();
    size_t total = ubytes + vbytes + ibytes + pbytes + pvbytes;
    if (total) {
      auto* p = MapUpload(total);
      if (!upload_bytes_.empty()) std::memcpy(p, upload_bytes_.data(), upload_bytes_.size());
      if (vbytes) std::memcpy(p + ubytes, verts_.data(), vbytes);
      if (ibytes) std::memcpy(p + ubytes + vbytes, indices_.data(), ibytes);
      if (pbytes) std::memcpy(p + ubytes + vbytes + ibytes, pmem_.data(), pbytes);
      if (pvbytes) std::memcpy(p + ubytes + vbytes + ibytes + pbytes, pv_staging_.data(), pvbytes);
      SDL_UnmapGPUTransferBuffer(dev_, upload_tb_);

      cmd = Begin();  // after MapUpload, which may submit it
      if (Labels()) SDL_PushGPUDebugGroup(cmd, "pass uploads");
      SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
      for (const Upload& u : uploads_) {
        SDL_GPUTextureTransferInfo src{};
        src.transfer_buffer = upload_tb_;
        src.offset = upload_at_ + u.offset;
        src.pixels_per_row = u.pixels_per_row;
        src.rows_per_layer = u.rows;
        SDL_GPUTextureRegion dst{};
        dst.texture = u.tex;
        dst.layer = Uint32(u.layer);
        dst.mip_level = Uint32(u.level);
        dst.x = u.x;
        dst.y = u.y;
        dst.z = u.z;
        dst.w = u.w;
        dst.h = u.h;
        dst.d = 1;
        SDL_UploadToGPUTexture(cp, &src, &dst, u.cycle);
      }
      if (vbytes) {
        EnsureBuffer(vbuf_, vbuf_cap_, vbytes, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ);
        SDL_GPUTransferBufferLocation src{upload_tb_, Uint32(upload_at_ + ubytes)};
        SDL_GPUBufferRegion dst{vbuf_, 0, Uint32(vbytes)};
        SDL_UploadToGPUBuffer(cp, &src, &dst, true);
      }
      if (ibytes) {
        EnsureBuffer(ibuf_, ibuf_cap_, ibytes, SDL_GPU_BUFFERUSAGE_INDEX);
        SDL_GPUTransferBufferLocation src{upload_tb_, Uint32(upload_at_ + ubytes + vbytes)};
        SDL_GPUBufferRegion dst{ibuf_, 0, Uint32(ibytes)};
        SDL_UploadToGPUBuffer(cp, &src, &dst, true);
      }
      if (pbytes) {
        EnsureBuffer(pbuf_, pbuf_cap_, pbytes, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ);
        SDL_GPUTransferBufferLocation src{upload_tb_, Uint32(upload_at_ + ubytes + vbytes + ibytes)};
        SDL_GPUBufferRegion dst{pbuf_, 0, Uint32(pbytes)};
        SDL_UploadToGPUBuffer(cp, &src, &dst, true);
      }
      if (pvbytes) {
        if (!pvbuf_) {
          uint32_t cap = 0;
          EnsureBuffer(pvbuf_, cap, kPvCap,
                       SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ | SDL_GPU_BUFFERUSAGE_INDEX);
        }
        const size_t base = upload_at_ + ubytes + vbytes + ibytes + pbytes;
        for (const PvUpload& u : pv_uploads_) {
          SDL_GPUTransferBufferLocation src{upload_tb_, Uint32(base + u.from)};
          SDL_GPUBufferRegion dst{pvbuf_, u.at, u.size};
          SDL_UploadToGPUBuffer(cp, &src, &dst, false);
        }
        pv_staging_.clear();
        pv_uploads_.clear();
      }
      SDL_EndGPUCopyPass(cp);
      if (Labels()) SDL_PopGPUDebugGroup(cmd);
    }

    if (!cmd) cmd = Begin();
    if (!occl_zero_.empty()) {  // the queries begun since: their counters from 0
      SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
      for (uint32_t slot : occl_zero_) {
        SDL_GPUTransferBufferLocation src{occl_zeros_, 0};
        SDL_GPUBufferRegion dst{occl_buf_, slot * 4, 4};
        SDL_UploadToGPUBuffer(cp, &src, &dst, false);
      }
      SDL_EndGPUCopyPass(cp);
      occl_zero_.clear();
    }
    if (draw && (pass_color_ || pass_depth_)) {
      // A clear clears every target, as D3D9's does.
      SDL_GPUColorTargetInfo cts[4]{};
      for (int i = 0; i < pass_targets_; i++) {
        SDL_GPUColorTargetInfo& ct = cts[i];
        ct.texture = pass_colors_[i];
        ct.mip_level = Uint32(pass_levels_[i]);
        ct.load_op = pass_clear_color_ ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
        ct.clear_color = clear_color_;
        ct.store_op = SDL_GPU_STOREOP_STORE;
      }
      SDL_GPUDepthStencilTargetInfo dt{};
      if (pass_depth_) {
        dt.texture = pass_depth_;
        dt.load_op = pass_clear_depth_ ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
        dt.clear_depth = clear_depth_;
        dt.store_op = SDL_GPU_STOREOP_STORE;
        dt.stencil_load_op = pass_clear_stencil_ ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
        dt.stencil_store_op = SDL_GPU_STOREOP_STORE;
        dt.clear_stencil = clear_stencil_;
        dt.layer = Uint8(pass_depth_layer_);
      }
      // GFX_LABELS=1: each pass in a debug group named after its target and
      // draws, which GPU traces show (tools/gpu_passes.py joins them to the
      // GPU's intervals).
      if (Labels()) {
        char pass_name[96];
        std::snprintf(pass_name, sizeof pass_name, "pass %dx%d %s%s draws %zu", pass_w_, pass_h_,
                      pass_color_ == backbuffer_ ? "backbuffer" : pass_color_ ? "rt" : "depth-only",
                      pass_depth_ ? "+depth" : "", cmds_.size());
        SDL_PushGPUDebugGroup(cmd, pass_name);
      }
      SDL_GPURenderPass* rp =
          SDL_BeginGPURenderPass(cmd, cts, Uint32(pass_targets_), pass_depth_ ? &dt : nullptr);
      if (vbuf_) SDL_BindGPUVertexStorageBuffers(rp, 0, &vbuf_, 1);
      SDL_GPUBuffer* bound_ib = nullptr;  // ibuf_, or the heap at a draw's GPU buffer
      uint32_t bound_at = 0;
      bool bound_short = false;
      auto bind_indices = [&](SDL_GPUBuffer* b, uint32_t at = 0, bool short_indices = false) {
        if (b == bound_ib && at == bound_at && short_indices == bound_short) return;
        SDL_GPUBufferBinding ib{b, at};
        SDL_BindGPUIndexBuffer(rp, &ib, short_indices ? SDL_GPU_INDEXELEMENTSIZE_16BIT
                                                       : SDL_GPU_INDEXELEMENTSIZE_32BIT);
        bound_ib = b;
        bound_at = at;
        bound_short = short_indices;
      };
      if (ibuf_ && ibytes) bind_indices(ibuf_);
      SDL_GPUGraphicsPipeline* bound = nullptr;
      bool vertex_storage = vbuf_ != nullptr;  // vertex_bound at slot 0 for the other draws
      SDL_GPUBuffer* vertex_bound = vbuf_;
      int vs = -1, fs = -1, vl = -1;
      uint32_t pushed[6];  // ubytes_ offsets pushed per shader uniform slot
      std::fill(std::begin(pushed), std::end(pushed), ~0u);
      bool have_constant = false;
      SDL_FColor constant{};
      // Viewport, scissor, stencil reference and the fixed-function
      // draws' samplers are set only when they change: SDL encodes every
      // call, and consecutive draws mostly share them.
      bool have_rect = false;
      SDL_GPUViewport viewport{};
      SDL_Rect scissor{};
      uint8_t stencil_ref = 0;
      auto set_rects = [&](const Cmd& c) {
        if (have_rect && !std::memcmp(&viewport, &c.viewport, sizeof viewport) &&
            !std::memcmp(&scissor, &c.scissor, sizeof scissor) && stencil_ref == c.stencil_ref)
          return;
        SDL_SetGPUViewport(rp, &c.viewport);
        SDL_SetGPUScissor(rp, &c.scissor);
        SDL_SetGPUStencilReference(rp, c.stencil_ref);
        viewport = c.viewport;
        scissor = c.scissor;
        stencil_ref = c.stencil_ref;
        have_rect = true;
      };
      SDL_GPUTextureSamplerBinding ff_samplers[16];
      bool have_ff_samplers = false;  // ff_samplers are bound (no other draw's since)
      for (const Cmd& c : cmds_) {
        if (c.program < 0) {
          SDL_GPUBuffer* want = c.pv ? pvbuf_ : vbuf_;
          if (want && (!vertex_storage || vertex_bound != want)) {
            SDL_BindGPUVertexStorageBuffers(rp, 0, &want, 1);
            vertex_storage = true;
            vertex_bound = want;
          }
        }
        if (c.program >= 0) {
          const ProgramCmd& pc = program_cmds_[size_t(c.program)];
          if (pc.pipeline != bound) {
            SDL_BindGPUGraphicsPipeline(rp, pc.pipeline);
            bound = pc.pipeline;
          }
          std::fill(std::begin(pushed), std::end(pushed), ~0u);
          vs = fs = vl = -1;
          for (int st = 0; st < 2; st++) {
            const ProgramStageCmd& sc = pc.stage[st];
            for (uint32_t i = 0; i < sc.nuni; i++) {
              const uint8_t* data = ubytes_.data() + sc.uoff[i];
              if (st) SDL_PushGPUFragmentUniformData(cmd, i, data, sc.ulen[i]);
              else SDL_PushGPUVertexUniformData(cmd, i, data, sc.ulen[i]);
            }
            SDL_GPUTextureSamplerBinding tb[16];
            for (uint32_t i = 0; i < sc.ntex; i++) tb[i] = {sc.tex[i], sc.smp[i]};
            if (sc.ntex) {
              if (st) SDL_BindGPUFragmentSamplers(rp, 0, tb, sc.ntex);
              else SDL_BindGPUVertexSamplers(rp, 0, tb, sc.ntex);
              have_ff_samplers = false;
            }
            if (sc.storage) {
              SDL_GPUBuffer* bufs[2] = {pbuf_, heap_};
              if (st) SDL_BindGPUFragmentStorageBuffers(rp, 0, bufs, 2);
              else SDL_BindGPUVertexStorageBuffers(rp, 0, bufs, 2);
              if (!st) vertex_storage = false;
            }
          }
          if (!have_constant || std::memcmp(&constant, &pc.blend_constant, sizeof constant)) {
            SDL_SetGPUBlendConstants(rp, pc.blend_constant);
            constant = pc.blend_constant;
            have_constant = true;
          }
          set_rects(c);
          if (c.indirect) {
            bind_indices(c.indirect, c.buffer_at);
            SDL_DrawGPUIndexedPrimitivesIndirect(rp, c.indirect, c.buffer_at, 1);
          } else if (c.indexed) {
            if (c.indices) bind_indices(c.indices, c.buffer_at, c.short_indices);
            else bind_indices(ibuf_);
            SDL_DrawGPUIndexedPrimitives(rp, c.count, pc.instances, c.first, c.base, 0);
          } else {
            SDL_DrawGPUPrimitives(rp, c.count, pc.instances, c.first, 0);
          }
          continue;
        }
        if (c.shader >= 0) {
          const ShaderCmd& sc = shader_cmds_[size_t(c.shader)];
          SDL_GPUGraphicsPipeline* p = ShaderPipeline(sc.key);
          if (p != bound) {
            SDL_BindGPUGraphicsPipeline(rp, p);
            bound = p;
          }
          for (int i = 0; i < 6; i++) {
            if (pushed[i] == sc.uoff[i]) continue;
            const uint8_t* data = ubytes_.data() + sc.uoff[i];
            if (i < 3) SDL_PushGPUVertexUniformData(cmd, Uint32(i), data, sc.ulen[i]);
            else SDL_PushGPUFragmentUniformData(cmd, Uint32(i - 3), data, sc.ulen[i]);
            pushed[i] = sc.uoff[i];
          }
          vs = fs = vl = -1;  // the fixed-function uniforms' slots were overwritten
          if (sc.occl) SDL_BindGPUFragmentStorageBuffers(rp, 0, &occl_buf_, 1);
          SDL_GPUTextureSamplerBinding tb[20];
          for (uint32_t i = 0; i < 20; i++) tb[i] = {sc.tex[i], sc.smp[i]};
          if (sc.nvs) SDL_BindGPUVertexSamplers(rp, 0, tb + 16, sc.nvs);
          if (sc.nps) SDL_BindGPUFragmentSamplers(rp, 0, tb, sc.nps);
          have_ff_samplers = false;
          if (!have_constant || std::memcmp(&constant, &sc.blend_constant, sizeof constant)) {
            SDL_SetGPUBlendConstants(rp, sc.blend_constant);
            constant = sc.blend_constant;
            have_constant = true;
          }
        } else {
          SDL_GPUGraphicsPipeline* p = FfPipeline(c.key, c.fs);
          if (p != bound) {
            SDL_BindGPUGraphicsPipeline(rp, p);
            bound = p;
          }
          std::fill(std::begin(pushed), std::end(pushed), ~0u);
          if (c.vs != vs) {
            SDL_PushGPUVertexUniformData(cmd, 0, &vs_list_[size_t(c.vs)], sizeof(VsUniforms));
            vs = c.vs;
          }
          if (c.vl != vl) {
            SDL_PushGPUVertexUniformData(cmd, 1, &vl_list_[size_t(c.vl)], sizeof(VsLayout));
            vl = c.vl;
          }
          if (c.fs != fs) {
            SDL_PushGPUFragmentUniformData(cmd, 0, &fs_list_[size_t(c.fs)], sizeof(FsUniforms));
            fs = c.fs;
          }
          // ff.frag's eight 2D samplers, then its eight cube ones: a
          // stage's texture in the one of its type, a dummy in the other.
          SDL_GPUTextureSamplerBinding tb[16];
          for (int i = 0; i < 8; i++) {
            tb[i] = {c.tex[i] && !c.cube[i] ? c.tex[i] : dummy_, c.smp[i]};
            tb[8 + i] = {c.cube[i] ? c.tex[i] : dummy_cube_, c.smp[i]};
          }
          if (!have_ff_samplers || std::memcmp(tb, ff_samplers, sizeof tb)) {
            SDL_BindGPUFragmentSamplers(rp, 0, tb, 16);
            std::memcpy(ff_samplers, tb, sizeof tb);
            have_ff_samplers = true;
          }
        }
        set_rects(c);
        if (c.indexed) {
          bind_indices(c.pv_indices ? pvbuf_ : ibuf_);
          SDL_DrawGPUIndexedPrimitives(rp, c.count, c.instances, c.first, c.base, 0);
        } else {
          SDL_DrawGPUPrimitives(rp, c.count, c.instances, c.first, 0);
        }
      }
      SDL_EndGPURenderPass(rp);
      if (Labels()) SDL_PopGPUDebugGroup(cmd);
    }
    flushes_++;
    if (occl) {  // the counters as this flush leaves them
      OcclRead r{flushes_, submitted_ + 1, OcclTransfer()};
      SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
      SDL_GPUBufferRegion src{occl_buf_, 0, kOcclSlots * 4};
      SDL_GPUTransferBufferLocation dst{r.tb, 0};
      SDL_DownloadFromGPUBuffer(cp, &src, &dst);
      SDL_EndGPUCopyPass(cp);
      occl_reads_.push_back(r);
      occl_draws_ = false;
    }
    Submit(cmd);

    cmds_.clear();
    shader_cmds_.clear();
    program_cmds_.clear();
    pmem_.clear();
    draw_read_.clear();
    ubytes_.clear();
    uploads_.clear();
    upload_bytes_.clear();
    verts_.clear();
    zeros_at_ = -1;
    locked_ = {};
    indices_.clear();
    vs_list_.clear();
    vl_list_.clear();
    fs_list_.clear();
    variant_fs_ = -1;
    blit_used_ = 0;
    pass_clear_color_ = pass_clear_depth_ = pass_clear_stencil_ = false;
  }

  // Front buffer to the window: scaled to fit with its aspect kept,
  // black around it (FitRect).
  void Show(SDL_GPUCommandBuffer* cmd) {
    SDL_GPUTexture* sc = nullptr;
    Uint32 sw = 0, sh = 0;
    if (!window_) return;  // no swap chain
    if (mailbox_emulated_) {
      // One frame per refresh, on a grid of refresh periods: a frame
      // before the next grid point stays in the front buffer unshown, so
      // the vsync queue never fills and acquiring does not wait. If it
      // does wait (the grid drifted ahead of the display), the grid
      // restarts from the moment a drawable came free.
      const uint64_t now = SDL_GetTicksNS();
      if (next_show_ns_ && now < next_show_ns_) return;
      next_show_ns_ = next_show_ns_ && now < next_show_ns_ + refresh_ns_
                          ? next_show_ns_ + refresh_ns_
                          : now + refresh_ns_;
      const bool ok = SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window_, &sc, &sw, &sh);
      const uint64_t after = SDL_GetTicksNS();
      if (after - now > refresh_ns_ / 4) next_show_ns_ = after + refresh_ns_;
      if (!ok || !sc) return;
    } else if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window_, &sc, &sw, &sh) || !sc) {
      return;
    }
    const Rect r = FitRect(bb_w_, bb_h_, int(sw), int(sh));
    const int w = r.right - r.left, h = r.bottom - r.top;
    SDL_GPUBlitInfo b{};
    b.source.texture = front_;
    b.source.w = Uint32(bb_w_);
    b.source.h = Uint32(bb_h_);
    b.destination.texture = sc;
    b.destination.x = Uint32(r.left);
    b.destination.y = Uint32(r.top);
    b.destination.w = Uint32(w);
    b.destination.h = Uint32(h);
    b.load_op = SDL_GPU_LOADOP_DONT_CARE;
    if (w != int(sw) || h != int(sh)) {
      // The bars: clear the whole image (a blit's own clear is only
      // specified for its destination region), then blit over it.
      SDL_GPUColorTargetInfo t{};
      t.texture = sc;
      t.load_op = SDL_GPU_LOADOP_CLEAR;
      t.store_op = SDL_GPU_STOREOP_STORE;
      t.clear_color = {0, 0, 0, 1};
      SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd, &t, 1, nullptr));
      b.load_op = SDL_GPU_LOADOP_LOAD;
    }
    b.filter = (w == bb_w_ && h == bb_h_) ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
    if (gamma_on_) ShowGamma(cmd, sc, r, b.filter, b.load_op);
    else SDL_BlitGPUTexture(cmd, &b);
    NotePresentation({bb_w_, bb_h_, int(sw), int(sh), r});
  }

  // Show's blit through the gamma ramp (SetGammaRamp): the front buffer
  // drawn over `r` of the swap chain image, each channel looked up in the
  // ramp (between its entries, as the 8-bit value lands on one).
  void ShowGamma(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* sc, const Rect& r, SDL_GPUFilter filter,
                 SDL_GPULoadOp load) {
    if (!gamma_tex_) {
      SDL_GPUTextureCreateInfo ti{};
      ti.type = SDL_GPU_TEXTURETYPE_2D;
      ti.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UNORM;
      ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
      ti.width = 256;
      ti.height = 1;
      ti.layer_count_or_depth = 1;
      ti.num_levels = 1;
      gamma_tex_ = SDL_CreateGPUTexture(dev_, &ti);
      SDL_GPUTransferBufferCreateInfo bi{};
      bi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
      bi.size = sizeof gamma_lut_;
      gamma_tb_ = SDL_CreateGPUTransferBuffer(dev_, &bi);
      if (!gamma_tex_ || !gamma_tb_) FatalF("gfx: the gamma ramp: %s", SDL_GetError());
    }
    const SDL_GPUTextureFormat fmt = SDL_GetGPUSwapchainTextureFormat(dev_, window_);
    if (!gamma_pipeline_ || gamma_format_ != fmt) CreateGammaPipeline(fmt);
    if (gamma_dirty_) {
      std::memcpy(SDL_MapGPUTransferBuffer(dev_, gamma_tb_, true), gamma_lut_, sizeof gamma_lut_);
      SDL_UnmapGPUTransferBuffer(dev_, gamma_tb_);
      SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
      SDL_GPUTextureTransferInfo src{gamma_tb_, 0, 256, 1};
      SDL_GPUTextureRegion dst{};
      dst.texture = gamma_tex_;
      dst.w = 256;
      dst.h = 1;
      dst.d = 1;
      SDL_UploadToGPUTexture(cp, &src, &dst, true);
      SDL_EndGPUCopyPass(cp);
      gamma_dirty_ = false;
    }
    SDL_GPUColorTargetInfo t{};
    t.texture = sc;
    t.load_op = load == SDL_GPU_LOADOP_LOAD ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_DONT_CARE;
    t.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &t, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(rp, gamma_pipeline_);
    SamplerKey fk{};
    if (filter == SDL_GPU_FILTER_LINEAR) fk.mag = fk.min = SDL_GPU_FILTER_LINEAR;
    SamplerKey lk{};
    lk.mag = lk.min = SDL_GPU_FILTER_LINEAR;
    SDL_GPUTextureSamplerBinding tb[2] = {{front_, Sampler(fk)}, {gamma_tex_, Sampler(lk)}};
    SDL_BindGPUFragmentSamplers(rp, 0, tb, 2);
    SDL_GPUViewport vp{float(r.left), float(r.top), float(r.right - r.left), float(r.bottom - r.top), 0, 1};
    SDL_SetGPUViewport(rp, &vp);
    SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
    SDL_EndGPURenderPass(rp);
  }

  void CreateGammaPipeline(SDL_GPUTextureFormat fmt) {
    if (gamma_pipeline_) SDL_ReleaseGPUGraphicsPipeline(dev_, gamma_pipeline_);
    static const char* kVs =
        "#version 450\n"
        "layout(location = 0) out vec2 uv;\n"
        "void main() {\n"
        "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
        "  uv = p;\n"
        "  gl_Position = vec4(p * vec2(2, -2) + vec2(-1, 1), 0, 1);\n"
        "}\n";
    static const char* kFs =
        "#version 450\n"
        "layout(set = 2, binding = 0) uniform sampler2D img;\n"
        "layout(set = 2, binding = 1) uniform sampler2D ramp;\n"
        "layout(location = 0) in vec2 uv;\n"
        "layout(location = 0) out vec4 o;\n"
        "void main() {\n"
        "  vec4 c = texture(img, uv);\n"
        "  vec3 at = (clamp(c.rgb, 0.0, 1.0) * 255.0 + 0.5) / 256.0;\n"
        "  o = vec4(texture(ramp, vec2(at.r, 0.5)).r, texture(ramp, vec2(at.g, 0.5)).g,\n"
        "           texture(ramp, vec2(at.b, 0.5)).b, c.a);\n"
        "}\n";
    SDL_GPUShader* sh[2];
    for (int i = 0; i < 2; i++) {
      std::vector<uint32_t> spirv;
      std::string log;
      if (!GlslToSpirv(i ? kFs : kVs, i ? ShaderStage::Fragment : ShaderStage::Vertex, &spirv, &log))
        FatalF("gfx: the gamma shader: %s", log.c_str());
      SDL_GPUShaderCreateInfo si{};
#if defined(__APPLE__)
      std::string msl;
      if (!SpirvToMsl(spirv, &msl, &log)) FatalF("gfx: the gamma shader: %s", log.c_str());
      si.code = reinterpret_cast<const Uint8*>(msl.c_str());
      si.code_size = msl.size();
      si.entrypoint = "main0";
      si.format = SDL_GPU_SHADERFORMAT_MSL;
#else
      si.code = reinterpret_cast<const Uint8*>(spirv.data());
      si.code_size = spirv.size() * 4;
      si.entrypoint = "main";
      si.format = SDL_GPU_SHADERFORMAT_SPIRV;
#endif
      si.stage = i ? SDL_GPU_SHADERSTAGE_FRAGMENT : SDL_GPU_SHADERSTAGE_VERTEX;
      si.num_samplers = i ? 2 : 0;
      sh[i] = SDL_CreateGPUShader(dev_, &si);
      if (!sh[i]) FatalF("gfx: the gamma shader: %s", SDL_GetError());
    }
    SDL_GPUGraphicsPipelineCreateInfo pi{};
    pi.vertex_shader = sh[0];
    pi.fragment_shader = sh[1];
    pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pi.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUColorTargetDescription cd{};
    cd.format = fmt;
    pi.target_info.color_target_descriptions = &cd;
    pi.target_info.num_color_targets = 1;
    gamma_pipeline_ = SDL_CreateGPUGraphicsPipeline(dev_, &pi);
    if (!gamma_pipeline_) FatalF("gfx: the gamma pipeline: %s", SDL_GetError());
    gamma_format_ = fmt;
    SDL_ReleaseGPUShader(dev_, sh[0]);
    SDL_ReleaseGPUShader(dev_, sh[1]);
  }

  void ReadTexture(SDL_GPUTexture* tex, SDL_GPUTextureFormat tex_fmt, int tw, int th, const Rect& r,
                   void* dst, uint32_t pitch, Format fmt, int level = 0) {
    Read rd = StartRead(tex, tex_fmt, tw, th, r, true, level);
    FinishRead(rd, dst, pitch, fmt);
  }

  SDL_GPUTransferBuffer* NewDownload(uint32_t bytes) {
    SDL_GPUTransferBufferCreateInfo tbi{};
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tbi.size = bytes;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev_, &tbi);
    if (!tb) FatalF("gfx: download buffer: %s", SDL_GetError());
    return tb;
  }

  // download_tb_ with room for `bytes`, for a download waited for at once.
  SDL_GPUTransferBuffer* SharedDownload(uint32_t bytes) {
    if (!download_tb_ || download_tb_cap_ < bytes) {
      if (download_tb_) SDL_ReleaseGPUTransferBuffer(dev_, download_tb_);
      download_tb_cap_ = std::max(bytes, 1u << 20);
      download_tb_ = NewDownload(download_tb_cap_);
    }
    return download_tb_;
  }

  // Copies `r` of `tex` (of tex_fmt, mip `level`, tw x th; after
  // everything recorded so far) into a download transfer buffer: the
  // shared one for a read finished at once (ReadTexture), else one of the
  // read's own.
  Read StartRead(SDL_GPUTexture* tex, SDL_GPUTextureFormat tex_fmt, int tw, int th, const Rect& r,
                 bool shared, int level = 0) {
    Flush();
    Read rd;
    rd.r = r;
    rd.c = Clip(r, tw, th);
    rd.fmt = tex_fmt;
    int w = rd.c.right - rd.c.left, h = rd.c.bottom - rd.c.top;
    if (w <= 0 || h <= 0 || !tex) return rd;
    uint32_t bytes = uint32_t(w) * uint32_t(h) * SDL_GPUTextureFormatTexelBlockSize(tex_fmt);
    if (!shared) {
      rd.tb = NewDownload(bytes);
      rd.own = true;
    } else {
      rd.tb = SharedDownload(bytes);
    }
    SubmitOpen();  // the work before it, tracked with the ring
    SDL_GPUCommandBuffer* cmd = Begin();
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion src{};
    src.texture = tex;
    src.mip_level = Uint32(level);
    src.x = Uint32(rd.c.left);
    src.y = Uint32(rd.c.top);
    src.w = Uint32(w);
    src.h = Uint32(h);
    src.d = 1;
    SDL_GPUTextureTransferInfo d{};
    d.transfer_buffer = rd.tb;
    d.pixels_per_row = Uint32(w);
    d.rows_per_layer = Uint32(h);
    SDL_DownloadFromGPUTexture(cp, &src, &d);
    SDL_EndGPUCopyPass(cp);
    open_ = nullptr;  // submitted here, with a fence of its own
    rd.fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (!rd.fence) FatalF("gfx: SDL_SubmitGPUCommandBuffer: %s", SDL_GetError());
    return rd;
  }

  // Waits for `rd` and converts it into dst, or only drops it if dst is
  // null. A B8G8R8A8 target converts to any format EncodePixel writes;
  // others are read only into their own format.
  void FinishRead(Read& rd, void* dst, uint32_t pitch, Format fmt) {
    if (!rd.fence) return;
    int w = rd.c.right - rd.c.left, h = rd.c.bottom - rd.c.top;
    if (dst) {
      const bool bgra = rd.fmt == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM ||
                        rd.fmt == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
      if (!bgra && rd.fmt != GpuFormat(fmt))
        FatalF("gfx: reading back a target of GPU format %d as format %d", int(rd.fmt), int(fmt));
      SDL_WaitForGPUFences(dev_, true, &rd.fence, 1);
      const auto* px = static_cast<const uint32_t*>(SDL_MapGPUTransferBuffer(dev_, rd.tb, false));
      uint32_t bpp = BitsPerPixel(fmt) / 8;
      // Rows of the requested rectangle that fell outside the target stay
      // as they were.
      int ox = rd.c.left - rd.r.left, oy = rd.c.top - rd.r.top;
      for (int y = 0; y < h; y++) {
        const uint32_t* s = px + size_t(y) * uint32_t(w);
        uint8_t* out = static_cast<uint8_t*>(dst) + size_t(y + oy) * pitch + size_t(ox) * bpp;
        if (!bgra) {
          const size_t row = size_t(w) * SDL_GPUTextureFormatTexelBlockSize(rd.fmt);
          std::memcpy(out, reinterpret_cast<const uint8_t*>(px) + size_t(y) * row, row);
        } else if (fmt == Format::X8R8G8B8 || fmt == Format::A8R8G8B8) {
          auto* o = reinterpret_cast<uint32_t*>(out);
          for (int x = 0; x < w; x++) o[x] = s[x] | 0xFF000000u;
        } else {
          for (int x = 0; x < w; x++) EncodePixel(out + size_t(x) * bpp, fmt, s[x] | 0xFF000000u);
        }
      }
      SDL_UnmapGPUTransferBuffer(dev_, rd.tb);
    }
    SDL_ReleaseGPUFence(dev_, rd.fence);
    if (rd.own) SDL_ReleaseGPUTransferBuffer(dev_, rd.tb);
    rd = {};
  }

  static void ConvertToArgb(const uint8_t* src, uint32_t pitch, Format f, int w, int h,
                            uint32_t* out) {
    uint32_t bpp = BitsPerPixel(f) / 8;
    for (int y = 0; y < h; y++) {
      const uint8_t* s = src + size_t(y) * pitch;
      uint32_t* d = out + size_t(y) * uint32_t(w);
      if (f == Format::A8R8G8B8) {
        std::memcpy(d, s, size_t(w) * 4);
      } else if (f == Format::X8R8G8B8) {
        const auto* p = reinterpret_cast<const uint32_t*>(s);
        for (int x = 0; x < w; x++) d[x] = p[x] | 0xFF000000u;
      } else if (const uint32_t* lut = Lut16(f)) {
        // 16-bit formats: a table of DecodePixel's results, so the same
        // values at a fraction of the cost (full-screen 2D blits).
        const auto* p = reinterpret_cast<const uint16_t*>(s);
        for (int x = 0; x < w; x++) d[x] = lut[p[x]];
      } else {
        for (int x = 0; x < w; x++) d[x] = DecodePixel(s + size_t(x) * bpp, f);
      }
    }
  }

  // DecodePixel of every value of a 16-bit format, built on first use;
  // null for other formats.
  static const uint32_t* Lut16(Format f) {
    static const Format kFormats[] = {Format::R5G6B5, Format::X1R5G5B5, Format::A1R5G5B5,
                                      Format::A4R4G4B4, Format::X4R4G4B4};
    static std::once_flag once[std::size(kFormats)];
    static std::vector<uint32_t> tables[std::size(kFormats)];
    for (size_t i = 0; i < std::size(kFormats); i++) {
      if (kFormats[i] != f) continue;
      std::call_once(once[i], [&] {
        tables[i].resize(65536);
        for (uint32_t v = 0; v < 65536; v++) {
          uint16_t raw = uint16_t(v);
          tables[i][v] = DecodePixel(reinterpret_cast<const uint8_t*>(&raw), f);
        }
      });
      return tables[i].data();
    }
    return nullptr;
  }

  // ---- DirectDraw blits: a textured screen quad with private state

  // A blit; with `blend`, an A8R8G8B8 image blended by its alpha.
  void DrawImage(const void* src, uint32_t pitch, Format fmt, const Rect& sr, const Rect& dr,
                 bool colorkey, uint32_t key, bool blend = false) {
    int w = sr.right - sr.left, h = sr.bottom - sr.top;
    if (w <= 0 || h <= 0 || dr.right <= dr.left || dr.bottom <= dr.top) return;
    std::vector<uint32_t> px(size_t(w) * h);
    uint32_t bpp = BitsPerPixel(fmt) / 8;
    const uint8_t* base = static_cast<const uint8_t*>(src) + size_t(sr.top) * pitch +
                          size_t(sr.left) * bpp;
    if (!colorkey) {
      ConvertToArgb(base, pitch, fmt, w, h, px.data());
      if (!blend)
        for (uint32_t& p : px) p |= 0xFF000000u;
    } else {
      for (int y = 0; y < h; y++) {
        const uint8_t* s = base + size_t(y) * pitch;
        for (int x = 0; x < w; x++, s += bpp)
          px[size_t(y) * w + x] = RawPixel(s, fmt) == key ? 0 : (DecodePixel(s, fmt) | 0xFF000000u);
      }
    }
    // A pool texture per blit in this flush.
    if (blit_used_ == blit_pool_.size()) blit_pool_.push_back({nullptr, 0, 0});
    size_t slot = blit_used_++;
    if (blit_pool_[slot].w < w || blit_pool_[slot].h < h) {
      if (blit_pool_[slot].tex) {
        Flush();  // recorded blits may still use the old one
        SDL_ReleaseGPUTexture(dev_, blit_pool_[slot].tex);
        blit_used_ = slot + 1;
      }
      BlitTex& b = blit_pool_[slot];
      b.w = std::max({w, b.w, 256});
      b.h = std::max({h, b.h, 256});
      SDL_GPUTextureCreateInfo ti{};
      ti.type = SDL_GPU_TEXTURETYPE_2D;
      ti.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
      ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
      ti.width = Uint32(b.w);
      ti.height = Uint32(b.h);
      ti.layer_count_or_depth = 1;
      ti.num_levels = 1;
      b.tex = SDL_CreateGPUTexture(dev_, &ti);
      if (!b.tex) FatalF("gfx: blit texture %dx%d: %s", b.w, b.h, SDL_GetError());
    }
    const BlitTex& bt = blit_pool_[slot];
    QueueUpload(bt.tex, 0, 0, 0, w, h, px.data(), uint32_t(w) * 4, uint32_t(h),
                uint32_t(w) * 4, true);

    PipelineKey k = BaseKey(SDL_GPU_PRIMITIVETYPE_TRIANGLELIST);
    if (blend) {
      k.blend = 1;
      k.src = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
      k.dst = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    }
    Cmd c = InternalCmd(k, 0x004 | 0x100, true, colorkey);
    c.tex[0] = bt.tex;
    SamplerKey sk{};
    sk.min = sk.mag = SDL_GPU_FILTER_LINEAR;
    sk.mip = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sk.u = sk.v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sk.aniso = 1;
    c.smp[0] = Sampler(sk);
    Rect clip = Intersect(dr, {0, 0, pass_w_, pass_h_});
    if (clip.right <= clip.left || clip.bottom <= clip.top) return;
    c.scissor = {clip.left, clip.top, clip.right - clip.left, clip.bottom - clip.top};
    float u1 = float(w) / float(bt.w), v1 = float(h) / float(bt.h);
    struct V {
      float x, y, z, rhw, u, v;
    };
    // Pixel edges, in D3D9's integer-centred coordinates.
    float x0 = float(dr.left) - 0.5f, y0 = float(dr.top) - 0.5f;
    float x1 = float(dr.right) - 0.5f, y1 = float(dr.bottom) - 0.5f;
    V q[6] = {{x0, y0, 0, 1, 0, 0},   {x1, y0, 0, 1, u1, 0}, {x0, y1, 0, 1, 0, v1},
              {x1, y0, 0, 1, u1, 0},  {x1, y1, 0, 1, u1, v1}, {x0, y1, 0, 1, 0, v1}};
    AddVertices(c, q, 6, sizeof(V));
    cmds_.push_back(c);
  }

  // A draw of our own (clear quads, blits): pre-transformed vertices,
  // stage 0 selecting the texture or the diffuse colour, nothing else.
  Cmd InternalCmd(const PipelineKey& k, uint32_t fvf, bool textured, bool alpha_test = false) {
    Cmd c{};
    c.key = k;
    VsUniforms vu{};
    VsLayout vl;
    PatchLayout(vl, DecodeFvf(fvf, fvf & 0x100 ? 24 : 20));
    c.vl = PushVl(vl);
    vu.target[0] = float(pass_w_);
    vu.target[1] = float(pass_h_);
    vu.mode[0] = 1;
    for (int s = 0; s < 8; s++) vu.tci[s] = s;
    c.vs = PushVs(vu);
    FsUniforms fu{};
    int arg = textured ? 2 : 0;
    fu.color[0][0] = 2;
    fu.color[0][1] = arg;
    fu.alpha[0][0] = 2;
    fu.alpha[0][1] = arg;
    fu.stage[0][1] = textured ? 1 : 0;
    for (int s = 0; s < 8; s++) fu.stage[s][2] = -1;
    for (int s = 1; s < 8; s++) fu.color[s][0] = fu.alpha[s][0] = 1;
    if (alpha_test) {
      fu.mode[0] = 5;  // GREATER 0: colour-keyed texels have alpha 0
      fu.mode[1] = 0;
    }
    c.fs = PushFs(fu);
    SamplerKey sk{};
    sk.aniso = 1;
    for (int i = 0; i < 8; i++) c.smp[i] = Sampler(sk);
    c.viewport = FullViewport();
    c.scissor = {0, 0, pass_w_, pass_h_};
    return c;
  }

  void AddVertices(Cmd& c, const void* v, uint32_t n, uint32_t stride) {
    c.indexed = false;
    c.first = AppendVertices(v, n, stride);
    c.count = n;
  }

  // ---- per-draw state

  PipelineKey BaseKey(SDL_GPUPrimitiveType prim) const {
    PipelineKey k;
    std::memset(&k, 0, sizeof k);
    k.prim = uint8_t(prim);
    k.fill = SDL_GPU_FILLMODE_FILL;
    k.cull = SDL_GPU_CULLMODE_NONE;
    k.front = SDL_GPU_FRONTFACE_CLOCKWISE;
    k.src = SDL_GPU_BLENDFACTOR_ONE;
    k.dst = SDL_GPU_BLENDFACTOR_ZERO;
    k.op = SDL_GPU_BLENDOP_ADD;
    k.write_mask = 0xF;
    k.depth_func = SDL_GPU_COMPAREOP_ALWAYS;
    k.s_fail = k.s_zfail = k.s_pass = SDL_GPU_STENCILOP_KEEP;
    k.s_func = SDL_GPU_COMPAREOP_ALWAYS;
    k.depth_format = uint8_t(pass_depth_format_);
    // Fixed-function draws write target 0 only.
    k.targets = uint8_t(pass_targets_);
    for (int i = 0; i < 4; i++) k.fmt[i] = i < pass_targets_ ? pass_fmts_[i] : 0;
    return k;
  }

  PipelineKey DrawKey(const FfState& st, SDL_GPUPrimitiveType prim) const {
    PipelineKey k = BaseKey(prim);
    if (st.rs[8] != 3) k.fill = SDL_GPU_FILLMODE_LINE;
    bool tri = prim == SDL_GPU_PRIMITIVETYPE_TRIANGLELIST ||
               prim == SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
    // D3D culls by screen winding: CW (2) or CCW (3) triangles.
    if (tri && st.rs[22] == 2) {
      k.cull = SDL_GPU_CULLMODE_BACK;
      k.front = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    } else if (tri && st.rs[22] == 3) {
      k.cull = SDL_GPU_CULLMODE_BACK;
      k.front = SDL_GPU_FRONTFACE_CLOCKWISE;
    }
    if (st.rs[27]) {
      k.blend = 1;
      uint32_t s = st.rs[19], d = st.rs[20];
      if (s == 12) { s = 5; d = 6; }        // BOTHSRCALPHA
      else if (s == 13) { s = 6; d = 5; }   // BOTHINVSRCALPHA
      k.src = uint8_t(D3DBlendFactor(s, pass_alpha_));
      k.dst = uint8_t(D3DBlendFactor(d, pass_alpha_));
      k.op = uint8_t(D3DBlendOp(st.rs[171]));
    }
    k.write_mask = uint8_t(st.rs[168] & 0xF);
    if (pass_depth_) {
      k.depth_test = st.rs[7] ? 1 : 0;
      k.depth_write = (st.rs[7] && st.rs[14]) ? 1 : 0;
      k.depth_func = uint8_t(D3DCompareOp(st.rs[23]));
      if (pass_stencil_ && st.rs[52]) {
        k.stencil = 1;
        k.s_fail = uint8_t(D3DStencilOp(st.rs[53]));
        k.s_zfail = uint8_t(D3DStencilOp(st.rs[54]));
        k.s_pass = uint8_t(D3DStencilOp(st.rs[55]));
        k.s_func = uint8_t(D3DCompareOp(st.rs[56]));
        k.s_read = uint8_t(st.rs[58]);
        k.s_write = uint8_t(st.rs[59]);
      }
      // D3D9's DEPTHBIAS is in depth-range units; D3D7's ZBIAS 0..16
      // as the d3d9 backend maps it.
      float bias = AsFloat(st.rs[195]) - float(st.rs[47]) * 0.000005f;
      k.bias_const = bias * pass_bias_scale_;
      k.bias_slope = AsFloat(st.rs[175]);
    }
    return k;
  }

  // ---- fixed-function fragment shader variants
  //
  // ff.frag interprets the texture-stage state from uniforms: a loop over
  // eight stages with switches, for every fragment. Each state's draws
  // get a variant with that state compiled in (FsVariantKey), compiled on
  // a worker thread; until it is ready they use the general shader, which
  // computes the same. <ID>_FF_VARIANTS=off keeps the general shader;
  // =sync compiles each variant when first needed, on the device's thread
  // (tests: a variant that fails to compile is then fatal).

  struct FsVariant {
    std::atomic<SDL_GPUShader*> shader{nullptr};
    std::atomic<bool> failed{false};
    std::unordered_map<PipelineKey, SDL_GPUGraphicsPipeline*, BytesHash<PipelineKey>> pipelines;
  };

  // The pipeline for a fixed-function draw with fs_list_[fs].
  SDL_GPUGraphicsPipeline* FfPipeline(const PipelineKey& k, int fs) {
    const FsUniforms& f = fs_list_[size_t(fs)];
    const bool discard = NeedsDiscard(k, f);
    // Draws in a row mostly share their uniforms (fs_list_ entry).
    if (fs != variant_fs_ || discard != variant_discard_) {
      variant_fs_ = fs;
      variant_discard_ = discard;
      variant_last_ = Variant(f, discard);
    }
    if (FsVariant* v = variant_last_) {
      if (SDL_GPUShader* sh = v->shader.load(std::memory_order_acquire)) {
        auto it = v->pipelines.find(k);
        if (it == v->pipelines.end()) it = v->pipelines.emplace(k, CreatePipeline(k, vs_, sh, nullptr)).first;
        return it->second;
      }
    }
    return Pipeline(k, discard);
  }

  // The variant for `f` (requested on first use), or null when variants
  // are off.
  FsVariant* Variant(const FsUniforms& f, bool discard) {
    if (variant_mode_ == VariantMode::Off) return nullptr;
    FsVariantKey key{};
    for (int s = 0; s < 8; s++) {
      if (f.color[s][0] == 1) {  // DISABLE ends the cascade (ff.frag)
        key.color[s][0] = 1;
        break;
      }
      for (int i = 0; i < 4; i++) {
        key.color[s][i] = int8_t(f.color[s][i]);
        key.alpha[s][i] = int8_t(f.alpha[s][i]);
      }
      for (int i = 0; i < 4; i++) key.stage[s][i] = int8_t(f.stage[s][i]);
    }
    key.func = discard ? int8_t(f.mode[0]) : 0;
    key.fog = int8_t(f.mode[2]);
    key.table = f.mode[2] == 2 ? int8_t(f.mode[3]) : 0;
    key.flat = int8_t(f.mode2[0] != 0);
    key.specular = int8_t(f.mode2[1] != 0);
    key.discard = discard;
    auto [it, fresh] = variants_.try_emplace(key);
    FsVariant* v = &it->second;
    if (fresh && variant_mode_ == VariantMode::Sync) {
      VariantJob job{VariantSource(key), v};
      if (!CompileVariant(job)) FatalF("gfx: fixed-function shader variant failed (FF_VARIANTS=sync)");
    } else if (fresh) {
      std::lock_guard<std::mutex> l(variant_mu_);
      variant_jobs_.push_back({VariantSource(key), v});
      if (!variant_worker_.joinable()) variant_worker_ = std::thread([this] { VariantWorker(); });
      variant_cv_.notify_one();
    }
    return v->failed.load(std::memory_order_relaxed) ? nullptr : v;
  }

  static std::string VariantSource(const FsVariantKey& k) {
    std::string src = kFfFragGlsl;
    std::string defs = "#define SPECIALIZED\n";
#if defined(__APPLE__)
    // As the general shaders are built (ff.frag, HALF); FF_HALF=0 keeps
    // 32-bit arithmetic in the variants (comparisons).
    const char* he = Env("FF_HALF");
    if (!(he && *he == '0')) defs = "#define HALF\n" + defs;
#endif
    if (k.discard) defs += "#define ALPHA_TEST\n";
    auto ivec4s = [&](const char* name, auto&& at) {
      defs += std::string("const ivec4 ") + name + "[8] = ivec4[8](";
      for (int s = 0; s < 8; s++) {
        const auto v = at(s);
        defs += (s ? ", " : "") + std::string("ivec4(") + std::to_string(v[0]) + ", " +
                std::to_string(v[1]) + ", " + std::to_string(v[2]) + ", " + std::to_string(v[3]) + ")";
      }
      defs += ");\n";
    };
    ivec4s("k_color", [&](int s) { return std::array<int, 4>{k.color[s][0], k.color[s][1], k.color[s][2], k.color[s][3]}; });
    ivec4s("k_alpha", [&](int s) { return std::array<int, 4>{k.alpha[s][0], k.alpha[s][1], k.alpha[s][2], k.alpha[s][3]}; });
    ivec4s("k_stage", [&](int s) { return std::array<int, 4>{k.stage[s][0], k.stage[s][1], k.stage[s][2], k.stage[s][3]}; });
    defs += "const ivec4 k_mode = ivec4(" + std::to_string(k.func) + ", 0, " + std::to_string(k.fog) +
            ", " + std::to_string(k.table) + ");\n";
    defs += "const ivec4 k_mode2 = ivec4(" + std::to_string(k.flat) + ", " +
            std::to_string(k.specular) + ", 0, 0);\n";
    const size_t eol = src.find('\n');  // after #version
    return src.insert(eol + 1, defs);
  }

  struct VariantJob {
    std::string glsl;
    FsVariant* variant;
  };

  // Compiles requested variants. SDL_CreateGPUShader only uses the
  // device here (Metal: MTLDevice; Vulkan: vkCreateShaderModule), which is
  // safe from any thread.
  void VariantWorker() {
    for (;;) {
      VariantJob job;
      {
        std::unique_lock<std::mutex> l(variant_mu_);
        variant_cv_.wait(l, [&] { return variant_quit_ || !variant_jobs_.empty(); });
        if (variant_quit_) return;
        job = std::move(variant_jobs_.front());
        variant_jobs_.pop_front();
      }
      CompileVariant(job);
    }
  }

  // Compiles a variant into job.variant; false (marked failed) on error.
  bool CompileVariant(VariantJob& job) {
    std::vector<uint32_t> spirv;
    std::string log, code;
    bool ok = GlslToSpirv(job.glsl, ShaderStage::Fragment, &spirv, &log);
#if defined(__APPLE__)
    ok = ok && SpirvToMsl(spirv, &code, &log);
    const SDL_GPUShaderFormat format = SDL_GPU_SHADERFORMAT_MSL;
    const char* entry = "main0";
#else
    code.assign(reinterpret_cast<const char*>(spirv.data()), spirv.size() * 4);
    const SDL_GPUShaderFormat format = SDL_GPU_SHADERFORMAT_SPIRV;
    const char* entry = "main";
#endif
    SDL_GPUShader* sh = nullptr;
    if (ok) {
      SDL_GPUShaderCreateInfo fs{};
      fs.code = reinterpret_cast<const Uint8*>(code.data());
      fs.code_size = code.size();
      fs.entrypoint = entry;
      fs.format = format;
      fs.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
      fs.num_uniform_buffers = 1;
      fs.num_samplers = 16;  // ff.frag: eight 2D, then eight cube
      sh = SDL_CreateGPUShader(dev_, &fs);
      if (!sh) log = SDL_GetError();
    }
    if (sh) {
      job.variant->shader.store(sh, std::memory_order_release);
      return true;
    }
    WarnF("gfx: fixed-function shader variant: %s\n", log.c_str());
    job.variant->failed.store(true, std::memory_order_relaxed);
    return false;
  }

  // Whether a draw needs ff.frag's alpha test (the shader that can
  // discard). Not when the test is off (BuildUniforms also turns off one
  // no fragment can fail), nor for "alpha > 0" under SRCALPHA blending
  // (INVSRCALPHA or ONE) that writes no depth or stencil: a fragment the
  // test would reject has alpha below half a step (1/510) and moves the
  // target by less than half a step, which rounds away. Blending in half
  // precision can leave such a pixel one step off, which no one can see;
  // in exchange the GPU keeps hidden surface removal for those draws.
  static bool NeedsDiscard(const PipelineKey& k, const FsUniforms& f) {
    if (f.mode[0] == 0) return false;
    const bool fades = k.blend && k.src == SDL_GPU_BLENDFACTOR_SRC_ALPHA &&
                       (k.dst == SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA ||
                        k.dst == SDL_GPU_BLENDFACTOR_ONE) &&
                       k.op == SDL_GPU_BLENDOP_ADD && !k.depth_write && !k.stencil &&
                       k.targets <= 1;
    return !(fades && f.mode[0] == 5 && f.mode[1] == 0);
  }

  // `alpha_test`: the draw needs the fragment shader that can discard
  // (NeedsDiscard).
  SDL_GPUGraphicsPipeline* Pipeline(const PipelineKey& k, bool alpha_test) {
    if (last_pipeline_ && alpha_test == last_alpha_test_ && k == last_key_) return last_pipeline_;
    auto& cache = alpha_test ? pipelines_alpha_test_ : pipelines_;
    auto it = cache.find(k);
    if (it == cache.end())
      it = cache.emplace(k, CreatePipeline(k, vs_, alpha_test ? fs_alpha_test_ : fs_, nullptr)).first;
    last_key_ = k;
    last_alpha_test_ = alpha_test;
    return last_pipeline_ = it->second;
  }

  // `alpha`: separate alpha blend factors and op (src, dst, op), or null
  // for the colour ones.
  SDL_GPUGraphicsPipeline* CreatePipeline(const PipelineKey& k, SDL_GPUShader* vs,
                                          SDL_GPUShader* fs, const uint8_t* alpha) {
    SDL_GPUColorTargetDescription ctds[4]{};
    for (int i = 0; i < k.targets; i++) {
      SDL_GPUColorTargetDescription& ctd = ctds[i];
      ctd.format = SDL_GPUTextureFormat(k.fmt[i]);
      ctd.blend_state.enable_blend = k.blend != 0;
      ctd.blend_state.src_color_blendfactor = SDL_GPUBlendFactor(k.src);
      ctd.blend_state.dst_color_blendfactor = SDL_GPUBlendFactor(k.dst);
      ctd.blend_state.color_blend_op = SDL_GPUBlendOp(k.op);
      ctd.blend_state.src_alpha_blendfactor = SDL_GPUBlendFactor(alpha ? alpha[0] : k.src);
      ctd.blend_state.dst_alpha_blendfactor = SDL_GPUBlendFactor(alpha ? alpha[1] : k.dst);
      ctd.blend_state.alpha_blend_op = SDL_GPUBlendOp(alpha ? alpha[2] : k.op);
      ctd.blend_state.color_write_mask = i == 0 ? k.write_mask : k.mask[i - 1];
      ctd.blend_state.enable_color_write_mask = true;
    }
    SDL_GPUGraphicsPipelineCreateInfo pi{};
    pi.vertex_shader = vs;
    pi.fragment_shader = fs;
    pi.primitive_type = SDL_GPUPrimitiveType(k.prim);
    pi.rasterizer_state.fill_mode = SDL_GPUFillMode(k.fill);
    pi.rasterizer_state.cull_mode = SDL_GPUCullMode(k.cull);
    pi.rasterizer_state.front_face = SDL_GPUFrontFace(k.front);
    pi.rasterizer_state.enable_depth_clip = true;
    if (k.bias_const != 0 || k.bias_slope != 0) {
      pi.rasterizer_state.enable_depth_bias = true;
      pi.rasterizer_state.depth_bias_constant_factor = k.bias_const;
      pi.rasterizer_state.depth_bias_slope_factor = k.bias_slope;
    }
    pi.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    auto& ds = pi.depth_stencil_state;
    ds.enable_depth_test = k.depth_test != 0;
    ds.enable_depth_write = k.depth_write != 0;
    ds.compare_op = SDL_GPUCompareOp(k.depth_func);
    ds.enable_stencil_test = k.stencil != 0;
    SDL_GPUStencilOpState so{SDL_GPUStencilOp(k.s_fail), SDL_GPUStencilOp(k.s_pass),
                             SDL_GPUStencilOp(k.s_zfail), SDL_GPUCompareOp(k.s_func)};
    ds.front_stencil_state = ds.back_stencil_state = so;
    ds.compare_mask = k.s_read;
    ds.write_mask = k.s_write;
    pi.target_info.color_target_descriptions = ctds;
    pi.target_info.num_color_targets = k.targets;
    pi.target_info.has_depth_stencil_target = k.depth_format != SDL_GPU_TEXTUREFORMAT_INVALID;
    pi.target_info.depth_stencil_format = SDL_GPUTextureFormat(k.depth_format);
    const Uint64 t0 = SDL_GetTicksNS();
    SDL_GPUGraphicsPipeline* p = SDL_CreateGPUGraphicsPipeline(dev_, &pi);
    if (!p) FatalF("gfx: SDL_CreateGPUGraphicsPipeline: %s", SDL_GetError());
    AddStall(Stall::Pipeline, SDL_GetTicksNS() - t0);
    return p;
  }

  SDL_GPUSampler* Sampler(const SamplerKey& k) {
    auto it = samplers_.find(k);
    if (it != samplers_.end()) return it->second;
    SDL_GPUSamplerCreateInfo si{};
    si.min_filter = SDL_GPUFilter(k.min);
    si.mag_filter = SDL_GPUFilter(k.mag);
    si.mipmap_mode = SDL_GPUSamplerMipmapMode(k.mip);
    si.address_mode_u = SDL_GPUSamplerAddressMode(k.u);
    si.address_mode_v = SDL_GPUSamplerAddressMode(k.v);
    si.address_mode_w = SDL_GPUSamplerAddressMode(k.w);
    si.mip_lod_bias = k.lod_bias;
    si.min_lod = k.min_lod;
    // MIPFILTER NONE samples only the top level (or MAXMIPLEVEL).
    si.max_lod = k.mip == 0xFF ? k.min_lod : 1000.0f;
    if (k.mip == 0xFF) si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.enable_anisotropy = k.aniso > 1;
    si.max_anisotropy = float(k.aniso);
    if (k.pad & 1) {  // a shadow map's: lit where the coordinate's depth is not nearer
      si.enable_compare = true;
      si.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
      si.enable_anisotropy = false;
    }
    SDL_GPUSampler* s = SDL_CreateGPUSampler(dev_, &si);
    if (!s) FatalF("gfx: SDL_CreateGPUSampler: %s", SDL_GetError());
    samplers_[k] = s;
    return s;
  }

  SamplerKey StageSampler(const FfState& st, int s) const {
    const uint32_t* t = st.tss[s];
    SamplerKey k{};
    k.mag = (t[16] == 1) ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
    k.min = (t[17] == 1) ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
    k.mip = t[18] == 2 ? SDL_GPU_SAMPLERMIPMAPMODE_NEAREST
          : t[18] == 3 ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : 0xFF;
    k.u = uint8_t(D3DAddressMode(t[13]));
    k.v = uint8_t(D3DAddressMode(t[14]));
    bool aniso = t[16] == 5 || t[17] == 3;
    k.aniso = uint8_t(aniso ? std::clamp<uint32_t>(t[21], 1, 16) : 1);
    k.lod_bias = AsFloat(t[19]);
    k.min_lod = float(t[20]);
    return k;
  }

  // Each stage's texture and sampler, kept per stage from the draw before.
  void BindStageTextures(const FfState& st, Cmd& c) {
    for (int s = 0; s < 8; s++) {
      StageBinding& b = stage_bindings_[s];
      if (b.gen != tex_gen_ || b.id != st.textures[s]) {
        auto it = textures_.find(st.textures[s]);
        b.gen = tex_gen_;
        b.id = st.textures[s];
        b.tex = it != textures_.end() ? it->second.tex : nullptr;
        b.cube = it != textures_.end() && it->second.cube;
      }
      const SamplerKey k = StageSampler(st, s);
      if (!b.smp || std::memcmp(&k, &b.key, sizeof k) != 0) {
        b.key = k;
        b.smp = Sampler(k);
      }
      c.tex[s] = b.tex;
      c.cube[s] = b.cube;
      c.smp[s] = b.smp;
    }
  }

  // ---- uniforms from the D3D state

  // Interleaved vertices (the vertex number counting from the arena's
  // start).
  void PatchLayout(VsLayout& u, const FvfLayout& l) const {
    u.layout[0] = l.stride;
    u.layout[1] = 0;
    u.layout[2] = l.normal;
    u.layout[3] = l.diffuse;
    u.layout2[0] = l.specular;
    u.layout2[1] = l.psize;
    u.layout2[2] = l.weights;
    u.layout2[3] = 0;
    for (int& st : u.strides) st = l.stride;
    for (int i = 0; i < 8; i++) {
      u.tex_offset[i] = l.tex_offset[i];
      u.tex_stride[i] = l.stride;
      u.tex_size[i] = l.tex_size[i] ? l.tex_size[i] : 2;
    }
  }

  // Puts a draw's streams in the vertex arena as they lie, each attribute
  // of vertex v at its address + v * its stride: streams within the same
  // records (interleaved ones) copied together, a locked stream once per
  // lock, an absent one as zeros. False, placing no vertices, when a
  // stride is not a multiple of 4 or attributes in one record are not 4
  // bytes apart (the shader reads words).
  bool PlaceStreams(const FfDraw& d, VsLayout& u) {
    const FvfLayout lay = DecodeFvf(d.fvf, 0);
    struct Attr {
      const FfStream* s;
      uint32_t size;
      int32_t *addr, *stride;
    };
    Attr at[13];
    int n = 0;
    u.layout[2] = u.layout[3] = u.layout2[0] = u.layout2[1] = -1;
    at[n++] = {&d.position, lay.pos_kind ? 16u : 12u + 4 * uint32_t(lay.weights), &u.layout[1],
               &u.layout[0]};
    if (lay.normal >= 0) at[n++] = {&d.normal, 12, &u.layout[2], &u.strides[0]};
    if (lay.psize >= 0) at[n++] = {&d.psize, 4, &u.layout2[1], &u.strides[3]};
    if (lay.diffuse >= 0) at[n++] = {&d.diffuse, 4, &u.layout[3], &u.strides[1]};
    if (lay.specular >= 0) at[n++] = {&d.specular, 4, &u.layout2[0], &u.strides[2]};
    for (int i = 0; i < 8; i++) {
      u.tex_offset[i] = -1;
      u.tex_size[i] = lay.tex_size[i] ? lay.tex_size[i] : 2;
      if (lay.tex_offset[i] >= 0)
        at[n++] = {&d.tex[i], 4 * uint32_t(lay.tex_size[i]), &u.tex_offset[i], &u.tex_stride[i]};
    }
    for (int a = 0; a < n; a++)
      if (at[a].s->stride & 3) return false;
    u.layout2[2] = lay.weights;
    u.layout2[3] = d.rgba_colors ? 1 : 0;

    const uint32_t first = d.first_vertex, count = d.vertex_count;
    // Runs of streams within one record: [lo, hi) of the first vertex's.
    struct Run {
      uintptr_t lo, hi;
      uint32_t stride;
      int32_t at = -1;
    };
    Run runs[13];
    int run_of[13], nruns = 0;
    for (int a = 0; a < n; a++) {
      const FfStream& st = *at[a].s;
      run_of[a] = -1;
      if (!st.data) {  // zeros
        if (zeros_at_ < 0) {
          zeros_at_ = int32_t(ReserveBytes(64));
          std::memset(verts_.data() + zeros_at_, 0, 64);
        }
        *at[a].addr = zeros_at_;
        *at[a].stride = 0;
        continue;
      }
      if (st.lock) {
        auto it = std::find_if(locked_.begin(), locked_.end(), [&](const Locked& k) {
          return k.lock == st.lock && k.data == st.data && k.stride == st.stride &&
                 k.first == first && k.count == count && k.size == at[a].size;
        });
        if (it != locked_.end()) {
          *at[a].addr = it->at;
          *at[a].stride = int32_t(st.stride);
          continue;
        }
      }
      uintptr_t lo = uintptr_t(st.data) + uintptr_t(first) * st.stride, hi = lo + at[a].size;
      int r = 0;
      for (; r < nruns; r++) {
        Run& run = runs[r];
        if (run.stride == st.stride && st.stride &&
            std::max(run.hi, hi) - std::min(run.lo, lo) <= st.stride) {
          run.lo = std::min(run.lo, lo);
          run.hi = std::max(run.hi, hi);
          break;
        }
      }
      if (r == nruns) runs[nruns++] = {lo, hi, st.stride};
      run_of[a] = r;
    }
    for (int a = 0; a < n; a++)
      if (run_of[a] >= 0 &&
          ((uintptr_t(at[a].s->data) + uintptr_t(first) * at[a].s->stride - runs[run_of[a]].lo) & 3))
        return false;
    // Every attribute in a run of a locked stream: the runs may come from
    // the persistent vertices (PlacePersistent).
    bool persistent = pv_mode_ != PvMode::Off && nruns > 0;
    uint32_t run_lock[13] = {};
    for (int a = 0; a < n && persistent; a++) {
      if (run_of[a] < 0) {
        persistent = false;
        break;
      }
      uint32_t lk = at[a].s->lock;
      uint32_t& rl = run_lock[run_of[a]];
      if (!lk || (rl && rl != lk)) persistent = false;
      rl = lk;
    }
    if (persistent && PlacePersistent(runs, nruns, run_lock, count)) {
      placed_pv_ = true;
      for (int a = 0; a < n; a++) {
        const FfStream& st = *at[a].s;
        const Run& run = runs[run_of[a]];
        uintptr_t lo = uintptr_t(st.data) + uintptr_t(first) * st.stride;
        *at[a].addr = run.at + int32_t(lo - run.lo);
        *at[a].stride = int32_t(st.stride);
      }
      return true;
    }
    for (int r = 0; r < nruns; r++) {
      Run& run = runs[r];
      size_t bytes = size_t(run.stride) * (count - 1) + (run.hi - run.lo);
      run.at = int32_t(ReserveBytes(bytes));
      std::memcpy(verts_.data() + run.at, reinterpret_cast<const void*>(run.lo), bytes);
    }
    for (int a = 0; a < n; a++) {
      if (run_of[a] < 0) continue;
      const FfStream& st = *at[a].s;
      const Run& run = runs[run_of[a]];
      uintptr_t lo = uintptr_t(st.data) + uintptr_t(first) * st.stride;
      *at[a].addr = run.at + int32_t(lo - run.lo);
      *at[a].stride = int32_t(st.stride);
      if (st.lock) {
        locked_[locked_next_] = {st.lock, st.data, st.stride, first, count, at[a].size, *at[a].addr};
        locked_next_ = (locked_next_ + 1) % locked_.size();
      }
    }
    return true;
  }

  // Persistent vertices: a locked stream's range that an earlier frame drew
  // too (a static vertex buffer's) is uploaded once to pvbuf_, and draws
  // read it there for as long as the lock stays the same, instead of a copy
  // in each pass's arena. The lock is the buffer's ticket, which d3d9
  // changes at every Unlock, so a range is the same bytes while its lock
  // is. Ranges seen first in this frame are not kept: a buffer refilled
  // every frame gets a new lock each time. Sets each run's place in
  // pvbuf_, or returns false (nothing placed) when one cannot have it.
  // FF_VCACHE=off turns it off; =check keeps a copy of each range and
  // compares it at every use (fatal if they differ: the game wrote to a
  // vertex buffer without Lock/Unlock).
  template <class Run>
  bool PlacePersistent(Run* runs, int nruns, const uint32_t* run_lock, uint32_t count) {
    uint32_t got[13];
    bool all = true;  // every run in pvbuf_ (each one is noted or placed either way)
    for (int r = 0; r < nruns; r++) {
      const Run& run = runs[r];
      const uint32_t bytes = uint32_t(size_t(run.stride) * (count - 1) + (run.hi - run.lo));
      const PvKey k{run_lock[r], run.stride, bytes, run.lo};
      auto it = pv_at_.find(k);
      if (it != pv_at_.end()) {
        if (pv_mode_ == PvMode::Check &&
            std::memcmp(pv_shadow_[k].data(), reinterpret_cast<const void*>(run.lo), bytes))
          FatalF("gfx: a vertex buffer changed without Lock/Unlock (FF_VCACHE=check): %u bytes at %p",
                 bytes, reinterpret_cast<const void*>(run.lo));
        got[r] = it->second;
        continue;
      }
      auto [seen, fresh] = pv_seen_.try_emplace(k, pv_frame_);
      const uint32_t need = (bytes + 15) & ~15u;
      if (fresh || seen->second == pv_frame_ || bytes > kPvCap / 8) {
        all = false;
        continue;
      }
      if (pv_head_ + need > kPvCap) {
        pv_full_ = true;  // emptied at the next Present
        all = false;
        continue;
      }
      const uint32_t from = uint32_t(pv_staging_.size());
      pv_staging_.resize(from + need);
      std::memcpy(pv_staging_.data() + from, reinterpret_cast<const void*>(run.lo), bytes);
      pv_uploads_.push_back({pv_head_, from, need});
      if (pv_mode_ == PvMode::Check)
        pv_shadow_[k].assign(reinterpret_cast<const uint8_t*>(run.lo),
                             reinterpret_cast<const uint8_t*>(run.lo) + bytes);
      pv_at_.emplace(k, pv_head_);
      got[r] = pv_head_;
      pv_head_ += need;
    }
    if (!all) return false;
    for (int r = 0; r < nruns; r++) runs[r].at = int32_t(got[r]);
    return true;
  }

  // Persistent indices, as PlacePersistent's vertices: a locked index
  // range drawn in an earlier frame too is converted once (32-bit, clamped
  // to the draw's vertex range, as the arena path does) into pvbuf_. Its
  // place there (a byte offset), or -1 when it cannot have one.
  int64_t PlaceIndices(const void* src, uint32_t isz, uint32_t n, uint32_t lock, uint32_t lo,
                       uint32_t hi) {
    const PiKey k{lock, n, isz, lo, hi, uintptr_t(src)};
    auto convert = [&](uint32_t* out) {
      if (isz == 4)
        for (uint32_t i = 0; i < n; i++)
          out[i] = std::clamp(static_cast<const uint32_t*>(src)[i], lo, hi);
      else
        for (uint32_t i = 0; i < n; i++)
          out[i] = std::clamp(uint32_t(static_cast<const uint16_t*>(src)[i]), lo, hi);
    };
    auto it = pi_at_.find(k);
    if (it != pi_at_.end()) {
      if (pv_mode_ == PvMode::Check &&
          std::memcmp(pi_shadow_[k].data(), src, size_t(n) * isz))
        FatalF("gfx: an index buffer changed without Lock/Unlock (FF_VCACHE=check): %u indices at %p",
               n, src);
      return it->second;
    }
    auto [seen, fresh] = pi_seen_.try_emplace(k, pv_frame_);
    const uint32_t need = (n * 4 + 15) & ~15u;
    if (fresh || seen->second == pv_frame_ || need > kPvCap / 8) return -1;
    if (pv_head_ + need > kPvCap) {
      pv_full_ = true;  // emptied at the next Present
      return -1;
    }
    const uint32_t from = uint32_t(pv_staging_.size());
    pv_staging_.resize(from + need);
    std::vector<uint32_t> tmp(n);
    convert(tmp.data());
    std::memcpy(pv_staging_.data() + from, tmp.data(), size_t(n) * 4);
    pv_uploads_.push_back({pv_head_, from, need});
    if (pv_mode_ == PvMode::Check)
      pi_shadow_[k].assign(static_cast<const uint8_t*>(src),
                           static_cast<const uint8_t*>(src) + size_t(n) * isz);
    pi_at_.emplace(k, pv_head_);
    const uint32_t at = pv_head_;
    pv_head_ += need;
    return at;
  }

  // At Present: a new frame for PlacePersistent; when pvbuf_ filled up,
  // it starts over once the GPU is done with it.
  void PersistentFrame() {
    pv_frame_++;
    if (pv_seen_.size() > (1u << 20)) pv_seen_.clear();
    if (pi_seen_.size() > (1u << 20)) pi_seen_.clear();
    if (!pv_full_) return;
    Drain();
    pv_at_.clear();
    pi_at_.clear();
    pi_shadow_.clear();
    pv_shadow_.clear();
    pv_head_ = 0;
    pv_full_ = false;
  }

  void BuildUniforms(const FfState& st) {
    if (std::memcmp(&st, &built_, sizeof st) != 0) {
      built_ = st;
      vs_dirty_ = fs_dirty_ = true;
    }
    if (vs_dirty_) {
      Mat view, proj;
      std::memcpy(view.data(), st.view, 64);
      std::memcpy(proj.data(), st.proj, 64);
      VsUniforms& u = vs_cur_;
      std::memset(&u, 0, sizeof u);
      // The view and world matrices mostly repeat from draw to draw: their
      // products, normal matrices and the view's inverse are kept with
      // the bytes they were computed from (the same inputs give the same
      // results).
      if (!xf_.valid || std::memcmp(xf_.view, st.view, 64) != 0) {
        std::memcpy(xf_.view, st.view, 64);
        xf_.vinv = Inverse(view);
        xf_.valid = true;
        for (bool& w : xf_.world_valid) w = false;
      }
      for (int i = 0; i < 4; i++) {
        if (!xf_.world_valid[i] || std::memcmp(xf_.world[i], st.world[i], 64) != 0) {
          Mat world;
          std::memcpy(world.data(), st.world[i], 64);
          std::memcpy(xf_.world[i], st.world[i], 64);
          xf_.wv[i] = Mul(world, view);
          xf_.nm[i] = NormalMatrix(xf_.wv[i]);
          xf_.world_valid[i] = true;
        }
        std::memcpy(u.world_view[i], xf_.wv[i].data(), 64);
        std::memcpy(u.normal[i], xf_.nm[i].data(), 64);
      }
      std::memcpy(u.proj, st.proj, 64);
      std::memcpy(u.tex, st.tex, sizeof u.tex);
      u.viewport[0] = float(st.viewport.x);
      u.viewport[1] = float(st.viewport.y);
      u.viewport[2] = float(std::max(st.viewport.width, 1u));
      u.viewport[3] = float(std::max(st.viewport.height, 1u));
      u.target[0] = float(pass_w_);
      u.target[1] = float(pass_h_);
      u.target[2] = proj[11] != 0 ? 1.0f : 0.0f;  // _34: perspective
      Argb(u.ambient, st.rs[139]);
      Vec4(u.mat_diffuse, st.material.diffuse);
      Vec4(u.mat_ambient, st.material.ambient);
      Vec4(u.mat_specular, st.material.specular);
      Vec4(u.mat_emissive, st.material.emissive);
      u.mat_power[0] = st.material.power;
      u.fog[0] = AsFloat(st.rs[36]);
      u.fog[1] = AsFloat(st.rs[37]);
      u.fog[2] = AsFloat(st.rs[38]);
      // Clip planes are in world space; the shader has view space.
      const Mat& vinv = xf_.vinv;
      for (int i = 0; i < 8; i++)
        for (int j = 0; j < 4; j++) {
          float s = 0;
          for (int k = 0; k < 4; k++) s += vinv[j * 4 + k] * st.clip[i][k];
          u.clip[i][j] = s;
        }
      int n = 0;
      for (uint32_t li = 0; li < st.light_count; li++) {
        const Light& l = st.lights[li];
        GpuLight& g = u.lights[n++];
        Vec4(g.diffuse, l.diffuse);
        Vec4(g.specular, l.specular);
        Vec4(g.ambient, l.ambient);
        float p[4] = {l.position[0], l.position[1], l.position[2], 1};
        float d[4] = {l.direction[0], l.direction[1], l.direction[2], 0};
        for (int c = 0; c < 3; c++) {
          g.position[c] = p[0] * view[c] + p[1] * view[4 + c] + p[2] * view[8 + c] + view[12 + c];
          g.direction[c] = -(d[0] * view[c] + d[1] * view[4 + c] + d[2] * view[8 + c]);
        }
        float len = std::sqrt(g.direction[0] * g.direction[0] + g.direction[1] * g.direction[1] +
                              g.direction[2] * g.direction[2]);
        if (len > 0)
          for (int c = 0; c < 3; c++) g.direction[c] /= len;
        g.position[3] = l.range > 0 ? l.range : 1e30f;
        g.direction[3] = l.falloff;
        g.atten[0] = l.attenuation0;
        g.atten[1] = l.attenuation1;
        g.atten[2] = l.attenuation2;
        g.atten[3] = float(l.type);
        g.spot[0] = std::cos(l.theta / 2);
        g.spot[1] = std::cos(l.phi / 2);
      }
      u.mode[1] = st.rs[137] ? 1 : 0;
      u.mode[2] = st.rs[143] ? 1 : 0;
      u.mode[3] = st.rs[142] ? 1 : 0;
      u.mode2[0] = st.rs[141] ? 1 : 0;
      u.mode2[1] = int(st.rs[145]);
      u.mode2[2] = int(st.rs[146]);
      u.mode2[3] = int(st.rs[147]);
      u.mode3[0] = int(st.rs[148]);
      u.mode3[1] = int(st.rs[140]);
      u.mode3[2] = int(st.rs[35]);
      u.mode3[3] = st.rs[48] ? 1 : 0;
      u.mode4[0] = st.rs[29] ? 1 : 0;
      uint32_t vb = st.rs[151];
      u.mode4[1] = (vb >= 1 && vb <= 3) ? int(vb) : 0;
      u.mode4[2] = int(st.rs[152] & 0xFF);
      u.mode4[3] = n;
      u.mode5[0] = st.rs[28] ? 1 : 0;
      for (int s = 0; s < 8; s++) {
        u.tci[s] = int(st.tss[s][11]);
        u.ttf[s] = int(st.tss[s][24]);
      }
      u.point[0] = AsFloat(st.rs[154]);
      vs_dirty_ = false;
    }
    if (fs_dirty_) {
      FsUniforms& u = fs_cur_;
      std::memset(&u, 0, sizeof u);
      for (int s = 0; s < 8; s++) {
        const auto& t = st.tss[s];
        const bool bound = textures_.count(st.textures[s]) != 0;
        u.color[s][0] = int(t[1]);
        // A stage with no texture whose colour operation takes TEXTURE as
        // an argument it uses ends the cascade, as DISABLE does (alpha
        // arguments and implicit reads see zeros instead, ff.frag).
        if (!bound) {
          uint32_t used = 6;  // the arguments it uses: bit 0 ARG0, 1 ARG1, 2 ARG2
          switch (t[1]) {
            case 1: case 22: case 23: used = 0; break;  // DISABLE, BUMPENVMAP(LUMINANCE)
            case 2: case 17: used = 2; break;           // SELECTARG1, PREMODULATE
            case 3: used = 4; break;                    // SELECTARG2
            case 25: case 26: used = 7; break;          // MULTIPLYADD, LERP
          }
          if (((used & 1) && (t[26] & 0xF) == 2) || ((used & 2) && (t[2] & 0xF) == 2) ||
              ((used & 4) && (t[3] & 0xF) == 2))
            u.color[s][0] = 1;
        }
        u.color[s][1] = int(t[2]);
        u.color[s][2] = int(t[3]);
        u.color[s][3] = int(t[26]);
        u.alpha[s][0] = int(t[4]);
        u.alpha[s][1] = int(t[5]);
        u.alpha[s][2] = int(t[6]);
        u.alpha[s][3] = int(t[27]);
        u.stage[s][0] = int(t[28]);
        u.stage[s][1] = bound ? 1 : 0;
        u.stage[s][3] = bound && textures_.at(st.textures[s]).cube ? 1 : 0;
        uint32_t ttf = t[24], count = ttf & 0xFF;
        u.stage[s][2] = (ttf & 256) && count >= 2 ? int(count) - 1 : -1;
      }
      Argb(u.tfactor, st.rs[60]);
      Argb(u.fog_color, st.rs[34]);
      u.fog[0] = AsFloat(st.rs[36]);
      u.fog[1] = AsFloat(st.rs[37]);
      u.fog[2] = AsFloat(st.rs[38]);
      u.mode[0] = st.rs[15] ? int(st.rs[25]) : 0;
      u.mode[1] = int(st.rs[24] & 0xFF);
      // An alpha test no fragment can fail is turned off, so the draw
      // gets the fragment shader without discard (Pipeline): AlphaPass
      // (ff.frag) would pass anyway. Margins of half a step cover float
      // rounding in sampling and the cascade.
      if (u.mode[0] == 8) {
        u.mode[0] = 0;  // ALWAYS
      } else if (u.mode[0] == 5 || u.mode[0] == 7) {
        const float lo = AlphaLowerBound(u, st) * 255.0f;
        if (lo >= float(u.mode[1]) + (u.mode[0] == 5 ? 1.0f : 0.5f)) u.mode[0] = 0;
      }
      u.mode[2] = st.rs[28] ? (st.rs[35] ? 2 : 1) : 0;
      u.mode[3] = int(st.rs[35]);
      u.mode2[0] = st.rs[9] == 1 ? 1 : 0;
      u.mode2[1] = st.rs[29] ? 1 : 0;
      fs_dirty_ = false;
    }
    // The render target size feeds the vertex shader too.
    vs_cur_.target[0] = float(pass_w_);
    vs_cur_.target[1] = float(pass_h_);
  }

  // A lower bound on the alpha ff.frag's texture-stage cascade can
  // produce (cur.a at the alpha test) for the uniforms `u` of state `st`:
  // 0 where unknown. Diffuse and specular are unknown; TFACTOR is exact; a
  // texture contributes its lowest texel alpha (Tex::alpha_min; filtering
  // blends texels, so it never goes lower), and the border colour's with
  // BORDER addressing. Operations with all arguments non-negative are
  // monotonic, so bounds of the arguments bound the result.
  float AlphaLowerBound(const FsUniforms& u, const FfState& st) const {
    const float tf = float(st.rs[60] >> 24) / 255.0f;
    float cur = 0.0f, temp = 0.0f;
    for (int s = 0; s < 8; s++) {
      const int cop = u.color[s][0];
      if (cop == 1) break;
      float tex = 0.0f;
      if (u.stage[s][1]) tex = TextureMinAlpha(st.textures[s], st.tss[s]);
      auto arg = [&](int a) -> float {
        float v;
        switch (a & 0xF) {
          case 1: v = cur; break;
          case 2: v = tex; break;
          case 3: v = tf; break;
          case 5: v = temp; break;
          default: v = 0.0f; break;  // DIFFUSE, SPECULAR: unknown; others read zero
        }
        if (a & 0x10) v = (a & 0xF) == 3 ? 1.0f - tf : 0.0f;  // COMPLEMENT: TFACTOR's is exact
        return v;
      };
      float al = cur;
      const int aop = u.alpha[s][0];
      if (aop != 1) {
        const float a1 = arg(u.alpha[s][1]), a2 = arg(u.alpha[s][2]);
        switch (aop) {
          case 2: al = a1; break;
          case 3: al = a2; break;
          case 4: al = a1 * a2; break;
          case 5: al = std::min(1.0f, a1 * a2 * 2.0f); break;
          case 6: al = std::min(1.0f, a1 * a2 * 4.0f); break;
          case 7: al = std::min(1.0f, a1 + a2); break;
          default: al = 0.0f; break;
        }
      }
      if (cop == 24) al = 0.0f;  // DOTPRODUCT3 puts a colour into alpha
      if (u.stage[s][0] == 5) temp = al;
      else cur = al;
    }
    return cur;
  }

  // The lowest alpha sampling texture `id` with stage states `tss` can
  // return: 0 unless every level's alpha is known (Tex::alpha_min).
  float TextureMinAlpha(TextureId id, const uint32_t* tss) const {
    auto it = textures_.find(id);
    if (it == textures_.end()) return 0.0f;
    const Tex& t = it->second;
    const uint32_t all = t.levels >= 32 ? ~0u : (1u << t.levels) - 1;
    if (!t.alpha_tracked || (t.alpha_levels & all) != all) return 0.0f;
    uint32_t lo = t.alpha_min;
    if (tss[13] == 4 || tss[14] == 4) lo = std::min(lo, tss[15] >> 24);  // BORDER: BORDERCOLOR
    return float(lo) / 255.0f;
  }

  int PushVs(const VsUniforms& u) {
    if (vs_list_.empty() || std::memcmp(&vs_list_.back(), &u, sizeof u) != 0)
      vs_list_.push_back(u);
    return int(vs_list_.size()) - 1;
  }

  int PushVl(const VsLayout& u) {
    if (vl_list_.empty() || std::memcmp(&vl_list_.back(), &u, sizeof u) != 0)
      vl_list_.push_back(u);
    return int(vl_list_.size()) - 1;
  }

  int PushFs(const FsUniforms& u) {
    if (fs_list_.empty() || std::memcmp(&fs_list_.back(), &u, sizeof u) != 0)
      fs_list_.push_back(u);
    return int(fs_list_.size()) - 1;
  }

  // ---- rectangles

  static Rect Intersect(const Rect& a, const Rect& b) {
    return {std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right),
            std::min(a.bottom, b.bottom)};
  }
  static Rect Clip(const Rect& r, int w, int h) { return Intersect(r, {0, 0, w, h}); }

  static Rect ViewportRect(const FfState& st, int w, int h) {
    Rect v{int32_t(st.viewport.x), int32_t(st.viewport.y), int32_t(st.viewport.x + st.viewport.width),
           int32_t(st.viewport.y + st.viewport.height)};
    return Clip(v, w, h);
  }

  static SDL_GPUViewport D3DViewport(const FfState& st) {
    return {float(st.viewport.x), float(st.viewport.y), float(std::max(st.viewport.width, 1u)),
            float(std::max(st.viewport.height, 1u)), st.viewport.min_z, st.viewport.max_z};
  }
  SDL_GPUViewport FullViewport() const {
    return {0, 0, float(pass_w_), float(pass_h_), 0, 1};
  }

  // Scissor of a draw: the target, the scissor rect if enabled, and for
  // pre-transformed vertices (drawn with a full-target viewport) the
  // viewport. False if nothing is left.
  bool SetupScissor(const FfState& st, Cmd& c, bool rhw) const {
    Rect r{0, 0, pass_w_, pass_h_};
    if (st.rs[174]) r = Intersect(r, st.scissor);
    if (rhw) r = Intersect(r, ViewportRect(st, pass_w_, pass_h_));
    if (r.right <= r.left || r.bottom <= r.top) return false;
    c.scissor = {r.left, r.top, r.right - r.left, r.bottom - r.top};
    return true;
  }

  // Uniform block `slot` of a shader draw (0-2 vertex, 3-5 pixel) in
  // ubytes_; the previous draw's copy when equal. At least 16 bytes.
  void PushUniform(ShaderCmd& sc, int slot, const void* data, uint32_t len) {
    static const uint8_t kZero[16] = {};
    if (!data || !len) {
      data = kZero;
      len = sizeof kZero;
    }
    if (!shader_cmds_.empty()) {
      const ShaderCmd& prev = shader_cmds_.back();
      if (prev.ulen[slot] == len && std::memcmp(ubytes_.data() + prev.uoff[slot], data, len) == 0) {
        sc.uoff[slot] = prev.uoff[slot];
        sc.ulen[slot] = len;
        return;
      }
    }
    size_t off = (ubytes_.size() + 15) & ~size_t(15);
    ubytes_.resize(off + len);
    std::memcpy(ubytes_.data() + off, data, len);
    sc.uoff[slot] = uint32_t(off);
    sc.ulen[slot] = len;
  }

  SDL_GPUGraphicsPipeline* ShaderPipeline(const ShaderKey& k) {
    auto it = shader_pipelines_.find(k);
    if (it != shader_pipelines_.end()) return it->second;
    const uint8_t alpha[3] = {k.src_a, k.dst_a, k.op_a};
    SDL_GPUGraphicsPipeline* p =
        CreatePipeline(k.k, Gpu(shaders_.at(k.vs)), Gpu(shaders_.at(k.ps)), alpha);
    shader_pipelines_[k] = p;
    return p;
  }

  // D3DSAMPLERSTATETYPE values (index 1-13; null: D3D9's defaults).
  static SamplerKey D3D9Sampler(const uint32_t* ss) {
    static const uint32_t kDefaults[14] = {0, 1, 1, 1, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0};
    if (!ss) ss = kDefaults;
    SamplerKey k{};
    // D3DTEXF: NONE 0, POINT 1, LINEAR 2, ANISOTROPIC 3, ...
    k.mag = ss[5] <= 1 ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
    k.min = ss[6] <= 1 ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
    k.mip = ss[7] == 1 ? SDL_GPU_SAMPLERMIPMAPMODE_NEAREST
          : ss[7] >= 2 ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : 0xFF;
    k.u = uint8_t(D3DAddressMode(ss[1]));
    k.v = uint8_t(D3DAddressMode(ss[2]));
    k.w = uint8_t(D3DAddressMode(ss[3]));
    bool aniso = ss[5] == 3 || ss[6] == 3;
    k.aniso = uint8_t(aniso ? std::clamp<uint32_t>(ss[10], 1, 16) : 1);
    k.lod_bias = AsFloat(ss[8]);
    k.min_lod = float(ss[9]);  // MAXMIPLEVEL: the most detailed level used
    return k;
  }

  // State the translated-shader path does not model yet, named once.
  void WarnUnsupportedShaderState(const FfState& st) {
    struct Check {
      uint32_t rs;
      const char* name;
    };
    static const Check kChecks[] = {
        {185, "TWOSIDEDSTENCILMODE"},
    };
    for (const Check& ch : kChecks) {
      if (!st.rs[ch.rs] || (warned_rs_ & (1ull << (&ch - kChecks)))) continue;
      warned_rs_ |= 1ull << (&ch - kChecks);
      WarnF("gfx: %s with shaders is not supported yet\n", ch.name);
    }
  }

  // ---- programs

  struct Program {
    bool compute = false;
    ProgramStage vs, fs, cs;  // without their GLSL
    SDL_GPUShader* vsh = nullptr;
    SDL_GPUShader* fsh = nullptr;
    SDL_GPUComputePipeline* cp = nullptr;
  };
  // A stage of a recorded program draw: its textures, its uniform blocks
  // in ubytes_ (the buffer table, then the program's own) and whether it
  // reads the arena (pmem_).
  struct ProgramStageCmd {
    SDL_GPUTexture* tex[16] = {};
    SDL_GPUSampler* smp[16] = {};
    uint32_t ntex = 0, nuni = 0;
    uint32_t uoff[2] = {}, ulen[2] = {};
    bool storage = false;
  };
  struct ProgramCmd {
    SDL_GPUGraphicsPipeline* pipeline = nullptr;
    uint32_t instances = 1;
    SDL_FColor blend_constant{};
    ProgramStageCmd stage[2];  // vertex, fragment
  };

  void ReleaseProgram(Program& p) {
    if (p.vsh) SDL_ReleaseGPUShader(dev_, p.vsh);
    if (p.fsh) SDL_ReleaseGPUShader(dev_, p.fsh);
    if (p.cp) SDL_ReleaseGPUComputePipeline(dev_, p.cp);
  }

  // A program stage's code as SDL_GPU takes it: its SPIR-V, or on Metal
  // the MSL SPIRV-Cross makes of it.
  struct ProgramCode {
    explicit ProgramCode(std::vector<uint32_t> s) : spirv(std::move(s)) {
#if defined(__APPLE__)
      std::string log;
      if (!SpirvToMsl(spirv, &msl, &log)) FatalF("gfx: a program's SPIR-V to MSL: %s", log.c_str());
      code = reinterpret_cast<const Uint8*>(msl.c_str());
      size = msl.size();
      entry = "main0";
      format = SDL_GPU_SHADERFORMAT_MSL;
#else
      code = reinterpret_cast<const Uint8*>(spirv.data());
      size = spirv.size() * 4;
#endif
    }
    std::vector<uint32_t> spirv;
    std::string msl;
    const Uint8* code = nullptr;
    size_t size = 0;
    const char* entry = "main";
    SDL_GPUShaderFormat format = SDL_GPU_SHADERFORMAT_SPIRV;
  };

  static uint32_t ProgramUniformBlocks(const ProgramStage& st) {
    return (st.buffers || st.rw_buffers ? 1 : 0) + (st.uniform_bytes ? 1 : 0);
  }

  // The stage's prelude (ProgramDesc) in SDL_GPU's SPIR-V sets, and its
  // SPIR-V. On Metal the binding numbers are the MSL indices SDL_GPU
  // expects (SpirvToMsl keeps them; sets do not matter): textures from 0
  // and storage images after them; uniform buffers from 0, then the
  // read-only storage buffers, then the read-write one.
  static std::vector<uint32_t> CompileProgramStage(const ProgramStage& st, ShaderStage stage,
                                                   const uint32_t* local_size) {
    if (st.textures > 16) FatalF("gfx: a program stage with %u textures", st.textures);
#if defined(__APPLE__)
    const bool metal = true;
#else
    const bool metal = false;
#endif
    const int res = metal ? 0 : stage == ShaderStage::Fragment ? 2 : 0;
    const int uni = metal ? 0 : stage == ShaderStage::Vertex ? 1 : stage == ShaderStage::Fragment ? 3 : 2;
    const uint32_t nuni = ProgramUniformBlocks(st);
    const uint32_t mem = metal ? nuni : st.textures;
    char b[256];
    std::string pre = "#version 450\n";
    std::snprintf(b, sizeof b, "#define R_TEXTURE(n) set = %d, binding = (n)\n", res);
    pre += b;
    if (st.buffers) {
      std::snprintf(b, sizeof b,
                    "layout(std430, set = %d, binding = %u) readonly buffer RMem { uint r_mem[]; };\n"
                    "layout(std430, set = %d, binding = %u) readonly buffer RHeap { uint r_heap[]; };\n",
                    res, mem, res, mem + 1);
      pre += b;
    }
    if (stage == ShaderStage::Compute) {
      if (metal)
        std::snprintf(b, sizeof b, "#define R_IMAGE(n) set = 0, binding = (%u + (n))\n", st.textures);
      else
        std::snprintf(b, sizeof b, "#define R_IMAGE(n) set = 1, binding = (n)\n");
      pre += b;
      if (st.rw_buffers) {
        std::snprintf(b, sizeof b, "layout(std430, set = %d, binding = %u) buffer RRw { uint r_rw[]; };\n",
                      metal ? 0 : 1, metal ? nuni + (st.buffers ? 2 : 0) : st.rw_images);
        pre += b;
      }
      std::snprintf(b, sizeof b, "layout(local_size_x = %u, local_size_y = %u, local_size_z = %u) in;\n",
                    local_size[0], local_size[1], local_size[2]);
      pre += b;
    }
    uint32_t binding = 0;
    if (st.buffers || st.rw_buffers) {
      std::snprintf(b, sizeof b, "layout(std140, set = %d, binding = 0) uniform RBufs {", uni);
      pre += b;
      if (st.buffers) {
        std::snprintf(b, sizeof b, " uvec4 r_buf[%u];", st.buffers);
        pre += b;
      }
      if (st.rw_buffers) {
        std::snprintf(b, sizeof b, " uvec4 r_rwbuf[%u];", st.rw_buffers);
        pre += b;
      }
      pre += " };\n";
      binding = 1;
    }
    // Buffer b's word i: in the arena (r_mem) or, a GPU buffer, the heap.
    if (st.buffers)
      pre += "uint r_ld(uint b, uint i) {\n  uint w = r_buf[b].x + i;\n"
             "  return r_buf[b].z != 0u ? r_heap[w] : r_mem[w];\n}\n";
    std::snprintf(b, sizeof b, "#define R_UNIFORMS set = %d, binding = %u\n", uni, binding);
    pre += b;
    const std::string glsl = pre + st.glsl;
    std::vector<uint32_t> spirv;
    std::string log;
    if (!GlslToSpirv(glsl, stage, &spirv, &log)) {
      std::fprintf(stderr, "%s\n", glsl.c_str());
      FatalF("gfx: compiling a program (source above): %s", log.c_str());
    }
    return spirv;
  }

  struct GpuBuffer {
    uint32_t at;  // its place in heap_
    uint32_t size;
  };
  struct BufferCopy {
    SDL_GPUBuffer* buffer;
    uint32_t at;    // its offset in an arena
    uint32_t size;  // bytes
    uint32_t held = 0;  // the bytes it takes in the arena
    uint32_t from = 0;  // its offset in the buffer
  };
  // A GPU buffer's `size` bytes from `from` (no more than it has) and
  // their place in an arena.
  BufferCopy GpuBufferCopy(BufferId id, size_t at, uint32_t size, uint32_t from = 0) {
    auto it = gpu_buffers_.find(id);
    if (it == gpu_buffers_.end() || from > it->second.size) FatalF("gfx: no buffer %u", id);
    return {heap_, uint32_t(at), std::min(size, it->second.size - from) & ~3u, 0,
            it->second.at + from};
  }
  // Copies GPU buffers into an arena (in) or the arena back into them.
  static void CopyBuffers(SDL_GPUCopyPass* cp, const std::vector<BufferCopy>& copies,
                          SDL_GPUBuffer* arena, bool in) {
    for (const BufferCopy& c : copies) {
      if (!c.size) continue;
      SDL_GPUBufferLocation a{arena, c.at}, b{c.buffer, c.from};
      if (in) SDL_CopyGPUBufferToBuffer(cp, &b, &a, c.size, false);
      else SDL_CopyGPUBufferToBuffer(cp, &a, &b, c.size, false);
    }
  }

  // GPU buffers are places in one, the heap: draws and dispatches read
  // them there (bound beside their arenas, a table entry's z set) instead
  // of copies in the arenas, and draws take indices and indirect
  // arguments from them. Copies to and from the arenas (read-write
  // buffers, uploads) are of their places. It grows by doubling, after a
  // flush (the recorded draws name it).
  static constexpr uint32_t kHeapAlign = 256;
  uint32_t HeapAlloc(uint32_t size) {
    size = (size + kHeapAlign - 1) & ~(kHeapAlign - 1);
    for (;;) {
      for (auto it = heap_free_.begin(); it != heap_free_.end(); ++it) {
        if (it->second < size) continue;
        const uint32_t at = it->first, left = it->second - size;
        heap_free_.erase(it);
        if (left) heap_free_[at + size] = left;
        return at;
      }
      GrowHeap(size);
    }
  }
  void HeapFree(uint32_t at, uint32_t size) {
    size = (size + kHeapAlign - 1) & ~(kHeapAlign - 1);
    auto next = heap_free_.lower_bound(at);
    if (next != heap_free_.end() && at + size == next->first) {
      size += next->second;
      next = heap_free_.erase(next);
    }
    if (next != heap_free_.begin()) {
      auto prev = std::prev(next);
      if (prev->first + prev->second == at) {
        prev->second += size;
        return;
      }
    }
    heap_free_[at] = size;
  }
  void GrowHeap(uint32_t need) {
    const uint64_t cap = std::max<uint64_t>(heap_cap_ ? 2ull * heap_cap_ : 64u << 20,
                                            uint64_t(heap_cap_) + need);
    if (cap > (2ull << 30)) FatalF("gfx: GPU buffers past 2 GiB");
    Flush();
    SDL_GPUBufferCreateInfo bi{};
    bi.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ |
               SDL_GPU_BUFFERUSAGE_INDEX | SDL_GPU_BUFFERUSAGE_INDIRECT;
    bi.size = Uint32(cap);
    SDL_GPUBuffer* b = SDL_CreateGPUBuffer(dev_, &bi);
    if (!b) FatalF("gfx: cannot create a %u-byte buffer: %s", bi.size, SDL_GetError());
    if (heap_) {
      SDL_GPUCommandBuffer* cmd = Begin();
      SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
      SDL_GPUBufferLocation from{heap_, 0}, to{b, 0};
      SDL_CopyGPUBufferToBuffer(cp, &from, &to, heap_cap_, false);
      SDL_EndGPUCopyPass(cp);
      Submit(cmd);
      SDL_ReleaseGPUBuffer(dev_, heap_);
    }
    HeapFree(heap_cap_, uint32_t(cap - heap_cap_));
    heap_ = b;
    heap_cap_ = uint32_t(cap);
  }
  // A read-only GPU buffer's table entry: its words' place in the heap,
  // its bytes (no more than it has), and z set.
  void HeapEntry(const BufferData& bd, uint32_t* e) {
    auto it = gpu_buffers_.find(bd.buffer);
    if (it == gpu_buffers_.end() || bd.offset > it->second.size) FatalF("gfx: no buffer %u", bd.buffer);
    e[0] = (it->second.at + bd.offset) / 4;
    e[1] = std::min(bd.size, it->second.size - bd.offset);
    e[2] = 1;
  }
  void DrawRead(BufferId id) {
    if (draw_read_.empty() || draw_read_.back() != id) draw_read_.push_back(id);
  }

  // Appends size bytes of data, padded to words with zeros; without data,
  // zeros (an Arena: room left as it is, for a GPU buffer's copy).
  template <typename V>
  static void AppendWords(V& v, const void* data, uint32_t size) {
    const size_t at = v.size(), n = (size_t(size) + 3) & ~size_t(3);
    v.resize(at + n);
    if (data && size) {
      std::memcpy(v.data() + at, data, size);
      std::memset(v.data() + at + size, 0, n - size);
    }
  }

  // A stage's textures (or depth targets), and for a draw its buffers
  // (into pmem_, their table and its own uniforms into ubytes_).
  void RecordStage(ProgramStageCmd& sc, const ProgramStage& st, const ProgramResources& r,
                   bool draw = false) {
    sc.ntex = st.textures;
    for (uint32_t i = 0; i < st.textures; i++) {
      const TextureId id = r.textures ? r.textures[i] : 0;
      SDL_GPUTexture* t = nullptr;
      if (auto it = textures_.find(id); it != textures_.end()) t = it->second.tex;
      else if (auto dt = depths_.find(id); dt != depths_.end()) t = dt->second.tex;
      if (!t) {
        switch (st.texture_kinds[i]) {
          case TextureKind::Cube: t = dummy_cube_; break;
          case TextureKind::Tex3D: t = dummy_3d_; break;
          case TextureKind::Tex2DArray: t = dummy_array_; break;
          default: t = dummy_2d_; break;
        }
      }
      sc.tex[i] = t;
      sc.smp[i] = ProgramSampler(r.samplers ? r.samplers[i] : SamplerDesc{});
    }
    if (!draw) return;
    if (st.buffers) {
      std::vector<uint32_t> table(4 * size_t(st.buffers), 0);
      for (uint32_t b = 0; b < st.buffers; b++) {
        const BufferData& bd = r.buffers[b];
        if (bd.buffer) {
          HeapEntry(bd, &table[4 * b]);
          DrawRead(bd.buffer);
          continue;
        }
        if (pmem_.size() + bd.size > kMaxArena) FatalF("gfx: a draw's buffers pass the arena");
        table[4 * b] = uint32_t(DrawBuffer(bd) / 4);
        table[4 * b + 1] = bd.size;
      }
      if (pmem_.empty()) AppendWords(pmem_, nullptr, 4);  // pbuf_ is bound
      sc.storage = true;
      AppendUniform(sc, table.data(), uint32_t(table.size() * 4));
    }
    if (st.uniform_bytes) AppendUniform(sc, r.uniforms, st.uniform_bytes);
  }

  // A draw's host buffer in the arena: its byte offset there. Buffers
  // that overlap one copied earlier in the same draw (a vertex buffer's
  // interleaved streams, each bound from its own offset) share its copy:
  // inside it, they are found there; past its end, if it is the arena's
  // last, it grows by what they add.
  size_t DrawBuffer(const BufferData& bd) {
    const auto* p = static_cast<const uint8_t*>(bd.data);
    if (p && bd.size) {
      for (DrawSpan& sp : draw_spans_) {
        if (p < sp.p || (p - sp.p) % 4) continue;
        const size_t off = size_t(p - sp.p), end = off + bd.size;
        if (end <= sp.n) return sp.at + off;
        if (off > sp.n || sp.at + ((sp.n + 3) & ~size_t(3)) != pmem_.size()) continue;
        pmem_.resize(sp.at + ((end + 3) & ~size_t(3)), 0);
        std::memcpy(pmem_.data() + sp.at + sp.n, sp.p + sp.n, end - sp.n);
        sp.n = end;
        return sp.at + off;
      }
    }
    const size_t at = pmem_.size();
    AppendWords(pmem_, bd.data, bd.size);
    if (!p) std::memset(pmem_.data() + at, 0, pmem_.size() - at);
    if (p && bd.size) draw_spans_.push_back({p, at, bd.size});
    return at;
  }

  // The parts of an arena of `size` bytes that come from the host: all
  // but the places of the GPU buffers copied into it (in arena order).
  template <typename F>
  static void HostSpans(const std::vector<BufferCopy>& copies, size_t size, F f) {
    size_t at = 0;
    for (const BufferCopy& c : copies) {
      if (c.at > at) f(at, c.at - at);
      at = std::max(at, size_t(c.at) + c.held);
    }
    if (at < size) f(at, size - at);
  }

  void AppendUniform(ProgramStageCmd& sc, const void* data, uint32_t len) {
    const size_t off = (ubytes_.size() + 15) & ~size_t(15);
    ubytes_.resize(off + std::max(len, 16u), 0);
    if (data) std::memcpy(ubytes_.data() + off, data, len);
    sc.uoff[sc.nuni] = uint32_t(off);
    sc.ulen[sc.nuni] = len;
    sc.nuni++;
  }

  SDL_GPUSampler* ProgramSampler(const SamplerDesc& d) {
    ProgramSamplerKey k{};
    k.min = d.min_linear ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    k.mag = d.mag_linear ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    k.mip = d.mip_linear ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    k.u = uint8_t(d.u);
    k.v = uint8_t(d.v);
    k.w = uint8_t(d.w);
    k.compare = d.compare;
    k.compare_op = uint8_t(uint8_t(d.compare_op) + 1);
    k.lod_bias = d.lod_bias;
    k.min_lod = d.min_lod;
    k.max_lod = d.max_lod;
    k.aniso = d.max_anisotropy;
    auto it = program_samplers_.find(k);
    if (it != program_samplers_.end()) return it->second;
    SDL_GPUSamplerCreateInfo si{};
    si.min_filter = SDL_GPUFilter(k.min);
    si.mag_filter = SDL_GPUFilter(k.mag);
    si.mipmap_mode = SDL_GPUSamplerMipmapMode(k.mip);
    si.address_mode_u = SDL_GPUSamplerAddressMode(k.u);
    si.address_mode_v = SDL_GPUSamplerAddressMode(k.v);
    si.address_mode_w = SDL_GPUSamplerAddressMode(k.w);
    si.mip_lod_bias = k.lod_bias;
    si.min_lod = k.min_lod;
    si.max_lod = k.max_lod;
    si.enable_anisotropy = k.aniso > 1;
    si.max_anisotropy = k.aniso;
    si.enable_compare = k.compare;
    si.compare_op = SDL_GPUCompareOp(k.compare_op);
    SDL_GPUSampler* s = SDL_CreateGPUSampler(dev_, &si);
    if (!s) FatalF("gfx: SDL_CreateGPUSampler: %s", SDL_GetError());
    program_samplers_[k] = s;
    return s;
  }

  SDL_GPUGraphicsPipeline* ProgramPipeline(ProgramId id, const PipelineState& st, int targets,
                                           const std::array<uint8_t, 4>& fmts,
                                           const Depth* depth) {
    ProgramKey k{};
    k.program = id;
    k.topology = uint8_t(st.topology);
    k.cull = uint8_t(st.cull);
    k.front_ccw = st.front_ccw;
    k.depth_test = st.depth_test && depth;
    k.depth_write = st.depth_write && depth;
    k.depth_clip = st.depth_clip;
    k.depth_compare = uint8_t(st.depth_compare);
    k.stencil = st.stencil && depth && depth->stencil;
    for (int f = 0; f < 2; f++) {
      const StencilFace& s = f ? st.stencil_back : st.stencil_front;
      k.s_ops[f][0] = uint8_t(s.fail);
      k.s_ops[f][1] = uint8_t(s.depth_fail);
      k.s_ops[f][2] = uint8_t(s.pass);
      k.s_ops[f][3] = uint8_t(s.compare);
    }
    k.s_read = st.stencil_read_mask;
    k.s_write = st.stencil_write_mask;
    k.targets = uint8_t(targets);
    k.depth_format = depth ? uint8_t(depth->gpu_format) : 0;
    for (int i = 0; i < targets; i++) {
      const TargetBlend& b = st.blend[i];
      k.fmt[i] = fmts[size_t(i)];
      k.blend[i][0] = b.enable;
      k.blend[i][1] = uint8_t(b.src_color);
      k.blend[i][2] = uint8_t(b.dst_color);
      k.blend[i][3] = uint8_t(b.color_op);
      k.blend[i][4] = uint8_t(b.src_alpha);
      k.blend[i][5] = uint8_t(b.dst_alpha);
      k.blend[i][6] = uint8_t(b.alpha_op);
      k.blend[i][7] = b.write_mask & 0xF;
    }
    k.bias = st.depth_bias;
    k.bias_slope = st.depth_bias_slope;
    k.bias_clamp = st.depth_bias_clamp;
    auto it = program_pipelines_.find(k);
    if (it != program_pipelines_.end()) return it->second;

    const Program& pr = programs_.at(id);
    SDL_GPUGraphicsPipelineCreateInfo pi{};
    pi.vertex_shader = pr.vsh;
    pi.fragment_shader = pr.fsh;
    static const SDL_GPUPrimitiveType kTopology[] = {
        SDL_GPU_PRIMITIVETYPE_POINTLIST, SDL_GPU_PRIMITIVETYPE_LINELIST,
        SDL_GPU_PRIMITIVETYPE_LINESTRIP, SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP};
    pi.primitive_type = kTopology[k.topology];
    pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pi.rasterizer_state.cull_mode = SDL_GPUCullMode(k.cull);
    pi.rasterizer_state.front_face =
        k.front_ccw ? SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE : SDL_GPU_FRONTFACE_CLOCKWISE;
    pi.rasterizer_state.enable_depth_bias = k.bias != 0 || k.bias_slope != 0;
    pi.rasterizer_state.depth_bias_constant_factor = k.bias;
    pi.rasterizer_state.depth_bias_slope_factor = k.bias_slope;
    pi.rasterizer_state.depth_bias_clamp = k.bias_clamp;
    pi.rasterizer_state.enable_depth_clip = k.depth_clip;
    pi.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    pi.depth_stencil_state.enable_depth_test = k.depth_test;
    pi.depth_stencil_state.enable_depth_write = k.depth_write;
    pi.depth_stencil_state.compare_op = SDL_GPUCompareOp(k.depth_compare + 1);
    pi.depth_stencil_state.enable_stencil_test = k.stencil;
    for (int f = 0; f < 2; f++) {
      SDL_GPUStencilOpState& s =
          f ? pi.depth_stencil_state.back_stencil_state : pi.depth_stencil_state.front_stencil_state;
      s.fail_op = SDL_GPUStencilOp(k.s_ops[f][0] + 1);
      s.depth_fail_op = SDL_GPUStencilOp(k.s_ops[f][1] + 1);
      s.pass_op = SDL_GPUStencilOp(k.s_ops[f][2] + 1);
      s.compare_op = SDL_GPUCompareOp(k.s_ops[f][3] + 1);
    }
    pi.depth_stencil_state.compare_mask = k.s_read;
    pi.depth_stencil_state.write_mask = k.s_write;
    SDL_GPUColorTargetDescription cds[4]{};
    for (int i = 0; i < targets; i++) {
      SDL_GPUColorTargetDescription& cd = cds[i];
      cd.format = SDL_GPUTextureFormat(k.fmt[i]);
      SDL_GPUColorTargetBlendState& b = cd.blend_state;
      b.enable_blend = k.blend[i][0];
      b.src_color_blendfactor = SDL_GPUBlendFactor(k.blend[i][1] + 1);
      b.dst_color_blendfactor = SDL_GPUBlendFactor(k.blend[i][2] + 1);
      b.color_blend_op = SDL_GPUBlendOp(k.blend[i][3] + 1);
      b.src_alpha_blendfactor = SDL_GPUBlendFactor(k.blend[i][4] + 1);
      b.dst_alpha_blendfactor = SDL_GPUBlendFactor(k.blend[i][5] + 1);
      b.alpha_blend_op = SDL_GPUBlendOp(k.blend[i][6] + 1);
      b.color_write_mask = SDL_GPUColorComponentFlags(k.blend[i][7]);
      b.enable_color_write_mask = true;
    }
    pi.target_info.color_target_descriptions = cds;
    pi.target_info.num_color_targets = Uint32(targets);
    pi.target_info.has_depth_stencil_target = depth != nullptr;
    pi.target_info.depth_stencil_format = depth ? depth->gpu_format : SDL_GPU_TEXTUREFORMAT_INVALID;
    const Uint64 t0 = SDL_GetTicksNS();
    SDL_GPUGraphicsPipeline* p = SDL_CreateGPUGraphicsPipeline(dev_, &pi);
    if (!p) FatalF("gfx: SDL_CreateGPUGraphicsPipeline (program): %s", SDL_GetError());
    AddStall(Stall::Pipeline, SDL_GetTicksNS() - t0);
    program_pipelines_[k] = p;
    return p;
  }

  // DrawImage's: a triangle over the back buffer fetching the image's
  // texels where the region maps (clamped to it), encoded back to sRGB for
  // sRGB images, so the back buffer holds the stored values.
  void CreatePresentPipeline() {
    static const char* kVs =
        "#version 450\n"
        "void main() {\n"
        "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
        "  gl_Position = vec4(p * vec2(2, -2) + vec2(-1, 1), 0, 1);\n"
        "}\n";
    static const char* kFs =
        "#version 450\n"
        "layout(set = 2, binding = 0) uniform sampler2D img;\n"
        "layout(std140, set = 3, binding = 0) uniform U { ivec4 rect; ivec4 srgb; };\n"
        "layout(location = 0) out vec4 o;\n"
        "void main() {\n"
        "  ivec2 at = clamp(ivec2(gl_FragCoord.xy) + rect.xy, rect.xy, rect.zw);\n"
        "  vec4 c = texelFetch(img, at, 0);\n"
        "  if (srgb.x != 0) {\n"
        "    bvec3 lo = lessThanEqual(c.rgb, vec3(0.0031308));\n"
        "    c.rgb = mix(1.055 * pow(c.rgb, vec3(1.0 / 2.4)) - 0.055, c.rgb * 12.92, lo);\n"
        "  }\n"
        "  o = c;\n"
        "}\n";
    SDL_GPUShader* sh[2];
    for (int i = 0; i < 2; i++) {
      std::vector<uint32_t> spirv;
      std::string log;
      if (!GlslToSpirv(i ? kFs : kVs, i ? ShaderStage::Fragment : ShaderStage::Vertex, &spirv, &log))
        FatalF("gfx: the present shader: %s", log.c_str());
      SDL_GPUShaderCreateInfo si{};
#if defined(__APPLE__)
      // Metal takes MSL (binding numbers kept: texture 0, uniform buffer 0).
      std::string msl;
      if (!SpirvToMsl(spirv, &msl, &log)) FatalF("gfx: the present shader: %s", log.c_str());
      si.code = reinterpret_cast<const Uint8*>(msl.c_str());
      si.code_size = msl.size();
      si.entrypoint = "main0";
      si.format = SDL_GPU_SHADERFORMAT_MSL;
#else
      si.code = reinterpret_cast<const Uint8*>(spirv.data());
      si.code_size = spirv.size() * 4;
      si.entrypoint = "main";
      si.format = SDL_GPU_SHADERFORMAT_SPIRV;
#endif
      si.stage = i ? SDL_GPU_SHADERSTAGE_FRAGMENT : SDL_GPU_SHADERSTAGE_VERTEX;
      si.num_samplers = i ? 1 : 0;
      si.num_uniform_buffers = i ? 1 : 0;
      sh[i] = SDL_CreateGPUShader(dev_, &si);
      if (!sh[i]) FatalF("gfx: the present shader: %s", SDL_GetError());
    }
    SDL_GPUGraphicsPipelineCreateInfo pi{};
    pi.vertex_shader = sh[0];
    pi.fragment_shader = sh[1];
    pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pi.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUColorTargetDescription cd{};
    cd.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    pi.target_info.color_target_descriptions = &cd;
    pi.target_info.num_color_targets = 1;
    present_pipeline_ = SDL_CreateGPUGraphicsPipeline(dev_, &pi);
    if (!present_pipeline_) FatalF("gfx: the present pipeline: %s", SDL_GetError());
    SDL_ReleaseGPUShader(dev_, sh[0]);
    SDL_ReleaseGPUShader(dev_, sh[1]);
  }

  // ---- members

  std::recursive_mutex mu_;
  SDL_Window* window_;
  SDL_GPUDevice* dev_ = nullptr;
  SDL_GPUShader* vs_ = nullptr;
  SDL_GPUShader* fs_ = nullptr;
  SDL_GPUShader* fs_alpha_test_ = nullptr;  // ff.frag with ALPHA_TEST
  // Fixed-function fragment variants (FfPipeline): created and looked up
  // on the device's thread; the worker fills in their shaders.
  std::unordered_map<FsVariantKey, FsVariant, BytesHash<FsVariantKey>> variants_;
  std::mutex variant_mu_;
  std::condition_variable variant_cv_;
  std::deque<VariantJob> variant_jobs_;
  bool variant_quit_ = false;
  std::thread variant_worker_;
  // The translated shaders' compile pool (CompileAsync).
  std::mutex compile_mu_;
  std::condition_variable compile_cv_;
  std::deque<std::shared_ptr<CompileJob>> compile_jobs_;
  std::vector<std::thread> compile_threads_;
  bool compile_quit_ = false;
  enum class VariantMode { Async, Sync, Off } variant_mode_ = VariantMode::Async;
  int variant_fs_ = -1;  // FfPipeline's last lookup: fs_list_ index, ...
  bool variant_discard_ = false;
  FsVariant* variant_last_ = nullptr;
  SDL_GPUTextureFormat depth24_ = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
  SDL_GPUTextureFormat depth24s8_ = SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
  SDL_GPUTexture* backbuffer_ = nullptr;  // the game's back buffer
  SDL_GPUTexture* front_ = nullptr;       // the DirectDraw primary
  SDL_GPUTexture* dummy_ = nullptr;
  int bb_w_ = 0, bb_h_ = 0;
  // Mailbox emulated on vsync (ResetSwapChain, Show).
  bool mailbox_emulated_ = false;
  uint64_t refresh_ns_ = 0, next_show_ns_ = 0;
  std::map<TextureId, Tex> textures_;
  std::map<DepthId, Depth> depths_;
  uint32_t next_id_ = 1;
  TextureId current_color_ = 0;
  int current_level_ = 0;
  std::array<ColorTarget, 3> current_extra_{};  // targets 1-3
  DepthId current_depth_ = 0;

  // The pass being recorded: pass_color_ is target 0 of pass_colors_.
  std::array<SDL_GPUTexture*, 4> pass_colors_{};
  std::array<int, 4> pass_levels_{};  // mip level of each
  std::array<uint8_t, 4> pass_fmts_{};
  int pass_targets_ = 0;
  SDL_GPUTexture* pass_color_ = nullptr;
  SDL_GPUTexture* pass_depth_ = nullptr;
  int pass_depth_layer_ = 0;
  int pass_w_ = 0, pass_h_ = 0;
  bool pass_alpha_ = false, pass_stencil_ = false;
  SDL_GPUTextureFormat pass_depth_format_ = SDL_GPU_TEXTUREFORMAT_INVALID;
  float pass_bias_scale_ = 1.0f;
  bool pass_clear_color_ = false, pass_clear_depth_ = false, pass_clear_stencil_ = false;
  SDL_FColor clear_color_{0, 0, 0, 1};
  float clear_depth_ = 1;
  uint8_t clear_stencil_ = 0;
  std::vector<Cmd> cmds_;
  std::vector<VsUniforms> vs_list_;
  std::vector<VsLayout> vl_list_;
  std::vector<FsUniforms> fs_list_;
  std::vector<uint8_t, NoZero<uint8_t>> verts_;
  int32_t zeros_at_ = -1;  // 64 zero bytes in verts_ (PlaceStreams), -1 if none yet
  // Persistent vertices (PlacePersistent).
  struct PvKey {
    uint32_t lock, stride, bytes;
    uintptr_t lo;
    bool operator==(const PvKey&) const = default;
  };
  struct PvHash {
    size_t operator()(const PvKey& k) const {
      return size_t(uint64_t(k.lo) * 0x9E3779B97F4A7C15ull ^ (uint64_t(k.lock) << 32 | k.bytes) ^
                    (uint64_t(k.stride) << 20));
    }
  };
  struct PvUpload {
    uint32_t at, from, size;  // pvbuf_ offset, pv_staging_ offset, bytes
  };
  static constexpr uint32_t kPvCap = 256u << 20;
  enum class PvMode { Off, On, Check } pv_mode_ = PvMode::On;
  SDL_GPUBuffer* pvbuf_ = nullptr;
  uint32_t pv_head_ = 0;
  uint64_t pv_frame_ = 0;
  bool pv_full_ = false;
  bool placed_pv_ = false;  // PlaceStreams' draw is in pvbuf_
  std::unordered_map<PvKey, uint32_t, PvHash> pv_at_;     // place in pvbuf_
  std::unordered_map<PvKey, uint64_t, PvHash> pv_seen_;   // frame first drawn
  std::unordered_map<PvKey, std::vector<uint8_t>, PvHash> pv_shadow_;  // FF_VCACHE=check
  // Persistent indices (PlaceIndices).
  struct PiKey {
    uint32_t lock, count, size, lo, hi;
    uintptr_t src;
    bool operator==(const PiKey&) const = default;
  };
  struct PiHash {
    size_t operator()(const PiKey& k) const {
      return size_t(uint64_t(k.src) * 0x9E3779B97F4A7C15ull ^ (uint64_t(k.lock) << 32 | k.count) ^
                    (uint64_t(k.lo) << 20) ^ (uint64_t(k.hi) << 40) ^ k.size);
    }
  };
  std::unordered_map<PiKey, uint32_t, PiHash> pi_at_;    // place in pvbuf_
  std::unordered_map<PiKey, uint64_t, PiHash> pi_seen_;  // frame first drawn
  std::unordered_map<PiKey, std::vector<uint8_t>, PiHash> pi_shadow_;  // FF_VCACHE=check
  std::vector<uint8_t> pv_staging_;
  std::vector<PvUpload> pv_uploads_;
  // Locked streams placed since the last flush (PlaceStreams), the last
  // few: a lock's stages reuse the vertices they share.
  struct Locked {
    uint32_t lock = 0;
    const void* data = nullptr;
    uint32_t stride = 0, first = 0, count = 0, size = 0;
    int32_t at = 0;
  };
  // Stream ranges copied this pass, by lock ticket (FfStream::lock): a
  // frame's draws often reuse a few vertex buffers, each draw a range of
  // one, so this holds enough to catch the repeats across them.
  std::array<Locked, 64> locked_{};
  size_t locked_next_ = 0;
  std::vector<uint32_t, NoZero<uint32_t>> indices_;
  std::vector<Upload> uploads_;
  std::vector<uint8_t> upload_bytes_;
  std::vector<BlitTex> blit_pool_;
  size_t blit_used_ = 0;

  SDL_GPUBuffer* vbuf_ = nullptr;
  SDL_GPUBuffer* ibuf_ = nullptr;
  uint32_t vbuf_cap_ = 0, ibuf_cap_ = 0;
  SDL_GPUTransferBuffer* upload_tb_ = nullptr;
  static constexpr size_t kMaxInFlight = 32;
  static constexpr uint32_t kBatch = 16;
  SDL_GPUCommandBuffer* open_ = nullptr;  // Begin's
  uint32_t open_units_ = 0;
  struct InFlight {
    SDL_GPUFence* fence;
    uint64_t upload_end;  // upload_head_ when it was submitted
    uint64_t submission;
  };
  std::deque<InFlight> in_flight_;  // submitted, not known finished
  uint64_t upload_head_ = 0, upload_tail_ = 0;  // ring positions, not wrapped
  Uint32 upload_at_ = 0;                        // MapUpload's offset
  uint64_t submitted_ = 0, retired_ = 0;        // submissions, in order
  // A dispatch's read-write buffers for the host, downloaded by its
  // submission and written back once that has finished.
  struct Landing {
    struct Out {
      void* data;
      uint32_t at, size;
    };
    SDL_GPUTransferBuffer* tb = nullptr;
    Uint32 cap = 0;
    uint64_t submission = 0;
    bool later = false;  // land_later
    std::vector<Out> out;
  };
  // ---- occlusion queries: a counter per query in occl_buf_ (a ring of
  // kOcclSlots), zeroed by the flush after its BeginOcclusion, added to by
  // the counting variants of the pixel shaders drawn while it is open, and
  // read back by every flush that zeroed or counted (occl_reads_).
  static constexpr uint32_t kOcclSlots = 4096;
  struct OcclQuery {
    uint32_t slot = 0;
    uint64_t end_flush = 0;  // the flush its last draw is in (0: still open)
  };
  struct OcclRead {
    uint64_t flush = 0;       // the flush whose counters it holds
    uint64_t submission = 0;  // the submission that copies them
    SDL_GPUTransferBuffer* tb = nullptr;
  };
  SDL_GPUBuffer* occl_buf_ = nullptr;
  SDL_GPUTransferBuffer* occl_zeros_ = nullptr;
  std::unordered_map<uint32_t, OcclQuery> occl_;
  std::vector<uint32_t> occl_zero_;  // slots to zero before the next flush's draws
  std::deque<OcclRead> occl_reads_;
  std::vector<SDL_GPUTransferBuffer*> occl_free_;
  uint32_t occl_open_ = 0, occl_next_id_ = 1, occl_next_slot_ = 0;
  bool occl_draws_ = false;  // this flush has counting draws
  uint64_t flushes_ = 0;

  std::deque<Landing> landings_;
  std::vector<Landing> download_pool_;  // their transfer buffers, free
  uint64_t writes_queued_ = 0, writes_landed_ = 0;
  SDL_GPUTransferBuffer* download_tb_ = nullptr;
  std::map<uint32_t, Read> reads_;  // StartReadBack tickets
  uint32_t next_read_ = 1;
  uint32_t upload_tb_cap_ = 0, download_tb_cap_ = 0;
  std::unordered_map<PipelineKey, SDL_GPUGraphicsPipeline*, BytesHash<PipelineKey>> pipelines_;
  std::unordered_map<PipelineKey, SDL_GPUGraphicsPipeline*, BytesHash<PipelineKey>>
      pipelines_alpha_test_;
  PipelineKey last_key_{};  // Pipeline's last lookup
  bool last_alpha_test_ = false;
  SDL_GPUGraphicsPipeline* last_pipeline_ = nullptr;
  uint64_t tex_gen_ = 0;  // changes when textures_ does
  struct StageBinding {  // BindStageTextures' per stage
    uint64_t gen = ~0ull;
    TextureId id = 0;
    SDL_GPUTexture* tex = nullptr;
    bool cube = false;
    SamplerKey key{};
    SDL_GPUSampler* smp = nullptr;
  };
  StageBinding stage_bindings_[8];
  std::unordered_map<ShaderKey, SDL_GPUGraphicsPipeline*, BytesHash<ShaderKey>> shader_pipelines_;
  std::map<ShaderId, Shader> shaders_;
  std::vector<ShaderCmd> shader_cmds_;
  std::vector<uint8_t> ubytes_;  // shader draws' uniform blocks
  SDL_GPUTexture* dummy_2d_ = nullptr;  // (0, 0, 0, 1): shaders' unbound samplers
  SDL_GPUTexture* dummy_cube_ = nullptr;
  SDL_GPUTexture* dummy_3d_ = nullptr;
  uint64_t warned_rs_ = 0;
  std::map<SamplerKey, SDL_GPUSampler*> samplers_;

  // Programs (console GPUs): their draws' buffers in one arena per flush
  // (pmem_ into pbuf_), a dispatch's in cro_ and crw_.
  static constexpr size_t kMaxArena = size_t(1) << 30;
  std::map<ProgramId, Program> programs_;
  std::unordered_map<ProgramKey, SDL_GPUGraphicsPipeline*, BytesHash<ProgramKey>> program_pipelines_;
  std::map<ProgramSamplerKey, SDL_GPUSampler*> program_samplers_;
  std::vector<ProgramCmd> program_cmds_;
  Arena pmem_;
  struct DrawSpan {
    const uint8_t* p;  // guest memory copied
    size_t at;         // its offset in pmem_
    size_t n;          // bytes
  };
  std::vector<DrawSpan> draw_spans_;  // the current draw's (DrawBuffer)
  std::map<BufferId, GpuBuffer> gpu_buffers_;
  std::vector<BufferId> draw_read_;  // GPU buffers the recorded draws read
  SDL_GPUBuffer* heap_ = nullptr;   // the GPU buffers (HeapAlloc)
  uint32_t heap_cap_ = 0;
  std::map<uint32_t, uint32_t> heap_free_;  // its free runs: place, bytes
  SDL_GPUBuffer* pbuf_ = nullptr;
  SDL_GPUBuffer* cro_ = nullptr;
  SDL_GPUBuffer* crw_ = nullptr;
  uint32_t pbuf_cap_ = 0, cro_cap_ = 0, crw_cap_ = 0;
  SDL_GPUGraphicsPipeline* present_pipeline_ = nullptr;
  // SetGammaRamp: the ramp as a texture, the pass Show draws through it.
  bool gamma_on_ = false, gamma_dirty_ = false;
  uint16_t gamma_lut_[256 * 4] = {};
  SDL_GPUTexture* gamma_tex_ = nullptr;
  SDL_GPUTransferBuffer* gamma_tb_ = nullptr;
  SDL_GPUGraphicsPipeline* gamma_pipeline_ = nullptr;
  SDL_GPUTextureFormat gamma_format_ = SDL_GPU_TEXTUREFORMAT_INVALID;
  SDL_GPUTexture* dummy_array_ = nullptr;

  // The state the current uniforms were built from.
  FfState built_{};
  bool vs_dirty_ = true, fs_dirty_ = true;
  // BuildUniforms' matrices, by the bytes of their inputs.
  struct {
    bool valid = false;
    float view[16];
    Mat vinv;
    bool world_valid[4] = {};
    float world[4][16];
    Mat wv[4], nm[4];
  } xf_;
  VsUniforms vs_cur_{};
  FsUniforms fs_cur_{};
};

}  // namespace

std::unique_ptr<Device> CreateGpuDevice(SDL_Window* window) {
  auto d = std::make_unique<GpuDevice>(window);
  if (!d->Init()) return nullptr;
  return d;
}

}  // namespace recomp::gfx
