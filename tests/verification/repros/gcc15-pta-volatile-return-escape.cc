// SPDX-License-Identifier: Apache-2.0
// UB-free reduction of the GCC 15.2 OpenPtex miscompile (no Ptex code).
// get(0) returns a freshly constructed C; another path returns a pointer read
// from a volatile object. GCC 15 points-to analysis gives that volatile read
// ANYTHING, sets ESCAPED_RETURN = { ANYTHING }, and then fails to treat the
// heap object as escaping through the return, so DSE/DCE delete the
// constructor stores (vptr, v). The call b->f() then reads garbage.
//   g++-15 -O1|-O2|-O3 gcc15-pta-volatile-return-escape.cc && ./a.out  -> SIGSEGV
//   g++-15 -O2 -fno-tree-pta, g++-14 -O2, clang++-21 -O2           -> exit 0
// The only sanitizer report is the intentional 16-byte leak.
struct B { virtual int f() = 0; virtual ~B() {} };
struct C : B { int v; C(int x) : v(x) {} int f() { return v; } };
struct E { B* volatile val; };
E table[4];
__attribute__((noinline)) B* get(int k) {
  if (k == 0) return new C(42);
  return table[k & 3].val;
}
int main() { B* b = get(0); return b->f() == 42 ? 0 : 1; }
