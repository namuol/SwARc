#pragma once
// The sections and .text symbols of a 32-bit x86 ELF relocatable object:
// the lifter fixtures clang assembles without linking (fixture/discovery.S).
// They must have no relocations: their addresses are resolved by the
// assembler.

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace fixture_elf {

struct Object {
  std::map<std::string, std::vector<uint8_t>> sections;
  std::map<uint32_t, std::string> text_symbols;  // offset in .text -> name
};

inline bool IsObject(const std::vector<uint8_t>& f) {
  return f.size() >= 52 && !std::memcmp(f.data(), "\x7f" "ELF", 4) && f[4] == 1 /* 32-bit */ &&
         f[5] == 1 /* little-endian */ && f[16] == 1 && f[17] == 0 /* ET_REL */ &&
         f[18] == 3 && f[19] == 0 /* EM_386 */;
}

inline bool ReadFile(const std::string& path, std::vector<uint8_t>* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

inline bool Read(const std::string& path, Object* obj, std::string* err) {
  std::vector<uint8_t> f;
  if (!ReadFile(path, &f) || !IsObject(f)) {
    *err = path + ": not a 32-bit x86 ELF object";
    return false;
  }
  auto u16 = [&](size_t o) { uint16_t v; std::memcpy(&v, &f[o], 2); return v; };
  auto u32 = [&](size_t o) { uint32_t v; std::memcpy(&v, &f[o], 4); return v; };
  const uint32_t shoff = u32(0x20);
  const uint16_t shentsize = u16(0x2E), shnum = u16(0x30), shstrndx = u16(0x32);
  if (shentsize != 40 || shstrndx >= shnum || shoff + uint64_t(shnum) * 40 > f.size()) {
    *err = path + ": bad section headers";
    return false;
  }
  struct Sh {
    uint32_t name, type, offset, size, link;
  };
  std::vector<Sh> sh(shnum);
  for (uint16_t i = 0; i < shnum; i++) {
    const size_t o = shoff + i * 40u;
    sh[i] = {u32(o), u32(o + 4), u32(o + 16), u32(o + 20), u32(o + 24)};
    if (sh[i].type != 8 /* NOBITS */ && uint64_t(sh[i].offset) + sh[i].size > f.size()) {
      *err = path + ": section past the end of the file";
      return false;
    }
  }
  auto str = [&](uint32_t table, uint32_t off) {
    const Sh& s = sh[table];
    if (off >= s.size) return std::string();
    const char* p = reinterpret_cast<const char*>(&f[s.offset + off]);
    return std::string(p, strnlen(p, s.size - off));
  };
  int text = -1;
  for (uint16_t i = 0; i < shnum; i++) {
    if (sh[i].type == 9 || sh[i].type == 4) {  // REL, RELA
      *err = path + ": has relocations (" + str(shstrndx, sh[i].name) + ")";
      return false;
    }
    if (sh[i].type != 1) continue;  // PROGBITS
    std::string name = str(shstrndx, sh[i].name);
    if (name == ".text") text = i;
    obj->sections[name].assign(f.begin() + sh[i].offset, f.begin() + sh[i].offset + sh[i].size);
  }
  for (uint16_t i = 0; i < shnum; i++) {
    if (sh[i].type != 2 || sh[i].link >= shnum) continue;  // SYMTAB
    for (uint32_t o = sh[i].offset + 16; o + 16 <= sh[i].offset + sh[i].size; o += 16)
      if (text >= 0 && u16(o + 14) == text) obj->text_symbols[u32(o + 4)] = str(sh[i].link, u32(o));
  }
  if (text < 0) {
    *err = path + ": no .text";
    return false;
  }
  return true;
}

// The section's dwords.
inline std::vector<uint32_t> Words(const Object& obj, const std::string& name) {
  std::vector<uint32_t> w;
  auto it = obj.sections.find(name);
  if (it == obj.sections.end()) return w;
  for (size_t o = 0; o + 4 <= it->second.size(); o += 4) {
    uint32_t v;
    std::memcpy(&v, &it->second[o], 4);
    w.push_back(v);
  }
  return w;
}

}  // namespace fixture_elf
