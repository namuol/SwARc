// Compiled D3DX effects as data (win32/d3dx9_effect_format.h), on effect
// files and shaders built here to the fx_2_0 layout: parameters (numeric,
// sampler with a Texture reference), a technique's pass states and its
// shader; the shader's constant table and register layout (matrices by
// columns and by rows, ints as floats); a preshader's literals, inputs,
// temporaries, scalar and relative operands and its opcodes; and that
// malformed files are refused.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "win32/d3dx9_effect_format.h"

namespace {

using namespace recomp::d3dx;

int g_failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    g_failures++;
  }
}

uint32_t F(float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  return v;
}
float AsF(uint32_t v) {
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

// DWORD blobs with offsets into themselves.
struct Blob {
  std::vector<uint32_t> w;
  uint32_t Off() const { return uint32_t(w.size() * 4); }
  uint32_t Put(std::initializer_list<uint32_t> v) {
    uint32_t o = Off();
    w.insert(w.end(), v);
    return o;
  }
  uint32_t PutBytes(const std::string& s) {  // null-terminated, padded
    uint32_t o = Off();
    std::vector<uint8_t> b(s.begin(), s.end());
    b.push_back(0);
    while (b.size() % 4) b.push_back(0);
    for (size_t i = 0; i < b.size(); i += 4) {
      uint32_t v;
      std::memcpy(&v, &b[i], 4);
      w.push_back(v);
    }
    return o;
  }
};

constexpr uint32_t FourCC(const char* s) {
  return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 |
         uint32_t(uint8_t(s[3])) << 24;
}

// A comment token holding `fourcc` and `payload`.
void Comment(std::vector<uint32_t>* out, const char* fourcc, const std::vector<uint32_t>& payload) {
  out->push_back(uint32_t(1 + payload.size()) << 16 | 0xFFFE);
  out->push_back(FourCC(fourcc));
  out->insert(out->end(), payload.begin(), payload.end());
}

struct CtabEntry {
  std::string name;
  uint16_t set, reg, count, cls;
};

// A CTAB payload (after its FourCC): header, constant infos, type infos,
// names; offsets from the payload's start.
std::vector<uint32_t> Ctab(const std::vector<CtabEntry>& cs) {
  Blob b;
  b.Put({28, 0, 0xFFFE0200, uint32_t(cs.size()), 28, 0, 0});
  std::vector<uint32_t> info;
  for (size_t i = 0; i < cs.size(); i++) info.push_back(b.Put({0, 0, 0, 0, 0}));
  for (size_t i = 0; i < cs.size(); i++) {
    const uint32_t type = b.Put({uint32_t(cs[i].cls) | (3u << 16), 0, 0, 0});
    const uint32_t name = b.PutBytes(cs[i].name);
    uint32_t* ci = &b.w[info[i] / 4];
    ci[0] = name;
    ci[1] = uint32_t(cs[i].set) | uint32_t(cs[i].reg) << 16;
    ci[2] = cs[i].count;
    ci[3] = type;
  }
  return b.w;
}

// A preshader instruction: control, inputs, output; each operand is
// {table, offset} or {index table, index offset, table, offset}.
void Instr(std::vector<uint32_t>* fx, uint32_t op, uint32_t comps, bool scalar,
           const std::vector<std::vector<uint32_t>>& in, const std::vector<uint32_t>& out) {
  fx->push_back(comps | op << 20 | (scalar ? 1u << 31 : 0));
  fx->push_back(uint32_t(in.size()));
  auto operand = [&](const std::vector<uint32_t>& o) {
    fx->push_back(o.size() == 4 ? 1 : 0);
    fx->insert(fx->end(), o.begin(), o.end());
  };
  for (auto& o : in) operand(o);
  operand(out);
}

// A vs_2_0 shader: CTAB of `consts`, and the preshader (if `pres`).
std::vector<uint32_t> Shader(const std::vector<CtabEntry>& consts, const std::vector<uint32_t>* pres) {
  std::vector<uint32_t> s = {0xFFFE0200};
  Comment(&s, "CTAB", Ctab(consts));
  if (pres) Comment(&s, "PRES", *pres);
  s.push_back(0x0000FFFF);
  return s;
}

// The preshader under test. Inputs: scale (float4, c0), m (float4x4 by
// columns, c1-c4), sel (float, c5). Literals 2, 10, 20, 1.
//   c8   = 2 * scale            (scalar first operand)
//   c9.x = dot(scale, scale)
//   t0   = scale + scale; c10 = t0
//   c11  = cmp(-scale, 10, 20)  (per component: -scale >= 0 ? 10 : 20)
//   c12  = m column [sel] + 0   (relative: input c(1 + sel))
//   c13.xy = rcp(scale.xy)
std::vector<uint32_t> TestPreshader() {
  std::vector<uint32_t> p = {0x46580200};
  Comment(&p, "CTAB", Ctab({{"scale", 2, 0, 1, kVector}, {"m", 2, 1, 4, kMatrixColumns},
                            {"sel", 2, 5, 1, kScalar}}));
  std::vector<uint32_t> lit = {4};
  for (double d : {2.0, 10.0, 20.0, 1.0}) {
    uint32_t w[2];
    std::memcpy(w, &d, 8);
    lit.push_back(w[0]);
    lit.push_back(w[1]);
  }
  Comment(&p, "CLIT", lit);
  std::vector<uint32_t> fx = {8};
  Instr(&fx, 0x205, 4, true, {{1, 0}, {2, 0}}, {4, 32});            // mul
  Instr(&fx, 0x500, 4, false, {{2, 0}, {2, 0}}, {4, 36});           // dot
  Instr(&fx, 0x204, 4, false, {{2, 0}, {2, 0}}, {7, 0});            // add -> t0
  Instr(&fx, 0x100, 4, false, {{7, 0}}, {4, 40});                   // mov
  Instr(&fx, 0x101, 4, false, {{2, 0}}, {7, 4});                    // neg -> t1
  Instr(&fx, 0x300, 1, false, {{7, 4}, {1, 1}, {1, 2}}, {4, 44});   // c11.x
  Instr(&fx, 0x100, 4, false, {{2, 20, 2, 4}}, {4, 48});            // c12 = c[1 + sel]
  Instr(&fx, 0x103, 2, false, {{2, 0}}, {4, 52});                   // rcp
  Comment(&p, "FXLC", fx);
  Comment(&p, "PRSI", {0});
  p.push_back(0x0000FFFF);
  return p;
}

// An effect file: parameters scale (float4), m (float4x4), sel (float),
// tex (texture2D), samp (sampler2D: AddressU wrap, Texture <tex>); one
// technique "t" with pass "p0": ZENABLE 1 and vertex shader object 1.
std::vector<uint8_t> TestEffect(const std::vector<uint32_t>& shader) {
  Blob d;
  d.Put({0});  // offset 0 means "none"
  auto type = [&](uint32_t t, uint32_t cls, const char* name, uint32_t cols, uint32_t rows) {
    const uint32_t name_off = d.Put({uint32_t(std::strlen(name) + 1)});
    d.PutBytes(name);
    return d.Put({t, cls, name_off, 0, 0, cols, rows});
  };
  const uint32_t t_scale = type(kFloat, kVector, "scale", 4, 1);
  const uint32_t v_scale = d.Put({F(1), F(-2), F(4), F(0.5f)});
  const uint32_t t_m = type(kFloat, kMatrixRows, "m", 4, 4);
  std::vector<uint32_t> mv;
  for (int i = 0; i < 16; i++) mv.push_back(F(float(i)));  // m[r][c] = 4r + c
  const uint32_t v_m = d.Off();
  d.w.insert(d.w.end(), mv.begin(), mv.end());
  const uint32_t t_sel = type(kFloat, kScalar, "sel", 1, 1);
  const uint32_t v_sel = d.Put({F(2)});
  const uint32_t t_tex = d.Put({kTexture2D, kObject, 0, 0, 0});
  {
    const uint32_t name_off = d.Put({4});
    d.PutBytes("tex");
    d.w[t_tex / 4 + 2] = name_off;
  }
  const uint32_t v_tex = d.Put({0});
  const uint32_t t_dw = d.Put({kInt, kScalar, 0, 0, 0, 1, 1});
  const uint32_t v_wrap = d.Put({1}), v_one = d.Put({1}), v_obj1 = d.Put({1}), v_obj0 = d.Put({0});
  const uint32_t t_samp = d.Put({kSampler2D, kObject, 0, 0, 0});
  {
    const uint32_t name_off = d.Put({5});
    d.PutBytes("samp");
    d.w[t_samp / 4 + 2] = name_off;
  }
  // The sampler's states: AddressU (op 165) = wrap, Texture (op 164).
  const uint32_t v_samp = d.Put({2, 165, 0, t_dw, v_wrap, 164, 0, t_dw, v_obj0});
  const uint32_t t_vs = d.Put({kVertexShader, kObject, 0, 0, 0});
  const uint32_t tech_name = d.Put({2});
  d.PutBytes("t");
  const uint32_t pass_name = d.Put({3});
  d.PutBytes("p0");

  Blob r;  // counts and records
  r.Put({5, 1, 0, 2});
  r.Put({t_scale, v_scale, 0, 0});
  r.Put({t_m, v_m, 0, 1, 0, 0});  // one annotation, skipped
  r.Put({t_sel, v_sel, 0, 0});
  r.Put({t_tex, v_tex, 0, 0});
  r.Put({t_samp, v_samp, 0, 0});
  r.Put({tech_name, 0, 1});
  r.Put({pass_name, 0, 2});
  r.Put({0, 0, t_dw, v_one});      // ZENABLE = 1
  r.Put({146, 0, t_vs, v_obj1});   // VertexShader = object 1
  r.Put({0, 2});                   // strings, resources
  // The pass's state 1: the shader.
  r.Put({0, 0, 0xFFFFFFFF, 1, 0, uint32_t(shader.size() * 4)});
  r.w.insert(r.w.end(), shader.begin(), shader.end());
  // Parameter 4's state 1: the texture's name.
  r.Put({0xFFFFFFFF, 4, 0xFFFFFFFF, 1, 0, 4});
  r.PutBytes("tex");

  std::vector<uint32_t> all = {kFx20, d.Off()};
  all.insert(all.end(), d.w.begin(), d.w.end());
  all.insert(all.end(), r.w.begin(), r.w.end());
  std::vector<uint8_t> out(all.size() * 4);
  std::memcpy(out.data(), all.data(), out.size());
  return out;
}

bool Near(float a, float b) { return std::fabs(a - b) < 1e-6f; }

}  // namespace

int main() {
  const std::vector<uint32_t> pres = TestPreshader();
  const std::vector<uint32_t> shader =
      Shader({{"scale", 2, 0, 1, kVector}, {"m", 2, 1, 4, kMatrixColumns},
              {"samp", 3, 0, 1, kObject}}, &pres);
  const std::vector<uint8_t> file = TestEffect(shader);

  // ---- the file
  EffectFile e;
  std::string err;
  Check(Load(&e, file, &err), "Load: a well-formed effect");
  Check(e.params.size() == 5, "Load: five parameters");
  if (e.params.size() == 5) {
    const Param& s = e.params[0];
    Check(s.name == "scale" && s.type == kFloat && s.cls == kVector && s.columns == 4 &&
              s.rows == 1 && s.value.size() == 4 && AsF(s.value[1]) == -2.0f,
          "Load: float4 parameter");
    const Param& m = e.params[1];
    Check(m.name == "m" && m.rows == 4 && m.columns == 4 && m.value.size() == 16 &&
              AsF(m.value[6]) == 6.0f,
          "Load: float4x4 parameter, past an annotation");
    Check(e.params[3].name == "tex" && IsTexture(e.params[3].type), "Load: texture parameter");
    const Param& sp = e.params[4];
    Check(sp.name == "samp" && IsSampler(sp.type) && sp.sampler.size() == 2 &&
              sp.sampler[0].op == 165 && sp.sampler[0].value == 1 && sp.sampler[1].op == 164 &&
              sp.sampler[1].ref == "tex",
          "Load: sampler states and its Texture's reference");
  }
  Check(e.techniques.size() == 1 && e.techniques[0].name == "t" &&
            e.techniques[0].passes.size() == 1 && e.techniques[0].passes[0].name == "p0",
        "Load: technique and pass");
  if (!e.techniques.empty() && !e.techniques[0].passes.empty()) {
    const auto& st = e.techniques[0].passes[0].states;
    Check(st.size() == 2 && st[0].op == 0 && st[0].value == 1 && st[1].op == 146 &&
              st[1].value == 1,
          "Load: pass states");
  }
  Check(e.shader_code.count(1) && e.shader_code[1] == shader, "Load: the shader's bytecode");

  // ---- malformed files
  {
    EffectFile x;
    std::vector<uint8_t> bad = file;
    bad[0] ^= 1;
    Check(!Load(&x, bad, &err), "Load: refuses another tag");
    for (size_t cut : {size_t(8), file.size() / 2, file.size() - 8}) {
      EffectFile y;
      std::vector<uint8_t> t(file.begin(), file.begin() + long(cut));
      Check(!Load(&y, t, &err), "Load: refuses a truncated file");
    }
  }

  // ---- the state table
  Check(StateOf(0).cls == StateClass::Render && StateOf(0).value == 7, "StateOf: ZENABLE");
  Check(StateOf(20).value == 38, "StateOf: FOGDENSITY");
  Check(StateOf(75).cls == StateClass::Render && StateOf(75).value == 171, "StateOf: BLENDOP");
  Check(StateOf(146).cls == StateClass::VertexShader && StateOf(147).cls == StateClass::PixelShader,
        "StateOf: shaders");
  Check(StateOf(164).cls == StateClass::Texture, "StateOf: Texture");
  Check(StateOf(165).cls == StateClass::Sampler && StateOf(165).value == 1 &&
            StateOf(175).value == 11,
        "StateOf: sampler states");
  Check(StateOf(40).cls == StateClass::None, "StateOf: unfilled operations are None");

  // ---- constant table and registers
  const std::vector<CtabConstant> ct = ReadCtab(shader);
  Check(ct.size() == 3 && ct[0].name == "scale" && ct[1].name == "m" && ct[1].reg == 1 &&
            ct[1].count == 4 && ct[1].cls == kMatrixColumns && ct[2].set == 3,
        "ReadCtab: names, sets, registers, classes");
  Check(ReadCtab({0xFFFE0200, 0x0000FFFF}).empty(), "ReadCtab: none without a CTAB");
  if (e.params.size() == 5) {
    const Param& m = e.params[1];
    std::vector<uint32_t> cols = Registers(m, {"m", 2, 0, 4, kMatrixColumns}, true);
    Check(AsF(cols[0]) == 0 && AsF(cols[1]) == 4 && AsF(cols[2]) == 8 && AsF(cols[4]) == 1,
          "Registers: by columns, register j is column j");
    std::vector<uint32_t> rows = Registers(m, {"m", 2, 0, 3, kMatrixRows}, true);
    Check(rows.size() == 12 && AsF(rows[1]) == 1 && AsF(rows[4]) == 4 && AsF(rows[11]) == 11,
          "Registers: by rows, as many registers as the table gives");
  }
  {
    Param ip;
    ip.type = kInt;
    ip.cls = kVector;
    ip.columns = 2;
    ip.value = {3, uint32_t(-5)};
    std::vector<uint32_t> f = Registers(ip, {"i", 2, 0, 1, kVector}, true);
    Check(AsF(f[0]) == 3 && AsF(f[1]) == -5 && f[2] == 0, "Registers: ints to float registers");
    Param fp;
    fp.type = kFloat;
    fp.value = {F(7.9f)};
    std::vector<uint32_t> i = Registers(fp, {"f", 1, 0, 1, kScalar}, false);
    Check(i[0] == 7, "Registers: floats to int registers truncate");
  }

  // ---- the preshader
  Preshader ps;
  Check(!ReadPreshader({0xFFFE0200, 0x0000FFFF}, &ps), "ReadPreshader: none without a PRES");
  Check(ReadPreshader(shader, &ps), "ReadPreshader: finds it");
  Check(ps.inputs.size() == 3 && ps.literals.size() == 4 && ps.literals[1] == 10.0 &&
            ps.code.size() == 8,
        "ReadPreshader: inputs, literals, instructions");
  if (ps.code.size() == 8) {
    Check(ps.code[0].op == 0x205 && ps.code[0].scalar && ps.code[0].comps == 4,
          "ReadPreshader: control word");
    Check(ps.code[6].in[0].relative && ps.code[6].in[0].index_offset == 20,
          "ReadPreshader: relative operand");
  }
  std::map<uint32_t, std::array<float, 4>> out;
  RunPreshader(e.params, ps, &out);
  Check(out.size() == 6, "RunPreshader: the registers written, only those");
  auto is = [&](uint32_t reg, std::array<float, 4> v) {
    if (!out.count(reg)) return false;
    for (int k = 0; k < 4; k++)
      if (!Near(out[reg][k], v[k])) return false;
    return true;
  };
  Check(is(8, {2, -4, 8, 1}), "RunPreshader: mul with a scalar literal");
  Check(is(9, {1 + 4 + 16 + 0.25f, 0, 0, 0}), "RunPreshader: dot into component 0");
  Check(is(10, {2, -4, 8, 1}), "RunPreshader: through a temporary");
  Check(is(11, {20, 0, 0, 0}), "RunPreshader: cmp (-1 < 0)");
  // sel = 2: c[1 + 2] = m's register 2 = column 2 = (2, 6, 10, 14).
  Check(is(12, {2, 6, 10, 14}), "RunPreshader: relative input by a parameter");
  Check(is(13, {1, -0.5f, 0, 0}), "RunPreshader: rcp, two components");

  if (g_failures) {
    std::printf("%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("d3dx_effect: all passed\n");
  return 0;
}
