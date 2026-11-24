#pragma once
// Turns one discovered Function into C++ source. The output uses only the
// helpers in src/x86/lifted.h and calls other lifted functions by
// name (f_XXXXXXXX), so files can be compiled independently.

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "function.h"

namespace lifter {

struct EmitStats {
  uint64_t insns = 0;
  std::map<std::string, uint64_t> unimplemented;  // mnemonic -> count
};

std::string FnName(uint64_t va);

class Emitter {
 public:
  Emitter(const Program& p, const Decoder& dec) : p_(p), dec_(dec) {}
  // --light-call-flags (32-bit): which calls and returns may carry only the
  // system flags (DF, AC, ID, NT) in Ctx::eflags, from FlagsAcrossCalls
  // over the whole program.
  struct LightFlags {
    bool on = false;
    std::set<uint64_t> entry_readers;  // calls into these carry the flags
    std::set<uint64_t> full_returns;   // these return the flags
    bool indirect_calls = false;       // indirect calls may be light
    bool returns = false;              // returns may be light (but full_returns)
  };
  void set_light_flags(const LightFlags* l) { light_ = l; }

  // Appends the function definition to *out. Every function it references
  // (calls, tail calls) is added to *refs for forward declarations.
  // `entries` (addresses in f's body, may be null) are secondary entries:
  // each gets an f_X of its own that enters f there, through
  // Ctx::entry_label, instead of a copy of the code from that label.
  void EmitFunction(const Function& f, std::string* out,
                    std::set<uint64_t>* refs, EmitStats* stats,
                    const std::vector<uint64_t>* entries = nullptr);

 private:
  std::map<uint64_t, uint64_t> FindX87Runs(const Function& f,
                                           const std::set<uint64_t>& labels) const;
  std::set<uint64_t> FindPollLoops(const Function& f) const;
  void EmitX87Run(const Function& f, std::map<uint64_t, Insn>::const_iterator first,
                  std::map<uint64_t, Insn>::const_iterator last, std::string* out,
                  std::set<uint64_t>* refs, EmitStats* stats) const;
  const Program& p_;
  const Decoder& dec_;
  const LightFlags* light_ = nullptr;
};

}  // namespace lifter
