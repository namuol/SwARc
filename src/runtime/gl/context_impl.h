#pragma once
// gl::Context's objects (context.h), shared by its source files.

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "runtime/gl/context.h"
#include "runtime/gl/glsl.h"

namespace recomp::gl {

inline bool UsesMipmaps(Enum min_filter) {
  return min_filter != NEAREST && min_filter != LINEAR;
}

struct Context::Texture {
  gfx::TextureId id = 0;
  int w = 0, h = 0, levels = 0;
  bool compressed = false;
  gfx::Format fmt = gfx::Format::Unknown;
  // GL base-format facts the environment rules depend on (storage always
  // has four channels).
  bool has_alpha = true, has_color = true;
  std::vector<bool> defined;
  Enum min_filter = NEAREST_MIPMAP_LINEAR, mag_filter = LINEAR;
  Enum wrap_s = REPEAT, wrap_t = REPEAT;
  float aniso = 1;
  bool Complete() const {
    if (!id || defined.empty() || !defined[0]) return false;
    if (!UsesMipmaps(min_filter)) return true;
    return std::all_of(defined.begin(), defined.end(), [](bool b) { return b; });
  }
};

struct Context::Unit {
  uint32_t bound = 0;
  bool tex2d = false;
  Enum env_mode = MODULATE;
  float env_color[4] = {0, 0, 0, 0};
  Enum combine_rgb = MODULATE, combine_alpha = MODULATE;
  Enum src_rgb[3] = {TEXTURE, PREVIOUS, CONSTANT};
  Enum op_rgb[3] = {SRC_COLOR, SRC_COLOR, SRC_ALPHA};
  Enum src_alpha[3] = {TEXTURE, PREVIOUS, CONSTANT};
  Enum op_alpha[3] = {SRC_ALPHA, SRC_ALPHA, SRC_ALPHA};
  float rgb_scale = 1, alpha_scale = 1;
};

// A buffer object: its contents here (fixed-function draws and reads
// take them from here), mirrored in a device buffer for program draws
// once they change (the bytes from dirty_lo to dirty_hi).
struct Context::Buffer {
  std::vector<uint8_t> data;
  Enum usage = 0x88E4;  // GL_STATIC_DRAW
  gfx::BufferId gpu = 0;
  uint32_t gpu_size = 0;
  size_t dirty_lo = 0, dirty_hi = 0;
};

struct Context::Shader {
  Enum type = 0;
  std::string source;
  bool compiled = false, deleted = false;
  int attached = 0;  // programs it is attached to
};

// A linked program: its translation, its uniforms' values (the block's
// bytes, and each sampler's texture unit), and the device programs made
// for the vertex layouts it was drawn with.
struct Context::ProgramObj {
  std::vector<uint32_t> attached;
  std::map<std::string, int> bound_attribs;
  bool linked = false, deleted = false;
  std::string log;
  glsl::Program glsl;
  std::vector<uint8_t> uniforms;
  std::vector<int> sampler_units;
  std::map<std::string, gfx::ProgramId> device;
};

struct Context::Renderbuffer {
  int w = 0, h = 0;
  Enum internal = 0;
  gfx::DepthId depth = 0;
};

struct Context::Framebuffer {
  uint32_t color = 0;  // texture name
  int level = 0;
  uint32_t depth = 0;  // renderbuffer name (depth or depth-stencil)
};

struct Context::Shared {
  explicit Shared(gfx::Device* d) : dev(d) {}
  ~Shared() {
    for (auto& [n, t] : textures) {
      if (t->id) dev->DestroyTexture(t->id);
      delete t;
    }
    for (auto& [n, b] : buffers) {
      if (b->gpu) dev->DestroyBuffer(b->gpu);
      delete b;
    }
    for (auto& [n, s] : shaders) delete s;
    for (auto& [n, p] : programs) {
      for (auto& [k, id] : p->device) dev->DestroyProgram(id);
      delete p;
    }
    for (auto& [n, r] : renderbuffers) {
      if (r->depth) dev->DestroyDepth(r->depth);
      delete r;
    }
  }
  gfx::Device* dev;
  std::map<uint32_t, Texture*> textures;
  uint32_t next_texture = 1;
  std::map<uint32_t, Buffer*> buffers;
  uint32_t next_buffer = 1;
  // ARB_shader_objects: shaders and programs share one namespace.
  std::map<uint32_t, Shader*> shaders;
  std::map<uint32_t, ProgramObj*> programs;
  uint32_t next_object = 1;
  std::map<uint32_t, Renderbuffer*> renderbuffers;
  uint32_t next_renderbuffer = 1;
};


}  // namespace recomp::gl
