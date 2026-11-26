#pragma once
// GLSL to the SDL_GPU backend's shader code at run time: SPIR-V (glslang)
// for Vulkan, and on macOS MSL source from that SPIR-V (SPIRV-Cross, with
// the binding numbers as Metal indices, as the build does for the
// fixed-function shaders).

#include <cstdint>
#include <string>
#include <vector>

namespace recomp::gfx {

enum class ShaderStage { Vertex, Fragment, Compute };

// False with the compiler's messages in *log.
bool GlslToSpirv(const std::string& glsl, ShaderStage stage, std::vector<uint32_t>* spirv,
                 std::string* log);

#if defined(__APPLE__)
bool SpirvToMsl(const std::vector<uint32_t>& spirv, std::string* msl, std::string* log);
#endif

// The backend's code for `glsl`: MSL source on macOS, SPIR-V (as bytes)
// elsewhere. Games that create their shaders just before drawing with them
// would otherwise stall on compiling them every run, so the result is kept
// in a per-user disk cache (ShaderCacheDir) and the next run reads it back.
// Thread-safe. False with the compiler's messages in *log.
bool GlslToBackendCode(const std::string& glsl, ShaderStage stage, std::string* code,
                       std::string* log);

// Where GlslToBackendCode keeps its entries: <ID>_SHADER_CACHE, else the
// user's cache directory (~/Library/Caches/<id>/shaders on macOS,
// $XDG_CACHE_HOME/<id>/shaders or ~/.cache/<id>/shaders elsewhere); empty
// (no cache) when <ID>_SHADER_CACHE is "0" or there is no home directory.
std::string ShaderCacheDir();

}  // namespace recomp::gfx
