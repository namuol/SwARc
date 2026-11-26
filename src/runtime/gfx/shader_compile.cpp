#include "runtime/gfx/shader_compile.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <glslang/build_info.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "runtime/env.h"

#if defined(__APPLE__)
#include <spirv_cross/spirv_cross_c.h>
#include <spirv_cross/spirv_msl.hpp>
#endif

namespace recomp::gfx {

bool GlslToSpirv(const std::string& glsl, ShaderStage stage, std::vector<uint32_t>* spirv,
                 std::string* log) {
  static std::once_flag once;
  std::call_once(once, [] { glslang::InitializeProcess(); });
  const EShLanguage lang = stage == ShaderStage::Vertex   ? EShLangVertex
                           : stage == ShaderStage::Compute ? EShLangCompute
                                                           : EShLangFragment;
  glslang::TShader sh(lang);
  const char* src = glsl.c_str();
  sh.setStrings(&src, 1);
  sh.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
  // Vulkan 1.1 (the device asks for it): subgroup operations.
  sh.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
  sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
  const EShMessages msg = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
  if (!sh.parse(GetDefaultResources(), 450, false, msg)) {
    *log = sh.getInfoLog();
    return false;
  }
  glslang::TProgram prog;
  prog.addShader(&sh);
  if (!prog.link(msg)) {
    *log = prog.getInfoLog();
    return false;
  }
  spirv->clear();
  glslang::GlslangToSpv(*prog.getIntermediate(lang), *spirv);
  return true;
}

#if defined(__APPLE__)
bool SpirvToMsl(const std::vector<uint32_t>& spirv, std::string* msl, std::string* log) {
  try {
    spirv_cross::CompilerMSL c(spirv);
    spirv_cross::CompilerMSL::Options o = c.get_msl_options();
    o.set_msl_version(2, 1);
    o.enable_decoration_binding = true;
    o.enable_clip_distance_user_varying = false;
    c.set_msl_options(o);
    *msl = c.compile();
  } catch (const std::exception& e) {
    *log = e.what();
    return false;
  }
  return true;
}
#endif

namespace {

// What decides the code a cache entry holds besides the GLSL: the compiler
// versions and their settings above. Bump kSettings whenever GlslToSpirv's
// or SpirvToMsl's options change, or old entries keep the old code.
constexpr const char* kSettings = "1";
#define RECOMP_STR2(x) #x
#define RECOMP_STR(x) RECOMP_STR2(x)
constexpr const char* kCompilers =
    "glslang " RECOMP_STR(GLSLANG_VERSION_MAJOR) "." RECOMP_STR(GLSLANG_VERSION_MINOR) "." RECOMP_STR(
        GLSLANG_VERSION_PATCH)
#if defined(__APPLE__)
        " spirv-cross-c " RECOMP_STR(SPVC_C_API_VERSION_MAJOR) "." RECOMP_STR(
            SPVC_C_API_VERSION_MINOR) "." RECOMP_STR(SPVC_C_API_VERSION_PATCH) " msl"
#else
        " spirv"
#endif
    ;
constexpr char kMagic[8] = {'S', 'W', 'S', 'H', 'C', 'A', 'C', '1'};

uint64_t Fnv1a(const std::string& s) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (unsigned char c : s) h = (h ^ c) * 0x100000001b3ull;
  return h;
}

void MakeDirs(const std::string& path) {
  for (size_t i = 1; i <= path.size(); i++)
    if (i == path.size() || path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
}

// An entry: kMagic, the key's length and bytes, the code's length and
// bytes (lengths as 8 bytes, host order). The whole key is stored and
// compared, so a hash collision is a miss, never the wrong code.
bool ReadEntry(const std::string& path, const std::string& key, std::string* code) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  bool ok = false;
  char magic[8];
  uint64_t n = 0;
  if (std::fread(magic, 1, 8, f) == 8 && std::memcmp(magic, kMagic, 8) == 0 &&
      std::fread(&n, 8, 1, f) == 1 && n == key.size()) {
    std::string k(n, '\0');
    if (std::fread(k.data(), 1, n, f) == n && k == key && std::fread(&n, 8, 1, f) == 1 &&
        n < (uint64_t(1) << 30)) {
      code->resize(n);
      ok = std::fread(code->data(), 1, n, f) == n;
    }
  }
  std::fclose(f);
  return ok;
}

// Written to a file of its own and renamed into place: compile threads and
// other processes never see half an entry.
void WriteEntry(const std::string& dir, const std::string& path, const std::string& key,
                const std::string& code) {
  static std::atomic<unsigned> seq{0};
  MakeDirs(dir);
  const std::string tmp = path + "." + std::to_string(getpid()) + "." + std::to_string(seq++) + ".tmp";
  std::FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) return;
  const uint64_t kn = key.size(), cn = code.size();
  bool ok = std::fwrite(kMagic, 1, 8, f) == 8 && std::fwrite(&kn, 8, 1, f) == 1 &&
            std::fwrite(key.data(), 1, kn, f) == kn && std::fwrite(&cn, 8, 1, f) == 1 &&
            std::fwrite(code.data(), 1, cn, f) == cn;
  ok = std::fclose(f) == 0 && ok;
  if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) std::remove(tmp.c_str());
}

}  // namespace

std::string ShaderCacheDir() {
  static const std::string dir = [] {
    if (const char* e = Env("SHADER_CACHE"); e && *e) return std::string(std::strcmp(e, "0") ? e : "");
    const std::string& id = EnvId();
    if (id.empty()) return std::string();
#if defined(__APPLE__)
    const char* h = std::getenv("HOME");
    return h && *h ? std::string(h) + "/Library/Caches/" + id + "/shaders" : std::string();
#else
    if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x)
      return std::string(x) + "/" + id + "/shaders";
    const char* h = std::getenv("HOME");
    return h && *h ? std::string(h) + "/.cache/" + id + "/shaders" : std::string();
#endif
  }();
  return dir;
}

bool GlslToBackendCode(const std::string& glsl, ShaderStage stage, std::string* code,
                       std::string* log) {
  const std::string dir = ShaderCacheDir();
  std::string key, path;
  if (!dir.empty()) {
    key = std::string(kCompilers) + "\n" + kSettings + "\n" + char('0' + int(stage)) + "\n" + glsl;
    char name[32];
    std::snprintf(name, sizeof name, "/%016llx", static_cast<unsigned long long>(Fnv1a(key)));
    path = dir + name;
    if (ReadEntry(path, key, code)) return true;
  }
  std::vector<uint32_t> spirv;
  if (!GlslToSpirv(glsl, stage, &spirv, log)) return false;
#if defined(__APPLE__)
  if (!SpirvToMsl(spirv, code, log)) return false;
#else
  code->assign(reinterpret_cast<const char*>(spirv.data()), spirv.size() * 4);
#endif
  if (!dir.empty()) WriteEntry(dir, path, key, *code);
  return true;
}

}  // namespace recomp::gfx
