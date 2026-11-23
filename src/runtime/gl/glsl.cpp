// GLSL 1.10-3.30 programs to the device's program GLSL (see glsl.h).
//
// The source is tokenised (comments dropped, preprocessor lines kept as
// they are); top-level declarations of uniforms, vertex inputs, varyings
// and fragment outputs are taken out and declared again in the form the
// device needs, ahead of the rest. Function bodies are kept token for
// token, but for renamed identifiers. Array sizes in those declarations
// may be integer literals or object-like #defines of one.

#include "runtime/gl/glsl.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace recomp::gl::glsl {
namespace {

// ---- tokens ------------------------------------------------------------------

enum class Kind : uint8_t { Ident, Number, Punct, Space, Preproc };
struct Token {
  Kind kind;
  std::string text;
};

std::vector<Token> Tokenize(const std::string& src) {
  std::vector<Token> out;
  size_t i = 0, n = src.size();
  bool line_start = true;
  while (i < n) {
    char ch = src[i];
    if (ch == '\r') {
      i++;
      continue;
    }
    if (ch == '/' && i + 1 < n && src[i + 1] == '/') {  // line comment
      while (i < n && src[i] != '\n') i++;
      continue;
    }
    if (ch == '/' && i + 1 < n && src[i + 1] == '*') {  // block comment: keep its newlines
      size_t e = src.find("*/", i + 2);
      e = e == std::string::npos ? n : e + 2;
      std::string sp(" ");
      for (size_t k = i; k < e; k++)
        if (src[k] == '\n') sp += '\n';
      out.push_back({Kind::Space, sp});
      i = e;
      continue;
    }
    if (ch == '\n' || ch == ' ' || ch == '\t' || ch == '\f' || ch == '\v') {
      if (ch == '\n') line_start = true;
      out.push_back({Kind::Space, std::string(1, ch)});
      i++;
      continue;
    }
    if (ch == '#' && line_start) {  // a whole line, with continuations
      std::string line;
      while (i < n) {
        if (src[i] == '\\' && i + 1 < n && (src[i + 1] == '\n' || src[i + 1] == '\r')) {
          i += src[i + 1] == '\r' && i + 2 < n && src[i + 2] == '\n' ? 3 : 2;
          line += ' ';
          continue;
        }
        if (src[i] == '\n') break;
        if (src[i] == '/' && i + 1 < n && src[i + 1] == '/') {
          while (i < n && src[i] != '\n') i++;
          break;
        }
        if (src[i] != '\r') line += src[i];
        i++;
      }
      out.push_back({Kind::Preproc, line});
      continue;
    }
    line_start = false;
    if (std::isalpha(uint8_t(ch)) || ch == '_') {
      size_t s = i;
      while (i < n && (std::isalnum(uint8_t(src[i])) || src[i] == '_')) i++;
      out.push_back({Kind::Ident, src.substr(s, i - s)});
      continue;
    }
    if (std::isdigit(uint8_t(ch)) || (ch == '.' && i + 1 < n && std::isdigit(uint8_t(src[i + 1])))) {
      size_t s = i;
      while (i < n && (std::isalnum(uint8_t(src[i])) || src[i] == '.' || src[i] == '_' ||
                       ((src[i] == '+' || src[i] == '-') && (src[i - 1] == 'e' || src[i - 1] == 'E') &&
                        !(src[s] == '0' && s + 1 < n && (src[s + 1] == 'x' || src[s + 1] == 'X')))))
        i++;
      out.push_back({Kind::Number, src.substr(s, i - s)});
      continue;
    }
    // Two-character operators matter only as single tokens here.
    static const char* const kTwo[] = {"++", "--", "<=", ">=", "==", "!=", "&&", "||", "^^",
                                       "+=", "-=", "*=", "/=", "%=", "<<", ">>", "&=", "|=", "^="};
    std::string p(1, ch);
    for (const char* t : kTwo)
      if (i + 1 < n && ch == t[0] && src[i + 1] == t[1]) p = t;
    out.push_back({Kind::Punct, p});
    i += p.size();
  }
  return out;
}

// ---- types ---------------------------------------------------------------------

struct TypeInfo {
  Base base;
  int comps, cols;
};

bool ParseType(const std::string& t, TypeInfo* out) {
  static const std::map<std::string, TypeInfo> kTypes = {
      {"float", {Base::Float, 1, 1}},  {"vec2", {Base::Float, 2, 1}},
      {"vec3", {Base::Float, 3, 1}},   {"vec4", {Base::Float, 4, 1}},
      {"int", {Base::Int, 1, 1}},      {"ivec2", {Base::Int, 2, 1}},
      {"ivec3", {Base::Int, 3, 1}},    {"ivec4", {Base::Int, 4, 1}},
      {"uint", {Base::UInt, 1, 1}},    {"uvec2", {Base::UInt, 2, 1}},
      {"uvec3", {Base::UInt, 3, 1}},   {"uvec4", {Base::UInt, 4, 1}},
      {"bool", {Base::Bool, 1, 1}},    {"bvec2", {Base::Bool, 2, 1}},
      {"bvec3", {Base::Bool, 3, 1}},   {"bvec4", {Base::Bool, 4, 1}},
      {"mat2", {Base::Float, 2, 2}},   {"mat3", {Base::Float, 3, 3}},
      {"mat4", {Base::Float, 4, 4}},   {"mat2x2", {Base::Float, 2, 2}},
      {"mat3x3", {Base::Float, 3, 3}}, {"mat4x4", {Base::Float, 4, 4}},
      {"sampler2D", {Base::Sampler2D, 1, 1}}, {"samplerCube", {Base::SamplerCube, 1, 1}},
  };
  auto it = kTypes.find(t);
  if (it == kTypes.end()) return false;
  *out = it->second;
  return true;
}

// std140: the alignment and size of one value (an array's elements are
// rounded up to 16 bytes, as are a matrix's columns).
uint32_t Align140(const TypeInfo& t, bool array) {
  if (t.cols > 1 || array) return 16;
  return t.comps == 1 ? 4 : t.comps == 2 ? 8 : 16;
}
uint32_t Size140(const TypeInfo& t, bool array) {
  if (t.cols > 1) return 16 * uint32_t(t.cols);
  if (array) return 16;
  return 4 * uint32_t(t.comps);
}

// ---- declarations ------------------------------------------------------------------

struct Decl {
  std::vector<std::string> qualifiers;  // in, out, uniform, flat... (layout separate)
  int location = -1;                    // layout(location = N)
  std::string type;
  struct Var {
    std::string name;
    int array = 0;  // 0: not an array
  };
  std::vector<Var> vars;
  bool Has(const char* q) const {
    return std::find(qualifiers.begin(), qualifiers.end(), q) != qualifiers.end();
  }
};

const std::set<std::string> kQualifiers = {
    "uniform", "in", "out", "attribute", "varying", "flat", "smooth", "noperspective",
    "centroid", "invariant", "highp", "mediump", "lowp", "const"};

// Identifiers later GLSL reserved, or that the translation's prelude uses:
// renamed wherever they appear in the program.
const std::set<std::string> kReserved = {
    "sample",   "buffer",    "shared",   "coherent", "volatile",  "restrict",
    "readonly", "writeonly", "resource", "patch",    "subroutine", "precise",
    "input",    "output",    "filter",   "common",   "partition",  "active",
    "atomic_uint"};

// The built-ins the translation provides, as the prelude names them.
const std::map<std::string, std::string> kBuiltinsVs = {
    {"gl_ModelViewProjectionMatrix", "r_mvp"}, {"gl_ModelViewMatrix", "r_mv"},
    {"gl_ProjectionMatrix", "r_proj"},          {"gl_TextureMatrix", "r_texmat"},
    {"gl_NormalMatrix", "mat3(r_normalmat)"},   {"gl_Vertex", "r_in_vertex"},
    {"gl_Normal", "r_in_normal"},               {"gl_Color", "r_in_color"},
    {"gl_SecondaryColor", "r_in_color2"},       {"gl_MultiTexCoord0", "r_in_tc0"},
    {"gl_MultiTexCoord1", "r_in_tc1"},          {"gl_MultiTexCoord2", "r_in_tc2"},
    {"gl_MultiTexCoord3", "r_in_tc3"},          {"gl_FrontColor", "r_FrontColor"},
    {"gl_BackColor", "r_BackColor"},            {"gl_FrontSecondaryColor", "r_FrontColor2"},
    {"gl_BackSecondaryColor", "r_BackColor2"},  {"gl_TexCoord", "r_TexCoord"},
    {"gl_FogFragCoord", "r_FogFragCoord"},      {"gl_ClipVertex", "r_ClipVertex"},
    {"gl_VertexID", "gl_VertexIndex"},          {"gl_InstanceID", "gl_InstanceIndex"},
};
const std::map<std::string, std::string> kBuiltinsFs = {
    {"gl_ModelViewProjectionMatrix", "r_mvp"}, {"gl_ModelViewMatrix", "r_mv"},
    {"gl_ProjectionMatrix", "r_proj"},          {"gl_TextureMatrix", "r_texmat"},
    {"gl_NormalMatrix", "mat3(r_normalmat)"},   {"gl_Color", "r_FrontColor"},
    {"gl_SecondaryColor", "r_FrontColor2"},     {"gl_TexCoord", "r_TexCoord"},
    {"gl_FragColor", "r_FragColor"},            {"gl_FragData", "r_FragData"},
    {"gl_FragCoord", "r_FragCoord"},            {"gl_FogFragCoord", "r_FogFragCoord"},
};
// Functions of GLSL before 1.30 that later GLSL calls otherwise.
const std::map<std::string, std::string> kFunctions = {
    {"texture2D", "texture"},         {"texture2DLod", "textureLod"},
    {"texture2DProj", "textureProj"}, {"texture2DProjLod", "textureProjLod"},
    {"textureCube", "texture"},       {"textureCubeLod", "textureLod"},
    {"texture2DGrad", "textureGrad"},
};

struct Stage {
  std::vector<Token> kept;   // the source without the declarations taken out
  std::vector<Decl> decls;   // the declarations taken out
  std::map<std::string, int> defines;  // object-like #defines of an integer
  std::set<std::string> idents;        // every identifier the stage uses
  std::string error;
};

int ArraySize(const std::vector<Token>& t, size_t* i, const Stage& s, std::string* err) {
  // t[*i] is '['
  size_t j = *i + 1;
  while (j < t.size() && t[j].kind == Kind::Space) j++;
  int v = -1;
  if (j < t.size() && t[j].kind == Kind::Number) {
    v = int(std::strtol(t[j].text.c_str(), nullptr, 0));
  } else if (j < t.size() && t[j].kind == Kind::Ident && s.defines.count(t[j].text)) {
    v = s.defines.at(t[j].text);
  } else {
    *err = "an array size that is not an integer or a #define of one";
    return 0;
  }
  j++;
  while (j < t.size() && t[j].kind == Kind::Space) j++;
  if (j >= t.size() || t[j].text != "]") {
    *err = "an array size expression";
    return 0;
  }
  *i = j + 1;
  return v;
}

// Parses a declaration statement (tokens without spaces, the ';' left
// out). False if it is not one of the forms taken out.
bool ParseDecl(const std::vector<Token>& t, const Stage& s, Decl* d, std::string* err) {
  size_t i = 0;
  bool storage = false;
  for (; i < t.size(); i++) {
    if (t[i].text == "layout") {
      // layout(location = N, ...)
      size_t j = i + 1;
      if (j >= t.size() || t[j].text != "(") return false;
      for (j++; j < t.size() && t[j].text != ")"; j++)
        if (t[j].text == "location" && j + 2 < t.size() && t[j + 1].text == "=") {
          const Token& v = t[j + 2];
          if (v.kind == Kind::Number) d->location = int(std::strtol(v.text.c_str(), nullptr, 0));
          else if (s.defines.count(v.text)) d->location = s.defines.at(v.text);
        }
      i = j;
      continue;
    }
    if (!kQualifiers.count(t[i].text)) break;
    d->qualifiers.push_back(t[i].text);
    if (t[i].text == "uniform" || t[i].text == "in" || t[i].text == "out" ||
        t[i].text == "attribute" || t[i].text == "varying")
      storage = true;
  }
  if (!storage || d->Has("const") || i >= t.size()) return false;
  d->type = t[i++].text;
  if (i < t.size() && t[i].text == "{") {
    *err = "interface blocks are not supported";
    return false;
  }
  while (i < t.size()) {
    if (t[i].kind != Kind::Ident) {
      *err = "a declaration of " + d->type + " not understood";
      return false;
    }
    Decl::Var v{t[i++].text, 0};
    if (i < t.size() && t[i].text == "[") {
      v.array = ArraySize(t, &i, s, err);
      if (!err->empty()) return false;
    }
    if (i < t.size() && t[i].text == "=") {
      *err = "an initialised uniform (" + v.name + ")";
      return false;
    }
    d->vars.push_back(v);
    if (i < t.size() && t[i].text == ",") i++;
  }
  return !d->vars.empty();
}

Stage Scan(const std::string& src) {
  Stage s;
  std::vector<Token> t = Tokenize(src);
  for (const Token& k : t)
    if (k.kind == Kind::Ident) s.idents.insert(k.text);
  int depth = 0;
  std::vector<Token> stmt;  // the current top-level statement, spaces included
  auto flush = [&] {
    for (Token& k : stmt) s.kept.push_back(std::move(k));
    stmt.clear();
  };
  for (size_t i = 0; i < t.size(); i++) {
    Token& k = t[i];
    if (k.kind == Kind::Preproc) {
      // #version and #extension go: the prelude sets its own.
      std::string d = k.text.substr(1);
      d.erase(0, d.find_first_not_of(" \t"));
      if (d.rfind("version", 0) == 0 || d.rfind("extension", 0) == 0) continue;
      if (d.rfind("define", 0) == 0) {
        char name[128];
        long v;
        if (std::sscanf(d.c_str(), "define %127s %li", name, &v) == 2) s.defines[name] = int(v);
      }
      // A preprocessor line ends the statement before it only in the
      // output's order: keep it where it is.
      stmt.push_back(k);
      continue;
    }
    if (depth > 0) {
      if (k.text == "{") depth++;
      if (k.text == "}") depth--;
      stmt.push_back(k);
      if (depth == 0) flush();
      continue;
    }
    if (k.text == "{") {
      depth = 1;
      stmt.push_back(k);
      continue;
    }
    if (k.text == "precision") {  // precision statements go
      while (i < t.size() && t[i].text != ";") i++;
      continue;
    }
    stmt.push_back(k);
    if (k.text != ";") continue;
    std::vector<Token> words;
    for (const Token& w : stmt)
      if (w.kind != Kind::Space && w.kind != Kind::Preproc) words.push_back(w);
    words.pop_back();  // the ';'
    Decl d;
    std::string err;
    if (ParseDecl(words, s, &d, &err)) {
      // Keep the statement's preprocessor lines and newlines.
      std::vector<Token> rest;
      for (Token& w : stmt)
        if (w.kind == Kind::Preproc || (w.kind == Kind::Space && w.text.find('\n') != std::string::npos))
          rest.push_back(std::move(w));
      stmt = std::move(rest);
      s.decls.push_back(std::move(d));
    } else if (!err.empty() && s.error.empty()) {
      s.error = err;
    }
    flush();
  }
  flush();
  return s;
}

// The kept source with identifiers renamed, built-ins mapped and `main`
// called r_main.
std::string Emit(const Stage& s, const std::map<std::string, std::string>& builtins,
                 const std::set<std::string>& renamed) {
  std::string out;
  const std::vector<Token>& t = s.kept;
  for (size_t i = 0; i < t.size(); i++) {
    const Token& k = t[i];
    if (k.kind == Kind::Preproc) {
      // Identifiers in #define bodies are renamed too.
      std::vector<Token> line = Tokenize(k.text.substr(1));
      out += "#";
      for (const Token& w : line) {
        if (w.kind == Kind::Ident && renamed.count(w.text)) out += "r_u_" + w.text;
        else if (w.kind == Kind::Ident && builtins.count(w.text)) out += builtins.at(w.text);
        else if (w.kind == Kind::Ident && kFunctions.count(w.text)) out += kFunctions.at(w.text);
        else out += w.text;
      }
      out += '\n';
      continue;
    }
    if (k.kind != Kind::Ident) {
      out += k.text;
      continue;
    }
    // The next token after spaces.
    size_t j = i + 1;
    while (j < t.size() && t[j].kind == Kind::Space) j++;
    const bool call = j < t.size() && t[j].text == "(";
    if (k.text == "main" && call) out += "r_main";
    else if (renamed.count(k.text) && !(k.text == "texture" && call)) out += "r_u_" + k.text;
    else if (builtins.count(k.text)) out += builtins.at(k.text);
    else if (kFunctions.count(k.text) && call) out += kFunctions.at(k.text);
    else out += k.text;
  }
  return out;
}

const char* GlslBase(Base b, int comps) {
  static const char* const kF[] = {"float", "vec2", "vec3", "vec4"};
  static const char* const kI[] = {"int", "ivec2", "ivec3", "ivec4"};
  static const char* const kU[] = {"uint", "uvec2", "uvec3", "uvec4"};
  static const char* const kB[] = {"bool", "bvec2", "bvec3", "bvec4"};
  switch (b) {
    case Base::Int: return kI[comps - 1];
    case Base::UInt: return kU[comps - 1];
    case Base::Bool: return kB[comps - 1];
    default: return kF[comps - 1];
  }
}

std::string ArraySuffix(int n) { return n ? "[" + std::to_string(n) + "]" : ""; }

constexpr const char* kFetchMarker = "// R_FETCH\n";

}  // namespace

Program Link(const std::string& vertex, const std::string& fragment,
             const std::map<std::string, int>& bound) {
  Program p;
  Stage vs = Scan(vertex), fs = Scan(fragment);
  auto fail = [&](const std::string& why) {
    p.ok = false;
    p.log += why + "\n";
    return p;
  };
  if (!vs.error.empty()) return fail("vertex shader: " + vs.error);
  if (!fs.error.empty()) return fail("fragment shader: " + fs.error);

  // Identifiers to rename: reserved words, and `texture` when the program
  // declares something by that name.
  std::set<std::string> renamed;
  for (const Stage* st : {&vs, &fs}) {
    for (const std::string& id : st->idents)
      if (kReserved.count(id)) renamed.insert(id);
    for (const Decl& d : st->decls)
      for (const Decl::Var& v : d.vars)
        if (v.name == "texture" || kReserved.count(v.name)) renamed.insert(v.name);
  }
  auto internal = [&](const std::string& n) { return renamed.count(n) ? "r_u_" + n : n; };

  // ---- uniforms: both stages', by name
  uint32_t at = Builtins::kEnd;
  std::string block, samplers_vs, samplers_fs;
  for (const Stage* st : {&vs, &fs}) {
    for (const Decl& d : st->decls) {
      if (!d.Has("uniform")) continue;
      TypeInfo ti;
      if (!ParseType(d.type, &ti)) return fail("uniform of type " + d.type + " not supported");
      for (const Decl::Var& v : d.vars) {
        auto it = std::find_if(p.uniforms.begin(), p.uniforms.end(),
                               [&](const Uniform& u) { return u.name == v.name; });
        if (it != p.uniforms.end()) {
          if (it->base != ti.base || it->comps != ti.comps || it->cols != ti.cols ||
              it->array != v.array)
            return fail("uniform " + v.name + " declared differently in the two shaders");
          continue;
        }
        Uniform u;
        u.name = v.name;
        u.base = ti.base;
        u.comps = ti.comps;
        u.cols = ti.cols;
        u.array = v.array;
        if (ti.base == Base::Sampler2D || ti.base == Base::SamplerCube) {
          if (v.array) return fail("sampler arrays are not supported (" + v.name + ")");
          u.sampler = p.samplers++;
        } else {
          const bool arr = v.array > 0;
          const uint32_t a = Align140(ti, arr);
          at = (at + a - 1) / a * a;
          u.offset = at;
          u.stride = Size140(ti, arr);
          at += u.stride * uint32_t(arr ? v.array : 1);
          char b[64];
          std::snprintf(b, sizeof b, "layout(offset = %u) ", u.offset);
          block += std::string("  ") + b + d.type + " " + internal(v.name) + ArraySuffix(v.array) + ";\n";
        }
        p.uniforms.push_back(u);
      }
    }
  }
  p.uniform_bytes = (at + 15) / 16 * 16;
  std::string samplers;
  for (const Uniform& u : p.uniforms)
    if (u.sampler >= 0)
      samplers += "layout(R_TEXTURE(" + std::to_string(u.sampler) + ")) uniform " +
                  (u.base == Base::SamplerCube ? "samplerCube " : "sampler2D ") +
                  internal(u.name) + ";\n";

  char b[1024];
  std::string uniforms = "layout(R_UNIFORMS, std140) uniform RGl {\n";
  std::snprintf(b, sizeof b,
                "  layout(offset = %u) mat4 r_mvp;\n  layout(offset = %u) mat4 r_mv;\n"
                "  layout(offset = %u) mat4 r_proj;\n  layout(offset = %u) mat4 r_texmat[4];\n"
                "  layout(offset = %u) mat4 r_normalmat;\n",
                Builtins::kMvp, Builtins::kMv, Builtins::kProj, Builtins::kTexMat,
                Builtins::kNormalMat);
  uniforms += b;
  std::snprintf(b, sizeof b,
                "  layout(offset = %u) ivec4 r_aoff[%d];\n  layout(offset = %u) vec4 r_cur[%d];\n"
                "  layout(offset = %u) int r_vbase;\n  layout(offset = %u) float r_ndc_y;\n"
                "  layout(offset = %u) float r_fb_h;\n  layout(offset = %u) float r_frag_flip;\n"
                "  layout(offset = %u) int r_alpha_func;\n  layout(offset = %u) float r_alpha_ref;\n",
                Builtins::kAttribOffset, kInputs / 4, Builtins::kCurrent, kInputs,
                Builtins::kVertexBase, Builtins::kNdcY, Builtins::kFbHeight, Builtins::kFragFlip,
                Builtins::kAlphaFunc, Builtins::kAlphaRef);
  uniforms += b + block + "};\n";

  // ---- varyings: the vertex stage's outputs, matched by name in the
  // fragment stage. Locations 0-1 are the colours, 2-5 gl_TexCoord.
  struct Var {
    std::string type, name;
    int array, slots;
    bool flat;
    int location;
  };
  std::vector<Var> varyings;
  int next_loc = 6;
  for (const Decl& d : vs.decls) {
    if (!(d.Has("out") || d.Has("varying"))) continue;
    TypeInfo ti;
    if (!ParseType(d.type, &ti) || ti.base == Base::Sampler2D || ti.base == Base::SamplerCube)
      return fail("varying of type " + d.type + " not supported");
    for (const Decl::Var& v : d.vars) {
      int slots = ti.cols * std::max(v.array, 1);
      varyings.push_back({d.type, v.name, v.array, slots, d.Has("flat"), next_loc});
      next_loc += slots;
    }
  }
  std::string vs_out = "layout(location = 0) out vec4 r_FrontColor;\n"
                       "layout(location = 1) out vec4 r_FrontColor2;\n"
                       "layout(location = 2) out vec4 r_TexCoord[4];\n"
                       "vec4 r_BackColor, r_BackColor2, r_ClipVertex;\nfloat r_FogFragCoord;\n";
  for (const Var& v : varyings)
    vs_out += "layout(location = " + std::to_string(v.location) + ") " + (v.flat ? "flat " : "") +
              "out " + v.type + " " + internal(v.name) + ArraySuffix(v.array) + ";\n";
  std::string fs_in = "layout(location = 0) in vec4 r_FrontColor;\n"
                      "layout(location = 1) in vec4 r_FrontColor2;\n"
                      "layout(location = 2) in vec4 r_TexCoord[4];\nfloat r_FogFragCoord;\n";
  for (const Decl& d : fs.decls) {
    if (!(d.Has("in") || d.Has("varying"))) continue;
    for (const Decl::Var& v : d.vars) {
      auto it = std::find_if(varyings.begin(), varyings.end(),
                             [&](const Var& x) { return x.name == v.name; });
      if (it == varyings.end())
        return fail("fragment input " + v.name + " is not a vertex shader output");
      if (it->type != d.type || it->array != v.array)
        return fail("varying " + v.name + " declared differently in the two shaders");
      fs_in += "layout(location = " + std::to_string(it->location) + ") " +
               (it->flat ? "flat " : "") + "in " + d.type + " " + internal(v.name) +
               ArraySuffix(v.array) + ";\n";
    }
  }

  // ---- fragment outputs: gl_FragColor / gl_FragData, or the shader's own
  std::string fs_outs, color_out = "r_FragColor";
  bool own_outputs = false;
  for (const Decl& d : fs.decls) {
    if (!d.Has("out")) continue;
    for (const Decl::Var& v : d.vars) {
      int loc = d.location >= 0 ? d.location : 0;
      fs_outs += "layout(location = " + std::to_string(loc) + ") out " + d.type + " " +
                 internal(v.name) + ArraySuffix(v.array) + ";\n";
      if (loc == 0) color_out = internal(v.name) + (v.array ? "[0]" : "");
      own_outputs = true;
    }
  }
  if (!own_outputs) {
    if (fs.idents.count("gl_FragData")) {
      fs_outs = "layout(location = 0) out vec4 r_FragData[4];\n";
      color_out = "r_FragData[0]";
    } else {
      fs_outs = "layout(location = 0) out vec4 r_FragColor;\n";
    }
  }

  // ---- vertex inputs
  std::string vs_in =
      "vec4 r_in_vertex, r_in_normal, r_in_color, r_in_color2, r_in_tc0, r_in_tc1, r_in_tc2, "
      "r_in_tc3;\n";
  const char* const kConventional[] = {"r_in_vertex", "r_in_normal", "r_in_color", "r_in_color2",
                                       "r_in_tc0",    "r_in_tc1",    "r_in_tc2",   "r_in_tc3"};
  for (int i = 0; i < kInConventional; i++) p.attribs.push_back({kConventional[i], i, Base::Float, 4});
  std::set<int> used_locations;
  for (const Decl& d : vs.decls)
    if ((d.Has("in") || d.Has("attribute")) && d.location >= 0) used_locations.insert(d.location);
  for (const auto& [n, loc] : bound) used_locations.insert(loc);
  int next_attrib = 1;
  for (const Decl& d : vs.decls) {
    if (!(d.Has("in") || d.Has("attribute"))) continue;
    TypeInfo ti;
    if (!ParseType(d.type, &ti) || ti.cols > 1 || ti.base == Base::Bool ||
        ti.base == Base::Sampler2D || ti.base == Base::SamplerCube)
      return fail("vertex input of type " + d.type + " not supported");
    for (const Decl::Var& v : d.vars) {
      if (v.array) return fail("vertex input arrays are not supported (" + v.name + ")");
      int loc = d.location;
      if (loc < 0 && bound.count(v.name)) loc = bound.at(v.name);
      if (loc < 0) {
        while (used_locations.count(next_attrib)) next_attrib++;
        loc = next_attrib++;
      }
      if (loc < 0 || loc >= kMaxAttribs) return fail("vertex input location out of range");
      used_locations.insert(loc);
      p.attrib_locations[v.name] = loc;
      p.attribs.push_back({internal(v.name), kInConventional + loc, ti.base, ti.comps});
      vs_in += d.type + " " + internal(v.name) + ";\n";
    }
  }

  // ---- the stages (built-ins and old texture functions mapped by Emit, in
  // #define bodies too)
  std::string vs_src = Emit(vs, kBuiltinsVs, renamed), fs_src = Emit(fs, kBuiltinsFs, renamed);
  p.vs = uniforms + samplers + vs_in + vs_out + kFetchMarker + vs_src +
         "\nvoid main() {\n  r_fetch();\n  r_main();\n"
         "  gl_Position.y *= r_ndc_y;\n"
         "  gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;\n}\n";
  // The alpha test on the colour output: GL's func - NEVER (0 NEVER .. 7
  // ALWAYS).
  std::string alpha =
      "  float r_a = " + color_out + ".a;\n"
      "  bool r_pass = r_alpha_func == 7 || (r_alpha_func == 1 && r_a < r_alpha_ref) ||\n"
      "      (r_alpha_func == 2 && r_a == r_alpha_ref) || (r_alpha_func == 3 && r_a <= r_alpha_ref) ||\n"
      "      (r_alpha_func == 4 && r_a > r_alpha_ref) || (r_alpha_func == 5 && r_a != r_alpha_ref) ||\n"
      "      (r_alpha_func == 6 && r_a >= r_alpha_ref);\n"
      "  if (!r_pass) discard;\n";
  p.fs = uniforms + samplers + fs_in + fs_outs + "vec4 r_FragCoord;\n" + fs_src +
         "\nvoid main() {\n"
         "  r_FragCoord = vec4(gl_FragCoord.x,\n"
         "      r_frag_flip != 0.0 ? r_fb_h - gl_FragCoord.y : gl_FragCoord.y,\n"
         "      gl_FragCoord.z, gl_FragCoord.w);\n"
         "  r_main();\n" + alpha + "}\n";
  p.ok = true;
  return p;
}

// The fetch for one layout: each input from its buffer (word loads, any
// alignment), or the current value in the block.
std::string VertexStage(const Program& p, const InputLayout (&layout)[kInputs]) {
  std::string f =
      "uint r_u32(uint b, uint a) {\n"
      "  uint s = (a & 3u) * 8u, lo = r_ld(b, a >> 2);\n"
      "  return s == 0u ? lo : (lo >> s) | (r_ld(b, (a >> 2) + 1u) << (32u - s));\n}\n"
      "void r_fetch() {\n  uint r_v = uint(gl_VertexIndex - r_vbase);\n";
  bool any_buffer = false;
  for (const Attrib& a : p.attribs) {
    const InputLayout& l = layout[a.input];
    const std::string cur = "r_cur[" + std::to_string(a.input) + "]";
    std::string value;
    if (l.buffer < 0) {
      value = cur;
    } else {
      any_buffer = true;
      const std::string addr = "(r_v * " + std::to_string(l.stride) + "u + uint(r_aoff[" +
                               std::to_string(a.input / 4) + "][" + std::to_string(a.input % 4) +
                               "]))";
      const std::string buf = std::to_string(l.buffer) + "u";
      std::string comps[4];
      for (int k = 0; k < 4; k++) {
        if (k >= l.size) {
          comps[k] = k == 3 ? "1.0" : "0.0";
          continue;
        }
        std::string e;
        auto word = [&](int bytes) {
          return "r_u32(" + buf + ", " + addr + " + " + std::to_string(k * bytes) + "u)";
        };
        switch (l.type) {
          case 0x1400:  // BYTE
            e = "float(int(" + word(1) + " << 24) >> 24)";
            if (l.normalized) e = "max(" + e + " / 127.0, -1.0)";
            break;
          case 0x1401:  // UNSIGNED_BYTE
            e = "float(" + word(1) + " & 255u)";
            if (l.normalized) e += " / 255.0";
            break;
          case 0x1402:  // SHORT
            e = "float(int(" + word(2) + " << 16) >> 16)";
            if (l.normalized) e = "max(" + e + " / 32767.0, -1.0)";
            break;
          case 0x1403:  // UNSIGNED_SHORT
            e = "float(" + word(2) + " & 65535u)";
            if (l.normalized) e += " / 65535.0";
            break;
          case 0x1404:  // INT
            e = "float(int(" + word(4) + "))";
            break;
          case 0x1405:  // UNSIGNED_INT
            e = "float(" + word(4) + ")";
            break;
          default:  // FLOAT
            e = "uintBitsToFloat(" + word(4) + ")";
            break;
        }
        comps[k] = e;
      }
      value = "vec4(" + comps[0] + ", " + comps[1] + ", " + comps[2] + ", " + comps[3] + ")";
    }
    std::string conv;
    if (a.base == Base::Int) conv = std::string(GlslBase(Base::Int, a.comps)) + "(";
    else if (a.base == Base::UInt) conv = std::string(GlslBase(Base::UInt, a.comps)) + "(";
    else if (a.comps < 4) conv = std::string(GlslBase(Base::Float, a.comps)) + "(";
    static const char* const kSwz[] = {".x", ".xy", ".xyz", ""};
    f += "  " + a.name + " = " + (conv.empty() ? value : conv + "(" + value + ")" + kSwz[a.comps - 1] + ")") +
         ";\n";
  }
  f += "}\n";
  // Without buffers the prelude has no r_ld; the fetch never calls it.
  if (!any_buffer)
    f = "uint r_ld(uint b, uint i) { return 0u; }\n" + f;
  std::string vs = p.vs;
  size_t m = vs.find(kFetchMarker);
  vs.replace(m, std::string(kFetchMarker).size(), f);
  return vs;
}

}  // namespace recomp::gl::glsl
