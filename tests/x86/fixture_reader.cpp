// recomp_lift_fixture's reader for the lifter fixtures clang assembles
// without linking (fixture_elf.h): .text mapped at the base its
// .lift_base section gives, with the function entries and switch tables
// of .lift_entries and .lift_tables standing in for a game's symbols
// (fixture/discovery.S describes them). Test-only: recomp_lift never
// reads these.

#include "fixture_elf.h"
#include "program.h"

namespace lifter {
namespace {

bool Match(const std::string& path, bool* x64) {
  std::vector<uint8_t> f;
  if (!fixture_elf::ReadFile(path, &f) || !fixture_elf::IsObject(f)) return false;
  *x64 = false;
  return true;
}

bool Load(Program& p, Module& m, const std::string& path, uint64_t, std::string* err) {
  fixture_elf::Object obj;
  if (!fixture_elf::Read(path, &obj, err)) return false;
  const auto base = fixture_elf::Words(obj, ".lift_base");
  const auto& text = obj.sections[".text"];
  if (base.size() != 1 || text.empty()) {
    *err = path + ": needs .lift_base and .text";
    return false;
  }
  if (!p.mem.Commit(base[0], text.size(), recomp::Prot::RX)) {
    *err = "cannot map .text";
    return false;
  }
  std::memcpy(p.mem.ptr(base[0]), text.data(), text.size());
  m.base = m.preferred_base = base[0];
  m.text_lo = base[0];
  m.text_hi = base[0] + text.size();
  for (uint32_t e : fixture_elf::Words(obj, ".lift_entries")) p.entries.insert(e);
  const auto t = fixture_elf::Words(obj, ".lift_tables");
  for (size_t i = 0; i + 2 <= t.size();) {
    const uint32_t jmp = t[i], n = t[i + 1];
    if (i + 2 + n > t.size()) {
      *err = path + ": .lift_tables truncated";
      return false;
    }
    p.jumptables[jmp].assign(t.begin() + i + 2, t.begin() + i + 2 + n);
    i += 2 + n;
  }
  return true;
}

const bool kRegistered = (RegisterImageReader({Match, Load}), true);

}  // namespace
}  // namespace lifter
