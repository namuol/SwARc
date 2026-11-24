// Direct3D 9 draws. The device's state goes with each draw to gfx::Device
// (DrawState: render states, viewport, scissor; the targets are bound).
// Draws with
// shaders (translated from D3DXCompileShader's HLSL or from bytecode)
// become gfx::ShaderDraw: the vertex declaration (or FVF) is matched to the
// vertex shader's input semantics, and the streams, indices, constant
// registers and samplers are passed as they are. Draws without shaders
// use gfx::Device's fixed-function pipeline (D3D7 semantics) with the
// D3D9 state in its terms: sampler states at D3D7's texture-stage
// positions, D3D9's transform numbers mapped. A vertex shader with the
// fixed-function pixel stage gets a pixel shader generated from the
// texture-stage state (FfPixelShader); a pixel shader after the
// fixed-function vertex stage gets a vertex shader standing in for that
// stage (FfVertexSource: FVF draws, and declarations with pretransformed
// positions, which bypass any vertex shader as in D3D9). A pixel shader with
// any other vertex declaration and no vertex shader, and fixed-function
// draws with a vertex declaration (no FVF), are not supported yet (fatal,
// named).

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "runtime/log.h"
#include "win32/d3d9_impl.h"
#include "win32/hle.h"

namespace recomp::d3d9 {

namespace {

// D3DDECLTYPE, D3DDECLUSAGE
enum : uint8_t {
  FLOAT1 = 0, FLOAT2 = 1, FLOAT3 = 2, FLOAT4 = 3, D3DCOLOR = 4, UBYTE4 = 5, UNUSED = 17,
};
enum : uint8_t {
  USAGE_POSITION = 0, USAGE_BLENDWEIGHT = 1, USAGE_BLENDINDICES = 2, USAGE_NORMAL = 3,
  USAGE_PSIZE = 4, USAGE_TEXCOORD = 5, USAGE_POSITIONT = 9, USAGE_COLOR = 10,
};

// The vertex declaration D3D9 makes for an FVF.
std::vector<guest::D3DVERTEXELEMENT9> FvfElements(uint32_t fvf) {
  std::vector<guest::D3DVERTEXELEMENT9> e;
  uint16_t off = 0;
  auto add = [&](uint8_t type, uint8_t usage, uint8_t index, uint16_t size) {
    e.push_back({0, off, type, 0, usage, index});
    off = uint16_t(off + size);
  };
  uint32_t pos = fvf & 0x400E;
  if (pos == 0x004) {  // XYZRHW
    add(FLOAT4, USAGE_POSITIONT, 0, 16);
  } else if (pos == 0x4002) {  // XYZW
    add(FLOAT4, USAGE_POSITION, 0, 16);
  } else if (pos >= 0x002 && pos <= 0x00E) {
    add(FLOAT3, USAGE_POSITION, 0, 12);
    // XYZB1-5: blend weights; with LASTBETA_UBYTE4 / _D3DCOLOR the last
    // one is the blend indices.
    int betas = pos == 0x002 ? 0 : int((pos - 4) / 2);
    bool indices = fvf & (0x1000 | 0x8000);
    int weights = betas - (indices ? 1 : 0);
    if (weights > 0) add(uint8_t(FLOAT1 + weights - 1), USAGE_BLENDWEIGHT, 0, uint16_t(4 * weights));
    if (indices) add((fvf & 0x8000) ? D3DCOLOR : UBYTE4, USAGE_BLENDINDICES, 0, 4);
  }
  if (fvf & 0x010) add(FLOAT3, USAGE_NORMAL, 0, 12);
  if (fvf & 0x020) add(FLOAT1, USAGE_PSIZE, 0, 4);
  if (fvf & 0x040) add(D3DCOLOR, USAGE_COLOR, 0, 4);
  if (fvf & 0x080) add(D3DCOLOR, USAGE_COLOR, 1, 4);
  uint32_t ntex = (fvf >> 8) & 0xF;
  for (uint32_t i = 0; i < ntex && i < 8; i++) {
    static const uint8_t kTypes[4] = {FLOAT2, FLOAT3, FLOAT4, FLOAT1};
    uint8_t t = kTypes[(fvf >> (16 + 2 * i)) & 3];
    add(t, USAGE_TEXCOORD, uint8_t(i), uint16_t(4 * (t - FLOAT1 + 1)));
  }
  e.push_back({0xFF, 0, UNUSED, 0, 0, 0});  // D3DDECL_END
  return e;
}

// The FVF of a vertex declaration with pretransformed positions
// (POSITIONT): XYZRHW, its colours and texture coordinates (sets 0 up to
// the highest one declared, each at its declared size). 0 if the
// declaration has no POSITIONT. D3D9 bypasses the vertex shader for such
// a declaration, so the fixed-function stand-in replaces it.
uint32_t PretransformedFvf(const std::vector<guest::D3DVERTEXELEMENT9>& elements) {
  bool positiont = false;
  uint32_t fvf = 0x004, ntex = 0;  // XYZRHW
  for (const auto& e : elements) {
    if (e.Stream == 0xFF) break;
    if (e.Usage == USAGE_POSITIONT && e.UsageIndex == 0) positiont = true;
    if (e.Usage == USAGE_COLOR && e.UsageIndex == 0) fvf |= 0x040;  // DIFFUSE
    if (e.Usage == USAGE_COLOR && e.UsageIndex == 1) fvf |= 0x080;  // SPECULAR
    if (e.Usage == USAGE_TEXCOORD && e.UsageIndex < 8) {
      ntex = std::max(ntex, uint32_t(e.UsageIndex) + 1);
      // FVF texture coordinate sizes: 0 2D, 1 3D, 2 4D, 3 1D.
      const uint32_t size = e.Type == FLOAT1 ? 3 : e.Type == FLOAT3 ? 1 : e.Type == FLOAT4 ? 2 : 0;
      fvf |= size << (16 + 2 * e.UsageIndex);
    }
  }
  return positiont ? fvf | (ntex << 8) : 0;
}

// Vertices (or indices) a draw of `count` primitives uses; 0 for an
// unknown type or more than 32 bits count.
uint32_t VerticesFor(uint32_t prim, uint32_t count) {
  uint64_t n;
  switch (prim) {
    case 1: n = count; break;                     // POINTLIST
    case 2: n = uint64_t(count) * 2; break;       // LINELIST
    case 3: n = uint64_t(count) + 1; break;       // LINESTRIP
    case 4: n = uint64_t(count) * 3; break;       // TRIANGLELIST
    case 5: case 6: n = uint64_t(count) + 2; break;  // TRIANGLESTRIP, TRIANGLEFAN
    default: return 0;
  }
  return n > UINT32_MAX ? 0 : uint32_t(n);
}

// A draw's state in gfx::Device's (D3D7's) terms: the render states, the
// viewport and the scissor, and for a fixed-function draw the texture
// stages (sampler states at D3D7's texture-stage positions), their
// textures and the transforms (Device::ff_base, D3D9's numbers mapped).
gfx::FfState DrawState(const Device& d, bool fixed) {
  // Fixed-function draws start from the defaults with the transforms set;
  // shader draws from the defaults alone.
  gfx::FfState f = fixed ? d.ff_base : gfx::DefaultFfState();
  std::memcpy(f.rs, d.rs, sizeof f.rs);
  const guest::D3DVIEWPORT9& v = d.viewport;
  f.viewport = {v.X, v.Y, v.Width, v.Height, v.MinZ, v.MaxZ};
  const guest::D3DRECT& r = d.scissor;
  f.scissor = {r.x1, r.y1, r.x2, r.y2};
  // User clip planes: shader draws apply them too (in clip space).
  std::memcpy(f.clip, d.ff_base.clip, sizeof f.clip);
  if (!fixed) return f;
  // Stages from the first with COLOROP DISABLE (1) on are unused.
  bool active = true;
  for (uint32_t s = 0; s < 8; s++) {
    active = active && d.tss[s][1] != 1;
    for (uint32_t t = 1; t < 33; t++)
      if (t < 13 || t > 21) f.tss[s][t] = d.tss[s][t];
    // D3D9 moved these to sampler states. Filters: D3DTEXF (NONE 0, POINT
    // 1, LINEAR 2, ANISOTROPIC 3) to D3D7's magnification (anisotropic 5)
    // and mip (NONE 1, POINT 2, LINEAR 3) values.
    const uint32_t* ss = d.ss[s];
    f.tss[s][13] = ss[1];                     // ADDRESSU
    f.tss[s][14] = ss[2];                     // ADDRESSV
    f.tss[s][15] = ss[4];                     // BORDERCOLOR
    f.tss[s][16] = ss[5] == 3 ? 5 : ss[5];    // MAGFILTER
    f.tss[s][17] = ss[6];                     // MINFILTER
    f.tss[s][18] = ss[7] + 1;                 // MIPFILTER
    f.tss[s][19] = ss[8];                     // MIPMAPLODBIAS
    f.tss[s][20] = ss[9];                     // MAXMIPLEVEL
    f.tss[s][21] = ss[10];                    // MAXANISOTROPY
    auto* t = active ? com::As<Texture>(d.textures[s]) : nullptr;
    f.textures[s] = t ? t->tex : 0;
    if (t) f.tss[s][20] = std::max(f.tss[s][20], t->lod);  // and the texture's SetLOD
  }
  // The material and the first eight enabled lights (in index order: the
  // pipeline does not see indices).
  f.material = d.material;
  f.light_count = 0;
  for (const auto& [i, on] : d.light_on) {
    if (!on) continue;
    if (f.light_count == 8) break;
    f.lights[f.light_count++] = d.lights.at(i);
  }
  return f;
}

// A draw without shaders: stream 0 with the FVF.
void DrawFixed(Device& d, gfx::ShaderDraw& sd, const char* what) {
  if (com::As<VertexDecl>(d.decl) || !d.fvf)
    FatalF("d3d9: fixed-function %s with a vertex declaration (only FVFs are supported)", what);
  uint32_t ticket = 0;  // the vertex buffer's (Buffer::ticket); UP draws none
  if (!sd.streams[0].data) {
    const Device::Stream& st = d.streams[0];
    auto* b = com::As<Buffer>(st.buffer);
    if (!b || st.offset >= b->length) return;
    sd.streams[0] = {static_cast<const uint8_t*>(GPtr(b->mem)) + st.offset, b->length - st.offset,
                     st.stride};
    ticket = b->ticket;
  }
  const gfx::VertexStream& vs = sd.streams[0];
  // Vertex (indexed draws: index) v is the stream's base_vertex + v, which
  // may be negative; the draw reads those from min_index (0 unindexed) on.
  uint32_t from = sd.indices ? sd.min_index : 0;
  int64_t lo = int64_t(sd.base_vertex) + from;
  if (!vs.stride || lo < 0 || uint64_t(lo + sd.vertex_count) * vs.stride > vs.size)
    FatalF("d3d9: fixed-function %s reads past its vertex buffer", what);
  gfx::FfDraw fd = gfx::FfInterleaved(
      sd.prim, d.fvf, vs.data + ptrdiff_t(sd.base_vertex) * ptrdiff_t(vs.stride), vs.stride);
  // Unchanged data in this draw's buffer: gfx copies each range once per
  // pass, or keeps it on the GPU across frames (FfStream::lock).
  for (gfx::FfStream* st : {&fd.position, &fd.normal, &fd.psize, &fd.diffuse, &fd.specular})
    st->lock = ticket;
  for (gfx::FfStream& st : fd.tex) st.lock = ticket;
  fd.first_vertex = from;
  fd.vertex_count = sd.vertex_count;
  if (sd.indices) {
    fd.indices = sd.indices;
    fd.index_count = VerticesFor(sd.prim, sd.primitives);
    fd.index_size = sd.index_size;
  }
  const gfx::FfState state = DrawState(d, true);
  BindTargets(d);
  d.gfx->DrawFf(state, fd);
}

// ---- the fixed-function pixel stage behind a vertex shader -----------------
// D3D9 lets a vertex shader feed the fixed-function pixel stage: its
// outputs oD0, oD1, oTn and oFog take the place of the fixed-function
// vertex stage's colours, texture coordinates and fog. That stage is
// generated here as an HLSL pixel shader for the current texture-stage
// and render state (unrolled: one function per state, cached by source),
// with the operations of gfx's fixed-function fragment shader
// (gfx/shaders/ff.frag), so both paths compute the same colours. The
// texture factor and fog colour are its c0 and c1 (FfPixelConstants).
// Outputs the vertex shader does not write read as D3D's defaults: white
// diffuse, black specular, zero texture coordinates, no fog.

constexpr uint32_t kRsFogEnable = 28, kRsSpecularEnable = 29, kRsTextureFactor = 60;
constexpr uint32_t kTssColorOp = 1, kTssColorArg1 = 2, kTssColorArg2 = 3, kTssAlphaOp = 4,
                   kTssAlphaArg1 = 5, kTssAlphaArg2 = 6, kTssTexCoordIndex = 11,
                   kTssTransformFlags = 24, kTssColorArg0 = 26, kTssAlphaArg0 = 27,
                   kTssResultArg = 28;

std::string FfArg(uint32_t a) {
  static const char* kNames[] = {"diff", "cur", "tex", "tf", "spec", "temp"};
  std::string v = (a & 0xF) < 6 ? kNames[a & 0xF] : "float4(0, 0, 0, 0)";
  if (a & 0x20) v = v + ".aaaa";            // D3DTA_ALPHAREPLICATE
  if (a & 0x10) v = "(1 - " + v + ")";      // D3DTA_COMPLEMENT
  return v;
}

std::string FfOp(uint32_t op, const std::string& a1, const std::string& a2,
                 const std::string& a0) {
  auto two = [&](const char* f) {
    char b[512];
    std::snprintf(b, sizeof b, f, a1.c_str(), a2.c_str());
    return std::string(b);
  };
  switch (op) {
    case 2: return a1;
    case 3: return a2;
    case 4: return two("(%s * %s)");
    case 5: return two("(%s * %s * 2)");
    case 6: return two("(%s * %s * 4)");
    case 7: return two("(%s + %s)");
    case 8: return two("(%s + %s - 0.5)");
    case 9: return two("((%s + %s - 0.5) * 2)");
    case 10: return two("(%s - %s)");
    case 11: return "(" + a1 + " + " + a2 + " - " + a1 + " * " + a2 + ")";
    case 12: return "lerp(" + a2 + ", " + a1 + ", diff.a)";
    case 13: return "lerp(" + a2 + ", " + a1 + ", tex.a)";
    case 14: return "lerp(" + a2 + ", " + a1 + ", tf.a)";
    case 15: return "(" + a1 + " + " + a2 + " * (1 - tex.a))";
    case 16: return "lerp(" + a2 + ", " + a1 + ", cur.a)";
    case 17: return a1;  // PREMODULATE: approximated, as in ff.frag
    case 18: return "float4(" + a1 + ".rgb + " + a1 + ".a * " + a2 + ".rgb, " + a1 + ".a)";
    case 19: return "float4(" + a1 + ".rgb * " + a2 + ".rgb + " + a1 + ".a, " + a1 + ".a)";
    case 20: return "float4((1 - " + a1 + ".a) * " + a2 + ".rgb + " + a1 + ".rgb, " + a1 + ".a)";
    case 21: return "float4((1 - " + a1 + ".rgb) * " + a2 + ".rgb + " + a1 + ".a, " + a1 + ".a)";
    case 24: return "saturate(4 * dot(" + a1 + ".rgb - 0.5, " + a2 + ".rgb - 0.5)).xxxx";
    case 25: return "(" + a0 + " + " + a1 + " * " + a2 + ")";
    case 26: return "lerp(" + a2 + ", " + a1 + ", " + a0 + ")";
    default: return a1;  // bump mapping: not modelled, as in ff.frag
  }
}

bool Writes(const hlsl::Program& vs, uint32_t usage, uint32_t index) {
  for (const hlsl::Semantic& v : vs.varyings)
    if (v.usage == usage && v.index == index) return true;
  return false;
}

std::string FfPixelSource(const Device& d, const hlsl::Program& vs) {
  std::string in, body;
  in += Writes(vs, hlsl::kColor, 0) ? "float4 i_diff : COLOR0, " : "";
  in += Writes(vs, hlsl::kColor, 1) ? "float4 i_spec : COLOR1, " : "";
  body += Writes(vs, hlsl::kColor, 0) ? "  float4 diff = i_diff;\n" : "  float4 diff = 1;\n";
  body += Writes(vs, hlsl::kColor, 1) ? "  float4 spec = i_spec;\n" : "  float4 spec = 0;\n";
  body += "  float4 cur = diff, temp = 0, tex;\n";
  std::string samplers;
  bool tc_in[8] = {};
  for (uint32_t s = 0; s < 8; s++) {
    const uint32_t* t = d.tss[s];
    uint32_t cop = t[kTssColorOp];
    if (cop == 1) break;  // DISABLE ends the cascade
    uint32_t set = t[kTssTexCoordIndex] & 0xFFFF;
    bool bound = d.textures[s] != 0 && set < 8;
    char b[256];
    if (bound) {
      std::snprintf(b, sizeof b, "sampler2D s%u : register(s%u);\n", s, s);
      samplers += b;
      std::string tc = "0";
      if (Writes(vs, hlsl::kTexcoord, set)) {
        tc_in[set] = true;
        std::snprintf(b, sizeof b, "i_t%u", set);
        tc = b;
      }
      uint32_t flags = t[kTssTransformFlags];
      uint32_t count = flags & 0xFF;
      if ((flags & 0x100) && count >= 2 && count <= 4 && tc != "0") {
        // D3DTTFF_PROJECTED: divide by the last of `count` components.
        static const char* kComp = "xyzw";
        std::snprintf(b, sizeof b, "  tex = tex2D(s%u, %s.xy / %s.%c);\n", s, tc.c_str(),
                      tc.c_str(), kComp[count - 1]);
      } else {
        std::snprintf(b, sizeof b, "  tex = tex2D(s%u, (%s).xy);\n", s,
                      tc == "0" ? "float2(0, 0)" : tc.c_str());
      }
      body += b;
    } else {
      body += "  tex = 0;\n";  // no texture: what D3D's arguments read
    }
    std::string rgb = "saturate(" + FfOp(cop, FfArg(t[kTssColorArg1]), FfArg(t[kTssColorArg2]),
                                         FfArg(t[kTssColorArg0])) + ").rgb";
    std::string al = "cur.a";
    if (t[kTssAlphaOp] != 1)
      al = "saturate(" + FfOp(t[kTssAlphaOp], FfArg(t[kTssAlphaArg1]), FfArg(t[kTssAlphaArg2]),
                              FfArg(t[kTssAlphaArg0])) + ").a";
    body += "  { float3 rgb = " + rgb + ";\n";
    body += "    float al = " + (cop == 24 ? std::string("rgb.r") : al) + ";\n";
    body += t[kTssResultArg] == 5 ? "    temp = float4(rgb, al); }\n"
                                  : "    cur = float4(rgb, al); }\n";
  }
  if (d.rs[kRsSpecularEnable]) body += "  cur.rgb = saturate(cur.rgb + spec.rgb);\n";
  for (uint32_t i = 0; i < 8; i++)
    if (tc_in[i]) {
      char b[64];
      std::snprintf(b, sizeof b, "float4 i_t%u : TEXCOORD%u, ", i, i);
      in += b;
    }
  if (in.size() >= 2) in.resize(in.size() - 2);
  return "float4 tf : register(c0);\n" + samplers +
         "float4 main(" + in + ") : COLOR0 {\n" + body + "  return cur;\n}\n";
}

// The generated pixel shader for the current state behind `vs`, and its
// constants (c0 texture factor) in *consts. Fog is the device's, after
// the shader (gfx).
gfx::ShaderId FfPixelShader(Device& d, const hlsl::Program& vs, float (*consts)[4]) {
  std::string src = FfPixelSource(d, vs);
  auto unpack = [](uint32_t argb, float* out) {
    out[0] = float((argb >> 16) & 0xFF) / 255.0f;
    out[1] = float((argb >> 8) & 0xFF) / 255.0f;
    out[2] = float(argb & 0xFF) / 255.0f;
    out[3] = float(argb >> 24) / 255.0f;
  };
  unpack(d.rs[kRsTextureFactor], consts[0]);
  auto it = d.ff_pixel_shaders.find(src);
  if (it != d.ff_pixel_shaders.end()) return it->second;
  hlsl::Program p;
  std::string errors;
  if (!hlsl::Compile(src, {}, "main", "ps_2_0", 0, &p, &errors))
    FatalF("d3d9: fixed-function pixel stage shader failed to compile: %s\n%s", errors.c_str(),
           src.c_str());
  gfx::ShaderId id = d.gfx->CreateShader(p);
  d.ff_pixel_shaders.emplace(src, id);
  return id;
}

// The fixed-function vertex stage of an FVF draw as HLSL, for draws that
// pair it with a pixel shader. Constants c0-c3: the columns of
// World*View*Projection (untransformed vertices); c4: the viewport's x, y,
// 2/width, -2/height (pretransformed XYZRHW ones, which become clip space
// with w = 1/rhw); c5-c8: the columns of World*View; c12 + 4s: those of
// texture stage s's transform. The outputs are what that stage gives a
// pixel shader: diffuse (white without vertex colour) and specular
// (black), and each stage's texture coordinates: its TEXCOORDINDEX set, or
// generated in camera space (normal, position, reflection vector, sphere
// map), then through the stage's transform when TEXTURETRANSFORMFLAGS has a
// count (D3D's rules: a 2D set enters as (u, v, 1, 0)), and the vertex fog
// factor (c44: FOGSTART, FOGEND, FOGDENSITY). Not done here (warned once):
// lighting.
std::string FfVertexSource(const Device& d, uint32_t fvf) {
  const bool rhw = (fvf & 0x400E) == 0x004;
  const bool normal = (fvf & 0x010) != 0;
  const uint32_t ntex = (fvf >> 8) & 0xF;
  std::string in = "struct VSIN {\n  float4 pos : POSITION;\n";
  if (normal && !rhw) in += "  float3 nrm : NORMAL;\n";
  if (fvf & 0x040) in += "  float4 diff : COLOR0;\n";
  if (fvf & 0x080) in += "  float4 spec : COLOR1;\n";
  for (uint32_t i = 0; i < ntex && i < 8; i++)
    in += "  float4 t" + std::to_string(i) + " : TEXCOORD" + std::to_string(i) + ";\n";
  in += "};\n";
  std::string out = "struct VSOUT {\n  float4 pos : POSITION;\n  float4 diff : COLOR0;\n"
                    "  float4 spec : COLOR1;\n";
  std::string body;
  if (rhw) {
    body += "  float w = v.pos.w != 0.0 ? 1.0 / v.pos.w : 1.0;\n"
            "  o.pos = float4((v.pos.x - vp.x) * vp.z - 1.0, (v.pos.y - vp.y) * vp.w + 1.0,"
            " v.pos.z, 1.0) * w;\n";
  } else {
    body += "  float4 p = float4(v.pos.xyz, 1.0);\n"
            "  o.pos = float4(dot(p, m0), dot(p, m1), dot(p, m2), dot(p, m3));\n";
  }
  body += (fvf & 0x040) ? "  o.diff = v.diff;\n" : "  o.diff = float4(1.0, 1.0, 1.0, 1.0);\n";
  body += (fvf & 0x080) ? "  o.spec = v.spec;\n" : "  o.spec = float4(0.0, 0.0, 0.0, 0.0);\n";
  // Vertex fog: the factor from the camera-space depth (or distance with
  // RANGEFOGENABLE) by FOGVERTEXMODE, with c44 = (start, end, density);
  // the specular alpha for pretransformed vertices or FOGVERTEXMODE NONE.
  // Table fog (FOGTABLEMODE) is the pixel stage's, from the depth.
  const uint32_t fog_mode = d.rs[140];
  const bool fog_depth = d.rs[kRsFogEnable] && !d.rs[35] && !rhw && fog_mode >= 1 && fog_mode <= 3;
  out += "  float fog : FOG;\n";
  bool camera = fog_depth;  // camera-space position / normal needed
  std::string gen;
  for (uint32_t s = 0; s < 8; s++) {
    const uint32_t set = d.tss[s][11] & 0xFFFF, tci = d.tss[s][11] >> 16;
    const uint32_t count = d.tss[s][24] & 0xFF;
    const std::string n = std::to_string(s);
    std::string c;
    if (tci == 0 || rhw) {
      if (set >= ntex) continue;
      // FVF texture coordinate size: 0 2D, 1 3D, 2 4D, 3 1D.
      const uint32_t size = (fvf >> (16 + 2 * set)) & 3;
      const std::string t = "v.t" + std::to_string(set);
      c = !count ? t
          : size == 0 ? "float4(" + t + ".xy, 1.0, 0.0)"
          : size == 3 ? "float4(" + t + ".x, 1.0, 0.0, 0.0)"
          : size == 1 ? "float4(" + t + ".xyz, 1.0)" : t;
    } else {
      camera = true;
      if (tci == 1) c = "float4(cn, 1.0)";
      else if (tci == 2) c = "float4(cp, 1.0)";
      else if (tci == 3) c = "float4(cr, 1.0)";
      else c = "float4(cr.xy / (2.0 * sqrt(cr.x * cr.x + cr.y * cr.y + (cr.z + 1.0) * (cr.z + 1.0))) + 0.5, 0.0, 1.0)";
    }
    out += "  float4 t" + n + " : TEXCOORD" + n + ";\n";
    if (count) {
      const std::string m = "tm" + n + "_";
      gen += "  { float4 c = " + c + ";\n    o.t" + n + " = float4(dot(c, " + m + "0), dot(c, " + m +
             "1), dot(c, " + m + "2), dot(c, " + m + "3)); }\n";
    } else {
      gen += "  o.t" + n + " = " + c + ";\n";
    }
  }
  if (camera) {
    // Camera space (World*View); the local viewer's eye vector.
    body += "  float3 cp = float3(dot(p, wv0), dot(p, wv1), dot(p, wv2));\n";
    body += normal ? "  float3 cn = normalize(float3(dot(v.nrm, wv0.xyz), dot(v.nrm, wv1.xyz),"
                     " dot(v.nrm, wv2.xyz)));\n"
                   : "  float3 cn = float3(0.0, 0.0, 0.0);\n";
    body += "  float3 e = -normalize(cp);\n  float3 cr = 2.0 * dot(e, cn) * cn - e;\n";
  }
  body += gen;
  if (fog_depth) {
    body += d.rs[48] ? "  float fd = length(cp);\n" : "  float fd = cp.z;\n";
    body += fog_mode == 1   ? "  o.fog = exp(-fd * fogp.z);\n"
            : fog_mode == 2 ? "  o.fog = exp(-(fd * fogp.z) * (fd * fogp.z));\n"
                            : "  o.fog = fogp.y == fogp.x ? (fd < fogp.y ? 1.0 : 0.0)"
                              " : (fogp.y - fd) / (fogp.y - fogp.x);\n";
  } else {
    body += "  o.fog = o.spec.a;\n";
  }
  out += "};\n";
  std::string decl = "float4 m0 : register(c0);\nfloat4 m1 : register(c1);\nfloat4 m2 : register(c2);\n"
                     "float4 m3 : register(c3);\nfloat4 vp : register(c4);\nfloat4 wv0 : register(c5);\n"
                     "float4 wv1 : register(c6);\nfloat4 wv2 : register(c7);\n"
                     "float4 fogp : register(c44);\n";
  for (uint32_t s = 0; s < 8; s++)
    if (d.tss[s][24] & 0xFF)
      for (uint32_t r = 0; r < 4; r++)
        decl += "float4 tm" + std::to_string(s) + "_" + std::to_string(r) + " : register(c" +
                std::to_string(12 + 4 * s + r) + ");\n";
  return decl + in + out + "VSOUT main(VSIN v) {\n  VSOUT o;\n" + body + "  return o;\n}\n";
}

// Whether the fixed-function vertex state is beyond FfVertexSource.
void NoteFfVertexGaps(const Device& d, uint32_t fvf) {
  static bool warned_light = false;
  const bool rhw = (fvf & 0x400E) == 0x004;
  if (!rhw && d.rs[137] && (fvf & 0x010) && !warned_light) {  // LIGHTING with normals
    warned_light = true;
    WarnF("d3d9: fixed-function lighting with a pixel shader is not emulated (vertex colours used)\n");
  }
}

// The stand-in vertex shader for d.fvf (compiled once per source) and its
// constants in `c`.
const Device::FfVertexShader& FfVertexShaderFor(Device& d, uint32_t fvf, float (*c)[4]) {
  std::string src = FfVertexSource(d, fvf);
  auto it = d.ff_vs.find(src);
  if (it == d.ff_vs.end()) {
    auto p = std::make_shared<hlsl::Program>();
    std::string err;
    if (!hlsl::Compile(src, {}, "main", "vs_2_0", 0, p.get(), &err))
      FatalF("d3d9: the fixed-function vertex shader for FVF %X does not translate: %s", fvf,
             err.c_str());
    Device::FfVertexShader v;
    v.id = d.gfx->CreateShader(*p);
    v.program = std::move(p);
    it = d.ff_vs.emplace(src, std::move(v)).first;
  }
  NoteFfVertexGaps(d, fvf);
  // World * View * Projection (row vectors), stored by column.
  static const std::array<float, 16> kIdentity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  auto get = [&](uint32_t t) {
    auto f = d.transforms.find(t);
    return f != d.transforms.end() ? f->second : kIdentity;
  };
  auto mul = [](const std::array<float, 16>& a, const std::array<float, 16>& b) {
    std::array<float, 16> r{};
    for (int i = 0; i < 4; i++)
      for (int j = 0; j < 4; j++)
        for (int k = 0; k < 4; k++) r[size_t(i * 4 + j)] += a[size_t(i * 4 + k)] * b[size_t(k * 4 + j)];
    return r;
  };
  const std::array<float, 16> wv = mul(get(256), get(2));
  std::array<float, 16> m = mul(wv, get(3));
  for (int j = 0; j < 4; j++)
    for (int i = 0; i < 4; i++) {
      c[j][i] = m[size_t(i * 4 + j)];
      c[5 + j][i] = wv[size_t(i * 4 + j)];
    }
  // Texture transforms (D3DTS_TEXTURE0 + s = 16 + s), by column.
  for (uint32_t s = 0; s < 8; s++) {
    if (!(d.tss[s][24] & 0xFF)) continue;
    const std::array<float, 16> t = get(16 + s);
    for (int j = 0; j < 4; j++)
      for (int i = 0; i < 4; i++) c[12 + 4 * s + uint32_t(j)][i] = t[size_t(i * 4 + j)];
  }
  const guest::D3DVIEWPORT9& v = d.viewport;
  c[4][0] = float(v.X);
  c[4][1] = float(v.Y);
  c[4][2] = v.Width ? 2.0f / float(v.Width) : 0.0f;
  c[4][3] = v.Height ? -2.0f / float(v.Height) : 0.0f;
  for (int i = 0; i < 3; i++) std::memcpy(&c[44][i], &d.rs[36 + i], 4);  // FOGSTART, END, DENSITY
  c[44][3] = 0.0f;
  return it->second;
}

// Completes `sd` (primitive, counts, and any streams already given) from
// the device's bindings and draws it.
void Draw(Device& d, gfx::ShaderDraw& sd, const char* what) {
  Shader* vs = com::As<Shader>(d.vs);
  Shader* ps = com::As<Shader>(d.ps);
  // A declaration with pretransformed positions (POSITIONT) bypasses the
  // vertex shader: the vertices are in screen space, as an XYZRHW FVF's.
  auto* decl = com::As<VertexDecl>(d.decl);
  const uint32_t decl_fvf = decl ? PretransformedFvf(decl->elements) : 0;
  if (decl_fvf && ps) vs = nullptr;
  if (!vs && !ps) {
    DrawFixed(d, sd, what);
    return;
  }
  // A pixel shader after the fixed-function vertex stage: a vertex shader
  // in its place (an FVF, or a declaration with pretransformed positions).
  const Device::FfVertexShader* ff = nullptr;
  float ff_vs_consts[256][4];
  if (!vs) {
    if (decl ? !decl_fvf : !d.fvf)
      FatalF("d3d9: %s with a pixel shader, no vertex shader and a vertex declaration", what);
    ff = &FfVertexShaderFor(d, decl ? decl_fvf : d.fvf, ff_vs_consts);
  }
  const hlsl::Program& vprog = ff ? *ff->program : *vs->program;

  std::vector<guest::D3DVERTEXELEMENT9> from_fvf;
  const std::vector<guest::D3DVERTEXELEMENT9>* elements = nullptr;
  if (decl) {
    elements = &decl->elements;
  } else if (d.fvf) {
    from_fvf = FvfElements(d.fvf);
    elements = &from_fvf;
  } else {
    FatalF("d3d9: %s without a vertex declaration or FVF", what);
  }
  // The stand-in shader reads a pretransformed position as POSITION.
  if (ff) {
    if (elements != &from_fvf) from_fvf = *elements;
    for (auto& e : from_fvf)
      if (e.Usage == USAGE_POSITIONT) e.Usage = USAGE_POSITION;
    elements = &from_fvf;
  }
  const auto& sem = vprog.inputs;
  std::vector<gfx::VertexInput> inputs(sem.size());
  for (size_t i = 0; i < sem.size(); i++) {
    for (const auto& e : *elements) {
      if (e.Stream == 0xFF) break;
      if (e.Usage == sem[i].usage && e.UsageIndex == sem[i].index) {
        inputs[i] = {e.Stream, e.Offset, e.Type};
        break;
      }
    }
  }
  sd.inputs = inputs.data();
  sd.input_count = uint32_t(inputs.size());

  // Instancing (SetStreamSourceFreq): stream 0's D3DSTREAMSOURCE_INDEXEDDATA
  // count of instances; INSTANCEDATA streams advance once per `n`
  // instances instead of per vertex.
  constexpr uint32_t kIndexedData = 0x40000000u, kInstanceData = 0x80000000u;
  uint32_t instances = 1, per_instance[16] = {};
  for (int s = 0; s < 16; s++) {
    const Device::Stream& st = d.streams[s];
    if (st.freq != 1 && st.buffer) {
      if (s == 0 && (st.freq & kIndexedData) && (st.freq & 0x3FFFFFFFu)) {
        instances = st.freq & 0x3FFFFFFFu;
      } else if (s > 0 && (st.freq & kInstanceData) && (st.freq & 0x3FFFFFFFu)) {
        per_instance[s] = st.freq & 0x3FFFFFFFu;
      } else {
        FatalF("d3d9: %s with stream %d frequency %08X: not supported", what, s, st.freq);
      }
    }
    if (sd.streams[s].data) continue;  // given by the caller (UP draws)
    auto* b = com::As<Buffer>(st.buffer);
    if (!b || st.offset >= b->length) continue;
    sd.streams[s] = {static_cast<const uint8_t*>(GPtr(b->mem)) + st.offset, b->length - st.offset,
                     st.stride, 0, b->ticket};
  }
  // An instanced draw becomes one ordinary draw of every instance's copy:
  // per-vertex streams' vertices repeated per instance, per-instance
  // streams' element for that instance repeated over its copy's vertices,
  // and the indices offset per copy (32-bit). Exact; the GPU backend need
  // not know about instancing.
  std::vector<std::vector<uint8_t>> expanded;
  std::vector<uint32_t> expanded_indices, strip_indices;
  const bool instanced =
      instances > 1 || std::any_of(per_instance, per_instance + 16, [](uint32_t n) { return n; });
  if (instanced && d.gfx->SupportsInstancing()) {  // the GPU draws the instances
    sd.instances = instances;
    for (int s = 0; s < 16; s++) sd.streams[s].divisor = per_instance[s];
  } else if (instanced) {
    // A non-indexed strip or fan becomes an indexed triangle list (strips
    // keep D3D's alternating winding), so each copy stays separate.
    if (sd.prim == 3 || sd.prim == 5 || sd.prim == 6) {
      const void* src = sd.indices;
      const uint32_t isz = sd.index_size;
      auto at = [&](uint32_t k) -> uint32_t {
        if (!src) return k;
        return isz == 4 ? static_cast<const uint32_t*>(src)[k] : static_cast<const uint16_t*>(src)[k];
      };
      strip_indices.reserve(size_t(sd.primitives) * 3);
      for (uint32_t i = 0; i < sd.primitives; i++) {
        if (sd.prim == 3) {  // line strip -> line list
          strip_indices.insert(strip_indices.end(), {at(i), at(i + 1)});
        } else if (sd.prim == 6) {
          strip_indices.insert(strip_indices.end(), {at(0), at(i + 1), at(i + 2)});
        } else if (i & 1) {
          strip_indices.insert(strip_indices.end(), {at(i + 1), at(i), at(i + 2)});
        } else {
          strip_indices.insert(strip_indices.end(), {at(i), at(i + 1), at(i + 2)});
        }
      }
      if (!src) sd.min_index = 0;
      sd.indices = strip_indices.data();
      sd.index_size = 4;
      sd.prim = sd.prim == 3 ? 2 : 4;
    }
    // List types only: each instance's copy is a whole number of primitives
    // (strips and fans would join one copy to the next).
    const bool list = sd.prim == 1 || sd.prim == 2 || sd.prim == 4;  // point/line/triangle list
    if (!list)
      FatalF("d3d9: instanced %s of primitive type %u (%s): not supported yet", what, sd.prim,
             sd.indices ? "indexed" : "not indexed");
    const uint32_t vc = sd.vertex_count, first = uint32_t(sd.base_vertex) + sd.min_index;
    const uint32_t ni = VerticesFor(sd.prim, sd.primitives);
    for (int s = 0; s < 16; s++) {
      gfx::VertexStream& vs_ = sd.streams[s];
      if (!vs_.data || !vs_.stride) continue;
      // Copies whole blocks (a vertex at a time cost more than the draw):
      // each vertex or element is read only if it lies wholly inside the
      // stream, and the rest stays zero.
      const size_t stride = vs_.stride, block = size_t(vc) * stride;
      std::vector<uint8_t> out(size_t(instances) * block, 0);
      // dst[0, unit) repeated over dst[0, total), by doubling.
      auto repeat = [](uint8_t* dst, size_t unit, size_t total) {
        for (size_t have = unit; unit && have < total;) {
          const size_t n = std::min(have, total - have);
          std::memcpy(dst + have, dst, n);
          have += n;
        }
      };
      if (per_instance[s]) {  // instance i's element over its vc vertices
        for (uint32_t i = 0; i < instances; i++) {
          const uint64_t src = uint64_t(i / per_instance[s]) * stride;
          if (!vc || src + stride > vs_.size) continue;
          uint8_t* dst = out.data() + size_t(i) * block;
          std::memcpy(dst, vs_.data + src, stride);
          repeat(dst, stride, block);
        }
      } else {  // vertices first .. first + vc - 1, the same for every instance
        const uint64_t lo = uint64_t(first) * stride;
        const size_t valid = lo < vs_.size ? size_t(std::min<uint64_t>(vc, (vs_.size - lo) / stride)) : 0;
        if (valid) std::memcpy(out.data(), vs_.data + lo, valid * stride);
        repeat(out.data(), block, out.size());
      }
      expanded.push_back(std::move(out));
      vs_ = {expanded.back().data(), uint32_t(expanded.back().size()), vs_.stride};
    }
    if (sd.indices) expanded_indices.resize(size_t(instances) * ni);
    for (uint32_t k = 0; sd.indices && k < ni; k++) {
      uint32_t idx = sd.index_size == 4 ? static_cast<const uint32_t*>(sd.indices)[k]
                                        : static_cast<const uint16_t*>(sd.indices)[k];
      uint32_t local = idx >= sd.min_index ? idx - sd.min_index : 0;
      for (uint32_t i = 0; i < instances; i++) expanded_indices[size_t(i) * ni + k] = i * vc + local;
    }
    if (sd.indices) {
      sd.indices = expanded_indices.data();
      sd.index_size = 4;
    }
    sd.base_vertex = 0;
    sd.min_index = 0;
    sd.vertex_count = instances * vc;
    sd.primitives *= instances;
  }

  // No pixel shader: the fixed-function pixel stage, generated (its own
  // constants; the game's pixel constant registers are not read).
  float ff_ps_consts[224][4] = {};
  sd.vs = ff ? ff->id : vs->id;
  sd.ps = ps ? ps->id : FfPixelShader(d, vprog, ff_ps_consts);
  sd.vs_float = ff ? ff_vs_consts : d.vs_f;
  sd.vs_int = d.vs_i;
  sd.vs_bool = reinterpret_cast<const uint32_t*>(d.vs_b);
  sd.ps_float = ps ? d.ps_f : ff_ps_consts;
  sd.ps_int = d.ps_i;
  sd.ps_bool = reinterpret_cast<const uint32_t*>(d.ps_b);
  // A managed texture's SetLOD is the most detailed level it is sampled at,
  // as the sampler's MAXMIPLEVEL is (D3D9 uses the larger of the two): games
  // that stream textures in raise the levels as they arrive, and the ones
  // not loaded yet must not be sampled.
  uint32_t ss[20][14];
  std::memcpy(ss, d.ss, sizeof ss);
  for (int i = 0; i < 20; i++) {
    auto* t = com::As<Texture>(d.textures[i]);
    sd.textures[i] = t ? t->tex : 0;
    if (t) ss[i][9] = std::max(ss[i][9], t->lod);
  }
  sd.sampler_states = ss;
  const gfx::FfState state = DrawState(d, false);
  sd.state = &state;
  BindTargets(d);
  d.gfx->DrawShader(sd);
}

}  // namespace
}  // namespace recomp::d3d9

namespace recomp::hle {
namespace {

using namespace d3d9;

Device* Dev(Ctx& c) { return DeviceOf(Arg(c, 0)); }

// (PrimitiveType, StartVertex, PrimitiveCount)
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::DrawPrimitive", D3DDev9_DrawPrimitive, 4) {
  gfx::ShaderDraw sd;
  sd.prim = Arg(c, 1);
  sd.primitives = Arg(c, 3);
  sd.base_vertex = int32_t(Arg(c, 2));
  sd.vertex_count = VerticesFor(sd.prim, sd.primitives);
  if (!sd.vertex_count) return D3DERR_INVALIDCALL;
  Draw(*Dev(c), sd, "DrawPrimitive");
  return D3D_OK;
}

// (Type, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount)
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::DrawIndexedPrimitive", D3DDev9_DrawIndexedPrimitive, 7) {
  Device* d = Dev(c);
  auto* ib = com::As<Buffer>(d->indices);
  gfx::ShaderDraw sd;
  sd.prim = Arg(c, 1);
  sd.primitives = Arg(c, 6);
  uint32_t n = VerticesFor(sd.prim, sd.primitives);
  if (!ib || !n) return D3DERR_INVALIDCALL;
  sd.index_size = ib->fvf_or_format == D3DFMT_INDEX32 ? 4 : 2;
  uint64_t start = uint64_t(Arg(c, 5)) * sd.index_size;
  if (start + uint64_t(n) * sd.index_size > ib->length) return D3DERR_INVALIDCALL;
  sd.indices = static_cast<const uint8_t*>(GPtr(ib->mem)) + start;
  sd.base_vertex = int32_t(Arg(c, 2));
  sd.min_index = Arg(c, 3);
  sd.vertex_count = Arg(c, 4);
  Draw(*d, sd, "DrawIndexedPrimitive");
  return D3D_OK;
}

// (PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride).
// Stream 0 is unbound afterwards, as in D3D9.
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::DrawPrimitiveUP", D3DDev9_DrawPrimitiveUP, 5) {
  Device* d = Dev(c);
  gfx::ShaderDraw sd;
  sd.prim = Arg(c, 1);
  sd.primitives = Arg(c, 2);
  uint32_t stride = Arg(c, 4);
  sd.vertex_count = VerticesFor(sd.prim, sd.primitives);
  uint64_t bytes = uint64_t(sd.vertex_count) * stride;
  if (!sd.vertex_count || bytes > UINT32_MAX || !EnsureReadable(c, Arg(c, 3), uint32_t(bytes)))
    return D3DERR_INVALIDCALL;
  sd.streams[0] = {static_cast<const uint8_t*>(GPtr(Arg(c, 3))), uint32_t(bytes), stride};
  Draw(*d, sd, "DrawPrimitiveUP");
  Bind(&d->streams[0].buffer, 0);
  d->streams[0].offset = d->streams[0].stride = 0;
  return D3D_OK;
}

// (PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData,
// IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride). Stream 0
// and the indices are unbound afterwards, as in D3D9.
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::DrawIndexedPrimitiveUP", D3DDev9_DrawIndexedPrimitiveUP, 9) {
  Device* d = Dev(c);
  gfx::ShaderDraw sd;
  sd.prim = Arg(c, 1);
  sd.min_index = Arg(c, 2);
  sd.vertex_count = Arg(c, 3);
  sd.primitives = Arg(c, 4);
  sd.index_size = Arg(c, 6) == D3DFMT_INDEX32 ? 4 : 2;
  uint32_t n = VerticesFor(sd.prim, sd.primitives), stride = Arg(c, 8);
  uint64_t ibytes = uint64_t(n) * sd.index_size,
           vbytes = (uint64_t(sd.min_index) + sd.vertex_count) * stride;
  if (!n || ibytes > UINT32_MAX || vbytes > UINT32_MAX ||
      !EnsureReadable(c, Arg(c, 5), uint32_t(ibytes)) ||
      !EnsureReadable(c, Arg(c, 7), uint32_t(vbytes)))
    return D3DERR_INVALIDCALL;
  sd.indices = GPtr(Arg(c, 5));
  sd.streams[0] = {static_cast<const uint8_t*>(GPtr(Arg(c, 7))), uint32_t(vbytes), stride};
  Draw(*d, sd, "DrawIndexedPrimitiveUP");
  Bind(&d->streams[0].buffer, 0);
  d->streams[0].offset = d->streams[0].stride = 0;
  Bind(&d->indices, 0);
  return D3D_OK;
}

// The gfx colour target behind a surface for StretchRect (tex 0: the back
// buffer); fatal for the kinds it does not support.
gfx::ColorTarget StretchTarget(Surface* s, const char* which) {
  switch (s->kind) {
    case Surface::Kind::BackBuffer:
      return {};
    case Surface::Kind::RenderTarget:
      return {s->tex, 0};
    case Surface::Kind::TextureLevel:
      if (!s->texture->cube && (s->texture->usage & D3DUSAGE_RENDERTARGET))
        return {s->texture->tex, int(s->level)};
      break;
    default:
      break;
  }
  FatalF("d3d9: StretchRect %s is not a colour target (kind %d, level %d)", which, int(s->kind),
         s->level);
}

gfx::Rect SurfaceRect(Surface* s, uint32_t rect) {
  if (!rect) return {0, 0, int32_t(s->desc.Width), int32_t(s->desc.Height)};
  const auto* r = G<guest::RECT>(rect);
  return {r->left, r->top, r->right, r->bottom};
}

// (pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter)
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::StretchRect", D3DDev9_StretchRect, 6) {
  Device* d = Dev(c);
  auto* src = com::As<Surface>(Arg(c, 1));
  auto* dst = com::As<Surface>(Arg(c, 3));
  if (!src || !dst || src == dst) return D3DERR_INVALIDCALL;
  gfx::ColorTarget s = StretchTarget(src, "source"), t = StretchTarget(dst, "destination");
  // D3DTEXF_LINEAR (2) filters; NONE and POINT take the nearest texel.
  d->gfx->StretchRect(s, SurfaceRect(src, Arg(c, 2)), t, SurfaceRect(dst, Arg(c, 4)),
                      Arg(c, 5) >= 2);
  return D3D_OK;
}

// (pSurface, pRect, Color): an A8R8G8B8 fill of a render target's rectangle
// (all of it without one). Render-target surfaces and the top level of a 2D
// render-target texture; other kinds are fatal and named.
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::ColorFill", D3DDev9_ColorFill, 4) {
  Device* d = Dev(c);
  auto* s = com::As<Surface>(Arg(c, 1));
  if (!s) return D3DERR_INVALIDCALL;
  gfx::TextureId t = 0;
  if (s->kind == Surface::Kind::RenderTarget) {
    t = s->tex;
  } else if (s->kind == Surface::Kind::TextureLevel && s->level == 0 && !s->texture->cube &&
             (s->texture->usage & D3DUSAGE_RENDERTARGET)) {
    t = s->texture->tex;
  } else {
    FatalF("d3d9: ColorFill on a surface of kind %d, level %d: not supported yet", int(s->kind),
           s->level);
  }
  d->gfx->FillTexture(t, SurfaceRect(s, Arg(c, 2)), Arg(c, 3));
  return D3D_OK;
}

// GetRenderTargetData(render target, system-memory surface): the target's
// pixels (the back buffer, a render target or a render-target texture's
// level) copied into the surface, which has its size and format. The GPU
// work so far is waited for (the copy is of what it drew).
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::GetRenderTargetData", D3DDev9_GetRenderTargetData, 3) {
  Device* d = Dev(c);
  auto* src = com::As<Surface>(Arg(c, 1));
  auto* dst = com::As<Surface>(Arg(c, 2));
  if (!src || !dst || dst->kind != Surface::Kind::Offscreen ||
      src->desc.Width != dst->desc.Width || src->desc.Height != dst->desc.Height)
    return D3DERR_INVALIDCALL;
  gfx::ColorTarget t = StretchTarget(src, "source");
  gfx::ColorTarget color[4] = {t};
  d->gfx->SetRenderTargets(color, 0);
  gfx::Rect r{0, 0, int32_t(dst->desc.Width), int32_t(dst->desc.Height)};
  d->gfx->ReadBack(r, GPtr(dst->mem), dst->pitch, FormatFromD3D(dst->desc.Format));
  BindTargets(*d);
  return D3D_OK;
}

}  // namespace
}  // namespace recomp::hle
