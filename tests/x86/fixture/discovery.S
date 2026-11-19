// Code for lift_discovery (lift_discovery_test.cpp): switches whose
// tables recomp_lift's discovery must recover, and .text code nothing
// refers to for --sweep-gaps. Assembled by clang for i686 and not linked:
// every address is BASE plus a difference of two labels, which the
// assembler resolves itself, so the object has no relocations and its
// .text is the image, mapped at BASE by fixture_reader.cpp.
//
// Sections besides .text stand in for what a game's symbols say and hold
// what the test expects:
//   .lift_base     BASE
//   .lift_entries  function entries, as the symbols would list them
//   .lift_tables   switch tables, as the symbols would list them:
//                  jmp, n, n targets
//   .lift_expect   kind | mode, address, n, n targets (kinds and modes in
//                  lift_discovery_test.cpp)

#define BASE 0x401000
#define A(l) (BASE + (l) - text_start)
#define PAD .p2align 4, 0xcc

#define TABLE 1      // the jmp's table holds exactly these targets
#define ENTRY 2      // a function entry
#define NO_ENTRY 3   // not a function entry
#define NO_TABLE 4   // the jmp has no table
#define SWEEP 0x100      // only with --sweep-gaps
#define NO_SWEEP 0x200   // only without it

  .text
text_start:

// A two-level switch whose byte index is loaded with mov into the low
// byte of a register zeroed after the ja. Its tables lie before the
// function, where the loose read (forward from the jmp) does not look:
// only the exact pattern recovers it.
  .p2align 2, 0xcc
xa_tbl: .long A(xa_c0), A(xa_c1), A(xa_c2), A(xa_def)
xa_idx: .byte 0, 1, 1, 2, 0, 3, 2
  PAD
xor_after:
  movl 4(%esp), %eax
  cmpl $6, %eax
  ja xa_def
  xorl %ecx, %ecx
  movb A(xa_idx)(%eax), %cl
xa_jmp:
  jmp *A(xa_tbl)(,%ecx,4)
xa_c0: movl $1, %eax
  ret
xa_c1: movl $2, %eax
  ret
xa_c2: movl $3, %eax
  ret
xa_def: xorl %eax, %eax
  ret

// The same with the register zeroed before the cmp.
  .p2align 2, 0xcc
xb_tbl: .long A(xb_c0), A(xb_c1), A(xb_def)
xb_idx: .byte 1, 0, 0, 2, 1
  PAD
xor_before:
  movl 4(%esp), %eax
  xorl %edx, %edx
  cmpl $4, %eax
  ja xb_def
  movb A(xb_idx)(%eax), %dl
xb_jmp:
  jmp *A(xb_tbl)(,%edx,4)
xb_c0: movl $1, %eax
  ret
xb_c1: movl $2, %eax
  ret
xb_def: xorl %eax, %eax
  ret

// Without the xor the register's upper bits are unknown: no table.
  .p2align 2, 0xcc
nx_tbl: .long A(nx_c0), A(nx_def)
nx_idx: .byte 0, 1, 0
  PAD
no_xor:
  movl 4(%esp), %eax
  movl 8(%esp), %ecx
  cmpl $2, %eax
  ja nx_def
  movb A(nx_idx)(%eax), %cl
nx_jmp:
  jmp *A(nx_tbl)(,%ecx,4)
nx_c0: movl $1, %eax
  ret
nx_def: xorl %eax, %eax
  ret

// Loose read: the index is spilled and reloaded into another register,
// and compared with a register holding the bound (mov ebp, 10: 11 cases).
// The dwords after the table point into the code too but are no part of
// it: the bound stops the read there.
  PAD
reg_bound:
  pushl %ebp
  subl $4, %esp
  movl $10, %ebp
  movl 12(%esp), %eax
  movl %eax, (%esp)
  cmpl %ebp, %eax
  ja rb_def
  movl (%esp), %ecx
rb_jmp:
  jmp *A(rb_tbl)(,%ecx,4)
rb_c0: movl $0, %eax
  jmp rb_out
rb_c1: movl $1, %eax
  jmp rb_out
rb_c2: movl $2, %eax
  jmp rb_out
rb_c3: movl $3, %eax
  jmp rb_out
rb_c4: movl $4, %eax
  jmp rb_out
rb_c5: movl $5, %eax
  jmp rb_out
rb_c6: movl $6, %eax
  jmp rb_out
rb_c7: movl $7, %eax
  jmp rb_out
rb_c8: movl $8, %eax
  jmp rb_out
rb_c9: movl $9, %eax
  jmp rb_out
rb_c10: movl $10, %eax
  jmp rb_out
rb_def: movl $-1, %eax
rb_out: addl $4, %esp
  popl %ebp
  ret
  .p2align 2, 0xcc
rb_tbl: .long A(rb_c0), A(rb_c1), A(rb_c2), A(rb_c3), A(rb_c4), A(rb_c5)
  .long A(rb_c6), A(rb_c7), A(rb_c8), A(rb_c9), A(rb_c10)
  .long A(reg_bound), A(rb_out), A(rb_out), A(reg_bound), A(reg_bound)

// Loose read with no bound in sight (the caller checks it), and the table
// of a second switch, reached only from one of the first one's cases,
// laid right after the first table: the read stops where it starts once
// the case holding the second switch is lifted.
  PAD
adjacent:
  movl 4(%esp), %eax
  movl %eax, %edx
ad_jmp1:
  jmp *A(ad_tbl1)(,%eax,4)
ad_c0: movl $0, %eax
  ret
ad_c1:
  movl 8(%esp), %eax
  cmpl $2, %eax
  ja ad_def
ad_jmp2:
  jmp *A(ad_tbl2)(,%eax,4)
ad_d0: movl $10, %eax
  ret
ad_d1: movl $11, %eax
  ret
ad_d2: movl $12, %eax
  ret
ad_def: movl $-1, %eax
  ret
ad_c2: movl $2, %eax
  ret
ad_c3: movl $3, %eax
  ret
  .p2align 2, 0xcc
ad_tbl1: .long A(ad_c0), A(ad_c1), A(ad_c2), A(ad_c3)
ad_tbl2: .long A(ad_d0), A(ad_d1), A(ad_d2)

// Loose read followed by a byte table the function loads from, whose
// bytes happen to read as a pointer into the code.
  PAD
byte_bound:
  movl 4(%esp), %eax
  movzbl A(bb_bytes)(%eax), %edx
  movl 8(%esp), %ecx
bb_jmp:
  jmp *A(bb_tbl)(,%ecx,4)
bb_c0: movl %edx, %eax
  ret
bb_c1: movl $1, %eax
  ret
bb_c2: movl $2, %eax
  ret
  .p2align 2, 0xcc
bb_tbl: .long A(bb_c0), A(bb_c1), A(bb_c2)
bb_bytes: .long A(byte_bound)

// Loose read of an and-masked index: exactly 4 cases, though the dword
// after the table points into the code too.
  PAD
masked:
  movl 4(%esp), %ecx
  andl $3, %ecx
mk_jmp:
  jmp *A(mk_tbl)(,%ecx,4)
mk_c0: movl $0, %eax
  ret
mk_c1: movl $1, %eax
  ret
mk_c2: movl $2, %eax
  ret
mk_c3: movl $3, %eax
  ret
  .p2align 2, 0xcc
mk_tbl: .long A(mk_c0), A(mk_c1), A(mk_c2), A(mk_c3)
  .long A(masked)

// Loose read (the index reloaded after the cmp) of a table whose cases
// lie after it.
  PAD
cases_after:
  movl 4(%esp), %eax
  cmpl $2, %eax
  ja ca_def
  movl 4(%esp), %ecx
ca_jmp:
  jmp *A(ca_tbl)(,%ecx,4)
  .p2align 2, 0xcc
ca_tbl: .long A(ca_c0), A(ca_c1), A(ca_c2)
ca_c0: movl $0, %eax
  ret
ca_c1: movl $1, %eax
  ret
ca_c2: movl $2, %eax
  ret
ca_def: movl $-1, %eax
  ret

// A table indexed 1..3 right after its jmp, addressed 4 bytes before its
// first entry: entry 0, never selected, is the jmp's own last bytes (they
// read as a pointer to the jmp's middle) and is skipped.
  PAD
skip0:
  movl 4(%esp), %eax
  cmpl $3, %eax
  ja sk_def
  movl 4(%esp), %ecx
sk_jmp:
  jmp *A(sk_tbl - 4)(,%ecx,4)
sk_tbl: .long A(sk_c1), A(sk_c2), A(sk_c3)
sk_c1: movl $1, %eax
  ret
sk_c2: movl $2, %eax
  ret
sk_c3: movl $3, %eax
  ret
sk_def: movl $-1, %eax
  ret

// A negated index (a backward copy's count) reads the slots before the
// table; the bytes at and after the table are the cases' code.
  PAD
negated:
  movl 4(%esp), %ecx
  negl %ecx
ng_jmp:
  jmp *A(ng_tbl)(,%ecx,4)
  .p2align 2, 0xcc
  .long A(ng_c3), A(ng_c2), A(ng_c1)
ng_tbl:
ng_c1: movl $1, %eax
  ret
ng_c2: movl $2, %eax
  ret
ng_c3: movl $3, %eax
  ret

// An index counted down past zero (sub 4, then jae away: the last 1..3
// bytes of a copy) reaches the 4 slots before its table, where the code
// after the jmp continues; those slots are another switch's table. Its
// cases, reached through the second jmp, are function entries too.
  PAD
sub_prev:
  movl 4(%esp), %eax
  cmpl $3, %eax
  ja sp_def
sp_jmp:
  jmp *A(sp_tbl)(,%eax,4)
sp_c0: movl $0, %eax
  ret
sp_c1: movl $1, %eax
  ret
sp_c2: movl $2, %eax
  ret
sp_c3: movl $3, %eax
  ret
sp_def: movl $-1, %eax
  ret
  PAD
sub_down:
  movl 4(%esp), %ecx
  subl $4, %ecx
  jae sd_def
sd_jmp:
  jmp *A(sd_tbl)(,%ecx,4)
  .p2align 2, 0xcc
sp_tbl: .long A(sp_c0), A(sp_c1), A(sp_c2), A(sp_c3)
sd_tbl:
sd_def: movl $-1, %eax
  ret

// A switch the symbols list short (4 of its 5 cases, and the default): the
// lifter adds the case they miss and keeps all they list.
  PAD
extend:
  movl 4(%esp), %eax
  cmpl $4, %eax
  ja ex_def
ex_jmp:
  jmp *A(ex_tbl)(,%eax,4)
ex_c0: movl $0, %eax
  ret
ex_c1: movl $1, %eax
  ret
ex_c2: movl $2, %eax
  ret
ex_c3: movl $3, %eax
  ret
ex_c4: movl $4, %eax
  ret
ex_def: movl $-1, %eax
  ret
  .p2align 2, 0xcc
ex_tbl: .long A(ex_c0), A(ex_c1), A(ex_c2), A(ex_c3), A(ex_c4)

// Code nothing refers to: only --sweep-gaps lifts it.
  PAD
unreferenced:
  movl 4(%esp), %eax
  addl $1, %eax
  ret

// Data in .text (port I/O, which no compiled user code has) running
// straight into code nothing refers to: the data is dropped, the code
// after it lifted.
  PAD
blob: .byte 0xec   // in (%dx), %al
blob2: .byte 0xee  // out %al, (%dx)
after_blob:
  movl $7, %eax
  ret
  PAD
text_end:

  .section .lift_base, "", @progbits
  .long BASE

  .section .lift_entries, "", @progbits
  .long A(xor_after), A(xor_before), A(no_xor), A(reg_bound), A(adjacent)
  .long A(byte_bound), A(extend), A(masked), A(cases_after), A(skip0)
  .long A(negated), A(sub_prev), A(sub_down)

  .section .lift_tables, "", @progbits
  .long A(ex_jmp), 5, A(ex_c0), A(ex_c1), A(ex_c2), A(ex_c3), A(ex_def)

  .section .lift_expect, "", @progbits
  .long TABLE, A(xa_jmp), 4, A(xa_c0), A(xa_c1), A(xa_c2), A(xa_def)
  .long TABLE, A(xb_jmp), 3, A(xb_c0), A(xb_c1), A(xb_def)
  .long NO_TABLE, A(nx_jmp), 0
  .long TABLE, A(rb_jmp), 11, A(rb_c0), A(rb_c1), A(rb_c2), A(rb_c3), A(rb_c4)
  .long A(rb_c5), A(rb_c6), A(rb_c7), A(rb_c8), A(rb_c9), A(rb_c10)
  .long TABLE, A(ad_jmp1), 4, A(ad_c0), A(ad_c1), A(ad_c2), A(ad_c3)
  .long TABLE, A(ad_jmp2), 3, A(ad_d0), A(ad_d1), A(ad_d2)
  .long TABLE, A(bb_jmp), 3, A(bb_c0), A(bb_c1), A(bb_c2)
  .long TABLE, A(mk_jmp), 4, A(mk_c0), A(mk_c1), A(mk_c2), A(mk_c3)
  .long TABLE, A(ca_jmp), 3, A(ca_c0), A(ca_c1), A(ca_c2)
  .long TABLE, A(sk_jmp), 3, A(sk_c1), A(sk_c2), A(sk_c3)
  .long TABLE, A(ng_jmp), 3, A(ng_c1), A(ng_c2), A(ng_c3)
  .long TABLE, A(sp_jmp), 4, A(sp_c0), A(sp_c1), A(sp_c2), A(sp_c3)
  .long TABLE, A(sd_jmp), 4, A(sp_c0), A(sp_c1), A(sp_c2), A(sp_c3)
  .long ENTRY, A(sp_c2), 0
  .long TABLE, A(ex_jmp), 6, A(ex_c0), A(ex_c1), A(ex_c2), A(ex_c3), A(ex_c4), A(ex_def)
  .long NO_ENTRY | NO_SWEEP, A(unreferenced), 0
  .long ENTRY | SWEEP, A(unreferenced), 0
  .long NO_ENTRY | NO_SWEEP, A(after_blob), 0
  .long ENTRY | SWEEP, A(after_blob), 0
  .long NO_ENTRY, A(blob), 0
  .long NO_ENTRY, A(blob2), 0
