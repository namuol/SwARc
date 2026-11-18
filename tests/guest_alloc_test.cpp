// guest_alloc_test: GuestAllocator reservations and decommits and
// GuestHeap sizes at the edges. Checks that:
//   - top-down reservations smaller than 64 KiB, one after another, each
//     get an aligned base of their own below the last;
//   - a decommit running past its reservation fails and leaves the next
//     one committed; size 0 decommits a whole reservation from its base
//     and fails anywhere else;
//   - heap sizes within 15 of 2^64 fail instead of rounding to 0;
//   - the heap hands out the addresses of its earlier implementation (best
//     fit in a size-keyed multimap, ties to the block freed first; RefHeap
//     below) through random allocations, frees and reallocations: the guest
//     sees them. Taking the newest block of a size instead, or not merging
//     with the preceding free block, makes it fail.

#include <cstdio>
#include <map>
#include <random>
#include <vector>

#include "runtime/guest_alloc.h"
#include "runtime/guest_memory.h"

using namespace recomp;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                   #cond);                                                \
      g_failures++;                                                       \
    }                                                                     \
  } while (0)

// The heap's policy before its free lists by size: ordered maps, best fit
// by lower_bound in a multimap (equal sizes in insertion order). One
// segment [base, base + size), no growth.
class RefHeap {
 public:
  RefHeap(uint64_t base, uint64_t size) : base_(base) { InsertFree(base, size); }
  uint64_t Alloc(uint64_t size) {
    uint64_t req = size;
    size = (std::max<uint64_t>(size, 1) + 15) & ~uint64_t(15);
    auto it = by_size_.lower_bound(size);
    if (it == by_size_.end()) return 0;
    uint64_t addr = it->second, have = it->first;
    EraseFree(addr);
    if (have - size >= 16) InsertFree(addr + size, have - size);
    else size = have;
    used_[addr] = {size, req};
    return addr;
  }
  bool Free(uint64_t p) {
    auto it = used_.find(p);
    if (it == used_.end()) return false;
    uint64_t addr = p, size = it->second.first;
    used_.erase(it);
    auto nx = free_.find(addr + size);
    if (nx != free_.end() && nx->first != base_) {
      uint64_t s = nx->second;
      EraseFree(nx->first);
      size += s;
    }
    auto pv = free_.lower_bound(addr);
    if (pv != free_.begin()) {
      --pv;
      if (pv->first + pv->second == addr && addr != base_) {
        uint64_t a = pv->first, s = pv->second;
        EraseFree(a);
        addr = a;
        size += s;
      }
    }
    InsertFree(addr, size);
    return true;
  }
  uint64_t ReAlloc(uint64_t p, uint64_t size, bool in_place_only) {
    auto it = used_.find(p);
    if (it == used_.end()) return 0;
    uint64_t old_size = it->second.first;
    uint64_t want = (std::max<uint64_t>(size, 1) + 15) & ~uint64_t(15);
    if (want <= old_size) {
      it->second.second = size;
      return p;
    }
    auto nx = free_.find(p + old_size);
    if (nx != free_.end() && nx->first != base_ && old_size + nx->second >= want) {
      uint64_t total = old_size + nx->second;
      EraseFree(nx->first);
      if (total - want >= 16) {
        InsertFree(p + want, total - want);
        total = want;
      }
      it->second = {total, size};
      return p;
    }
    if (in_place_only) return 0;
    uint64_t n = Alloc(size);
    if (!n) return 0;
    Free(p);
    return n;
  }
  uint64_t Size(uint64_t p) const {
    auto it = used_.find(p);
    return it == used_.end() ? UINT64_MAX : it->second.second;
  }

 private:
  void InsertFree(uint64_t addr, uint64_t size) {
    free_[addr] = size;
    by_size_.emplace(size, addr);
  }
  void EraseFree(uint64_t addr) {
    auto it = free_.find(addr);
    auto range = by_size_.equal_range(it->second);
    for (auto r = range.first; r != range.second; ++r)
      if (r->second == addr) {
        by_size_.erase(r);
        break;
      }
    free_.erase(it);
  }
  uint64_t base_;
  std::map<uint64_t, std::pair<uint64_t, uint64_t>> used_;  // addr -> size, requested
  std::map<uint64_t, uint64_t> free_;
  std::multimap<uint64_t, uint64_t> by_size_;
};

}  // namespace

int main() {
  GuestMemory mem;
  if (!mem.Reserve()) return 1;
  constexpr uint64_t kG = GuestAllocator::kGranularity;
  constexpr uint64_t kLo = 0x10000, kHi = 0x80000000u;

  {
    GuestAllocator vm;
    vm.Init(&mem, kLo, kHi);
    uint64_t last = kHi;
    for (int i = 0; i < 4; i++) {
      const uint64_t a = vm.Reserve(0, 0x1000, true);
      CHECK(a && a % kG == 0 && a < last);
      last = a;
    }
    CHECK(last == kHi - 4 * kG);
    // A gap above an unaligned end: the base is aligned above that end.
    CHECK(vm.ReserveAt(kHi - 6 * kG + 0x3000, 0x1000));
    const uint64_t b = vm.Reserve(0, 0x1000, true);
    CHECK(b == kHi - 5 * kG);
    const uint64_t c = vm.Reserve(0, 0x1000, true);
    CHECK(c == kHi - 6 * kG);  // below it in the same 64 KiB
    // Filling down to lo_, then nothing is left.
    GuestAllocator small;
    small.Init(&mem, kLo, kLo + 2 * kG);
    CHECK(small.Reserve(0, 0x1000, true) == kLo + kG);
    CHECK(small.Reserve(0, 0x1000, true) == kLo);
    CHECK(small.Reserve(0, 0x1000, true) == 0);
  }

  {
    GuestAllocator vm;
    vm.Init(&mem, kLo, kHi);
    const uint64_t a = vm.Reserve(0, kG), b = vm.Reserve(0, kG);
    CHECK(b == a + kG);
    CHECK(vm.Commit(a, kG, Prot::RW) && vm.Commit(b, kG, Prot::RW));
    CHECK(!vm.Decommit(a, 2 * kG));
    CHECK(mem.IsCommitted(a) && mem.IsCommitted(b));
    CHECK(!vm.Decommit(a + 0x1000, 0));
    CHECK(mem.IsCommitted(a + 0x1000));
    CHECK(vm.Decommit(a + 0x1000, 0x1000));
    CHECK(mem.IsCommitted(a) && !mem.IsCommitted(a + 0x1000) && mem.IsCommitted(a + 0x2000));
    CHECK(vm.Decommit(a, 0));
    CHECK(!mem.IsCommitted(a) && !mem.IsCommitted(a + kG - 0x1000) && mem.IsCommitted(b));
    CHECK(!vm.Commit(b, UINT64_MAX, Prot::RW));
    CHECK(vm.Release(a) && vm.Release(b));
  }

  {
    GuestAllocator vm;
    vm.Init(&mem, kLo, kHi);
    GuestHeap heap(vm, 0, 0);
    for (uint64_t d = 0; d < 16; d++) CHECK(heap.Alloc(UINT64_MAX - d, false) == 0);
    const uint64_t p = heap.Alloc(32, false);
    CHECK(p != 0);
    CHECK(heap.ReAlloc(p, UINT64_MAX - 3, false, false) == 0);
    CHECK(heap.Size(p) == 32);
    const uint64_t q = heap.Alloc(32, false);
    CHECK(q != 0 && q != p);
    CHECK(heap.Free(p) && heap.Free(q));
  }

  // The same addresses as RefHeap, step by step: sizes mostly small (the
  // free lists by size), some past their limit (the multimap), frees and
  // reallocations of random live blocks, until the 16 MiB heap is full now
  // and then.
  for (uint32_t seed = 1; seed <= 4; seed++) {
    GuestAllocator vm;
    vm.Init(&mem, kLo, kHi);
    constexpr uint64_t kHeap = 16u << 20;  // the smallest segment (kDefaultSegment)
    GuestHeap heap(vm, 0, kHeap);
    RefHeap ref(heap.handle(), kHeap);
    std::mt19937_64 rng(seed);
    std::vector<uint64_t> live;
    auto size = [&]() -> uint64_t {
      const uint32_t r = uint32_t(rng() % 100);
      if (r < 70) return 1 + rng() % 256;
      if (r < 95) return 1 + rng() % 8192;
      return 1 + rng() % (512u << 10);
    };
    int mismatches = 0;
    for (int step = 0; step < 200000 && mismatches < 5; step++) {
      const uint32_t op = uint32_t(rng() % 10);
      if (op < 5 || live.empty()) {
        const uint64_t n = size();
        const uint64_t a = heap.Alloc(n, false), b = ref.Alloc(n);
        if (a != b) mismatches++;
        if (a) live.push_back(a);
      } else if (op < 9) {
        const size_t i = size_t(rng() % live.size());
        const bool fa = heap.Free(live[i]), fb = ref.Free(live[i]);
        if (fa != fb) mismatches++;
        live[i] = live.back();
        live.pop_back();
      } else {
        const size_t i = size_t(rng() % live.size());
        const uint64_t n = size();
        const bool in_place = rng() % 4 == 0;
        const uint64_t a = heap.ReAlloc(live[i], n, false, in_place);
        const uint64_t b = ref.ReAlloc(live[i], n, in_place);
        if (a != b || (a && heap.Size(a) != ref.Size(b))) mismatches++;
        if (a) live[i] = a;
      }
    }
    CHECK(mismatches == 0);
  }

  if (g_failures) return 1;
  std::printf("guest allocator: ok\n");
  return 0;
}
