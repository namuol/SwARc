// gfx::GlslToBackendCode's disk cache: a second call reads back the entry
// the first one wrote, a damaged or mismatched entry is compiled again
// (never trusted), and different sources get different entries.

#include <dirent.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "runtime/env.h"
#include "runtime/gfx/shader_compile.h"

namespace {

int g_failures = 0;
void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    g_failures++;
  }
}

std::vector<std::string> Entries(const std::string& dir) {
  std::vector<std::string> v;
  if (DIR* d = opendir(dir.c_str())) {
    while (dirent* e = readdir(d))
      if (e->d_name[0] != '.') v.push_back(dir + "/" + e->d_name);
    closedir(d);
  }
  return v;
}

const char* kVs = R"(#version 450
layout(location = 0) in vec4 p;
void main() { gl_Position = p * 2.0; }
)";
const char* kFs = R"(#version 450
layout(location = 0) out vec4 c;
void main() { c = vec4(0.25, 0.5, 0.75, 1.0); }
)";

}  // namespace

int main() {
  using namespace recomp;
  using namespace recomp::gfx;
  char tmpl[] = "/tmp/shader_cache_test.XXXXXX";
  const char* root = mkdtemp(tmpl);
  if (!root) return 1;
  const std::string dir = std::string(root) + "/cache";
  setenv("SCTEST_SHADER_CACHE", dir.c_str(), 1);
  SetEnvId("sctest");
  Check(ShaderCacheDir() == dir, "ShaderCacheDir follows <ID>_SHADER_CACHE");

  std::string a, b, log;
  Check(GlslToBackendCode(kVs, ShaderStage::Vertex, &a, &log), "first compile");
  Check(!a.empty(), "code produced");
  std::vector<std::string> e = Entries(dir);
  Check(e.size() == 1, "one entry written");

  Check(GlslToBackendCode(kVs, ShaderStage::Vertex, &b, &log) && b == a, "read back the same code");
  Check(Entries(dir).size() == 1, "no second entry for the same source");

  // Another source's entry under the same name (as a hash collision would
  // leave it: a different key and different code) is a miss: the code is
  // compiled again, not read.
  if (e.size() == 1) {
    std::FILE* f = std::fopen(e[0].c_str(), "r+b");
    Check(f != nullptr, "open entry");
    if (f) {
      std::fseek(f, -int(a.size()) - 9, SEEK_END);  // the stored key's last byte
      int ch = std::fgetc(f);
      std::fseek(f, -1, SEEK_CUR);
      std::fputc(ch ^ 1, f);
      std::fseek(f, -1, SEEK_END);  // and the code's
      ch = std::fgetc(f);
      std::fseek(f, -1, SEEK_END);
      std::fputc(ch ^ 1, f);
      std::fclose(f);
    }
    b.clear();
    Check(GlslToBackendCode(kVs, ShaderStage::Vertex, &b, &log) && b == a,
          "a mismatched entry is compiled again");
    // A truncated one too.
    if ((f = std::fopen(e[0].c_str(), "wb"))) std::fclose(f);
    b.clear();
    Check(GlslToBackendCode(kVs, ShaderStage::Vertex, &b, &log) && b == a,
          "a truncated entry is compiled again");
  }

  Check(GlslToBackendCode(kFs, ShaderStage::Fragment, &b, &log) && b != a, "another shader");
  Check(Entries(dir).size() == 2, "its own entry");

  for (const std::string& p : Entries(dir)) std::remove(p.c_str());
  rmdir(dir.c_str());
  rmdir(root);
  if (g_failures == 0) std::puts("shader_cache_test: ok");
  return g_failures ? 1 : 0;
}
