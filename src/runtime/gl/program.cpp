// gl::Context's programmable pipeline (context.h): buffer objects, generic
// vertex attributes, GLSL shader and program objects (gl/glsl.h translates
// them into device programs), framebuffer objects, and the draws made with
// a program.
//
// Targets. Device programs draw only into images, so the default
// framebuffer becomes one (fb_image_) at the first draw that needs it, and
// SwapBuffers shows it (ShowFramebuffer). Fixed-function draws follow into
// it. In the default framebuffer, rows run top-down as on the back buffer
// (window y flipped, as context.cpp does for fixed-function draws); in a
// framebuffer object's texture they run bottom-up, as GL keeps texture
// rows, so a program's clip space is flipped vertically there (the
// translation's r_ndc_y) and fixed-function draws flip their projection.

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <numeric>

#include "runtime/gl/context.h"
#include "runtime/gl/context_impl.h"
#include "runtime/log.h"

namespace recomp::gl {

namespace {

int TypeBytes(Enum t) {
  switch (t) {
    case BYTE: case UNSIGNED_BYTE: return 1;
    case SHORT: case UNSIGNED_SHORT: return 2;
    case DOUBLE: return 8;
    default: return 4;
  }
}

gfx::BlendFactor DeviceBlend(Enum f) {
  using B = gfx::BlendFactor;
  switch (f) {
    case ZERO: return B::Zero;
    case ONE: return B::One;
    case SRC_COLOR: return B::SrcColor;
    case ONE_MINUS_SRC_COLOR: return B::OneMinusSrcColor;
    case DST_COLOR: return B::DstColor;
    case ONE_MINUS_DST_COLOR: return B::OneMinusDstColor;
    case SRC_ALPHA: return B::SrcAlpha;
    case ONE_MINUS_SRC_ALPHA: return B::OneMinusSrcAlpha;
    case DST_ALPHA: return B::DstAlpha;
    case ONE_MINUS_DST_ALPHA: return B::OneMinusDstAlpha;
    case CONSTANT_COLOR: return B::ConstantColor;
    case ONE_MINUS_CONSTANT_COLOR: return B::OneMinusConstantColor;
    case SRC_ALPHA_SATURATE: return B::SrcAlphaSaturate;
    default: return B::One;
  }
}

gfx::BlendOp DeviceBlendOp(Enum e) {
  using O = gfx::BlendOp;
  switch (e) {
    case FUNC_SUBTRACT: return O::Subtract;
    case FUNC_REVERSE_SUBTRACT: return O::ReverseSubtract;
    case MIN: return O::Min;
    case MAX: return O::Max;
    default: return O::Add;
  }
}

gfx::CompareOp DeviceCompare(Enum f) {
  return f >= NEVER && f <= ALWAYS ? gfx::CompareOp(f - NEVER) : gfx::CompareOp::Always;
}

gfx::StencilOp DeviceStencilOp(Enum op) {
  using S = gfx::StencilOp;
  switch (op) {
    case ZERO: return S::Zero;
    case 0x1E01: return S::Replace;  // GL_REPLACE
    case INCR: return S::IncrementClamp;
    case DECR: return S::DecrementClamp;
    case INVERT: return S::Invert;
    case 0x8507: return S::IncrementWrap;  // GL_INCR_WRAP
    case 0x8508: return S::DecrementWrap;  // GL_DECR_WRAP
    default: return S::Keep;
  }
}

// a * b of column-major 4x4 matrices.
std::array<float, 16> MatMul(const float* a, const float* b) {
  std::array<float, 16> r{};
  for (int c = 0; c < 4; c++)
    for (int row = 0; row < 4; row++) {
      float s = 0;
      for (int k = 0; k < 4; k++) s += a[k * 4 + row] * b[c * 4 + k];
      r[size_t(c * 4 + row)] = s;
    }
  return r;
}

// A uniform location: the uniform's index and the array element.
constexpr int kLocShift = 12;
int32_t Location(size_t index, int element) { return int32_t(index << kLocShift) | element; }

}  // namespace

int Context::Array::Stride() const {
  return stride ? stride : size * TypeBytes(type);
}

const uint8_t* Context::ArrayPtr(const Array& a) const {
  if (!a.buffer) return a.ptr;
  auto it = shared_->buffers.find(a.buffer);
  if (it == shared_->buffers.end() || a.offset > it->second->data.size()) return nullptr;
  return it->second->data.data() + a.offset;
}

Context::Array Context::Resolved(const Array& a) const {
  Array r = a;
  if (a.buffer) r.ptr = ArrayPtr(a);
  return r;
}

// ---- buffer objects ------------------------------------------------------------

void Context::GenBuffers(int n, uint32_t* names) {
  auto& bufs = shared_->buffers;
  for (int i = 0; i < n; i++) {
    while (bufs.count(shared_->next_buffer)) shared_->next_buffer++;
    names[i] = shared_->next_buffer;
    bufs[shared_->next_buffer++] = new Buffer;
  }
}

void Context::DeleteBuffers(int n, const uint32_t* names) {
  for (int i = 0; i < n; i++) {
    auto it = shared_->buffers.find(names[i]);
    if (!names[i] || it == shared_->buffers.end()) continue;
    if (it->second->gpu) dev_->DestroyBuffer(it->second->gpu);
    delete it->second;
    shared_->buffers.erase(it);
    if (array_buffer_ == names[i]) array_buffer_ = 0;
    if (element_buffer_ == names[i]) element_buffer_ = 0;
  }
}

void Context::BindBuffer(Enum target, uint32_t name) {
  if (name && !shared_->buffers.count(name)) shared_->buffers[name] = new Buffer;
  if (target == ARRAY_BUFFER) array_buffer_ = name;
  else if (target == ELEMENT_ARRAY_BUFFER) element_buffer_ = name;
  else Unsupported("glBindBuffer target", target);
}

uint32_t Context::BoundBuffer(Enum target) const {
  return target == ARRAY_BUFFER ? array_buffer_ : target == ELEMENT_ARRAY_BUFFER ? element_buffer_ : 0;
}

bool Context::IsBuffer(uint32_t name) const { return name && shared_->buffers.count(name); }

void Context::BufferData(Enum target, int64_t size, const void* data, Enum usage) {
  uint32_t name = BoundBuffer(target);
  if (!name || size < 0) return Error(name ? INVALID_VALUE : INVALID_OPERATION, "glBufferData");
  Buffer* b = shared_->buffers[name];
  b->data.assign(size_t(size), 0);
  if (data) std::memcpy(b->data.data(), data, size_t(size));
  b->usage = usage;
  b->dirty_lo = 0;
  b->dirty_hi = size_t(size);
}

void Context::BufferSubData(Enum target, int64_t offset, int64_t size, const void* data) {
  uint32_t name = BoundBuffer(target);
  if (!name) return Error(INVALID_OPERATION, "glBufferSubData");
  Buffer* b = shared_->buffers[name];
  if (offset < 0 || size < 0 || size_t(offset + size) > b->data.size())
    return Error(INVALID_VALUE, "glBufferSubData");
  if (!size) return;
  std::memcpy(b->data.data() + offset, data, size_t(size));
  if (b->dirty_hi <= b->dirty_lo) {
    b->dirty_lo = size_t(offset);
    b->dirty_hi = size_t(offset + size);
  } else {
    b->dirty_lo = std::min(b->dirty_lo, size_t(offset));
    b->dirty_hi = std::max(b->dirty_hi, size_t(offset + size));
  }
}

void Context::GetBufferSubData(Enum target, int64_t offset, int64_t size, void* out) {
  uint32_t name = BoundBuffer(target);
  if (!name) return Error(INVALID_OPERATION, "glGetBufferSubData");
  Buffer* b = shared_->buffers[name];
  if (offset < 0 || size < 0 || size_t(offset + size) > b->data.size())
    return Error(INVALID_VALUE, "glGetBufferSubData");
  std::memcpy(out, b->data.data() + offset, size_t(size));
}

bool Context::GetBufferParameteriv(Enum target, Enum pname, int32_t* out) {
  uint32_t name = BoundBuffer(target);
  if (!name) {
    Error(INVALID_OPERATION, "glGetBufferParameteriv");
    return false;
  }
  Buffer* b = shared_->buffers[name];
  if (pname == BUFFER_SIZE) *out = int32_t(b->data.size());
  else if (pname == BUFFER_USAGE) *out = int32_t(b->usage);
  else {
    Unsupported("glGetBufferParameteriv", pname);
    return false;
  }
  return true;
}

// ---- generic vertex attributes ------------------------------------------------------

void Context::VertexAttribPointer(uint32_t index, int size, Enum type, bool normalized, int stride,
                                  const void* p) {
  if (index >= uint32_t(glsl::kMaxAttribs) || size < 1 || size > 4)
    return Error(INVALID_VALUE, "glVertexAttribPointer");
  Array& a = attribs_[index];
  a.size = size;
  a.type = type;
  a.normalized = normalized;
  a.stride = stride;
  a.buffer = array_buffer_;
  a.offset = reinterpret_cast<uintptr_t>(p);
  a.ptr = array_buffer_ ? nullptr : static_cast<const uint8_t*>(p);
}

void Context::SetAttribArray(uint32_t index, bool on) {
  if (index >= uint32_t(glsl::kMaxAttribs)) return Error(INVALID_VALUE, "glEnableVertexAttribArray");
  attribs_[index].enabled = on;
}

void Context::VertexAttrib(uint32_t index, float x, float y, float z, float w) {
  if (index >= uint32_t(glsl::kMaxAttribs)) return Error(INVALID_VALUE, "glVertexAttrib");
  float* c = attrib_current_[index];
  c[0] = x;
  c[1] = y;
  c[2] = z;
  c[3] = w;
}

// ---- shader and program objects -------------------------------------------------------

uint32_t Context::CreateShaderObject(Enum type) {
  if (type != VERTEX_SHADER && type != FRAGMENT_SHADER) {
    Unsupported("glCreateShaderObject type", type);
    return 0;
  }
  uint32_t h = shared_->next_object++;
  Shader* s = new Shader;
  s->type = type;
  shared_->shaders[h] = s;
  return h;
}

uint32_t Context::CreateProgramObject() {
  uint32_t h = shared_->next_object++;
  shared_->programs[h] = new ProgramObj;
  return h;
}

Context::ProgramObj* Context::Program(uint32_t h) const {
  auto it = shared_->programs.find(h);
  return it == shared_->programs.end() ? nullptr : it->second;
}

void Context::ShaderSource(uint32_t shader, const std::string& source) {
  auto it = shared_->shaders.find(shader);
  if (it == shared_->shaders.end()) return Error(INVALID_VALUE, "glShaderSource");
  it->second->source = source;
  it->second->compiled = false;
}

// Compilation is the translation at link time (glsl::Link): a shader
// compiles as long as it has source.
void Context::CompileShader(uint32_t shader) {
  auto it = shared_->shaders.find(shader);
  if (it == shared_->shaders.end()) return Error(INVALID_VALUE, "glCompileShader");
  it->second->compiled = !it->second->source.empty();
}

void Context::AttachObject(uint32_t program, uint32_t shader) {
  ProgramObj* p = Program(program);
  auto it = shared_->shaders.find(shader);
  if (!p || it == shared_->shaders.end()) return Error(INVALID_VALUE, "glAttachObject");
  if (std::find(p->attached.begin(), p->attached.end(), shader) != p->attached.end()) return;
  p->attached.push_back(shader);
  it->second->attached++;
}

void Context::DetachObject(uint32_t program, uint32_t shader) {
  ProgramObj* p = Program(program);
  if (!p) return Error(INVALID_VALUE, "glDetachObject");
  auto at = std::find(p->attached.begin(), p->attached.end(), shader);
  if (at == p->attached.end()) return Error(INVALID_OPERATION, "glDetachObject");
  p->attached.erase(at);
  auto it = shared_->shaders.find(shader);
  if (it != shared_->shaders.end() && --it->second->attached == 0 && it->second->deleted) {
    delete it->second;
    shared_->shaders.erase(it);
  }
}

void Context::LinkProgram(uint32_t program) {
  ProgramObj* p = Program(program);
  if (!p) return Error(INVALID_VALUE, "glLinkProgram");
  std::string vs, fs;
  for (uint32_t h : p->attached) {
    const Shader* s = shared_->shaders.at(h);
    (s->type == VERTEX_SHADER ? vs : fs) += s->source + "\n";
  }
  for (auto& [k, id] : p->device) dev_->DestroyProgram(id);
  p->device.clear();
  p->linked = false;
  if (vs.empty() || fs.empty()) {
    // Fixed-function stages for one of them are not provided.
    p->log = "a program needs both a vertex and a fragment shader here\n";
    WarnF("gl: glLinkProgram: %s", p->log.c_str());
    return;
  }
  p->glsl = glsl::Link(vs, fs, p->bound_attribs);
  p->log = p->glsl.log;
  if (!p->glsl.ok) {
    WarnF("gl: glLinkProgram: %s", p->log.c_str());
    return;
  }
  p->linked = true;
  p->uniforms.assign(p->glsl.uniform_bytes, 0);
  p->sampler_units.assign(size_t(p->glsl.samplers), 0);
}

void Context::UseProgramObject(uint32_t program) {
  if (program && (!Program(program) || !Program(program)->linked))
    return Error(INVALID_OPERATION, "glUseProgramObject");
  Changed();
  program_ = program;
}

void Context::DeleteObject(uint32_t obj) {
  if (!obj) return;
  if (ProgramObj* p = Program(obj)) {
    if (program_ == obj) {  // stays in use until another is
      p->deleted = true;
      return;
    }
    for (uint32_t h : std::vector<uint32_t>(p->attached)) DetachObject(obj, h);
    for (auto& [k, id] : p->device) dev_->DestroyProgram(id);
    delete p;
    shared_->programs.erase(obj);
    return;
  }
  auto it = shared_->shaders.find(obj);
  if (it == shared_->shaders.end()) return Error(INVALID_VALUE, "glDeleteObject");
  if (it->second->attached) {
    it->second->deleted = true;
    return;
  }
  delete it->second;
  shared_->shaders.erase(it);
}

bool Context::GetObjectParameteriv(uint32_t obj, Enum pname, int32_t* out) {
  if (ProgramObj* p = Program(obj)) {
    switch (pname) {
      case OBJECT_TYPE: *out = int32_t(PROGRAM_OBJECT); return true;
      case OBJECT_DELETE_STATUS: *out = p->deleted; return true;
      case OBJECT_LINK_STATUS: *out = p->linked; return true;
      case OBJECT_VALIDATE_STATUS: *out = p->linked; return true;
      case OBJECT_INFO_LOG_LENGTH: *out = p->log.empty() ? 0 : int32_t(p->log.size() + 1); return true;
      case OBJECT_ATTACHED_OBJECTS: *out = int32_t(p->attached.size()); return true;
      case OBJECT_ACTIVE_UNIFORMS: *out = int32_t(p->glsl.uniforms.size()); return true;
      case OBJECT_ACTIVE_UNIFORM_MAX_LENGTH: {
        size_t n = 0;
        for (const glsl::Uniform& u : p->glsl.uniforms) n = std::max(n, u.name.size() + 4);
        *out = int32_t(n);
        return true;
      }
      case OBJECT_ACTIVE_ATTRIBUTES: *out = int32_t(p->glsl.attrib_locations.size()); return true;
      default: break;
    }
  } else if (auto it = shared_->shaders.find(obj); it != shared_->shaders.end()) {
    const Shader* s = it->second;
    switch (pname) {
      case OBJECT_TYPE: *out = int32_t(SHADER_OBJECT); return true;
      case OBJECT_SUBTYPE: *out = int32_t(s->type); return true;
      case OBJECT_DELETE_STATUS: *out = s->deleted; return true;
      case OBJECT_COMPILE_STATUS: *out = s->compiled; return true;
      case OBJECT_INFO_LOG_LENGTH: *out = 0; return true;
      case OBJECT_SHADER_SOURCE_LENGTH: *out = int32_t(s->source.size() + 1); return true;
      default: break;
    }
  } else {
    Error(INVALID_VALUE, "glGetObjectParameteriv");
    return false;
  }
  Unsupported("glGetObjectParameteriv", pname);
  return false;
}

std::string Context::InfoLog(uint32_t obj) const {
  if (ProgramObj* p = Program(obj)) return p->log;
  return {};
}

std::string Context::ShaderSourceOf(uint32_t shader) const {
  auto it = shared_->shaders.find(shader);
  return it == shared_->shaders.end() ? std::string() : it->second->source;
}

std::vector<uint32_t> Context::AttachedObjects(uint32_t program) const {
  ProgramObj* p = Program(program);
  return p ? p->attached : std::vector<uint32_t>();
}

int32_t Context::GetUniformLocation(uint32_t program, const std::string& name) {
  ProgramObj* p = Program(program);
  if (!p || !p->linked) {
    Error(INVALID_OPERATION, "glGetUniformLocation");
    return -1;
  }
  std::string base = name;
  int element = 0;
  if (size_t b = name.find('['); b != std::string::npos) {
    base = name.substr(0, b);
    element = std::atoi(name.c_str() + b + 1);
  }
  for (size_t i = 0; i < p->glsl.uniforms.size(); i++) {
    const glsl::Uniform& u = p->glsl.uniforms[i];
    if (u.name != base) continue;
    if (element < 0 || element >= std::max(u.array, 1)) return -1;
    return Location(i, element);
  }
  return -1;
}

int32_t Context::GetAttribLocation(uint32_t program, const std::string& name) {
  ProgramObj* p = Program(program);
  if (!p || !p->linked) return -1;
  auto it = p->glsl.attrib_locations.find(name);
  return it == p->glsl.attrib_locations.end() ? -1 : it->second;
}

void Context::BindAttribLocation(uint32_t program, uint32_t index, const std::string& name) {
  ProgramObj* p = Program(program);
  if (!p || index >= uint32_t(glsl::kMaxAttribs)) return Error(INVALID_VALUE, "glBindAttribLocation");
  p->bound_attribs[name] = int(index);
}

namespace {
// Stores `count` elements of `comps` values from location on (the
// uniform's element), converted to the uniform's type: floats, ints
// (bools as 0 or 1).
template <typename T>
bool StoreUniform(std::vector<uint8_t>& block, std::vector<int>& units, const glsl::Uniform& u,
                  int element, int comps, int count, const T* v) {
  if (u.cols > 1 || comps != u.comps) return false;
  if (u.sampler >= 0) {
    if (!std::is_integral_v<T> || comps != 1) return false;
    units[size_t(u.sampler)] = int(v[0]);
    return true;
  }
  const int n = std::min(count, std::max(u.array, 1) - element);
  for (int e = 0; e < n; e++)
    for (int k = 0; k < comps; k++) {
      const uint32_t at = u.offset + uint32_t(element + e) * u.stride + 4 * uint32_t(k);
      const T x = v[e * comps + k];
      uint32_t bits;
      if (u.base == glsl::Base::Float) {
        float f = float(x);
        std::memcpy(&bits, &f, 4);
      } else if (u.base == glsl::Base::Bool) {
        bits = x != T(0) ? 1 : 0;
      } else {
        bits = uint32_t(int32_t(x));
      }
      std::memcpy(block.data() + at, &bits, 4);
    }
  return true;
}
}  // namespace

void Context::Uniformf(int32_t location, int comps, int count, const float* v) {
  ProgramObj* p = Program(program_);
  if (location == -1) return;
  if (!p || location < 0 || size_t(location >> kLocShift) >= p->glsl.uniforms.size())
    return Error(INVALID_OPERATION, "glUniform*f");
  const glsl::Uniform& u = p->glsl.uniforms[size_t(location >> kLocShift)];
  if (!StoreUniform(p->uniforms, p->sampler_units, u, location & ((1 << kLocShift) - 1), comps,
                    count, v))
    Error(INVALID_OPERATION, "glUniform*f: type");
}

void Context::Uniformi(int32_t location, int comps, int count, const int32_t* v) {
  ProgramObj* p = Program(program_);
  if (location == -1) return;
  if (!p || location < 0 || size_t(location >> kLocShift) >= p->glsl.uniforms.size())
    return Error(INVALID_OPERATION, "glUniform*i");
  const glsl::Uniform& u = p->glsl.uniforms[size_t(location >> kLocShift)];
  if (!StoreUniform(p->uniforms, p->sampler_units, u, location & ((1 << kLocShift) - 1), comps,
                    count, v))
    Error(INVALID_OPERATION, "glUniform*i: type");
}

void Context::UniformMatrix(int32_t location, int n, int count, bool transpose, const float* v) {
  ProgramObj* p = Program(program_);
  if (location == -1) return;
  if (!p || location < 0 || size_t(location >> kLocShift) >= p->glsl.uniforms.size())
    return Error(INVALID_OPERATION, "glUniformMatrix");
  const glsl::Uniform& u = p->glsl.uniforms[size_t(location >> kLocShift)];
  if (u.cols != n || u.comps != n) return Error(INVALID_OPERATION, "glUniformMatrix: type");
  const int element = location & ((1 << kLocShift) - 1);
  const int cnt = std::min(count, std::max(u.array, 1) - element);
  for (int e = 0; e < cnt; e++)
    for (int c = 0; c < n; c++)
      for (int r = 0; r < n; r++) {
        const float x = transpose ? v[e * n * n + r * n + c] : v[e * n * n + c * n + r];
        std::memcpy(p->uniforms.data() + u.offset + uint32_t(element + e) * u.stride +
                        16 * uint32_t(c) + 4 * uint32_t(r),
                    &x, 4);
      }
}

bool Context::GetUniformfv(uint32_t program, int32_t location, float* out) {
  ProgramObj* p = Program(program);
  if (!p || location < 0 || size_t(location >> kLocShift) >= p->glsl.uniforms.size()) return false;
  const glsl::Uniform& u = p->glsl.uniforms[size_t(location >> kLocShift)];
  const int element = location & ((1 << kLocShift) - 1);
  if (u.sampler >= 0) {
    out[0] = float(p->sampler_units[size_t(u.sampler)]);
    return true;
  }
  for (int c = 0; c < u.cols; c++)
    for (int k = 0; k < u.comps; k++) {
      uint32_t bits;
      std::memcpy(&bits, p->uniforms.data() + u.offset + uint32_t(element) * u.stride +
                             16 * uint32_t(c) + 4 * uint32_t(k),
                  4);
      float f;
      if (u.base == glsl::Base::Float) std::memcpy(&f, &bits, 4);
      else f = float(int32_t(bits));
      out[c * u.comps + k] = f;
    }
  return true;
}

// ---- framebuffer objects -----------------------------------------------------------------

void Context::GenFramebuffers(int n, uint32_t* names) {
  for (int i = 0; i < n; i++) {
    while (framebuffers_.count(next_framebuffer_)) next_framebuffer_++;
    names[i] = next_framebuffer_;
    framebuffers_[next_framebuffer_++] = new Framebuffer;
  }
}

void Context::DeleteFramebuffers(int n, const uint32_t* names) {
  for (int i = 0; i < n; i++) {
    auto it = framebuffers_.find(names[i]);
    if (!names[i] || it == framebuffers_.end()) continue;
    delete it->second;
    framebuffers_.erase(it);
    if (framebuffer_ == names[i]) {
      framebuffer_ = 0;
      Changed();
      matrices_dirty_ = true;
    }
  }
}

// FRAMEBUFFER binds both the draw and the read framebuffer;
// EXT_framebuffer_blit's READ_ and DRAW_FRAMEBUFFER one each.
void Context::BindFramebuffer(Enum target, uint32_t name) {
  if (target != FRAMEBUFFER && target != READ_FRAMEBUFFER && target != DRAW_FRAMEBUFFER)
    return Unsupported("glBindFramebuffer target", target);
  if (name && !framebuffers_.count(name)) framebuffers_[name] = new Framebuffer;
  if (name) UseImageFramebuffer();
  if (target != DRAW_FRAMEBUFFER) read_framebuffer_ = name;
  if (target == READ_FRAMEBUFFER) return;
  framebuffer_ = name;
  Changed();
  matrices_dirty_ = true;
}

bool Context::IsFramebuffer(uint32_t name) const { return name && framebuffers_.count(name); }

void Context::FramebufferTexture2D(Enum target, Enum attachment, Enum textarget, uint32_t texture,
                                   int level) {
  const uint32_t fbo = target == READ_FRAMEBUFFER ? read_framebuffer_ : framebuffer_;
  if (!fbo) return Error(INVALID_OPERATION, "glFramebufferTexture2D");
  if (textarget != TEXTURE_2D && texture) return Unsupported("glFramebufferTexture2D target", textarget);
  Framebuffer* f = framebuffers_[fbo];
  if (attachment == COLOR_ATTACHMENT0) {
    f->color = texture;
    f->level = level;
  } else {
    return Unsupported("glFramebufferTexture2D attachment", attachment);
  }
  Changed();
  matrices_dirty_ = true;
}

void Context::FramebufferRenderbuffer(Enum target, Enum attachment, Enum rbtarget, uint32_t rb) {
  const uint32_t fbo = target == READ_FRAMEBUFFER ? read_framebuffer_ : framebuffer_;
  if (!fbo || (rb && rbtarget != RENDERBUFFER))
    return Error(INVALID_OPERATION, "glFramebufferRenderbuffer");
  Framebuffer* f = framebuffers_[fbo];
  if (attachment == DEPTH_ATTACHMENT || attachment == STENCIL_ATTACHMENT ||
      attachment == DEPTH_STENCIL_ATTACHMENT)
    f->depth = rb;  // one depth-stencil buffer serves both
  else
    return Unsupported("glFramebufferRenderbuffer attachment", attachment);
  Changed();
}

Enum Context::CheckFramebufferStatus(Enum target) {
  if (target != FRAMEBUFFER && target != READ_FRAMEBUFFER && target != DRAW_FRAMEBUFFER) return 0;
  const uint32_t fbo = target == READ_FRAMEBUFFER ? read_framebuffer_ : framebuffer_;
  if (!fbo) return FRAMEBUFFER_COMPLETE;
  const Framebuffer* f = framebuffers_[fbo];
  auto t = textures_.find(f->color);
  if (!f->color) return FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;
  if (t == textures_.end() || !t->second->id || t->second->compressed)
    return FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
  return FRAMEBUFFER_COMPLETE;
}

void Context::GenRenderbuffers(int n, uint32_t* names) {
  auto& rbs = shared_->renderbuffers;
  for (int i = 0; i < n; i++) {
    while (rbs.count(shared_->next_renderbuffer)) shared_->next_renderbuffer++;
    names[i] = shared_->next_renderbuffer;
    rbs[shared_->next_renderbuffer++] = new Renderbuffer;
  }
}

void Context::DeleteRenderbuffers(int n, const uint32_t* names) {
  for (int i = 0; i < n; i++) {
    auto it = shared_->renderbuffers.find(names[i]);
    if (!names[i] || it == shared_->renderbuffers.end()) continue;
    if (it->second->depth) dev_->DestroyDepth(it->second->depth);
    delete it->second;
    shared_->renderbuffers.erase(it);
    if (renderbuffer_ == names[i]) renderbuffer_ = 0;
  }
}

void Context::BindRenderbuffer(Enum target, uint32_t name) {
  if (target != RENDERBUFFER) return Unsupported("glBindRenderbuffer target", target);
  if (name && !shared_->renderbuffers.count(name)) shared_->renderbuffers[name] = new Renderbuffer;
  renderbuffer_ = name;
}

bool Context::IsRenderbuffer(uint32_t name) const {
  return name && shared_->renderbuffers.count(name);
}

// Depth and depth-stencil storage only (the colour attachments games use
// here are textures).
void Context::RenderbufferStorage(Enum target, Enum internal, int w, int h) {
  if (target != RENDERBUFFER || !renderbuffer_) return Error(INVALID_OPERATION, "glRenderbufferStorage");
  const bool depth = internal == DEPTH_COMPONENT || internal == 0x81A5 || internal == 0x81A6 ||
                     internal == 0x81A7 || internal == 0x84F9 || internal == 0x88F0 ||
                     internal == 0x8D48;  // DEPTH_COMPONENT16/24/32, DEPTH_STENCIL, 24_8, STENCIL_INDEX8
  if (!depth) return Unsupported("glRenderbufferStorage format", internal);
  Renderbuffer* r = shared_->renderbuffers[renderbuffer_];
  if (r->depth) dev_->DestroyDepth(r->depth);
  r->w = w;
  r->h = h;
  r->internal = internal;
  r->depth = dev_->CreateDepth(std::max(w, 1), std::max(h, 1), gfx::Format::D24S8);
  Changed();
}

// Mipmaps of the bound texture from its first level: read back, halved
// with a box filter on the host and uploaded (games generate them once per
// texture, after loading or drawing it).
void Context::GenerateMipmap(Enum target) {
  if (target != TEXTURE_2D) return Unsupported("glGenerateMipmap target", target);
  Texture* t = Bound(active_unit_);
  if (!t || !t->id || t->compressed || t->defined.empty() || !t->defined[0])
    return Error(INVALID_OPERATION, "glGenerateMipmap");
  std::vector<uint8_t> cur(size_t(t->w) * size_t(t->h) * 4);
  dev_->ReadImage(t->id, 0, 0, {0, 0, t->w, t->h}, cur.data(), uint32_t(t->w) * 4);
  int w = t->w, h = t->h;
  for (int level = 1; level < t->levels; level++) {
    const int nw = std::max(1, w >> 1), nh = std::max(1, h >> 1);
    std::vector<uint8_t> next(size_t(nw) * size_t(nh) * 4);
    for (int y = 0; y < nh; y++)
      for (int x = 0; x < nw; x++)
        for (int k = 0; k < 4; k++) {
          int sum = 0, n = 0;
          for (int dy = 0; dy < 2; dy++)
            for (int dx = 0; dx < 2; dx++) {
              int sx = std::min(2 * x + dx, w - 1), sy = std::min(2 * y + dy, h - 1);
              sum += cur[(size_t(sy) * size_t(w) + size_t(sx)) * 4 + size_t(k)];
              n++;
            }
          next[(size_t(y) * size_t(nw) + size_t(x)) * 4 + size_t(k)] = uint8_t((sum + n / 2) / n);
        }
    dev_->UpdateTexture(t->id, level, {0, 0, nw, nh}, next.data(), uint32_t(nw) * 4);
    t->defined[size_t(level)] = true;
    cur.swap(next);
    w = nw;
    h = nh;
  }
  Changed();
}

// ---- targets ------------------------------------------------------------------------------

void Context::UseImageFramebuffer() {
  if (fb_image_) return;
  gfx::ImageDesc d;
  d.w = fb_w_;
  d.h = fb_h_;
  d.fmt = gfx::Format::A8R8G8B8;
  d.color_target = true;
  fb_image_ = dev_->CreateImage(d);
  if (!fb_image_) FatalF("gl: no image for the default framebuffer (%dx%d)", fb_w_, fb_h_);
  // It starts as the back buffer was: what the frame drew so far is lost
  // (this happens at the first program draw or framebuffer object).
  dev_->ClearImage(fb_image_, 0, gfx::Color{0, 0, 0, 1});
  Changed();
  matrices_dirty_ = true;
}

void Context::Targets(gfx::ColorTarget* color, gfx::DepthId* depth, int* w, int* h, bool* flip) {
  TargetsOf(framebuffer_, color, depth, w, h, flip);
}

void Context::TargetsOf(uint32_t fbo, gfx::ColorTarget* color, gfx::DepthId* depth, int* w, int* h,
                        bool* flip) {
  *flip = false;
  *w = fb_w_;
  *h = fb_h_;
  *color = {fb_image_, 0};
  *depth = depth_;
  if (!fbo || !framebuffers_.count(fbo)) return;
  const Framebuffer* f = framebuffers_[fbo];
  auto t = textures_.find(f->color);
  if (t == textures_.end() || !t->second->id) return;  // incomplete: draws go to the window
  *color = {t->second->id, f->level};
  *w = std::max(1, t->second->w >> f->level);
  *h = std::max(1, t->second->h >> f->level);
  *flip = true;
  *depth = 0;
  if (f->depth) {
    auto r = shared_->renderbuffers.find(f->depth);
    if (r != shared_->renderbuffers.end()) *depth = r->second->depth;
  }
}

void Context::ApplyTarget() {
  gfx::ColorTarget c;
  gfx::DepthId d;
  int w, h;
  bool flip;
  Targets(&c, &d, &w, &h, &flip);
  if (w != tgt_w_ || h != tgt_h_ || flip != tgt_flip_) {
    Changed();
    matrices_dirty_ = true;
  }
  tgt_w_ = w;
  tgt_h_ = h;
  tgt_flip_ = flip;
  tgt_depth_ = d;
  if (c.level) {
    gfx::ColorTarget all[4] = {c, {}, {}, {}};
    dev_->SetRenderTargets(all, d);
  } else {
    dev_->SetRenderTarget(c.tex, d);
  }
}

void Context::ShowFramebuffer() {
  if (!fb_image_) return;
  dev_->DrawImage(fb_image_, {0, 0, fb_w_, fb_h_});
}

// ---- copies from the framebuffer --------------------------------------------------------

void Context::CopyTexSubImage2D(Enum target, int level, int xoff, int yoff, int x, int y, int w,
                                int h) {
  if (target != TEXTURE_2D) return Unsupported("glCopyTexSubImage2D target", target);
  Texture* t = Bound(active_unit_);
  if (!t || !t->id || t->compressed || level >= t->levels || w <= 0 || h <= 0)
    return Error(INVALID_OPERATION, "glCopyTexSubImage2D");
  // Through the host: read the target's rows, put them in GL's order
  // (bottom-up) and upload them.
  ApplyTarget();
  std::vector<uint8_t> px(size_t(w) * size_t(h) * 4), rows(px.size());
  const int top = tgt_flip_ ? y : tgt_h_ - (y + h);
  dev_->ReadBack({x, top, x + w, top + h}, px.data(), uint32_t(w) * 4, gfx::Format::A8R8G8B8);
  for (int r = 0; r < h; r++)
    std::memcpy(rows.data() + size_t(r) * size_t(w) * 4,
                px.data() + size_t(tgt_flip_ ? r : h - 1 - r) * size_t(w) * 4, size_t(w) * 4);
  dev_->UpdateTexture(t->id, level, {xoff, yoff, xoff + w, yoff + h}, rows.data(), uint32_t(w) * 4);
  Changed();
}

void Context::BlitFramebuffer(int sx0, int sy0, int sx1, int sy1, int dx0, int dy0, int dx1,
                              int dy1, uint32_t mask, Enum filter) {
  if (mask & ~uint32_t(COLOR_BUFFER_BIT))
    Unsupported("glBlitFramebuffer of depth or stencil", mask & ~uint32_t(COLOR_BUFFER_BIT));
  if (!(mask & COLOR_BUFFER_BIT)) return;
  UseImageFramebuffer();
  gfx::ColorTarget src, dst;
  gfx::DepthId sd, dd;
  int sw, sh, dw, dh;
  bool sflip, dflip;
  TargetsOf(read_framebuffer_, &src, &sd, &sw, &sh, &sflip);
  TargetsOf(framebuffer_, &dst, &dd, &dw, &dh, &dflip);
  if ((sx0 > sx1) != (dx0 > dx1)) return Unsupported("glBlitFramebuffer mirrored in x", 0);
  // GL rectangles (bottom-left origin, either way up) as rows of each
  // target; the copy is flipped when the two disagree.
  auto rows = [](int y0, int y1, int h, bool bottom_up, bool* reversed) {
    *reversed = y0 > y1;
    const int lo = std::min(y0, y1), hi = std::max(y0, y1);
    return bottom_up ? std::pair{lo, hi} : std::pair{h - hi, h - lo};
  };
  bool srev, drev;
  auto [st, sb] = rows(sy0, sy1, sh, sflip, &srev);
  auto [dt, db] = rows(dy0, dy1, dh, dflip, &drev);
  const gfx::Rect sr{std::min(sx0, sx1), st, std::max(sx0, sx1), sb};
  const gfx::Rect dr{std::min(dx0, dx1), dt, std::max(dx0, dx1), db};
  const bool linear = filter == LINEAR;
  if ((sflip != dflip) != (srev != drev)) dev_->StretchRectFlipped(src, sr, dst, dr, linear);
  else dev_->StretchRect(src, sr, dst, dr, linear);
}

void Context::BlendFuncSeparate(Enum src_rgb, Enum dst_rgb, Enum src_a, Enum dst_a) {
  Changed();
  blend_src_ = src_rgb;
  blend_dst_ = dst_rgb;
  blend_src_a_ = src_a;
  blend_dst_a_ = dst_a;
}

void Context::BlendEquationSeparate(Enum rgb, Enum alpha) {
  Changed();
  blend_eq_ = rgb;
  blend_eq_a_ = alpha;
}

// ---- drawing with a program ----------------------------------------------------------------

void Context::DrawWithProgram(Enum mode, const std::vector<uint32_t>& verts) {
  ProgramObj* p = Program(program_);
  if (!p || !p->linked) return Error(INVALID_OPERATION, "draw with an unlinked program");
  UseImageFramebuffer();
  gfx::ColorTarget color;
  gfx::DepthId depth;
  int tw, th;
  bool flip;
  Targets(&color, &depth, &tw, &th, &flip);
  ApplyTarget();
  if (!scene_) {
    dev_->BeginScene();
    scene_ = true;
  }

  // The device's primitive and its vertices (indices into the arrays).
  std::vector<uint32_t> idx;
  gfx::Topology topo = gfx::Topology::TriangleList;
  const size_t n = verts.size();
  switch (mode) {
    case POINTS: topo = gfx::Topology::PointList; idx = verts; break;
    case LINES: topo = gfx::Topology::LineList; idx = verts; break;
    case LINE_STRIP: topo = gfx::Topology::LineStrip; idx = verts; break;
    case LINE_LOOP:
      topo = gfx::Topology::LineStrip;
      idx = verts;
      if (n) idx.push_back(verts[0]);
      break;
    case TRIANGLES: idx = verts; break;
    case TRIANGLE_STRIP: topo = gfx::Topology::TriangleStrip; idx = verts; break;
    case TRIANGLE_FAN: case POLYGON:
      for (size_t i = 2; i < n; i++) idx.insert(idx.end(), {verts[0], verts[i - 1], verts[i]});
      break;
    case QUADS:
      for (size_t i = 0; i + 3 < n; i += 4)
        idx.insert(idx.end(), {verts[i], verts[i + 1], verts[i + 2], verts[i], verts[i + 2], verts[i + 3]});
      break;
    case QUAD_STRIP:
      for (size_t i = 0; i + 3 < n; i += 2)
        idx.insert(idx.end(), {verts[i], verts[i + 1], verts[i + 3], verts[i], verts[i + 3], verts[i + 2]});
      break;
    default: return Unsupported("draw mode", mode);
  }
  if (idx.empty()) return;
  const uint32_t lo = *std::min_element(verts.begin(), verts.end());
  const uint32_t hi = *std::max_element(verts.begin(), verts.end());

  // ---- inputs: each array's buffer (a buffer object's device buffer, or
  // the client memory the draw reads), and its layout
  glsl::InputLayout layout[glsl::kInputs];
  std::vector<gfx::BufferData> buffers;
  std::map<uint32_t, int> buffer_slot;  // by buffer object
  int32_t offsets[glsl::kInputs] = {};
  auto input = [&](int in, const Array& a, bool normalized) {
    if (!a.enabled) return;
    const uint32_t stride = uint32_t(a.Stride());
    glsl::InputLayout& l = layout[in];
    l.type = a.type;
    l.size = a.size;
    l.normalized = normalized;
    l.stride = stride;
    if (a.buffer) {
      auto it = shared_->buffers.find(a.buffer);
      if (it == shared_->buffers.end() || it->second->data.empty()) return;
      Buffer* b = it->second;
      // The device copy, created or brought up to date.
      const uint32_t size = uint32_t((b->data.size() + 3) & ~size_t(3));
      if (!b->gpu || b->gpu_size < size) {
        if (b->gpu) dev_->DestroyBuffer(b->gpu);
        b->gpu = dev_->CreateBuffer(size);
        b->gpu_size = size;
        b->dirty_lo = 0;
        b->dirty_hi = b->data.size();
      }
      if (b->dirty_hi > b->dirty_lo) {
        const size_t from = b->dirty_lo & ~size_t(3);
        const size_t to = std::min((b->dirty_hi + 3) & ~size_t(3), b->data.size() & ~size_t(3));
        if (to > from)
          dev_->UpdateBuffer(b->gpu, uint32_t(from), b->data.data() + from, uint32_t(to - from));
        if (b->data.size() & 3) {  // the last partial word, padded
          uint8_t tail[4] = {};
          const size_t at = b->data.size() & ~size_t(3);
          std::memcpy(tail, b->data.data() + at, b->data.size() - at);
          dev_->UpdateBuffer(b->gpu, uint32_t(at), tail, 4);
        }
        b->dirty_lo = b->dirty_hi = 0;
      }
      auto slot = buffer_slot.find(a.buffer);
      if (slot == buffer_slot.end()) {
        slot = buffer_slot.emplace(a.buffer, int(buffers.size())).first;
        gfx::BufferData bd;
        bd.buffer = b->gpu;
        bd.size = b->gpu_size;
        buffers.push_back(bd);
      }
      l.buffer = slot->second;
      offsets[in] = int32_t(a.offset + size_t(lo) * stride);
    } else if (a.ptr) {
      // Client memory: the vertices lo..hi (read at once, when the draw is
      // recorded), padded to whole words.
      gfx::BufferData bd;
      bd.data = a.ptr + size_t(lo) * stride;
      bd.size = uint32_t(((size_t(hi - lo) * stride + size_t(a.size * TypeBytes(a.type))) + 3) & ~size_t(3));
      l.buffer = int(buffers.size());
      buffers.push_back(bd);
      offsets[in] = 0;
    }
  };
  input(glsl::kInVertex, vertex_, false);
  input(glsl::kInNormal, normal_, true);
  input(glsl::kInColor, color_, true);
  for (int u = 0; u < 4; u++) input(glsl::kInTexCoord0 + u, texcoord_[u], false);
  for (int i = 0; i < glsl::kMaxAttribs; i++)
    input(glsl::kInConventional + i, attribs_[i], attribs_[i].normalized);

  // ---- the device program for this layout
  std::string key;
  for (const glsl::InputLayout& l : layout) {
    char b[48];
    std::snprintf(b, sizeof b, "%d:%x:%d:%d:%u;", l.buffer, l.type, l.size, l.normalized ? 1 : 0,
                  l.stride);
    key += b;
  }
  gfx::ProgramId prog;
  if (auto it = p->device.find(key); it != p->device.end()) {
    prog = it->second;
  } else {
    gfx::ProgramDesc d;
    d.vs.glsl = glsl::VertexStage(p->glsl, layout);
    d.vs.textures = uint32_t(p->glsl.samplers);
    d.vs.buffers = uint32_t(buffers.size());
    d.vs.uniform_bytes = p->glsl.uniform_bytes;
    d.fs.glsl = p->glsl.fs;
    d.fs.textures = uint32_t(p->glsl.samplers);
    d.fs.uniform_bytes = p->glsl.uniform_bytes;
    prog = dev_->CreateProgram(d);
    p->device[key] = prog;
  }
  if (!prog) return Error(INVALID_OPERATION, "draw: no device program");

  // ---- the uniform block: the program's values and the built-ins
  uniform_scratch_ = p->uniforms;
  uint8_t* u = uniform_scratch_.data();
  auto put = [&](uint32_t at, const void* v, size_t size) { std::memcpy(u + at, v, size); };
  using B = glsl::Builtins;
  const std::array<float, 16>& mv = stacks_[0].back();
  const std::array<float, 16>& proj = stacks_[1].back();
  const std::array<float, 16> mvp = MatMul(proj.data(), mv.data());
  put(B::kMvp, mvp.data(), 64);
  put(B::kMv, mv.data(), 64);
  put(B::kProj, proj.data(), 64);
  for (int t = 0; t < 4; t++) put(B::kTexMat + 64 * uint32_t(t), stacks_[2 + t].back().data(), 64);
  put(B::kNormalMat, mv.data(), 64);  // its upper 3x3 (exact for rotations and uniform scales)
  put(B::kAttribOffset, offsets, sizeof offsets);
  float cur[glsl::kInputs][4] = {};
  std::copy_n(current_.pos, 4, cur[glsl::kInVertex]);
  std::copy_n(current_.normal, 3, cur[glsl::kInNormal]);
  const uint32_t c = current_.color;  // A8R8G8B8
  cur[glsl::kInColor][0] = float((c >> 16) & 255) / 255;
  cur[glsl::kInColor][1] = float((c >> 8) & 255) / 255;
  cur[glsl::kInColor][2] = float(c & 255) / 255;
  cur[glsl::kInColor][3] = float(c >> 24) / 255;
  for (int t = 0; t < 4; t++) {
    cur[glsl::kInTexCoord0 + t][0] = current_.tc[t][0];
    cur[glsl::kInTexCoord0 + t][1] = current_.tc[t][1];
    cur[glsl::kInTexCoord0 + t][3] = 1;
  }
  for (int i = 0; i < glsl::kMaxAttribs; i++)
    std::copy_n(attrib_current_[i], 4, cur[glsl::kInConventional + i]);
  put(B::kCurrent, cur, sizeof cur);
  const int32_t vbase = int32_t(lo);
  const float ndc_y = flip ? -1.0f : 1.0f, fb_h = float(th), frag_flip = flip ? 0.0f : 1.0f;
  const bool alpha_test = enabled_.count(ALPHA_TEST);
  const int32_t alpha_func = alpha_test ? int32_t(alpha_func_ - NEVER) : 7;
  put(B::kVertexBase, &vbase, 4);
  put(B::kNdcY, &ndc_y, 4);
  put(B::kFbHeight, &fb_h, 4);
  put(B::kFragFlip, &frag_flip, 4);
  put(B::kAlphaFunc, &alpha_func, 4);
  put(B::kAlphaRef, &alpha_ref_, 4);

  // ---- textures: each sampler's unit
  std::vector<gfx::TextureId> textures(size_t(p->glsl.samplers), 0);
  std::vector<gfx::SamplerDesc> samplers(size_t(p->glsl.samplers));
  for (int s = 0; s < p->glsl.samplers; s++) {
    const int unit = p->sampler_units[size_t(s)];
    if (unit < 0 || unit >= kImageUnits) continue;
    const Texture* t = Bound(unit);
    if (!t || !t->id) continue;
    textures[size_t(s)] = t->id;
    gfx::SamplerDesc& sd = samplers[size_t(s)];
    sd.mag_linear = t->mag_filter == LINEAR;
    sd.min_linear = t->min_filter == LINEAR || t->min_filter == LINEAR_MIPMAP_NEAREST ||
                    t->min_filter == LINEAR_MIPMAP_LINEAR;
    sd.mip_linear = t->min_filter == NEAREST_MIPMAP_LINEAR || t->min_filter == LINEAR_MIPMAP_LINEAR;
    if (!UsesMipmaps(t->min_filter)) sd.max_lod = 0;
    auto wrap = [](Enum w) {
      return w == REPEAT ? gfx::AddressMode::Repeat
             : w == 0x8370 ? gfx::AddressMode::MirroredRepeat  // GL_MIRRORED_REPEAT
                           : gfx::AddressMode::ClampToEdge;
    };
    sd.u = wrap(t->wrap_s);
    sd.v = wrap(t->wrap_t);
    sd.max_anisotropy = t->aniso;
  }

  // ---- the pipeline
  gfx::ProgramDraw d;
  d.program = prog;
  gfx::PipelineState& ps = d.state;
  ps.topology = topo;
  const bool depth_test = enabled_.count(DEPTH_TEST) && depth;
  ps.depth_test = depth_test;
  ps.depth_write = depth_test && depth_mask_;
  ps.depth_compare = DeviceCompare(depth_func_);
  if (enabled_.count(POLYGON_OFFSET_FILL)) {
    ps.depth_bias = offset_units_;
    ps.depth_bias_slope = offset_factor_;
  }
  if (enabled_.count(CULL_FACE) && cull_face_ != FRONT_AND_BACK) {
    ps.cull = cull_face_ == BACK ? gfx::CullMode::Back : gfx::CullMode::Front;
    // GL's front faces are counter-clockwise as it shows them; the flip
    // into a framebuffer object mirrors them.
    ps.front_ccw = (front_face_ == CCW) != flip;
  }
  if (enabled_.count(STENCIL_TEST) && depth) {
    ps.stencil = true;
    gfx::StencilFace f;
    f.fail = DeviceStencilOp(stencil_fail_);
    f.depth_fail = DeviceStencilOp(stencil_zfail_);
    f.pass = DeviceStencilOp(stencil_zpass_);
    f.compare = DeviceCompare(stencil_func_);
    ps.stencil_front = ps.stencil_back = f;
    ps.stencil_read_mask = uint8_t(stencil_mask_);
    ps.stencil_write_mask = uint8_t(stencil_write_);
    ps.stencil_ref = uint8_t(stencil_ref_);
  }
  gfx::TargetBlend& tb = ps.blend[0];
  tb.enable = enabled_.count(BLEND);
  tb.src_color = DeviceBlend(blend_src_);
  tb.dst_color = DeviceBlend(blend_dst_);
  tb.color_op = DeviceBlendOp(blend_eq_);
  tb.src_alpha = DeviceBlend(blend_src_a_);
  tb.dst_alpha = DeviceBlend(blend_dst_a_);
  tb.alpha_op = DeviceBlendOp(blend_eq_a_);
  tb.write_mask = uint8_t(color_mask_);
  d.targets[0] = color;
  d.depth = depth;
  // Viewport and scissor in the target's rows (top-down in the default
  // framebuffer, bottom-up in a framebuffer object).
  const int vy = flip ? vp_[1] : th - (vp_[1] + vp_[3]);
  d.viewport = {uint32_t(std::max(vp_[0], 0)), uint32_t(std::max(vy, 0)), uint32_t(std::max(vp_[2], 0)),
                uint32_t(std::max(vp_[3], 0)), float(depth_near_), float(depth_far_)};
  d.scissor = {0, 0, tw, th};
  if (enabled_.count(SCISSOR_TEST)) {
    const int sy = flip ? scissor_[1] : th - (scissor_[1] + scissor_[3]);
    d.scissor = {std::clamp(scissor_[0], 0, tw), std::clamp(sy, 0, th),
                 std::clamp(scissor_[0] + scissor_[2], 0, tw), std::clamp(sy + scissor_[3], 0, th)};
  }
  d.vertex_count = uint32_t(idx.size());
  d.indices = idx.data();
  d.index_size = 4;
  for (gfx::ProgramResources* r : {&d.vs, &d.fs}) {
    r->textures = textures.data();
    r->samplers = samplers.data();
    r->uniforms = uniform_scratch_.data();
  }
  d.vs.buffers = buffers.data();
  dev_->DrawProgram(d);
}

}  // namespace recomp::gl
