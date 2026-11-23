#pragma once
// An OpenGL 1.x rendering context on gfx::Device: the GL state machine
// (fixed-function pipeline, texture objects, vertex arrays and immediate
// mode, matrix stacks) translated into the device's D3D-style state and
// draws. It takes plain host pointers and knows nothing of guest memory or
// Win32, so the opengl32 HLE marshals into it and native code can use it
// directly. One Context per GL context; calls come from one thread.
//
// Coverage is what the supported games use: GL 1.1 plus ARB_multitexture,
// EXT_texture_env_add, ARB_texture_env_combine, EXT_compiled_vertex_array,
// S3TC compressed textures and anisotropic filtering; and for games of the
// GL 2.x-3.x era the ARB extensions of its programmable pipeline: buffer
// objects (ARB_vertex_buffer_object), GLSL programs (ARB_shader_objects,
// ARB_vertex_shader, ARB_fragment_shader: their GLSL translated by
// gl/glsl.h and drawn as device programs), generic vertex attributes,
// framebuffer objects (EXT_framebuffer_object) and separate blending.
// Anything else records GL_INVALID_ENUM and warns once, so a gap is visible
// rather than silently drawn wrong.

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "runtime/gfx/device.h"
#include "runtime/gl/glsl.h"

namespace recomp::gl {

using Enum = uint32_t;

// The GL enum values the context understands (standard OpenGL numbering).
enum : Enum {
  NO_ERROR = 0, INVALID_ENUM = 0x0500, INVALID_VALUE = 0x0501,
  INVALID_OPERATION = 0x0502, STACK_OVERFLOW = 0x0503, STACK_UNDERFLOW = 0x0504,
  // primitives
  POINTS = 0x0000, LINES = 0x0001, LINE_LOOP = 0x0002, LINE_STRIP = 0x0003,
  TRIANGLES = 0x0004, TRIANGLE_STRIP = 0x0005, TRIANGLE_FAN = 0x0006,
  QUADS = 0x0007, QUAD_STRIP = 0x0008, POLYGON = 0x0009,
  // comparison
  NEVER = 0x0200, LESS = 0x0201, EQUAL = 0x0202, LEQUAL = 0x0203,
  GREATER = 0x0204, NOTEQUAL = 0x0205, GEQUAL = 0x0206, ALWAYS = 0x0207,
  // blending
  ZERO = 0, ONE = 1, SRC_COLOR = 0x0300, ONE_MINUS_SRC_COLOR = 0x0301,
  SRC_ALPHA = 0x0302, ONE_MINUS_SRC_ALPHA = 0x0303, DST_ALPHA = 0x0304,
  ONE_MINUS_DST_ALPHA = 0x0305, DST_COLOR = 0x0306, ONE_MINUS_DST_COLOR = 0x0307,
  SRC_ALPHA_SATURATE = 0x0308,
  // faces and polygon modes
  FRONT = 0x0404, BACK = 0x0405, FRONT_AND_BACK = 0x0408, CW = 0x0900,
  CCW = 0x0901, POINT = 0x1B00, LINE = 0x1B01, FILL = 0x1B02, FLAT = 0x1D00,
  SMOOTH = 0x1D01,
  // capabilities
  CULL_FACE = 0x0B44, LIGHTING = 0x0B50, FOG = 0x0B60, DEPTH_TEST = 0x0B71,
  STENCIL_TEST = 0x0B90, NORMALIZE = 0x0BA1, ALPHA_TEST = 0x0BC0,
  DITHER = 0x0BD0, BLEND = 0x0BE2, SCISSOR_TEST = 0x0C11, TEXTURE_2D = 0x0DE1,
  POLYGON_OFFSET_FILL = 0x8037, CLIP_PLANE0 = 0x3000, LIGHT0 = 0x4000,
  COLOR_MATERIAL = 0x0B57, TEXTURE_GEN_S = 0x0C60, TEXTURE_GEN_T = 0x0C61,
  RESCALE_NORMAL = 0x803A,
  // client arrays
  VERTEX_ARRAY = 0x8074, NORMAL_ARRAY = 0x8075, COLOR_ARRAY = 0x8076,
  TEXTURE_COORD_ARRAY = 0x8078,
  // types
  BYTE = 0x1400, UNSIGNED_BYTE = 0x1401, SHORT = 0x1402, UNSIGNED_SHORT = 0x1403,
  INT = 0x1404, UNSIGNED_INT = 0x1405, FLOAT = 0x1406, DOUBLE = 0x140A,
  // pixel formats
  ALPHA = 0x1906, RGB = 0x1907, RGBA = 0x1908, LUMINANCE = 0x1909,
  LUMINANCE_ALPHA = 0x190A, BGR_EXT = 0x80E0, BGRA_EXT = 0x80E1,
  DEPTH_COMPONENT = 0x1902,
  RGB5 = 0x8050, RGB8 = 0x8051, RGBA4 = 0x8056, RGB5_A1 = 0x8057, RGBA8 = 0x8058,
  ALPHA8 = 0x803C, LUMINANCE8 = 0x8040, LUMINANCE8_ALPHA8 = 0x8045,
  RGB4 = 0x804F, INTENSITY = 0x8049, INTENSITY8 = 0x804B,
  COMPRESSED_RGB_S3TC_DXT1 = 0x83F0, COMPRESSED_RGBA_S3TC_DXT1 = 0x83F1,
  COMPRESSED_RGBA_S3TC_DXT3 = 0x83F2, COMPRESSED_RGBA_S3TC_DXT5 = 0x83F3,
  // textures
  TEXTURE_MAG_FILTER = 0x2800, TEXTURE_MIN_FILTER = 0x2801,
  TEXTURE_WRAP_S = 0x2802, TEXTURE_WRAP_T = 0x2803, NEAREST = 0x2600,
  LINEAR = 0x2601, NEAREST_MIPMAP_NEAREST = 0x2700,
  LINEAR_MIPMAP_NEAREST = 0x2701, NEAREST_MIPMAP_LINEAR = 0x2702,
  LINEAR_MIPMAP_LINEAR = 0x2703, CLAMP = 0x2900, REPEAT = 0x2901,
  CLAMP_TO_EDGE = 0x812F, TEXTURE_MAX_ANISOTROPY_EXT = 0x84FE,
  MAX_TEXTURE_MAX_ANISOTROPY_EXT = 0x84FF, TEXTURE_BORDER_COLOR = 0x1004,
  // texture environment
  TEXTURE_ENV = 0x2300, TEXTURE_ENV_MODE = 0x2200, TEXTURE_ENV_COLOR = 0x2201,
  MODULATE = 0x2100, DECAL = 0x2101, ADD = 0x0104, REPLACE = 0x1E01,
  BLEND_MODE = 0x0BE2,  // GL_BLEND as an env mode
  COMBINE = 0x8570, COMBINE_RGB = 0x8571, COMBINE_ALPHA = 0x8572,
  RGB_SCALE = 0x8573, ADD_SIGNED = 0x8574, INTERPOLATE = 0x8575,
  CONSTANT = 0x8576, PRIMARY_COLOR = 0x8577, PREVIOUS = 0x8578,
  SUBTRACT = 0x84E7, DOT3_RGB = 0x86AE, DOT3_RGBA = 0x86AF,
  SOURCE0_RGB = 0x8580, SOURCE1_RGB = 0x8581, SOURCE2_RGB = 0x8582,
  SOURCE0_ALPHA = 0x8588, SOURCE1_ALPHA = 0x8589, SOURCE2_ALPHA = 0x858A,
  OPERAND0_RGB = 0x8590, OPERAND1_RGB = 0x8591, OPERAND2_RGB = 0x8592,
  OPERAND0_ALPHA = 0x8598, OPERAND1_ALPHA = 0x8599, OPERAND2_ALPHA = 0x859A,
  ALPHA_SCALE = 0x0D1C, TEXTURE = 0x1702,
  TEXTURE0 = 0x84C0,
  // matrices
  MODELVIEW = 0x1700, PROJECTION = 0x1701, MATRIX_MODE = 0x0BA0,
  MODELVIEW_MATRIX = 0x0BA6, PROJECTION_MATRIX = 0x0BA7,
  // fog
  FOG_DENSITY = 0x0B62, FOG_START = 0x0B63, FOG_END = 0x0B64,
  FOG_MODE = 0x0B65, FOG_COLOR = 0x0B66, EXP = 0x0800, EXP2 = 0x0801,
  // stencil
  KEEP = 0x1E00, INCR = 0x1E02, DECR = 0x1E03, INVERT = 0x150A,
  // clear bits
  DEPTH_BUFFER_BIT = 0x0100, STENCIL_BUFFER_BIT = 0x0400,
  COLOR_BUFFER_BIT = 0x4000,
  // lighting
  SPOT_EXPONENT = 0x1205, SPOT_CUTOFF = 0x1206, CONSTANT_ATTENUATION = 0x1207,
  LINEAR_ATTENUATION = 0x1208, QUADRATIC_ATTENUATION = 0x1209,
  AMBIENT = 0x1200, DIFFUSE = 0x1201, SPECULAR = 0x1202, POSITION = 0x1203,
  SPOT_DIRECTION = 0x1204, EMISSION = 0x1600, SHININESS = 0x1601,
  AMBIENT_AND_DIFFUSE = 0x1602, LIGHT_MODEL_LOCAL_VIEWER = 0x0B51,
  LIGHT_MODEL_TWO_SIDE = 0x0B52, LIGHT_MODEL_AMBIENT = 0x0B53,
  MAX_LIGHTS = 0x0D31, MAX_CLIP_PLANES = 0x0D32,
  // pixel store
  UNPACK_ALIGNMENT = 0x0CF5, PACK_ALIGNMENT = 0x0D05,
  UNPACK_ROW_LENGTH = 0x0CF2,
  // queries
  VENDOR = 0x1F00, RENDERER = 0x1F01, VERSION = 0x1F02, EXTENSIONS = 0x1F03,
  MAX_TEXTURE_SIZE = 0x0D33, MAX_TEXTURE_UNITS = 0x84E2, VIEWPORT = 0x0BA2,
  DEPTH_BITS = 0x0D56, STENCIL_BITS = 0x0D57, RED_BITS = 0x0D52,
  ALPHA_BITS = 0x0D55,
  TEXTURE_BINDING_2D = 0x8069, ACTIVE_TEXTURE = 0x84E0, CLIENT_ACTIVE_TEXTURE = 0x84E1,
  SHADING_LANGUAGE_VERSION = 0x8B8C,
  // buffer objects
  ARRAY_BUFFER = 0x8892, ELEMENT_ARRAY_BUFFER = 0x8893, ARRAY_BUFFER_BINDING = 0x8894,
  ELEMENT_ARRAY_BUFFER_BINDING = 0x8895, BUFFER_SIZE = 0x8764, BUFFER_USAGE = 0x8765,
  // shader objects
  PROGRAM_OBJECT = 0x8B40, SHADER_OBJECT = 0x8B48, FRAGMENT_SHADER = 0x8B30,
  VERTEX_SHADER = 0x8B31, OBJECT_TYPE = 0x8B4E, OBJECT_SUBTYPE = 0x8B4F,
  OBJECT_DELETE_STATUS = 0x8B80, OBJECT_COMPILE_STATUS = 0x8B81, OBJECT_LINK_STATUS = 0x8B82,
  OBJECT_VALIDATE_STATUS = 0x8B83, OBJECT_INFO_LOG_LENGTH = 0x8B84,
  OBJECT_ATTACHED_OBJECTS = 0x8B85, OBJECT_ACTIVE_UNIFORMS = 0x8B86,
  OBJECT_ACTIVE_UNIFORM_MAX_LENGTH = 0x8B87, OBJECT_SHADER_SOURCE_LENGTH = 0x8B88,
  OBJECT_ACTIVE_ATTRIBUTES = 0x8B89, OBJECT_ACTIVE_ATTRIBUTE_MAX_LENGTH = 0x8B8A,
  MAX_VERTEX_ATTRIBS = 0x8869, MAX_TEXTURE_COORDS = 0x8871, MAX_TEXTURE_IMAGE_UNITS = 0x8872,
  MAX_COMBINED_TEXTURE_IMAGE_UNITS = 0x8B4D,
  // framebuffer objects
  FRAMEBUFFER = 0x8D40, RENDERBUFFER = 0x8D41, FRAMEBUFFER_BINDING = 0x8CA6,
  READ_FRAMEBUFFER = 0x8CA8, DRAW_FRAMEBUFFER = 0x8CA9, READ_FRAMEBUFFER_BINDING = 0x8CAA,
  RENDERBUFFER_BINDING = 0x8CA7, COLOR_ATTACHMENT0 = 0x8CE0, DEPTH_ATTACHMENT = 0x8D00,
  STENCIL_ATTACHMENT = 0x8D20, DEPTH_STENCIL_ATTACHMENT = 0x821A,
  FRAMEBUFFER_COMPLETE = 0x8CD5, FRAMEBUFFER_INCOMPLETE_ATTACHMENT = 0x8CD6,
  FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT = 0x8CD7, MAX_RENDERBUFFER_SIZE = 0x84E8,
  // blending
  FUNC_ADD = 0x8006, MIN = 0x8007, MAX = 0x8008, FUNC_SUBTRACT = 0x800A,
  FUNC_REVERSE_SUBTRACT = 0x800B, CONSTANT_COLOR = 0x8001, ONE_MINUS_CONSTANT_COLOR = 0x8002,
  CONSTANT_ALPHA = 0x8003, ONE_MINUS_CONSTANT_ALPHA = 0x8004,
};

class Context {
 public:
  // The objects a share group of contexts has in common (wglShareLists):
  // texture objects, on one device. Contexts of one group may be current
  // on different threads one at a time each, but calls into the group are
  // not synchronised: like the GL contexts of the drivers games target,
  // they assume the game does not use two of them at once.
  struct Shared;
  static std::shared_ptr<Shared> NewShared(gfx::Device* dev);

  // A context with objects of its own.
  Context(gfx::Device* dev, int fb_w, int fb_h);
  // A context of a share group, drawing on its device.
  Context(std::shared_ptr<Shared> shared, int fb_w, int fb_h);
  ~Context();

  // This context becomes the current one on its device again after
  // another context of its share group drew: the device state it set is
  // set again at the next draw.
  void MakeCurrent();

  // Whether GL_EXTENSIONS lists the programmable pipeline's extensions
  // (they work either way): games of the GL 1.x era see the list they
  // were made against, so their paths stay as tested.
  void SetProgrammable(bool on) { programmable_ = on; }

  // The window (default framebuffer) changed size.
  void Resize(int fb_w, int fb_h);
  int Width() const { return fb_w_; }
  int Height() const { return fb_h_; }
  // SwapBuffers: the frame is complete; the caller presents the device.
  void EndFrame();

  Enum GetError();
  const char* GetString(Enum name) const;
  // Queries; false (and INVALID_ENUM) for parameters not modelled.
  bool GetIntegerv(Enum pname, int32_t* out);
  bool GetFloatv(Enum pname, float* out);

  // ---- fixed-function state
  void SetEnabled(Enum cap, bool on);
  bool IsEnabled(Enum cap) const;
  void BlendFunc(Enum src, Enum dst);
  void AlphaFunc(Enum func, float ref);
  void DepthFunc(Enum func);
  void DepthMask(bool on);
  void DepthRange(double near_, double far_);
  void ColorMask(bool r, bool g, bool b, bool a);
  void CullFace(Enum face);
  void FrontFace(Enum dir);
  void PolygonMode(Enum face, Enum mode);
  void PolygonOffset(float factor, float units);
  void ShadeModel(Enum mode);
  void Viewport(int x, int y, int w, int h);
  void Scissor(int x, int y, int w, int h);
  void StencilFunc(Enum func, int ref, uint32_t mask);
  void StencilOp(Enum fail, Enum zfail, Enum zpass);
  void StencilMask(uint32_t mask);
  void Fog(Enum pname, const float* v);
  void ClipPlane(Enum plane, const double eqn[4]);
  void ClearColor(float r, float g, float b, float a);
  void ClearDepth(double d);
  void ClearStencil(int s);
  void Clear(uint32_t mask);
  void PixelStore(Enum pname, int v);

  // ---- lighting
  void Light(Enum light, Enum pname, const float* v);
  void LightModel(Enum pname, const float* v);
  void Material(Enum face, Enum pname, const float* v);
  void ColorMaterial(Enum face, Enum mode);

  // ---- matrices
  void MatrixMode(Enum mode);
  void LoadIdentity();
  void LoadMatrix(const float m[16]);
  void MultMatrix(const float m[16]);
  void PushMatrix();
  void PopMatrix();
  void Ortho(double l, double r, double b, double t, double n, double f);
  void Frustum(double l, double r, double b, double t, double n, double f);
  void Translate(float x, float y, float z);
  void Rotate(float angle_deg, float x, float y, float z);
  void Scale(float x, float y, float z);

  // ---- textures
  void GenTextures(int n, uint32_t* names);
  void DeleteTextures(int n, const uint32_t* names);
  void BindTexture(Enum target, uint32_t name);
  void TexParameter(Enum target, Enum pname, const float* v);
  void TexImage2D(Enum target, int level, Enum internal_format, int w, int h,
                  int border, Enum format, Enum type, const void* pixels);
  void TexSubImage2D(Enum target, int level, int x, int y, int w, int h,
                     Enum format, Enum type, const void* pixels);
  void CompressedTexImage2D(Enum target, int level, Enum internal_format,
                            int w, int h, int border, int size, const void* data);
  void TexEnv(Enum target, Enum pname, const float* v);
  void ActiveTexture(Enum unit);
  void ClientActiveTexture(Enum unit);

  // ---- vertex arrays and drawing
  void VertexPointer(int size, Enum type, int stride, const void* p);
  void ColorPointer(int size, Enum type, int stride, const void* p);
  void TexCoordPointer(int size, Enum type, int stride, const void* p);
  void NormalPointer(Enum type, int stride, const void* p);
  void SetClientState(Enum array, bool on);
  // EXT_compiled_vertex_array: while locked, the vertex and normal arrays
  // as set at the lock keep their contents, and draws pass their streams
  // to the device as locked (gfx::FfStream::lock: uploaded once), as
  // drivers of the extension's day cached those arrays only.
  void LockArrays(int first, int count);
  void UnlockArrays();
  void DrawArrays(Enum mode, int first, int count);
  void DrawElements(Enum mode, int count, Enum type, const void* indices);
  void ArrayElement(int i);

  // ---- immediate mode
  void Begin(Enum mode);
  void End();
  void Vertex(float x, float y, float z, float w);
  void Color(float r, float g, float b, float a);
  void TexCoord(int unit, float s, float t);
  void Normal(float x, float y, float z);

  // ---- framebuffer readback (rows bottom-up, as GL returns them)
  void ReadPixels(int x, int y, int w, int h, Enum format, Enum type, void* out);
  // A level of the bound texture, its rows as they were uploaded.
  void GetTexImage(Enum target, int level, Enum format, Enum type, void* out);
  // Copies a rectangle of the framebuffer into the bound texture.
  void CopyTexSubImage2D(Enum target, int level, int xoff, int yoff, int x, int y, int w, int h);

  // ---- separate blending (EXT_blend_func_separate, EXT_blend_minmax,
  // EXT_blend_subtract, EXT_blend_equation_separate)
  void BlendFuncSeparate(Enum src_rgb, Enum dst_rgb, Enum src_a, Enum dst_a);
  void BlendEquationSeparate(Enum rgb, Enum alpha);

  // ---- buffer objects. While an ARRAY_BUFFER is bound, the array
  // pointers given to *Pointer are offsets into it (pass them as the
  // pointer's value), as are DrawElements' indices with an
  // ELEMENT_ARRAY_BUFFER.
  void GenBuffers(int n, uint32_t* names);
  void DeleteBuffers(int n, const uint32_t* names);
  void BindBuffer(Enum target, uint32_t name);
  uint32_t BoundBuffer(Enum target) const;
  bool IsBuffer(uint32_t name) const;
  void BufferData(Enum target, int64_t size, const void* data, Enum usage);
  void BufferSubData(Enum target, int64_t offset, int64_t size, const void* data);
  void GetBufferSubData(Enum target, int64_t offset, int64_t size, void* out);
  bool GetBufferParameteriv(Enum target, Enum pname, int32_t* out);

  // ---- generic vertex attributes (ARB_vertex_program's, as
  // ARB_vertex_shader takes them)
  void VertexAttribPointer(uint32_t index, int size, Enum type, bool normalized, int stride,
                           const void* p);
  void SetAttribArray(uint32_t index, bool on);
  void VertexAttrib(uint32_t index, float x, float y, float z, float w);

  // ---- shader and program objects (ARB_shader_objects: one namespace of
  // handles for both, shared by the share group)
  uint32_t CreateShaderObject(Enum type);
  uint32_t CreateProgramObject();
  void ShaderSource(uint32_t shader, const std::string& source);
  void CompileShader(uint32_t shader);
  void AttachObject(uint32_t program, uint32_t shader);
  void DetachObject(uint32_t program, uint32_t shader);
  void LinkProgram(uint32_t program);
  void UseProgramObject(uint32_t program);
  uint32_t CurrentProgram() const { return program_; }
  void DeleteObject(uint32_t obj);
  bool GetObjectParameteriv(uint32_t obj, Enum pname, int32_t* out);
  std::string InfoLog(uint32_t obj) const;
  std::string ShaderSourceOf(uint32_t shader) const;
  std::vector<uint32_t> AttachedObjects(uint32_t program) const;
  int32_t GetUniformLocation(uint32_t program, const std::string& name);
  int32_t GetAttribLocation(uint32_t program, const std::string& name);
  void BindAttribLocation(uint32_t program, uint32_t index, const std::string& name);
  // glUniform{1,2,3,4}{f,i}v (count elements of `comps` values) and
  // glUniformMatrix{2,3,4}fv, on the current program.
  void Uniformf(int32_t location, int comps, int count, const float* v);
  void Uniformi(int32_t location, int comps, int count, const int32_t* v);
  void UniformMatrix(int32_t location, int n, int count, bool transpose, const float* v);
  bool GetUniformfv(uint32_t program, int32_t location, float* out);

  // ---- framebuffer objects (EXT_framebuffer_object)
  void GenFramebuffers(int n, uint32_t* names);
  void DeleteFramebuffers(int n, const uint32_t* names);
  void BindFramebuffer(Enum target, uint32_t name);
  bool IsFramebuffer(uint32_t name) const;
  void FramebufferTexture2D(Enum target, Enum attachment, Enum textarget, uint32_t texture,
                            int level);
  void FramebufferRenderbuffer(Enum target, Enum attachment, Enum rbtarget, uint32_t rb);
  Enum CheckFramebufferStatus(Enum target);
  void GenRenderbuffers(int n, uint32_t* names);
  void DeleteRenderbuffers(int n, const uint32_t* names);
  void BindRenderbuffer(Enum target, uint32_t name);
  bool IsRenderbuffer(uint32_t name) const;
  void RenderbufferStorage(Enum target, Enum internal, int w, int h);
  void GenerateMipmap(Enum target);
  // EXT_framebuffer_blit: colour from the read framebuffer to the draw
  // framebuffer (depth and stencil are not copied).
  void BlitFramebuffer(int sx0, int sy0, int sx1, int sy1, int dx0, int dy0, int dx1, int dy1,
                       uint32_t mask, Enum filter);

  // SwapBuffers of a context whose default framebuffer is an image (see
  // fb_image_): it goes to the device's back buffer, to be presented.
  void ShowFramebuffer();

  void SetVsync(bool on);

  // ---- for native code that draws with the device itself (DrawFf),
  // mixed in any order with the context's own draws: the device, the
  // state a draw would have now (scene begun) with the number of texture
  // coordinate sets its stages read (set i for unit i; normals too when
  // LIGHTING is enabled), and ArrayDraw.
  gfx::Device* Device() const { return dev_; }
  const gfx::FfState& DrawState(int* tex_sets);
  // A draw of primitive `mode` from the client arrays' vertices first ..
  // last as the device's streams, the arrays as they are (no gathering;
  // the caller adds indices, which number the arrays' elements), and its
  // state. False when the device does not take the arrays as they are
  // (other types or sizes, a disabled array, which GL reads as the
  // current value, positions with w other than 1) or the primitive:
  // then the context's own draw does it.
  bool ArrayDraw(Enum mode, uint32_t first, uint32_t last, gfx::FfDraw* d,
                 const gfx::FfState** state);

  static constexpr int kUnits = 4;  // texture units advertised
  // Texture image units: what programs' samplers can read (the fixed-
  // function stages use the first kUnits).
  static constexpr int kImageUnits = 16;
  // The largest texture side: what SDL_GPU's backends (Metal on Apple
  // silicon, Vulkan on desktop GPUs) and Direct3D 9-class hardware take.
  static constexpr int kMaxTextureSize = 16384;

 private:
  struct Texture;
  struct Unit;
  struct Array {
    bool enabled = false;
    int size = 4;
    Enum type = FLOAT;
    int stride = 0;
    const uint8_t* ptr = nullptr;
    uint32_t buffer = 0;  // an ARRAY_BUFFER: ptr is null, offset into it
    uintptr_t offset = 0;
    bool normalized = false;  // generic attributes
    int Stride() const;
  };
  struct Vtx {  // one assembled vertex
    float pos[4];
    float normal[3];
    uint32_t color;  // D3DCOLOR (A8R8G8B8)
    float tc[kUnits][2];
  };

  void Error(Enum e, const char* what);
  void Unsupported(const char* what, Enum v);
  float* Top();  // current matrix
  void MarkMatrix();
  // The state ApplyState and ApplyTextures derive changed: the next draw
  // derives it again.
  void Changed() { state_dirty_ = tex_dirty_ = true; }
  Texture* Bound(int unit);
  void ApplyState();
  int ApplyTextures();  // returns the texture coordinate sets needed
  void ApplyMatrices();
  void RS(uint32_t state, uint32_t value);
  void TSS(uint32_t stage, uint32_t type, uint32_t value);
  void FetchVertex(int i, Vtx* v) const;
  struct Buffer;
  struct Shader;
  struct ProgramObj;
  struct Framebuffer;
  struct Renderbuffer;
  // An array's pointer: its buffer's contents at its offset, or the client
  // memory it points at.
  const uint8_t* ArrayPtr(const Array& a) const;
  Array Resolved(const Array& a) const;
  // The current colour and depth targets (the default framebuffer, or the
  // bound framebuffer object) and their size; `flip`: GL's rows bottom-up
  // in the target (a framebuffer object's texture).
  void Targets(gfx::ColorTarget* color, gfx::DepthId* depth, int* w, int* h, bool* flip);
  // The same for framebuffer `fbo` (0: the default one).
  void TargetsOf(uint32_t fbo, gfx::ColorTarget* color, gfx::DepthId* depth, int* w, int* h,
                 bool* flip);
  void ApplyTarget();
  void UseImageFramebuffer();
  // A draw with the current program: GL vertices (indices into the arrays)
  // of primitive `mode`.
  void DrawWithProgram(Enum mode, const std::vector<uint32_t>& verts);
  ProgramObj* Program(uint32_t h) const;
  // What a draw reads for each attribute; an array that is not enabled
  // (or has no pointer) stands for the current value.
  struct Sources {
    Array pos, normal, color, tc[kUnits];
    bool d3d_color = false;  // colours are D3DCOLORs (immediate mode)
  };
  Sources ClientSources() const;
  // Draws draw_verts_ (the source element of each device vertex) as
  // primitive `mode` over draw_seq_ (the device vertex of each GL vertex).
  void DrawVertices(Enum mode, const Sources& src);

  std::shared_ptr<Shared> shared_;
  gfx::Device* dev_;
  int fb_w_, fb_h_;
  gfx::DepthId depth_ = 0;
  Enum error_ = NO_ERROR;
  std::set<std::pair<std::string, Enum>> warned_;

  // fixed-function state
  std::set<Enum> enabled_;
  Enum blend_src_ = ONE, blend_dst_ = ZERO;
  Enum blend_src_a_ = ONE, blend_dst_a_ = ZERO, blend_eq_ = FUNC_ADD, blend_eq_a_ = FUNC_ADD;
  Enum alpha_func_ = ALWAYS;
  float alpha_ref_ = 0;
  Enum depth_func_ = LESS;
  bool depth_mask_ = true;
  double depth_near_ = 0, depth_far_ = 1;
  uint32_t color_mask_ = 0xF;
  Enum cull_face_ = BACK, front_face_ = CCW, polygon_mode_ = FILL;
  float offset_factor_ = 0, offset_units_ = 0;
  Enum shade_model_ = SMOOTH;
  int vp_[4] = {0, 0, 0, 0};
  int scissor_[4] = {0, 0, 0, 0};
  Enum stencil_func_ = ALWAYS;
  int stencil_ref_ = 0;
  uint32_t stencil_mask_ = 0xFFFFFFFF, stencil_write_ = 0xFFFFFFFF;
  Enum stencil_fail_ = KEEP, stencil_zfail_ = KEEP, stencil_zpass_ = KEEP;
  Enum fog_mode_ = EXP;
  float fog_density_ = 1, fog_start_ = 0, fog_end_ = 1;
  float fog_color_[4] = {0, 0, 0, 0};
  double clip_planes_[6][4] = {};  // eye space

  struct LightState {
    float ambient[4] = {0, 0, 0, 1}, diffuse[4] = {0, 0, 0, 1},
          specular[4] = {0, 0, 0, 1};
    float position[4] = {0, 0, 1, 0};  // eye space
    float spot_dir[3] = {0, 0, -1};    // eye space
    float spot_exp = 0, spot_cutoff = 180;
    float att[3] = {1, 0, 0};
  };
  LightState lights_[8];
  float model_ambient_[4] = {0.2f, 0.2f, 0.2f, 1};
  float mat_ambient_[4] = {0.2f, 0.2f, 0.2f, 1}, mat_diffuse_[4] = {0.8f, 0.8f, 0.8f, 1},
        mat_specular_[4] = {0, 0, 0, 1}, mat_emission_[4] = {0, 0, 0, 1};
  float mat_shininess_ = 0;
  Enum color_material_ = AMBIENT_AND_DIFFUSE;
  float clear_color_[4] = {0, 0, 0, 0};
  double clear_depth_ = 1;
  int clear_stencil_ = 0;
  int unpack_alignment_ = 4, pack_alignment_ = 4, unpack_row_length_ = 0;

  // matrices: stacks for modelview, projection, and one per texture unit
  Enum matrix_mode_ = MODELVIEW;
  std::vector<std::array<float, 16>> stacks_[2 + kImageUnits];
  bool matrices_dirty_ = true;

  // textures: the share group's (shared_)
  std::map<uint32_t, Texture*>& textures_;
  uint32_t& next_name_;

  // buffer objects bound; generic attributes; the program in use
  uint32_t array_buffer_ = 0, element_buffer_ = 0;
  Array attribs_[glsl::kMaxAttribs];
  float attrib_current_[glsl::kMaxAttribs][4] = {};
  uint32_t program_ = 0;
  // framebuffer objects: this context's (EXT_framebuffer_object shares
  // them; GL 3 does not, and games create them per context)
  std::map<uint32_t, Framebuffer*> framebuffers_;
  uint32_t next_framebuffer_ = 1, framebuffer_ = 0, renderbuffer_ = 0;
  uint32_t read_framebuffer_ = 0;  // EXT_framebuffer_blit's read binding
  // The default framebuffer as an image (colour target) instead of the
  // device's back buffer, from the first program draw or framebuffer
  // object on: device programs draw only into images.
  gfx::TextureId fb_image_ = 0;
  bool programmable_ = false;
  // The current target as ApplyTarget left it: its size and whether GL's
  // rows run bottom-up in it (a framebuffer object's texture), which
  // flips window coordinates, the projection and the winding.
  int tgt_w_ = 1, tgt_h_ = 1;
  bool tgt_flip_ = false;
  gfx::DepthId tgt_depth_ = 0;
  // A program's per-draw uniform block (scratch).
  std::vector<uint8_t> uniform_scratch_;
  int active_unit_ = 0, client_unit_ = 0;
  Unit* units_;

  // arrays and immediate mode
  Array vertex_, color_, normal_, texcoord_[kUnits];
  uint32_t lock_ = 0;  // the current lock's FfStream::lock, 0 when unlocked
  Array locked_vertex_, locked_normal_;  // as at the lock
  bool in_begin_ = false;
  Enum begin_mode_ = POINTS;
  std::vector<Vtx> immediate_;
  Vtx current_{};
  // per-draw scratch, kept between draws
  std::vector<uint32_t> draw_verts_;
  std::vector<uint16_t> draw_seq_;
  std::vector<uint32_t> remap_;  // DrawElements: epoch << 16 | device vertex
  uint32_t remap_epoch_ = 0;

  // the device state draws have (ApplyState, ApplyTextures, ApplyMatrices)
  gfx::FfState ff_ = gfx::DefaultFfState();
  bool state_dirty_ = true, tex_dirty_ = true;
  int tex_sets_ = 0;  // ApplyTextures' last result
  std::vector<uint8_t> draw_buf_;  // a draw's vertices and indices
  std::vector<uint16_t> draw_idx_;
  bool scene_ = false;
  bool vsync_ = false;
};

}  // namespace recomp::gl
