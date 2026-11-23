#pragma once
// GLSL of OpenGL 2.x-3.x programs (desktop GLSL 1.10 to 3.30, the
// compatibility profile's built-ins included) translated into the
// device's program GLSL (gfx::ProgramDesc: Vulkan-style GLSL 4.50 with
// the device's prelude), so gl::Context can draw with a game's shaders.
//
// What changes:
//   - uniforms go into one uniform block, laid out std140, beside the
//     built-in state the shaders read (gl_ModelViewProjectionMatrix and
//     the other matrices) and the values the translation adds; samplers
//     become the stage's textures, numbered in the order they are
//     declared;
//   - vertex inputs (gl_Vertex, gl_Color, gl_MultiTexCoordN, gl_Normal and
//     the program's own attributes) become globals that a fetch function,
//     made for each layout of the vertex arrays (FetchGlsl), reads from the
//     draw's buffers before the shader's main runs;
//   - varyings get explicit locations, matched by name between the
//     stages, with the built-in ones (gl_FrontColor, gl_TexCoord) first;
//   - fragment outputs (gl_FragColor, gl_FragData or the shader's own)
//     get locations; gl_FragCoord reads GL's bottom-left origin;
//   - the vertex stage's gl_Position goes from GL's clip space (depth -w..w)
//     to the device's (0..w), flipped vertically when drawing into a
//     framebuffer object (whose rows GL keeps bottom-up);
//   - the fixed-function alpha test, which GL still applies after a
//     fragment shader, runs at the end of the fragment stage;
//   - identifiers that later GLSL reserved (sample, buffer...) or that
//     hide a built-in the translation needs (a sampler named texture) are
//     renamed; texture2D and its relatives become texture().
// The rest of the source is kept as it is: it must be GLSL that a 4.50
// compiler takes once the above is done.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace recomp::gl::glsl {

// A value's base type in the uniform block or a vertex input.
enum class Base : uint8_t { Float, Int, UInt, Bool, Sampler2D, SamplerCube };

struct Uniform {
  std::string name;  // as the program declares it
  Base base = Base::Float;
  int comps = 1;   // vector size, or rows of a matrix
  int cols = 1;    // matrix columns (1: not a matrix)
  int array = 0;   // element count; 0: not an array
  uint32_t offset = 0;  // in the block (samplers: none)
  uint32_t stride = 0;  // between array elements
  int sampler = -1;     // samplers: the texture index
};

// The vertex arrays the fetch reads: the conventional ones, then the
// generic attributes 0..kMaxAttribs-1.
enum Input : int {
  kInVertex, kInNormal, kInColor, kInSecondaryColor, kInTexCoord0,
  kInConventional = kInTexCoord0 + 4,  // texture units with arrays
};
constexpr int kMaxAttribs = 16;
constexpr int kInputs = kInConventional + kMaxAttribs;

// One of the vertex stage's inputs.
struct Attrib {
  std::string name;  // the global the fetch sets
  int input = 0;     // Input, or kInConventional + generic index
  Base base = Base::Float;
  int comps = 4;
};

struct Program {
  bool ok = false;
  std::string log;  // the reasons it failed
  std::string vs, fs;  // translated; the vertex stage holds a marker for FetchGlsl
  std::vector<Uniform> uniforms;  // the program's (built-ins not included)
  uint32_t uniform_bytes = 0;     // the whole block, built-ins included
  int samplers = 0;
  std::vector<Attrib> attribs;
  // Generic attributes the program declares, by name, with their indices
  // (bound, explicit or assigned).
  std::map<std::string, int> attrib_locations;
};

// Offsets of the built-in members of the uniform block (Program::vs/fs
// declare them first, in this order).
struct Builtins {
  static constexpr uint32_t kMvp = 0, kMv = 64, kProj = 128, kTexMat = 192,  // 4 x mat4
      kNormalMat = 448,                         // mat4 (its upper 3x3 is used)
      kAttribOffset = 512,                      // ivec4[kInputs / 4]: byte offsets
      kCurrent = kAttribOffset + 16 * (kInputs / 4),  // vec4[kInputs]: constant inputs
      kVertexBase = kCurrent + 16 * kInputs,    // int: the first vertex in the buffers
      kNdcY = kVertexBase + 4,                  // float: 1, or -1 into framebuffer objects
      kFbHeight = kVertexBase + 8,              // float
      kFragFlip = kVertexBase + 12,             // float: 1 to flip gl_FragCoord.y
      kAlphaFunc = kVertexBase + 16,            // int: GL func - NEVER, 7 (ALWAYS): off
      kAlphaRef = kVertexBase + 20,             // float
      kEnd = kVertexBase + 32;
};
static_assert(kInputs % 4 == 0);

// Translates and links a vertex and a fragment shader (each the
// concatenation of its sources). `bound`: glBindAttribLocation's names.
Program Link(const std::string& vertex, const std::string& fragment,
             const std::map<std::string, int>& bound);

// The layout of one vertex input in the draw's buffers: buffer index
// (into the stage's buffers), the GL type and size, normalisation and
// stride. buffer < 0: not an array (the current value in the block).
struct InputLayout {
  int buffer = -1;
  uint32_t type = 0x1406;  // GL_FLOAT
  int size = 4;
  bool normalized = false;
  uint32_t stride = 0;
  bool operator==(const InputLayout&) const = default;
};

// The vertex stage for one layout of the inputs (indexed by Input).
std::string VertexStage(const Program& p, const InputLayout (&layout)[kInputs]);

}  // namespace recomp::gl::glsl
