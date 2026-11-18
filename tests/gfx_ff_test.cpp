// gfx::Device's fixed-function draws (DrawFf) on the SDL_GPU backend: a
// draw from separate streams (RGBA colours, 32-bit indices, a first
// vertex past 0), which the device reads as they lie, gives the same
// pixels as the same vertices in records with an attribute off a word
// boundary, which it interleaves first; so do interleaved vertices
// (D3DCOLORs, 16-bit indices) and a stream off a word boundary; an absent
// stream reads as zeros; a locked stream is taken once per lock; a stage
// whose colour takes an unbound texture is disabled, and an alpha taken
// from one reads zero. The fragment shader variants compiled per state
// (FF_VARIANTS=sync) give the same pixels as the general shader
// (FF_VARIANTS=off) for a range of states (within one step on Metal, where
// the arithmetic is 16-bit). An alpha test the device
// leaves out because the texture's alpha cannot fail it comes back when
// the texture gets a texel that fails. A draw from locked streams repeated
// over frames, drawn from the device's persistent vertices, gives the
// same pixels in every frame. Needs a Vulkan device
// (lavapipe will do); exits 77 (skipped) without one.

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "runtime/env.h"
#include "runtime/gfx/device.h"

namespace {

using namespace recomp::gfx;

constexpr int kSize = 32;
int failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    failures++;
  }
}

uint32_t Bits(float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}

// The scene's vertices: position, normal, colour (R, G, B, A bytes) and
// one texture coordinate set, kept in separate arrays as a renderer
// keeps them, with two unused vertices first.
struct Scene {
  std::vector<float> pos, normal, uv;
  std::vector<uint8_t> rgba;
  std::vector<uint32_t> indices;  // of the arrays' vertices
  static constexpr uint32_t kFirst = 2;
};

Scene MakeScene() {
  Scene s;
  auto vertex = [&](float x, float y, float z, uint32_t rgba, float u, float v) {
    s.pos.insert(s.pos.end(), {x, y, z, 1});  // 16 bytes apart; w unused
    s.normal.insert(s.normal.end(), {0, 0, -1});
    for (int i = 0; i < 4; i++) s.rgba.push_back(uint8_t(rgba >> (24 - 8 * i)));
    s.uv.insert(s.uv.end(), {u, v});
  };
  vertex(9, 9, 9, 0, 0, 0);  // unused
  vertex(9, 9, 9, 0, 0, 0);
  vertex(-0.9f, -0.9f, 0.5f, 0xFF4020FF, 0, 0);
  vertex(0.9f, -0.8f, 0.3f, 0x20FF40C0, 1, 0);
  vertex(-0.7f, 0.9f, 0.7f, 0x4020FF80, 0, 1);
  vertex(0.8f, 0.85f, 0.4f, 0xFFFFFF40, 1, 1);
  s.indices = {2, 3, 4, 3, 5, 4};
  return s;
}

// The state the draws have: blending, alpha test, a modulating texture
// stage, fog, lighting with two lights, a projection and world matrix.
struct StateValues {
  std::vector<std::pair<uint32_t, uint32_t>> rs;
  std::vector<std::pair<uint32_t, uint32_t>> tss;  // stage 0
  float world[16], proj[16];
  Material mat;
  Light lights[2];
  Viewport vp;
};

StateValues MakeState() {
  StateValues v{};
  v.rs = {{27, 1}, {19, 5}, {20, 6},                    // blend SRCALPHA, INVSRCALPHA
          {15, 1}, {25, 5}, {24, 0x30},                 // alpha test GREATER 0x30
          {22, 1},                                      // no culling
          {28, 1}, {35, 0}, {140, 3}, {34, 0x00306090},  // linear vertex fog
          {36, Bits(0.2f)}, {37, Bits(0.9f)},
          {137, 1}, {139, 0xFF202020}, {141, 1}, {145, 1}};
  v.tss = {{1, 4}, {2, 2}, {3, 0}, {4, 4}, {5, 2}, {6, 0}, {16, 2}, {17, 2}};
  const float world[16] = {0.9f, 0.1f, 0, 0, -0.1f, 0.9f, 0, 0, 0, 0, 1, 0, 0.05f, 0, 0, 1};
  const float proj[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  std::memcpy(v.world, world, sizeof world);
  std::memcpy(v.proj, proj, sizeof proj);
  v.mat = {{0.8f, 0.7f, 0.6f, 1}, {0.3f, 0.3f, 0.3f, 1}, {0, 0, 0, 0}, {0.1f, 0, 0, 0}, 0};
  v.lights[0] = {};
  v.lights[0].type = 3;
  v.lights[0].diffuse = {1, 0.9f, 0.8f, 1};
  v.lights[0].direction[2] = 1;
  v.lights[1] = {};
  v.lights[1].type = 1;
  v.lights[1].diffuse = {0.2f, 0.4f, 1, 1};
  v.lights[1].position[2] = -2;
  v.lights[1].range = 100;
  v.lights[1].attenuation0 = 1;
  v.vp = {2, 3, kSize - 4, kSize - 5, 0, 1};
  return v;
}

std::vector<uint32_t> Read(Device& dev) {
  std::vector<uint32_t> px(kSize * kSize);
  dev.ReadBack({0, 0, kSize, kSize}, px.data(), kSize * 4, Format::A8R8G8B8);
  return px;
}

void Clear(Device& dev) { dev.Clear(0, nullptr, 1, 0xFF102030, 1, 0); }

}  // namespace

int main() {
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::printf("skipped: SDL video: %s\n", SDL_GetError());
    return 77;
  }
  std::unique_ptr<Device> dev = CreateGpuDevice(nullptr);
  if (!dev) {
    std::printf("skipped: no SDL_GPU device\n");
    return 77;
  }
  dev->ResetSwapChain(kSize, kSize, false);
  dev->SetRenderTarget(0, 0);

  // A 2x2 texture.
  const TextureId tex = dev->CreateTexture(2, 2, 1, Format::A8R8G8B8, false);
  const uint32_t texels[4] = {0xFFFF0000, 0x80FFFFFF, 0xFF00FF00, 0xC00000FF};
  dev->UpdateTexture(tex, 0, {0, 0, 2, 2}, texels, 8);

  const Scene sc = MakeScene();
  const StateValues sv = MakeState();

  // ---- 1. The reference: records with the colour 2 bytes past a word
  // boundary (position, normal, 2 bytes, colour, texture coordinates),
  // which the device interleaves before it draws.
  FfState st = DefaultFfState();
  for (auto [s, v] : sv.rs) st.rs[s] = v;
  for (auto [t, v] : sv.tss) st.tss[0][t] = v;
  st.textures[0] = tex;
  std::memcpy(st.world[0], sv.world, 64);
  std::memcpy(st.proj, sv.proj, 64);
  st.material = sv.mat;
  st.lights[0] = sv.lights[0];
  st.lights[1] = sv.lights[1];
  st.light_count = 2;
  st.viewport = sv.vp;
  const uint32_t fvf = 0x002 | 0x010 | 0x040 | 0x100;  // XYZ NORMAL DIFFUSE TEX1
  const uint32_t nall = uint32_t(sc.pos.size() / 4), nv = nall - Scene::kFirst;
  constexpr uint32_t kRec = 40;
  std::vector<uint8_t> recs(size_t(nall) * kRec);
  for (uint32_t v = 0; v < nall; v++) {
    std::memcpy(&recs[v * kRec], &sc.pos[v * 4], 12);
    std::memcpy(&recs[v * kRec + 12], &sc.normal[v * 3], 12);
    std::memcpy(&recs[v * kRec + 26], &sc.rgba[v * 4], 4);
    std::memcpy(&recs[v * kRec + 32], &sc.uv[v * 2], 8);
  }
  FfDraw ref;
  ref.prim = 4;
  ref.fvf = fvf;
  ref.position = {recs.data(), kRec};
  ref.normal = {recs.data() + 12, kRec};
  ref.diffuse = {recs.data() + 26, kRec};
  ref.rgba_colors = true;
  ref.tex[0] = {recs.data() + 32, kRec};
  ref.first_vertex = Scene::kFirst;
  ref.vertex_count = nv;
  ref.indices = sc.indices.data();
  ref.index_count = uint32_t(sc.indices.size());
  ref.index_size = 4;
  Clear(*dev);
  dev->DrawFf(st, ref);
  const std::vector<uint32_t> want = Read(*dev);
  int drawn = 0;
  for (uint32_t p : want) drawn += p != 0xFF102030;
  Check(drawn > kSize * kSize / 4, "the reference draw covers part of the target");

  // ---- 2. The same vertices from separate streams.
  FfDraw fd = ref;
  fd.position = {sc.pos.data(), 16};
  fd.normal = {sc.normal.data(), 12};
  fd.diffuse = {sc.rgba.data(), 4};
  fd.tex[0] = {sc.uv.data(), 8};
  Clear(*dev);
  dev->DrawFf(st, fd);
  Check(Read(*dev) == want, "separate streams");
  FfDraw one_uv = fd;  // every vertex with vertex 0's coordinates (0, 0)
  one_uv.tex[0].stride = 0;
  Clear(*dev);
  dev->DrawFf(st, one_uv);
  Check(Read(*dev) != want, "texture coordinates per vertex");

  // ---- 3. Interleaved vertices with D3DCOLORs and 16-bit indices.
  const uint32_t stride = FvfStride(fvf);
  std::vector<uint8_t> inter;
  std::vector<uint16_t> idx16;
  for (uint32_t v = Scene::kFirst; v < nall; v++) {
    auto put = [&](const void* p, size_t n) {
      inter.insert(inter.end(), (const uint8_t*)p, (const uint8_t*)p + n);
    };
    put(&sc.pos[v * 4], 12);
    put(&sc.normal[v * 3], 12);
    const uint8_t* c = &sc.rgba[v * 4];
    const uint8_t bgra[4] = {c[2], c[1], c[0], c[3]};
    put(bgra, 4);
    put(&sc.uv[v * 2], 8);
  }
  for (uint32_t i : sc.indices) idx16.push_back(uint16_t(i - Scene::kFirst));
  FfDraw inter_d = FfInterleaved(4, fvf, inter.data(), stride);
  inter_d.vertex_count = nv;
  inter_d.indices = idx16.data();
  inter_d.index_count = uint32_t(idx16.size());
  Clear(*dev);
  dev->DrawFf(st, inter_d);
  Check(Read(*dev) == want, "interleaved vertices");

  // ---- 4. Without indices: a fan of the same vertices.
  FfDraw fan_ref = ref, fan = fd;
  fan_ref.prim = fan.prim = 6;
  fan_ref.indices = fan.indices = nullptr;
  Clear(*dev);
  dev->DrawFf(st, fan_ref);
  const std::vector<uint32_t> want_fan = Read(*dev);
  Clear(*dev);
  dev->DrawFf(st, fan);
  Check(want_fan != want && Read(*dev) == want_fan, "a non-indexed fan");

  // ---- 5. A stream off a word boundary (copied to one).
  std::vector<uint8_t> shifted(sc.pos.size() * 4 + 2);
  std::memcpy(shifted.data() + 2, sc.pos.data(), sc.pos.size() * 4);
  FfDraw odd = fd;
  odd.position = {shifted.data() + 2, 16};
  Clear(*dev);
  dev->DrawFf(st, odd);
  Check(Read(*dev) == want, "a stream off a word boundary");

  // ---- 6. A draw with another texture after one with the first (the
  // device keeps each stage's binding from the draw before).
  const TextureId tex2 = dev->CreateTexture(2, 2, 1, Format::A8R8G8B8, false);
  const uint32_t texels2[4] = {0xFF0000FF, 0xFF00FFFF, 0x80FF00FF, 0xFFFFFF00};
  dev->UpdateTexture(tex2, 0, {0, 0, 2, 2}, texels2, 8);
  FfState other_tex = st;
  other_tex.textures[0] = tex2;
  auto forget = [&] { dev->DestroyTexture(dev->CreateTexture(1, 1, 1, Format::A8R8G8B8, false)); };
  forget();
  Clear(*dev);
  dev->DrawFf(other_tex, fd);
  const std::vector<uint32_t> want_tex2 = Read(*dev);
  forget();
  Clear(*dev);
  dev->DrawFf(st, fd);
  Clear(*dev);
  dev->DrawFf(other_tex, fd);
  Check(want_tex2 != want && Read(*dev) == want_tex2, "a draw with another texture after one");

  // ---- 7. An absent stream reads as zeros.
  const std::vector<uint8_t> zeros(sc.rgba.size(), 0);
  FfDraw black = fd;
  black.diffuse = {zeros.data(), 4};
  Clear(*dev);
  dev->DrawFf(st, black);
  const std::vector<uint32_t> want_black = Read(*dev);
  black.diffuse = {};
  Clear(*dev);
  dev->DrawFf(st, black);
  Check(Read(*dev) == want_black, "an absent stream reads as zeros");

  // ---- 8. A locked stream, opaque, drawn twice between flushes (a
  // readback flushes): with the same lock the second draw keeps the
  // first one's positions, though they changed in between (which a
  // lock's promise rules out, to see it); with another lock it takes them
  // again.
  FfState opaque = st;
  opaque.rs[27] = 0;
  std::vector<float> pos = sc.pos;
  auto twice = [&](uint32_t lock1, uint32_t lock2) {
    pos = sc.pos;
    FfDraw ld = fd;
    ld.position = {pos.data(), 16, lock1};
    Clear(*dev);
    dev->DrawFf(opaque, ld);
    for (size_t i = 0; i < pos.size(); i += 4) pos[i] = pos[i] * 0.5f - 0.4f;
    ld.position.lock = lock2;
    dev->DrawFf(opaque, ld);
    return Read(*dev);
  };
  const std::vector<uint32_t> both = twice(0, 0);
  FfDraw once = fd;
  once.position = {sc.pos.data(), 16};
  Clear(*dev);
  dev->DrawFf(opaque, once);
  const std::vector<uint32_t> first_only = Read(*dev);
  Check(both != first_only, "the moved positions draw elsewhere");
  Check(twice(7, 7) == first_only, "the same lock keeps its stream");
  Check(twice(8, 9) == both, "another lock takes the stream again");

  // ---- 12. Persistent vertices: a draw whose streams are all locked,
  // repeated over frames, is noted in the first, uploaded to the device's
  // persistent buffer in the second and drawn from it after that, with the
  // same pixels every time; under another lock other data draws as itself.
  {
    std::vector<float> p12 = sc.pos;
    auto locked = [&](uint32_t lock) {
      FfDraw pd = fd;
      pd.position = {p12.data(), 16, lock};
      for (FfStream* s : {&pd.normal, &pd.diffuse, &pd.tex[0]}) s->lock = lock;
      return pd;
    };
    // First in each frame, a draw of other vertices (all at one point, so
    // no pixels) that the device copies to its per-pass arena: a draw read
    // from there instead of the persistent buffer draws nothing.
    std::vector<float> point(sc.pos.size(), 0.0f);
    FfDraw other = fd;
    other.position = {point.data(), 16};
    auto frames = [&](uint32_t lock) {
      bool same = true;
      std::vector<uint32_t> first;
      for (int f = 0; f < 4; f++) {
        Clear(*dev);
        dev->DrawFf(opaque, other);
        dev->DrawFf(opaque, locked(lock));
        std::vector<uint32_t> px = Read(*dev);
        if (f == 0) first = px;
        same = same && px == first;
        dev->Present();
      }
      return same ? first : std::vector<uint32_t>{};
    };
    Clear(*dev);
    dev->DrawFf(opaque, locked(0));
    const std::vector<uint32_t> still = Read(*dev);
    Check(frames(100) == still, "a persistent stream draws the same in every frame");
    for (size_t i = 0; i < p12.size(); i += 4) p12[i] = p12[i] * 0.5f - 0.4f;
    Clear(*dev);
    dev->DrawFf(opaque, locked(0));
    const std::vector<uint32_t> moved = Read(*dev);
    Check(moved != still, "the moved positions draw elsewhere (12)");
    Check(frames(101) == moved, "another lock's data draws as itself");
  }

  // ---- 9. No texture on stage 0. Its colour MODULATEs TEXTURE: the
  // stage is disabled, so the draw is the diffuse colour and alpha. Only
  // its alpha taking TEXTURE: that alpha is zero, which the alpha test
  // (GREATER 0x30) discards.
  FfState no_tex = st;
  no_tex.textures[0] = 0;
  FfState disabled = no_tex;
  disabled.tss[0][1] = 1;
  Clear(*dev);
  dev->DrawFf(disabled, fd);
  const std::vector<uint32_t> want_diffuse = Read(*dev);
  Clear(*dev);
  dev->DrawFf(no_tex, fd);
  Check(want_diffuse != want && Read(*dev) == want_diffuse,
        "a colour taking an unbound texture disables the stage");
  FfState alpha_only = no_tex;
  alpha_only.tss[0][1] = 2;  // COLOROP SELECTARG1
  alpha_only.tss[0][2] = 0;  // DIFFUSE
  alpha_only.tss[0][4] = 2;  // ALPHAOP SELECTARG1
  alpha_only.tss[0][5] = 2;  // TEXTURE
  Clear(*dev);
  dev->DrawFf(alpha_only, fd);
  int kept = 0;
  for (uint32_t p : Read(*dev)) kept += p != 0xFF102030;
  Check(kept == 0, "an alpha taken from an unbound texture is zero");

  // ---- 11 (before 10, on this device). The alpha test of a draw whose
  // alpha is an opaque texture's: no fragment fails it, until a texel's
  // alpha drops to 0, after which the draw keeps fewer pixels.
  {
    const TextureId op = dev->CreateTexture(2, 2, 1, Format::A8R8G8B8, false);
    const uint32_t opaque[4] = {0xFF808080, 0xFF20C040, 0xFFFFFFFF, 0xFF4060A0};
    dev->UpdateTexture(op, 0, {0, 0, 2, 2}, opaque, 8);
    FfState a = st;
    a.textures[0] = op;
    a.tss[0][4] = 2;  // ALPHAOP SELECTARG1
    a.tss[0][5] = 2;  // TEXTURE
    a.rs[27] = 0;     // no blending: the alpha test alone decides
    auto kept = [&] {
      Clear(*dev);
      dev->DrawFf(a, fd);
      int n = 0;
      for (uint32_t p : Read(*dev)) n += p != 0xFF102030;
      return n;
    };
    const int all = kept();
    const uint32_t clear_texel = 0x00808080;
    dev->UpdateTexture(op, 0, {0, 0, 1, 1}, &clear_texel, 4);
    const int after = kept();
    Check(all > kSize * kSize / 4 && after < all, "an alpha test comes back with a clear texel");
    dev->DestroyTexture(op);
  }

  // ---- 10. Fragment shader variants: each state compiled into its own
  // shader (sync: compiled before the draw) against the general shader,
  // on a device of each kind, pixel for pixel.
  {
    recomp::SetEnvId("gfxtest");
    setenv("GFXTEST_FF_VARIANTS", "off", 1);
    std::unique_ptr<Device> general = CreateGpuDevice(nullptr);
    setenv("GFXTEST_FF_VARIANTS", "sync", 1);
    std::unique_ptr<Device> special = CreateGpuDevice(nullptr);
    unsetenv("GFXTEST_FF_VARIANTS");
    Check(general && special, "devices for the variant comparison");
    if (general && special) {
      TextureId t[2][2];
      Device* devs[2] = {general.get(), special.get()};
      for (int d = 0; d < 2; d++) {
        devs[d]->ResetSwapChain(kSize, kSize, false);
        devs[d]->SetRenderTarget(0, 0);
        t[d][0] = devs[d]->CreateTexture(2, 2, 1, Format::A8R8G8B8, false);
        devs[d]->UpdateTexture(t[d][0], 0, {0, 0, 2, 2}, texels, 8);
        const uint32_t opaque[4] = {0xFF808080, 0xFF20C040, 0xFFFFFFFF, 0xFF4060A0};
        t[d][1] = devs[d]->CreateTexture(2, 2, 1, Format::A8R8G8B8, false);
        devs[d]->UpdateTexture(t[d][1], 0, {0, 0, 2, 2}, opaque, 8);
      }
      FfDraw two_uv = fd;  // stage 1 reads the same coordinates
      two_uv.fvf = (fvf & ~0xF00u) | 0x200;  // TEX2
      two_uv.tex[1] = fd.tex[0];
      std::vector<std::pair<const char*, FfState>> states;
      states.push_back({"the scene's state", st});
      FfState s1 = st;
      s1.rs[15] = 0;  // no alpha test
      states.push_back({"no alpha test", s1});
      s1 = st;
      s1.rs[24] = 0;  // GREATER 0 with SRCALPHA blending and no depth write
      s1.rs[14] = 0;
      states.push_back({"alpha > 0 under blending", s1});
      states.push_back({"a disabled stage", disabled});
      states.push_back({"an unbound texture's alpha", alpha_only});
      const uint32_t ops[][7] = {
          // COLOROP, ARG1, ARG2, ALPHAOP, ARG1, ARG2, TFACTOR
          {7, 2, 0, 2, 0, 0, 0},                // ADD; alpha SELECTARG1 DIFFUSE
          {10, 2, 0x10, 4, 2, 3, 0x80402010},   // SUBTRACT, COMPLEMENT; MODULATE TFACTOR
          {13, 0, 3, 3, 0, 2, 0xC0C0C0C0},      // BLENDTEXTUREALPHA; SELECTARG2
          {5, 0x22, 0, 7, 2, 0, 0},             // MODULATE2X, ALPHAREPLICATE; ADD
          {11, 2, 0, 6, 2, 0, 0},               // ADDSMOOTH; MODULATE4X
          {24, 2, 0, 2, 2, 0, 0},               // DOTPRODUCT3
      };
      for (const auto& o : ops) {
        s1 = st;
        s1.tss[0][1] = o[0], s1.tss[0][2] = o[1], s1.tss[0][3] = o[2];
        s1.tss[0][4] = o[3], s1.tss[0][5] = o[4], s1.tss[0][6] = o[5];
        s1.rs[60] = o[6];
        states.push_back({"stage 0 operations", s1});
      }
      s1 = st;  // stage 0 into TEMP, stage 1 modulates the second texture by it
      s1.tss[0][28] = 5;
      s1.tss[1][1] = 4, s1.tss[1][2] = 2, s1.tss[1][3] = 5;
      s1.tss[1][4] = 2, s1.tss[1][5] = 5;
      s1.tss[1][11] = 1;
      s1.textures[1] = 2;  // the second texture, by slot (fixed below)
      states.push_back({"two stages through TEMP", s1});
      s1 = st;
      s1.rs[35] = 3;  // table fog, linear
      s1.rs[38] = Bits(0.5f);
      states.push_back({"table fog", s1});
      s1 = st;
      s1.rs[9] = 1;  // flat shading
      states.push_back({"flat shading", s1});
      for (const auto& [what, base] : states) {
        std::vector<uint32_t> img[2];
        for (int d = 0; d < 2; d++) {
          FfState s2 = base;
          s2.textures[0] = base.textures[0] ? t[d][0] : 0;
          s2.textures[1] = base.textures[1] ? t[d][1] : 0;
          devs[d]->Clear(0, nullptr, 1, 0xFF102030, 1, 0);
          devs[d]->DrawFf(s2, base.textures[1] ? two_uv : fd);
          img[d] = Read(*devs[d]);
        }
        // Pixel for pixel; on Metal the colour arithmetic is 16-bit (ff.frag,
        // HALF), where the compiler may fold a variant's constants in another
        // order than the general shader computes them: within one step.
        int maxd = 0;
        for (size_t i = 0; i < img[0].size() && img[0].size() == img[1].size(); i++)
          for (int ch = 0; ch < 4; ch++)
            maxd = std::max(maxd, std::abs(int((img[0][i] >> (8 * ch)) & 0xFF) -
                                           int((img[1][i] >> (8 * ch)) & 0xFF)));
#if defined(__APPLE__)
        constexpr int kTolerance = 1;
#else
        constexpr int kTolerance = 0;
#endif
        if (maxd > kTolerance) std::printf("%s: off by %d\n", what, maxd);
        Check(img[0].size() == img[1].size() && maxd <= kTolerance, what);
      }
      for (int d = 0; d < 2; d++)
        for (TextureId id : t[d]) devs[d]->DestroyTexture(id);
    }
  }

  dev->DestroyTexture(tex2);
  dev->DestroyTexture(tex);
  if (failures) return 1;
  std::printf("ok\n");
  return 0;
}
