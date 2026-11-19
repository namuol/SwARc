// Compiled D3DX effects (fx_2_0) as data: the file's parameters,
// techniques, passes, states and shaders, the shaders' constant tables,
// and preshaders, read and evaluated without a device or guest memory
// (d3dx9_effect.cpp drives the device with them).
//
// The file: little-endian DWORDs, offsets from byte 8 (the base). Byte 0
// is the tag 0xFEFF0901, byte 4 the size of the definitions block (types,
// values, names), after which come the counts (parameters, techniques, -,
// objects), the parameters (type, value, flags, annotations), the
// techniques with their passes and each pass's states (operation, index,
// type, value), then the strings and the resources: a shader's bytecode
// for a technique's pass state, or the name of the parameter a sampler's
// Texture state refers to. A state's operation indexes D3DX's state table
// (render states, sampler states, shaders, ...; StateOf).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace recomp::d3dx {

constexpr uint32_t kFx20 = 0xFEFF0901;

// D3DX's effect state table, by operation: the class of state and its
// D3D enum value. Only the ranges games have been seen to use are filled;
// any other operation is None (the caller names it, fatal).
enum class StateClass : uint8_t { None, Render, Sampler, Texture, VertexShader, PixelShader };
struct StateDef {
  StateClass cls;
  uint32_t value;
};
StateDef StateOf(uint32_t op);

enum ParamClass : uint32_t { kScalar, kVector, kMatrixRows, kMatrixColumns, kObject, kStruct };
enum ParamType : uint32_t {
  kVoid, kBool, kInt, kFloat, kString, kTexture, kTexture1D, kTexture2D, kTexture3D,
  kTextureCube, kSampler, kSampler1D, kSampler2D, kSampler3D, kSamplerCube,
  kPixelShader, kVertexShader,
};
inline bool IsTexture(uint32_t t) { return t >= kTexture && t <= kTextureCube; }
inline bool IsSampler(uint32_t t) { return t >= kSampler && t <= kSamplerCube; }

struct State {
  uint32_t op = 0, index = 0;
  uint32_t value = 0;      // the DWORD value, or an object id (shaders, textures)
  std::string ref;         // a sampler's Texture state: the texture parameter's name
};

struct Param {
  std::string name, semantic;
  uint32_t cls = 0, type = 0, rows = 1, columns = 1, elements = 0;
  std::vector<uint32_t> value;  // numeric: components (float bits or ints), row-major
  uint32_t texture = 0;         // texture parameters: the guest texture (referenced)
  std::vector<State> sampler;   // sampler parameters: their states
};

struct Pass {
  std::string name;
  std::vector<State> states;
};
struct Technique {
  std::string name;
  std::vector<Pass> passes;
};

struct EffectFile {
  std::vector<Param> params;
  std::vector<Technique> techniques;
  std::map<uint32_t, std::vector<uint32_t>> shader_code;  // object id -> bytecode
};

// Reads a compiled effect into *e; false (and *err) if it is not one or
// is malformed. Struct parameters are fatal (not supported).
bool Load(EffectFile* e, const std::vector<uint8_t>& d, std::string* err);

// The parameter named `name`, -1 if none.
int ParamByName(const std::vector<Param>& params, const std::string& name);

// ---- shader constant tables -------------------------------------------------------------

struct CtabConstant {
  std::string name;
  uint32_t set = 0, reg = 0, count = 0, cls = 0;  // set: 0 bool, 1 int4, 2 float4, 3 sampler
};

// The constants of a CTAB payload (after its FourCC) of `size` bytes.
std::vector<CtabConstant> ParseCtab(const uint8_t* b, size_t size);
// The constants in a shader's CTAB comment (none if it has none).
std::vector<CtabConstant> ReadCtab(const std::vector<uint32_t>& code);

// Registers for parameter `p` as constant `k` lays it out: `k.count`
// registers of four components (float bits, or ints when !as_float),
// matrices by columns or rows as `k.cls` says.
std::vector<uint32_t> Registers(const Param& p, const CtabConstant& k, bool as_float);

// ---- preshaders -------------------------------------------------------------------------
// Expressions of parameters alone that the effect compiler moved out of a
// shader into a PRES comment, for D3DX to evaluate on the CPU before the
// draw: version, then comments CTAB (its inputs, laid out as the shader's
// constants are), CLIT (literals: count, doubles), FXLC (instructions),
// PRSI. An instruction: control (bits 0-15 component count, 20-30 opcode,
// 31 scalar: the first input's component 0 for every component), input
// count, then the inputs and the output, each (0, table, offset) or
// relative (1, index table, index offset, table, offset); offsets are
// register * 4 + component. Tables: 1 literals, 2 inputs, 4 outputs (the
// shader's float constants, by register), 7 temporaries. Arithmetic in
// doubles, as D3DX's.

struct PreOperand {
  uint32_t table = 0, offset = 0;
  bool relative = false;
  uint32_t index_table = 0, index_offset = 0;
};
struct PreInstr {
  uint32_t op = 0, comps = 1;
  bool scalar = false;
  std::vector<PreOperand> in;
  PreOperand out;
};
struct Preshader {
  std::vector<CtabConstant> inputs;
  std::vector<double> literals;
  std::vector<PreInstr> code;
};

// The shader's preshader, or false if it has none. Malformed ones are
// fatal.
bool ReadPreshader(const std::vector<uint32_t>& shader, Preshader* ps);

// Runs a preshader on `params`; the float constant registers it wrote, by
// register, in *out. Opcodes and tables it does not know are fatal.
void RunPreshader(const std::vector<Param>& params, const Preshader& ps,
                  std::map<uint32_t, std::array<float, 4>>* out);

}  // namespace recomp::d3dx
