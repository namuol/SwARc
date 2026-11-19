// d3dx9_35: effects (ID3DXEffect) from compiled effect files (fx_2_0
// binaries, as fxc /T fx_2_0 writes them), on the d3d9 HLE. The file, its
// constant tables and preshaders: d3dx9_effect_format.h.
//
// The device is driven through its own methods (the game's calls take
// the same path). Parameters reach the shaders through each shader's
// constant table (its CTAB comment): matching names, registers as the
// table places them (matrices by columns or rows as it says). Begin saves
// the states, textures and shaders the technique sets, End restores them,
// unless D3DXFX_DONOTSAVESTATE.
//
// Covered: what a compiled effect of render states, sampler states,
// textures and vertex/pixel shaders needs, with numeric and texture
// parameters set by handle or name. Not covered, fatal and named: effect
// source (HLSL text), other state classes (fixed-function lights,
// materials, transforms, ...), structs, effect pools, state managers,
// parameter blocks, and the methods no game has called yet.

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "runtime/log.h"
#include "win32/d3d9_impl.h"
#include "win32/d3dx9_effect_format.h"
#include "win32/hle.h"
#include "win32/kernel32_state.h"

namespace recomp::hle {
namespace {

using namespace d3d9;
using namespace d3dx;

constexpr uint32_t D3DXFX_DONOTSAVESTATE = 1;
constexpr uint32_t D3DXERR_INVALIDDATA = 0x88760B59;

// IDirect3DDevice9 methods the effect calls (vtable slots).
enum DevSlot : uint32_t {
  kSetRenderState = 57, kSetTexture = 65, kSetSamplerState = 69, kCreateVertexShader = 91,
  kSetVertexShader = 92, kSetVsConstF = 94, kSetVsConstI = 96, kSetVsConstB = 98,
  kCreatePixelShader = 106, kSetPixelShader = 107, kSetPsConstF = 109, kSetPsConstI = 111,
  kSetPsConstB = 113,
};

// The file's parameters, techniques and shaders (EffectFile), and what
// the effect holds on the device.
struct Effect : com::Object, EffectFile {
  uint32_t device = 0;
  std::map<uint32_t, uint32_t> shaders;                         // object id -> device shader
  std::map<uint32_t, std::shared_ptr<Preshader>> preshaders;    // object id -> its, or null
  // Handles: addresses inside these blocks, one byte per parameter,
  // technique and pass (a pass's is its technique's block + index).
  uint32_t param_handles = 0, technique_handles = 0;
  std::vector<uint32_t> pass_handles;  // per technique
  int technique = 0, pass = -1;
  bool begun = false, save = false;
  // Saved at Begin, for End: render states, sampler states, textures,
  // shaders, as they were before the technique first set them.
  std::map<uint32_t, uint32_t> saved_rs;
  std::map<std::pair<uint32_t, uint32_t>, uint32_t> saved_ss;
  std::map<uint32_t, uint32_t> saved_tex;
  bool saved_vs = false, saved_ps = false;
  uint32_t old_vs = 0, old_ps = 0;
  ~Effect() override {
    if (param_handles) HeapFreeProcess(param_handles);
    if (technique_handles) HeapFreeProcess(technique_handles);
    for (uint32_t h : pass_handles)
      if (h) HeapFreeProcess(h);
  }
};

// ---- COM calls on guest objects -------------------------------------------------------

uint32_t Method(uint32_t obj, uint32_t slot) { return Rd32(Rd32(obj) + 4 * slot); }
template <typename... A>
uint32_t Call(Ctx& c, uint32_t obj, uint32_t slot, A... args) {
  return uint32_t(CallGuest(c, Method(obj, slot), {uint64_t(obj), uint64_t(args)...}));
}
void AddRefObj(Ctx& c, uint32_t obj) {
  if (obj) Call(c, obj, 1);
}
void ReleaseObj(Ctx& c, uint32_t obj) {
  if (obj) Call(c, obj, 2);
}

// ---- applying ---------------------------------------------------------------------------

Effect* Fx(Ctx& c) {
  auto* e = com::As<Effect>(Arg(c, 0));
  if (!e) FatalF("ID3DXEffect call on unknown object %08X", uint32_t(Arg(c, 0)));
  return e;
}

int ParamByName(const Effect& e, const std::string& name) { return ParamByName(e.params, name); }
// A parameter handle (or name), -1 if none.
int ParamOf(const Effect& e, uint32_t h) {
  if (!h) return -1;
  if (h >= e.param_handles && h < e.param_handles + e.params.size()) return int(h - e.param_handles);
  return ParamByName(e, GStr(h));
}
int TechniqueOf(const Effect& e, uint32_t h) {
  if (h >= e.technique_handles && h < e.technique_handles + e.techniques.size())
    return int(h - e.technique_handles);
  if (!h) return -1;
  std::string name = GStr(h);
  for (size_t i = 0; i < e.techniques.size(); i++)
    if (e.techniques[i].name == name) return int(i);
  return -1;
}

uint32_t ShaderFor(Ctx& c, Effect& e, uint32_t id, bool pixel) {
  if (!id) return 0;
  auto it = e.shaders.find(id);
  if (it != e.shaders.end()) return it->second;
  auto code = e.shader_code.find(id);
  if (code == e.shader_code.end()) FatalF("ID3DXEffect: shader object %u has no code", id);
  const uint32_t bytes = uint32_t(code->second.size() * 4);
  const uint32_t mem = HeapAllocProcess(bytes + 4, false);
  std::memcpy(GPtr(mem), code->second.data(), bytes);
  Wr32(mem + bytes, 0);
  Call(c, e.device, pixel ? kCreatePixelShader : kCreateVertexShader, mem, mem + bytes);
  const uint32_t shader = Rd32(mem + bytes);
  HeapFreeProcess(mem);
  if (!shader) FatalF("ID3DXEffect: Create%sShader failed", pixel ? "Pixel" : "Vertex");
  e.shaders[id] = shader;
  return shader;
}

void SetRs(Ctx& c, Effect& e, uint32_t rs, uint32_t v) {
  if (e.save && !e.saved_rs.count(rs)) e.saved_rs[rs] = DeviceOf(e.device)->rs[rs & 255];
  Call(c, e.device, kSetRenderState, rs, v);
}
void SetSs(Ctx& c, Effect& e, uint32_t s, uint32_t st, uint32_t v) {
  if (e.save && !e.saved_ss.count({s, st})) {
    int idx = SamplerIndex(s);
    e.saved_ss[{s, st}] = idx >= 0 && st < 14 ? DeviceOf(e.device)->ss[idx][st] : 0;
  }
  Call(c, e.device, kSetSamplerState, s, st, v);
}
void SetTex(Ctx& c, Effect& e, uint32_t s, uint32_t tex) {
  if (e.save && !e.saved_tex.count(s)) {
    int idx = SamplerIndex(s);
    uint32_t old = idx >= 0 ? DeviceOf(e.device)->textures[idx] : 0;
    AddRefObj(c, old);
    e.saved_tex[s] = old;
  }
  Call(c, e.device, kSetTexture, s, tex);
}

// Parameters to one shader's registers, and its samplers' textures and
// states.
void Upload(Ctx& c, Effect& e, uint32_t shader_id, bool pixel) {
  auto code = e.shader_code.find(shader_id);
  if (code == e.shader_code.end()) return;
  const uint32_t mem = HeapAllocProcess(4 * 4 * 256, false);
  for (const CtabConstant& k : ReadCtab(code->second)) {
    int pi = ParamByName(e, k.name);
    if (pi < 0) continue;
    Param& p = e.params[pi];
    if (k.set == 3) {  // sampler: s<reg>, vertex samplers from D3DVERTEXTEXTURESAMPLER0
      const uint32_t s = pixel ? k.reg : 256 + k.reg;
      for (const State& st : p.sampler) {
        StateDef def = StateOf(st.op);
        if (def.cls == StateClass::Texture) {
          int ti = ParamByName(e, st.ref);
          SetTex(c, e, s, ti >= 0 ? e.params[ti].texture : 0);
        } else if (def.cls == StateClass::Sampler) {
          SetSs(c, e, s, def.value, st.value);
        } else {
          FatalF("ID3DXEffect: sampler %s: state operation %u not supported", p.name.c_str(),
                 st.op);
        }
      }
      continue;
    }
    if (k.count == 0 || k.count > 256) continue;
    std::vector<uint32_t> regs = Registers(p, k, k.set == 2);
    std::memcpy(GPtr(mem), regs.data(), regs.size() * 4);
    if (g_trace) {
      float f[4];
      std::memcpy(f, regs.data(), 16);
      RECOMP_TRACE("  d3dx: %s %c%u x%u <- %s (%g %g %g %g)\n", pixel ? "ps" : "vs",
                   k.set == 2 ? 'c' : k.set == 1 ? 'i' : 'b', k.reg, k.count, p.name.c_str(),
                   f[0], f[1], f[2], f[3]);
    }
    if (k.set == 2) {
      Call(c, e.device, pixel ? kSetPsConstF : kSetVsConstF, k.reg, mem, k.count);
    } else if (k.set == 1) {
      Call(c, e.device, pixel ? kSetPsConstI : kSetVsConstI, k.reg, mem, k.count);
    } else {
      for (uint32_t i = 0; i < k.count; i++) Wr32(mem + 4 * i, Rd32(mem + 16 * i) ? 1 : 0);
      Call(c, e.device, pixel ? kSetPsConstB : kSetVsConstB, k.reg, mem, k.count);
    }
  }
  // The preshader's results (registers the shader's own table leaves out).
  auto pre = e.preshaders.find(shader_id);
  if (pre == e.preshaders.end()) {
    Preshader ps;
    pre = e.preshaders.emplace(shader_id, ReadPreshader(code->second, &ps)
                                              ? std::make_shared<Preshader>(std::move(ps))
                                              : nullptr).first;
  }
  if (pre->second) {
    std::map<uint32_t, std::array<float, 4>> regs;
    RunPreshader(e.params, *pre->second, &regs);
    for (auto& [reg, v] : regs) {
      std::memcpy(GPtr(mem), v.data(), 16);
      Call(c, e.device, pixel ? kSetPsConstF : kSetVsConstF, reg, mem, 1);
    }
  }
  HeapFreeProcess(mem);
}

const Pass& CurrentPass(const Effect& e) { return e.techniques[e.technique].passes[e.pass]; }

void Commit(Ctx& c, Effect& e) {
  for (const State& st : CurrentPass(e).states) {
    StateDef def = StateOf(st.op);
    if (def.cls == StateClass::VertexShader) Upload(c, e, st.value, false);
    if (def.cls == StateClass::PixelShader) Upload(c, e, st.value, true);
  }
}

void ApplyPass(Ctx& c, Effect& e) {
  Device* d = DeviceOf(e.device);
  for (const State& st : CurrentPass(e).states) {
    StateDef def = StateOf(st.op);
    switch (def.cls) {
      case StateClass::Render:
        SetRs(c, e, def.value, st.value);
        break;
      case StateClass::VertexShader:
        if (e.save && !e.saved_vs) {
          e.saved_vs = true;
          e.old_vs = d->vs;
          AddRefObj(c, e.old_vs);
        }
        Call(c, e.device, kSetVertexShader, ShaderFor(c, e, st.value, false));
        break;
      case StateClass::PixelShader:
        if (e.save && !e.saved_ps) {
          e.saved_ps = true;
          e.old_ps = d->ps;
          AddRefObj(c, e.old_ps);
        }
        Call(c, e.device, kSetPixelShader, ShaderFor(c, e, st.value, true));
        break;
      default:
        FatalF("ID3DXEffect: pass %s: state operation %u not supported",
               CurrentPass(e).name.c_str(), st.op);
    }
  }
  Commit(c, e);
}

void Restore(Ctx& c, Effect& e) {
  for (auto& [rs, v] : e.saved_rs) Call(c, e.device, kSetRenderState, rs, v);
  for (auto& [k, v] : e.saved_ss) Call(c, e.device, kSetSamplerState, k.first, k.second, v);
  for (auto& [s, tex] : e.saved_tex) {
    Call(c, e.device, kSetTexture, s, tex);
    ReleaseObj(c, tex);
  }
  if (e.saved_vs) {
    Call(c, e.device, kSetVertexShader, e.old_vs);
    ReleaseObj(c, e.old_vs);
  }
  if (e.saved_ps) {
    Call(c, e.device, kSetPixelShader, e.old_ps);
    ReleaseObj(c, e.old_ps);
  }
  e.saved_rs.clear();
  e.saved_ss.clear();
  e.saved_tex.clear();
  e.saved_vs = e.saved_ps = false;
}

uint32_t EffectVtable() {
  return com::Vtable(
      "d3dx9_35.dll", "ID3DXEffect",
      {"QueryInterface", "AddRef", "Release",
       // ID3DXBaseEffect
       "GetDesc", "GetParameterDesc", "GetTechniqueDesc", "GetPassDesc", "GetFunctionDesc",
       "GetParameter", "GetParameterByName", "GetParameterBySemantic", "GetParameterElement",
       "GetTechnique", "GetTechniqueByName", "GetPass", "GetPassByName", "GetFunction",
       "GetFunctionByName", "GetAnnotation", "GetAnnotationByName", "SetValue", "GetValue",
       "SetBool", "GetBool", "SetBoolArray", "GetBoolArray", "SetInt", "GetInt",
       "SetIntArray", "GetIntArray", "SetFloat", "GetFloat", "SetFloatArray",
       "GetFloatArray", "SetVector", "GetVector", "SetVectorArray", "GetVectorArray",
       "SetMatrix", "GetMatrix", "SetMatrixArray", "GetMatrixArray",
       "SetMatrixPointerArray", "GetMatrixPointerArray", "SetMatrixTranspose",
       "GetMatrixTranspose", "SetMatrixTransposeArray", "GetMatrixTransposeArray",
       "SetMatrixTransposePointerArray", "GetMatrixTransposePointerArray", "SetString",
       "GetString", "SetTexture", "GetTexture", "GetPixelShader", "GetVertexShader",
       "SetArrayRange",
       // ID3DXEffect
       "GetPool", "SetTechnique", "GetCurrentTechnique", "ValidateTechnique",
       "FindNextValidTechnique", "IsParameterUsed", "Begin", "BeginPass", "CommitChanges",
       "EndPass", "End", "GetDevice", "OnLostDevice", "OnResetDevice", "SetStateManager",
       "GetStateManager", "BeginParameterBlock", "EndParameterBlock", "ApplyParameterBlock",
       "DeleteParameterBlock", "CloneEffect", "SetRawValue"});
}

// Sets `n` components of parameter `h` from `src` (float bits or ints, as
// `src_float` says), converted to the parameter's type.
uint32_t SetComponents(Ctx& c, const uint32_t* src, uint32_t n, bool src_float) {
  Effect* e = Fx(c);
  int pi = ParamOf(*e, Arg(c, 1));
  if (pi < 0) return D3DERR_INVALIDCALL;
  Param& p = e->params[pi];
  n = std::min<uint32_t>(n, uint32_t(p.value.size()));
  for (uint32_t i = 0; i < n; i++) {
    uint32_t v = src[i];
    if (p.type == kFloat && !src_float) {
      float f = float(int32_t(v));
      std::memcpy(&v, &f, 4);
    } else if (p.type != kFloat && src_float) {
      float f;
      std::memcpy(&f, &v, 4);
      v = p.type == kBool ? (f != 0.0f) : uint32_t(int32_t(f));
    }
    p.value[i] = v;
  }
  return D3D_OK;
}

// ---- creation ---------------------------------------------------------------------------

// D3DXCreateEffectFromFileA(device, file, defines, include, flags, pool,
// ppEffect, ppCompilationErrors)
HLE("d3dx9_35.dll", D3DXCreateEffectFromFileA, 8) {
  const uint32_t device = Arg(c, 0), out = Arg(c, 6);
  if (!device || !Arg(c, 1) || !out) return D3DERR_INVALIDCALL;
  if (Arg(c, 5)) FatalF("D3DXCreateEffectFromFileA: effect pools not supported");
  const std::string path = GStr(Arg(c, 1));
  std::vector<uint8_t> data;
  if (std::FILE* f = std::fopen(HostPath(path).c_str(), "rb")) {
    uint8_t buf[65536];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) data.insert(data.end(), buf, buf + n);
    std::fclose(f);
  } else {
    WarnF("D3DXCreateEffectFromFileA: %s not found\n", path.c_str());
    return 0x88760B54;  // D3DXERR_INVALIDDATA's neighbour: the file is missing (D3DERR_NOTFOUND)
  }
  if (data.size() < 4) return D3DXERR_INVALIDDATA;
  uint32_t tag;
  std::memcpy(&tag, data.data(), 4);
  if (tag != kFx20)
    FatalF("D3DXCreateEffectFromFileA(%s): effect source (HLSL text) not supported, only "
           "compiled fx_2_0 effects",
           path.c_str());
  auto e = std::make_shared<Effect>();
  e->device = device;
  std::string err;
  if (!Load(e.get(), data, &err)) {
    WarnF("D3DXCreateEffectFromFileA(%s): %s\n", path.c_str(), err.c_str());
    return D3DXERR_INVALIDDATA;
  }
  e->param_handles = HeapAllocProcess(uint32_t(std::max<size_t>(e->params.size(), 1)), true);
  e->technique_handles = HeapAllocProcess(uint32_t(std::max<size_t>(e->techniques.size(), 1)), true);
  for (const Technique& t : e->techniques)
    e->pass_handles.push_back(HeapAllocProcess(uint32_t(std::max<size_t>(t.passes.size(), 1)), true));
  AddRefObj(c, device);
  RECOMP_TRACE("  d3dx: effect %s: %zu parameters, %zu techniques\n", path.c_str(),
               e->params.size(), e->techniques.size());
  Wr32(out, com::Publish(e, EffectVtable()));
  if (Arg(c, 7)) Wr32(Arg(c, 7), 0);
  return D3D_OK;
}

// ---- ID3DXEffect --------------------------------------------------------------------------

HLE_NAMED("d3dx9_35.dll", "ID3DXEffect::AddRef", D3DXEffect_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("d3dx9_35.dll", "ID3DXEffect::Release", D3DXEffect_Release, 1) {
  Effect* e = Fx(c);
  // What the effect holds on the device, released with its last reference.
  std::vector<uint32_t> held;
  for (const Param& p : e->params)
    if (p.texture) held.push_back(p.texture);
  for (auto& [id, sh] : e->shaders) held.push_back(sh);
  held.push_back(e->device);
  const uint32_t left = com::Release(Arg(c, 0));
  if (left == 0)
    for (uint32_t o : held) ReleaseObj(c, o);
  return left;
}

// GetParameterByName(parent, name): top-level parameters only.
HLE_NAMED("d3dx9_35.dll", "ID3DXEffect::GetParameterByName", D3DXEffect_GetParameterByName, 3) {
  Effect* e = Fx(c);
  if (Arg(c, 1)) FatalF("ID3DXEffect::GetParameterByName: struct members not supported");
  int i = Arg(c, 2) ? ParamByName(*e, GStr(Arg(c, 2))) : -1;
  return i < 0 ? 0 : e->param_handles + uint32_t(i);
}
// GetParameter(parent, index)
HLE_NAMED("d3dx9_35.dll", "ID3DXEffect::GetParameter", D3DXEffect_GetParameter, 3) {
  Effect* e = Fx(c);
  if (Arg(c, 1)) FatalF("ID3DXEffect::GetParameter: struct members not supported");
  return Arg(c, 2) < e->params.size() ? e->param_handles + Arg(c, 2) : 0;
}
HLE_NAMED("d3dx9_35.dll", "ID3DXEffect::GetTechniqueByName", D3DXEffect_GetTechniqueByName, 2) {
  Effect* e = Fx(c);
  int i = TechniqueOf(*e, Arg(c, 1) ? Arg(c, 1) : 0);
  return i < 0 ? 0 : e->technique_handles + uint32_t(i);
}
HLE_NAMED("d3dx9_35.dll", "ID3DXEffect::GetTechnique", D3DXEffect_GetTechnique, 2) {
  Effect* e = Fx(c);
  return Arg(c, 1) < e->techniques.size() ? e->technique_handles + Arg(c, 1) : 0;
}
HLE_NAMED("d3dx9_35.dll", "ID3DXEffect::SetTechnique", D3DXEffect_SetTechnique, 2) {
  Effect* e = Fx(c);
  int i = TechniqueOf(*e, Arg(c, 1));
  if (i < 0) return D3DERR_INVALIDCALL;
  e->technique = i;
  return D3D_OK;
}
HLE_NAMED("d3dx9_35.dll", "ID3DXEffect::GetCurrentTechnique", D3DXEffect_GetCurrentTechnique, 1) {
  Effect* e = Fx(c);
  return e->technique_handles + uint32_t(e->technique);
}
HLE_NAMED("d3dx9_35.dll", "ID3DXEffect::ValidateTechnique", D3DXEffect_ValidateTechnique, 2) {
  return TechniqueOf(*Fx(c), Arg(c, 1)) < 0 ? D3DERR_INVALIDCALL : D3D_OK;
}

// FindNextValidTechnique(after, pNext): the technique after `after` (the
// first if null); every technique validates here. S_FALSE and null past
// the last.
HLE_NAMED("d3dx9_35.dll", "ID3DXEffect::FindNextValidTechnique", D3DXEffect_FindNextValidTechnique, 3) {
  Effect* e = Fx(c);
  if (!Arg(c, 2)) return D3DERR_INVALIDCALL;
  int next = 0;
  if (Arg(c, 1)) {
    int i = TechniqueOf(*e, Arg(c, 1));
    if (i < 0) return D3DERR_INVALIDCALL;
    next = i + 1;
  }
  if (next >= int(e->techniques.size())) {
    Wr32(Arg(c, 2), 0);
    return 1;  // S_FALSE
  }
  Wr32(Arg(c, 2), e->technique_handles + uint32_t(next));
  return D3D_OK;
}

// SetValue(h, data, bytes): raw components.
HLE_NAMED("d3dx9_35.dll", "ID3DXEffect::SetValue", D3DXEffect_SetValue, 4) {
  Effect* e = Fx(c);
  int pi = ParamOf(*e, Arg(c, 1));
  if (pi < 0 || !Arg(c, 2)) return D3DERR_INVALIDCALL;
  Param& p = e->params[pi];
  if (IsTexture(p.type)) FatalF("ID3DXEffect::SetValue on texture %s", p.name.c_str());
  const uint32_t n = std::min<uint32_t>(Arg(c, 3) / 4, uint32_t(p.value.size()));
  for (uint32_t i = 0; i < n; i++) p.value[i] = Rd32(Arg(c, 2) + 4 * i);
  return D3D_OK;
}
HLE_NAMED("d3dx9_35.dll", "ID3DXEffect::SetFloat", D3DXEffect_SetFloat, 3) {
  const uint32_t v = Arg(c, 2);
  return SetComponents(c, &v, 1, true);
}
HLE_NAMED("d3dx9_35.dll", "ID3DXEffect::SetInt", D3DXEffect_SetInt, 3) {
  const uint32_t v = Arg(c, 2);
  return SetComponents(c, &v, 1, false);
}
HLE_NAMED("d3dx9_35.dll", "ID3DXEffect::SetBool", D3DXEffect_SetBool, 3) {
  const uint32_t v = Arg(c, 2) ? 1 : 0;
  return SetComponents(c, &v, 1, false);
}
// SetFloatArray(h, floats, count)
HLE_NAMED("d3dx9_35.dll", "ID3DXEffect::SetFloatArray", D3DXEffect_SetFloatArray, 4) {
  std::vector<uint32_t> v(Arg(c, 3));
  for (uint32_t i = 0; i < v.size(); i++) v[i] = Rd32(Arg(c, 2) + 4 * i);
  return SetComponents(c, v.data(), uint32_t(v.size()), true);
}
// SetVector(h, D3DXVECTOR4*): as many components as the parameter has.
HLE_NAMED("d3dx9_35.dll", "ID3DXEffect::SetVector", D3DXEffect_SetVector, 3) {
  if (!Arg(c, 2)) return D3DERR_INVALIDCALL;
  uint32_t v[4];
  for (int i = 0; i < 4; i++) v[i] = Rd32(Arg(c, 2) + 4 * uint32_t(i));
  return SetComponents(c, v, 4, true);
}
// SetMatrix(h, D3DXMATRIX*): the parameter's rows x columns of it.
HLE_NAMED("d3dx9_35.dll", "ID3DXEffect::SetMatrix", D3DXEffect_SetMatrix, 3) {
  Effect* e = Fx(c);
  int pi = ParamOf(*e, Arg(c, 1));
  if (pi < 0 || !Arg(c, 2)) return D3DERR_INVALIDCALL;
  Param& p = e->params[pi];
  for (uint32_t i = 0; i < p.rows; i++)
    for (uint32_t j = 0; j < p.columns; j++)
      if (size_t(i) * p.columns + j < p.value.size())
        p.value[size_t(i) * p.columns + j] = Rd32(Arg(c, 2) + 4 * (i * 4 + j));
  return D3D_OK;
}
HLE_NAMED("d3dx9_35.dll", "ID3DXEffect::SetMatrixTranspose", D3DXEffect_SetMatrixTranspose, 3) {
  Effect* e = Fx(c);
  int pi = ParamOf(*e, Arg(c, 1));
  if (pi < 0 || !Arg(c, 2)) return D3DERR_INVALIDCALL;
  Param& p = e->params[pi];
  for (uint32_t i = 0; i < p.rows; i++)
    for (uint32_t j = 0; j < p.columns; j++)
      if (size_t(i) * p.columns + j < p.value.size())
        p.value[size_t(i) * p.columns + j] = Rd32(Arg(c, 2) + 4 * (j * 4 + i));
  return D3D_OK;
}
// SetTexture(h, IDirect3DBaseTexture9*): referenced while set.
HLE_NAMED("d3dx9_35.dll", "ID3DXEffect::SetTexture", D3DXEffect_SetTexture, 3) {
  Effect* e = Fx(c);
  int pi = ParamOf(*e, Arg(c, 1));
  if (pi < 0 || !IsTexture(e->params[pi].type)) return D3DERR_INVALIDCALL;
  Param& p = e->params[pi];
  if (p.texture == Arg(c, 2)) return D3D_OK;
  AddRefObj(c, Arg(c, 2));
  ReleaseObj(c, p.texture);
  p.texture = Arg(c, 2);
  return D3D_OK;
}

// Begin(pPasses, flags)
HLE_NAMED("d3dx9_35.dll", "ID3DXEffect::Begin", D3DXEffect_Begin, 3) {
  Effect* e = Fx(c);
  if (e->techniques.empty()) return D3DERR_INVALIDCALL;
  e->begun = true;
  e->save = !(Arg(c, 2) & D3DXFX_DONOTSAVESTATE);
  e->pass = -1;
  if (Arg(c, 1)) Wr32(Arg(c, 1), uint32_t(e->techniques[e->technique].passes.size()));
  return D3D_OK;
}
HLE_NAMED("d3dx9_35.dll", "ID3DXEffect::BeginPass", D3DXEffect_BeginPass, 2) {
  Effect* e = Fx(c);
  if (!e->begun || Arg(c, 1) >= e->techniques[e->technique].passes.size()) return D3DERR_INVALIDCALL;
  e->pass = int(Arg(c, 1));
  ApplyPass(c, *e);
  return D3D_OK;
}
HLE_NAMED("d3dx9_35.dll", "ID3DXEffect::CommitChanges", D3DXEffect_CommitChanges, 1) {
  Effect* e = Fx(c);
  if (e->pass >= 0) Commit(c, *e);
  return D3D_OK;
}
HLE_NAMED("d3dx9_35.dll", "ID3DXEffect::EndPass", D3DXEffect_EndPass, 1) {
  Fx(c)->pass = -1;
  return D3D_OK;
}
HLE_NAMED("d3dx9_35.dll", "ID3DXEffect::End", D3DXEffect_End, 1) {
  Effect* e = Fx(c);
  if (e->save) Restore(c, *e);
  e->begun = false;
  e->pass = -1;
  return D3D_OK;
}
// Device loss: shaders and parameters survive a reset (they are not in
// D3DPOOL_DEFAULT).
HLE_NAMED("d3dx9_35.dll", "ID3DXEffect::OnLostDevice", D3DXEffect_OnLostDevice, 1) { return D3D_OK; }
HLE_NAMED("d3dx9_35.dll", "ID3DXEffect::OnResetDevice", D3DXEffect_OnResetDevice, 1) { return D3D_OK; }
HLE_NAMED("d3dx9_35.dll", "ID3DXEffect::GetDevice", D3DXEffect_GetDevice, 2) {
  Effect* e = Fx(c);
  if (!Arg(c, 1)) return D3DERR_INVALIDCALL;
  AddRefObj(c, e->device);
  Wr32(Arg(c, 1), e->device);
  return D3D_OK;
}

}  // namespace
}  // namespace recomp::hle
