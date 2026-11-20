// lift_discovery: recomp_lift's pass 1 (functions and switch tables it
// finds on its own) on fixture/discovery.S. Runs the lifter
// (recomp_lift_fixture, which reads the unlinked fixture object) once
// without and once with --sweep-gaps, each with --dump-discovery, and
// checks the tables and entries it found against the fixture's
// .lift_expect records:
//   kind | mode, address, n, n targets
// kinds: 1 the jmp's table holds exactly the targets, 2 a function entry,
// 3 not a function entry, 4 the jmp has no table; mode 0x100: only with
// --sweep-gaps, 0x200: only without.
//
//   lift_discovery_test <recomp_lift_fixture> <discovery.o> <work dir>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "fixture_elf.h"

namespace {

fixture_elf::Object obj;
uint32_t base = 0;

std::string Name(uint32_t va) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "%08" PRIX32, va);
  auto it = obj.text_symbols.find(va - base);
  return it == obj.text_symbols.end() ? buf : it->second + " (" + buf + ")";
}

std::string Names(const std::set<uint32_t>& s) {
  std::string out;
  for (uint32_t v : s) out += " " + Name(v);
  return out.empty() ? " (none)" : out;
}

std::string Quote(const std::string& s) { return "'" + s + "'"; }

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: %s <recomp_lift_fixture> <discovery.o> <work dir>\n", argv[0]);
    return 2;
  }
  std::string err;
  if (!fixture_elf::Read(argv[2], &obj, &err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 2;
  }
  base = fixture_elf::Words(obj, ".lift_base").at(0);
  const auto expect = fixture_elf::Words(obj, ".lift_expect");
  int failures = 0;
  for (bool sweep : {false, true}) {
    const std::string mode = sweep ? "with --sweep-gaps" : "without --sweep-gaps";
    const auto dir = std::filesystem::path(argv[3]) / (sweep ? "sweep" : "nosweep");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::string cmd = Quote(argv[1]) + " --exe " + Quote(argv[2]) + " --symbols '' --manifest '' --out " +
                      Quote(dir.string()) + " --dump-discovery" + (sweep ? " --sweep-gaps" : "") +
                      " > " + Quote((dir / "log.txt").string()) + " 2>&1";
    if (std::system(cmd.c_str()) != 0) {
      std::printf("FAIL %s: the lifter failed (%s)\n", mode.c_str(), (dir / "log.txt").c_str());
      failures++;
      continue;
    }
    std::map<uint32_t, std::set<uint32_t>> tables;
    std::set<uint32_t> entries;
    {
      std::ifstream in(dir / "final_jumptables.txt");
      for (std::string line; std::getline(in, line);) {
        std::istringstream l(line);
        std::string jmp, t;
        if (!(l >> jmp)) continue;
        auto& ts = tables[std::stoul(jmp, nullptr, 16)];
        while (l >> t) ts.insert(std::stoul(t, nullptr, 16));
      }
      std::ifstream en(dir / "final_entries.txt");
      for (std::string line; std::getline(en, line);)
        if (!line.empty()) entries.insert(std::stoul(line, nullptr, 16));
    }
    for (size_t i = 0; i + 3 <= expect.size();) {
      const uint32_t kind = expect[i] & 0xff, only = expect[i] & 0x300, va = expect[i + 1],
                     n = expect[i + 2];
      std::set<uint32_t> want(expect.begin() + i + 3, expect.begin() + i + 3 + n);
      i += 3 + n;
      if ((only == 0x100 && !sweep) || (only == 0x200 && sweep)) continue;
      auto t = tables.find(va);
      std::string what;
      switch (kind) {
        case 1:
          if (t == tables.end())
            what = "no table for " + Name(va) + ", expected" + Names(want);
          else if (t->second != want)
            what = "table of " + Name(va) + ":" + Names(t->second) + "\n    expected" + Names(want);
          break;
        case 2:
          if (!entries.count(va)) what = Name(va) + " is not a function entry";
          break;
        case 3:
          if (entries.count(va)) what = Name(va) + " is a function entry";
          break;
        case 4:
          if (t != tables.end()) what = Name(va) + " has a table:" + Names(t->second);
          break;
        default:
          std::fprintf(stderr, "bad .lift_expect kind %u\n", kind);
          return 2;
      }
      if (!what.empty()) {
        std::printf("FAIL %s: %s\n", mode.c_str(), what.c_str());
        failures++;
      }
    }
  }
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("lift_discovery: all expectations hold\n");
  return 0;
}
