// Compiled D3DX effects as data (d3dx9_effect_format.h).

#include "win32/d3dx9_effect_format.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

#include "runtime/log.h"

namespace recomp::d3dx {

StateDef StateOf(uint32_t op) {
  // 0-20: render states D3DRS_ZENABLE ... D3DRS_FOGDENSITY.
  static const uint32_t kRs0[] = {7,  8,  9,  14, 15, 16, 19, 20, 22, 23, 24,
                                  25, 26, 27, 28, 29, 34, 35, 36, 37, 38};
  // 60-90: D3DRS_POINTSIZE ... D3DRS_CCW_STENCILZFAIL.
  static const uint32_t kRs60[] = {154, 155, 166, 156, 157, 158, 159, 160, 161, 162, 163,
                                   165, 167, 168, 170, 171, 172, 173, 174, 175, 176, 178,
                                   179, 180, 181, 182, 183, 184, 185, 186, 187};
  if (op <= 20) return {StateClass::Render, kRs0[op]};
  if (op >= 60 && op <= 90) return {StateClass::Render, kRs60[op - 60]};
  if (op == 146) return {StateClass::VertexShader, 0};
  if (op == 147) return {StateClass::PixelShader, 0};
  if (op == 164) return {StateClass::Texture, 0};
  // 165-175: D3DSAMP_ADDRESSU (1) ... D3DSAMP_SRGBTEXTURE (11).
  if (op >= 165 && op <= 175) return {StateClass::Sampler, op - 164};
  return {StateClass::None, 0};
}

namespace {

struct Reader {
  const std::vector<uint8_t>& d;
  bool ok = true;
  uint32_t U(size_t off) {
    if (off + 4 > d.size()) {
      ok = false;
      return 0;
    }
    uint32_t v;
    std::memcpy(&v, d.data() + off, 4);
    return v;
  }
  // A string at base offset `off`: length (with the null), bytes.
  std::string Str(uint32_t off) {
    if (!off) return "";
    uint32_t n = U(8 + off);
    if (!ok || 8 + off + 4 + n > d.size()) {
      ok = false;
      return "";
    }
    std::string s(reinterpret_cast<const char*>(d.data()) + 8 + off + 4, n);
    return s.substr(0, s.find('\0'));
  }
};

// The parameter's type (at base offset `to`) and value (at `vo`).
void ReadParam(Reader& r, uint32_t to, uint32_t vo, Param* p) {
  const size_t t = 8 + size_t(to);
  p->type = r.U(t);
  p->cls = r.U(t + 4);
  p->name = r.Str(r.U(t + 8));
  p->semantic = r.Str(r.U(t + 12));
  p->elements = r.U(t + 16);
  const uint32_t count = std::max(p->elements, 1u);
  if (p->cls <= kMatrixColumns) {
    p->columns = r.U(t + 20);
    p->rows = r.U(t + 24);
    const uint32_t n = p->rows * p->columns * count;
    for (uint32_t i = 0; i < n; i++) p->value.push_back(r.U(8 + size_t(vo) + 4 * i));
  } else if (p->cls == kObject && IsSampler(p->type)) {
    uint32_t n = r.U(8 + size_t(vo));
    size_t q = 8 + size_t(vo) + 4;
    for (uint32_t i = 0; i < n && r.ok; i++, q += 16) {
      State s;
      s.op = r.U(q);
      s.index = r.U(q + 4);
      s.value = r.U(8 + size_t(r.U(q + 12)));
      p->sampler.push_back(s);
    }
  } else if (p->cls == kStruct) {
    FatalF("D3DXCreateEffect: struct parameter %s not supported", p->name.c_str());
  }
}

}  // namespace

bool Load(EffectFile* ep, const std::vector<uint8_t>& d, std::string* err) {
  EffectFile& e = *ep;
  Reader r{d};
  if (r.U(0) != kFx20) {
    *err = "not a compiled fx_2_0 effect";
    return false;
  }
  size_t p = 8 + size_t(r.U(4));
  const uint32_t nparams = r.U(p), ntech = r.U(p + 4);
  p += 16;
  for (uint32_t i = 0; i < nparams && r.ok; i++) {
    Param prm;
    ReadParam(r, r.U(p), r.U(p + 4), &prm);
    p += 16 + 8 * size_t(r.U(p + 12));  // annotations skipped
    e.params.push_back(std::move(prm));
  }
  for (uint32_t t = 0; t < ntech && r.ok; t++) {
    Technique tech;
    tech.name = r.Str(r.U(p));
    const uint32_t npass = r.U(p + 8);
    p += 12 + 8 * size_t(r.U(p + 4));
    for (uint32_t i = 0; i < npass && r.ok; i++) {
      Pass pass;
      pass.name = r.Str(r.U(p));
      const uint32_t nstates = r.U(p + 8);
      p += 12 + 8 * size_t(r.U(p + 4));
      for (uint32_t s = 0; s < nstates && r.ok; s++, p += 16) {
        State st;
        st.op = r.U(p);
        st.index = r.U(p + 4);
        st.value = r.U(8 + size_t(r.U(p + 12)));
        pass.states.push_back(st);
      }
      tech.passes.push_back(std::move(pass));
    }
    e.techniques.push_back(std::move(tech));
  }
  const uint32_t nstrings = r.U(p), nres = r.U(p + 4);
  p += 8;
  for (uint32_t i = 0; i < nstrings && r.ok; i++) p += 8 + ((size_t(r.U(p + 4)) + 3) & ~size_t(3));
  for (uint32_t i = 0; i < nres && r.ok; i++) {
    const uint32_t tech = r.U(p), idx = r.U(p + 4), state = r.U(p + 12), size = r.U(p + 20);
    if (p + 24 + size > d.size()) {
      r.ok = false;
      break;
    }
    const uint8_t* data = d.data() + p + 24;
    if (tech == 0xFFFFFFFF) {
      // A parameter's state (a sampler's Texture): the name it refers to.
      if (idx < e.params.size() && state < e.params[idx].sampler.size()) {
        std::string s(reinterpret_cast<const char*>(data), size);
        e.params[idx].sampler[state].ref = s.substr(0, s.find('\0'));
      }
    } else if (tech < e.techniques.size() && idx < e.techniques[tech].passes.size() &&
               state < e.techniques[tech].passes[idx].states.size()) {
      const State& st = e.techniques[tech].passes[idx].states[state];
      std::vector<uint32_t> code(size / 4);
      std::memcpy(code.data(), data, code.size() * 4);
      e.shader_code[st.value] = std::move(code);
    }
    p += 24 + ((size_t(size) + 3) & ~size_t(3));
  }
  if (!r.ok) {
    *err = "truncated or malformed effect";
    return false;
  }
  return true;
}

int ParamByName(const std::vector<Param>& params, const std::string& name) {
  for (size_t i = 0; i < params.size(); i++)
    if (params[i].name == name) return int(i);
  return -1;
}

// The constants of a CTAB payload (after its FourCC) of `size` bytes.
std::vector<CtabConstant> ParseCtab(const uint8_t* b, size_t size) {
  std::vector<CtabConstant> out;
  {
    {
      auto u32 = [&](size_t o) {
        uint32_t v = 0;
        if (o + 4 <= size) std::memcpy(&v, b + o, 4);
        return v;
      };
      auto u16 = [&](size_t o) {
        uint16_t v = 0;
        if (o + 2 <= size) std::memcpy(&v, b + o, 2);
        return uint32_t(v);
      };
      const uint32_t n = u32(12), info = u32(16);
      for (uint32_t k = 0; k < n; k++) {
        const size_t ci = info + 20 * size_t(k);
        CtabConstant cc;
        const uint32_t name = u32(ci);
        for (size_t o = name; o < size && b[o]; o++) cc.name += char(b[o]);
        cc.set = u16(ci + 4);
        cc.reg = u16(ci + 6);
        cc.count = u16(ci + 8);
        cc.cls = u16(u32(ci + 12));
        out.push_back(cc);
      }
    }
  }
  return out;
}

// A comment block `fourcc` among `tokens` (from `start`, up to an END):
// its payload after the FourCC, or {nullptr, 0}.
namespace {
std::pair<const uint32_t*, size_t> FindComment(const uint32_t* tokens, size_t count,
                                               size_t start, uint32_t fourcc) {
  for (size_t i = start; i < count;) {
    const uint32_t tok = tokens[i];
    if ((tok & 0xFFFF) != 0xFFFE) {
      if (tok == 0x0000FFFF) break;
      i++;
      continue;
    }
    const uint32_t len = tok >> 16;
    if (len >= 1 && i + 1 + len <= count && tokens[i + 1] == fourcc)
      return {&tokens[i + 2], len - 1};
    i += 1 + len;
  }
  return {nullptr, 0};
}

constexpr uint32_t kCtab = 0x42415443, kPres = 0x53455250, kClit = 0x54494C43,
                   kFxlc = 0x434C5846;
}  // namespace

// The constants in a shader's CTAB comment.
std::vector<CtabConstant> ReadCtab(const std::vector<uint32_t>& code) {
  auto [p, n] = FindComment(code.data(), code.size(), 1, kCtab);
  if (!p) return {};
  return ParseCtab(reinterpret_cast<const uint8_t*>(p), n * 4);
}

// Registers for parameter `p` as constant `k` lays it out: `count`
// registers of four components (float bits or ints).
std::vector<uint32_t> Registers(const Param& p, const CtabConstant& k, bool as_float) {
  std::vector<uint32_t> regs(4 * size_t(k.count), 0);
  auto comp = [&](size_t i) -> uint32_t {
    if (i >= p.value.size()) return 0;
    uint32_t v = p.value[i];
    const bool is_float = p.type == kFloat;
    if (as_float && !is_float) {
      float f = float(int32_t(v));
      std::memcpy(&v, &f, 4);
    } else if (!as_float && is_float) {
      float f;
      std::memcpy(&f, &v, 4);
      v = uint32_t(int32_t(f));
    }
    return v;
  };
  const uint32_t per = p.rows * p.columns;
  const uint32_t count = std::max(p.elements, 1u);
  const bool matrix = p.cls == kMatrixRows || p.cls == kMatrixColumns;
  uint32_t reg = 0;
  for (uint32_t e = 0; e < count && reg < k.count; e++) {
    const size_t base = size_t(e) * per;
    if (!matrix) {
      for (uint32_t j = 0; j < std::min(per, 4u); j++) regs[4 * reg + j] = comp(base + j);
      reg++;
    } else if (k.cls == kMatrixColumns) {
      // Register j holds column j.
      for (uint32_t j = 0; j < p.columns && reg < k.count; j++, reg++)
        for (uint32_t i = 0; i < std::min(p.rows, 4u); i++)
          regs[4 * reg + i] = comp(base + size_t(i) * p.columns + j);
    } else {
      for (uint32_t i = 0; i < p.rows && reg < k.count; i++, reg++)
        for (uint32_t j = 0; j < std::min(p.columns, 4u); j++)
          regs[4 * reg + j] = comp(base + size_t(i) * p.columns + j);
    }
  }
  return regs;
}

// The shader's preshader, or false if it has none.
bool ReadPreshader(const std::vector<uint32_t>& shader, Preshader* ps) {
  auto [pres, n] = FindComment(shader.data(), shader.size(), 1, kPres);
  if (!pres || n < 2) return false;
  // pres[0] is the preshader's version; its comments follow.
  if (auto [ct, cn] = FindComment(pres, n, 1, kCtab); ct)
    ps->inputs = ParseCtab(reinterpret_cast<const uint8_t*>(ct), cn * 4);
  if (auto [cl, ln] = FindComment(pres, n, 1, kClit); cl && ln >= 1) {
    const uint32_t count = std::min<uint32_t>(cl[0], uint32_t((ln - 1) / 2));
    for (uint32_t i = 0; i < count; i++) {
      double v;
      std::memcpy(&v, &cl[1 + 2 * i], 8);
      ps->literals.push_back(v);
    }
  }
  auto [fx, fn] = FindComment(pres, n, 1, kFxlc);
  if (!fx || fn < 1) FatalF("ID3DXEffect: a preshader without instructions");
  size_t p = 1;
  auto word = [&]() -> uint32_t {
    if (p >= fn) FatalF("ID3DXEffect: truncated preshader");
    return fx[p++];
  };
  auto operand = [&]() {
    PreOperand o;
    const uint32_t flag = word();
    if (flag == 1) {
      o.relative = true;
      o.index_table = word();
      o.index_offset = word();
    } else if (flag != 0) {
      FatalF("ID3DXEffect: preshader operand flag %u", flag);
    }
    o.table = word();
    o.offset = word();
    return o;
  };
  for (uint32_t i = 0; i < fx[0]; i++) {
    PreInstr in;
    const uint32_t ctl = word();
    in.comps = ctl & 0xFFFF;
    in.op = (ctl >> 20) & 0x7FF;
    in.scalar = (ctl >> 31) != 0;
    const uint32_t nin = word();
    if (in.comps < 1 || in.comps > 4 || nin > 8)
      FatalF("ID3DXEffect: preshader instruction %08X with %u inputs", ctl, nin);
    for (uint32_t k = 0; k < nin; k++) in.in.push_back(operand());
    in.out = operand();
    ps->code.push_back(std::move(in));
  }
  return true;
}

void RunPreshader(const std::vector<Param>& params, const Preshader& ps,
                  std::map<uint32_t, std::array<float, 4>>* out) {
  // Inputs: the parameters, as the preshader's table places them.
  std::vector<double> in, temp(4 * 64, 0.0), oc(4 * 256, 0.0);
  std::set<uint32_t> written;
  for (const CtabConstant& k : ps.inputs) {
    int pi = ParamByName(params, k.name);
    if (pi < 0) continue;
    if (k.set != 2)
      FatalF("ID3DXEffect: preshader input %s in register set %u not supported",
             k.name.c_str(), k.set);
    std::vector<uint32_t> regs = Registers(params[pi], k, true);
    if (in.size() < 4 * size_t(k.reg + k.count)) in.resize(4 * size_t(k.reg + k.count), 0.0);
    for (size_t i = 0; i < regs.size(); i++) {
      float f;
      std::memcpy(&f, &regs[i], 4);
      in[4 * size_t(k.reg) + i] = f;
    }
  }
  auto table = [&](uint32_t t) -> std::vector<double>* {
    switch (t) {
      case 2: return &in;
      case 4: return &oc;
      case 7: return &temp;
      default: return nullptr;
    }
  };
  auto read = [&](const PreOperand& o, uint32_t comp) -> double {
    uint32_t off = o.offset + comp;
    if (o.relative) {
      PreOperand idx{o.index_table, o.index_offset};
      double v = 0;
      if (idx.table == 1) {
        v = idx.offset < ps.literals.size() ? ps.literals[idx.offset] : 0;
      } else if (std::vector<double>* t = table(idx.table)) {
        v = idx.offset < t->size() ? (*t)[idx.offset] : 0;
      } else {
        FatalF("ID3DXEffect: preshader index table %u not supported", idx.table);
      }
      off += 4 * uint32_t(int32_t(std::lround(v)));
    }
    if (o.table == 1) return off < ps.literals.size() ? ps.literals[off] : 0.0;
    std::vector<double>* t = table(o.table);
    if (!t) FatalF("ID3DXEffect: preshader table %u not supported", o.table);
    return off < t->size() ? (*t)[off] : 0.0;
  };
  auto write = [&](const PreOperand& o, uint32_t comp, double v) {
    std::vector<double>* t = table(o.table);
    if (!t || o.table == 2 || o.relative)
      FatalF("ID3DXEffect: preshader output to table %u not supported", o.table);
    const uint32_t off = o.offset + comp;
    if (off >= t->size()) t->resize(off + 4, 0.0);
    (*t)[off] = v;
    if (o.table == 4) written.insert(off / 4);
  };
  for (const PreInstr& in_ : ps.code) {
    auto arg = [&](size_t k, uint32_t comp) {
      return read(in_.in[k], k == 0 && in_.scalar ? 0 : comp);
    };
    if (in_.op == 0x500) {  // dot: all components at once, into component 0
      double sum = 0;
      for (uint32_t j = 0; j < in_.comps; j++) sum += arg(0, j) * arg(1, j);
      write(in_.out, 0, sum);
      continue;
    }
    for (uint32_t j = 0; j < in_.comps; j++) {
      double a = in_.in.size() > 0 ? arg(0, j) : 0, b = in_.in.size() > 1 ? arg(1, j) : 0;
      double r;
      switch (in_.op) {
        case 0x100: r = a; break;                                         // mov
        case 0x101: r = -a; break;                                        // neg
        case 0x103: r = 1.0 / a; break;                                   // rcp
        case 0x104: r = a - std::floor(a); break;                         // frc
        case 0x105: r = std::exp2(a); break;                              // exp
        case 0x106: r = a == 0 ? 0.0 : std::log2(std::fabs(a)); break;    // log
        case 0x107: r = a == 0 ? INFINITY : 1.0 / std::sqrt(std::fabs(a)); break;  // rsq
        case 0x108: r = std::sin(a); break;
        case 0x109: r = std::cos(a); break;
        case 0x10A: r = std::asin(a); break;
        case 0x10B: r = std::acos(a); break;
        case 0x10C: r = std::atan(a); break;
        case 0x200: r = std::fmin(a, b); break;
        case 0x201: r = std::fmax(a, b); break;
        case 0x202: r = a < b ? 1.0 : 0.0; break;                         // lt
        case 0x203: r = a >= b ? 1.0 : 0.0; break;                        // ge
        case 0x204: r = a + b; break;                                     // add
        case 0x205: r = a * b; break;                                     // mul
        case 0x206: r = std::atan2(a, b); break;
        case 0x208: r = a / b; break;                                     // div
        case 0x300: r = a >= 0 ? b : arg(2, j); break;                    // cmp
        default:
          FatalF("ID3DXEffect: preshader opcode %03X not supported", in_.op);
      }
      write(in_.out, j, r);
    }
  }
  for (uint32_t reg : written)
    for (int k = 0; k < 4; k++) (*out)[reg][k] = float(oc[4 * size_t(reg) + size_t(k)]);
}

}  // namespace recomp::d3dx
