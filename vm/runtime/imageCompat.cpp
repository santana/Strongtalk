/*
Copyright (c) 2026, Gerardo Santana Gomez Garrido.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

//
// Compatibility shim for the frozen 32-bit bootstrap image.
//
// `strongtalk.bst` was generated when `long` was 32 bits, so
// `Alien>>ensureLoaded:` allocates an alien sized for a 32-bit
// `unsigned long`:
//
//     (Alien new: 4) unsignedLongAt: 1 put: (self primLoadLibrary: libraryName)
//
// On an LP64 VM `primLoadLibrary:` returns a 64-bit `dlopen` handle, and the
// 8-byte store is correctly rejected by `alien_index_in_bounds()`
// (`vm/prims/byteArray_prims.cpp`).  The write therefore never happens,
// `libobjc` never loads, and the image's `Error>>defaultAction`
// (`StrongtalkSource/Error.dlt`) re-raises until the scheduler's soft stack
// limit trips -- reported as `Fatal: Stack overflow in scheduler`, which is a
// symptom of the failed library load and not the cause.
//
// `StrongtalkSource/Alien.dlt` in this tree is already correct
// (`(Alien new: 8)`), so this is purely a stale-image problem.  The image
// cannot be regenerated in this tree (see IMAGE_REGEN_PLAN.md), so the operand
// is rewritten in place once, immediately after the image is read.
//
// TODO: delete this file, and its call in `load_image()` (`shell.cpp`), once a
// 64-bit-compatible image exists.

#include "memory/universe.hpp"
#include "memory/oopFactory.hpp"
#include "memory/universe.store.hpp"
#include "oops/klassOop.hpp"
#include "oops/methodOop.hpp"
#include "oops/mixinOop.hpp"
#include "oops/objArrayOop.hpp"
#include "oops/smiOop.hpp"
#include "oops/symbolOop.hpp"
#include "oops/memOop.inline.hpp"
#include "oops/oop.inline.hpp"
#include "interpreter/codeIterator.hpp"
#include "utilities/lprintf.hpp"

#include <cstring>
#include <cstdlib>

#include "runtime/imageCompat.hpp"

namespace {

bool symbol_is(symbolOop s, const char* text) {
  if (s == nilObj || !s->is_symbol())
    return false;
  int n = int(strlen(text));
  if (s->length() != n)
    return false;
  for (int i = 0; i < n; i++)
    if (s->byte_at(i + 1) != (u_char)text[i])
      return false;
  return true;
}

// A klass holds methods in two places: the klass's own `methods` array
// (categorized additions, which shadow the mixin) and its mixin's.  The class
// side is a further mixin reached through `mixin()->class_mixin()`.
// `ensureLoaded:` is a class-side method, hence the last of the three.
methodOop find_in(objArrayOop methods, const char* selector) {
  if (methods == nilObj)
    return NULL;
  for (int i = methods->length(); i >= 1; i--) {
    methodOop mo = methodOop(methods->obj_at(i));
    if (mo != nilObj && symbol_is(mo->selector(), selector))
      return mo;
  }
  return NULL;
}

methodOop find_method(klassOop k, const char* selector) {
  if (k == nilObj || !k->is_klass())
    return NULL;
  methodOop mo = find_in(k->klass_part()->methods(), selector);
  if (mo != NULL)
    return mo;
  mixinOop m = k->klass_part()->mixin();
  if (m == nilObj)
    return NULL;
  mo = find_in(m->methods(), selector);
  if (mo != NULL)
    return mo;
  mixinOop cm = m->class_mixin();
  if (cm == nilObj)
    return NULL;
  return find_in(cm->methods(), selector);
}

// Inline operands are stored at unaligned bytecode offsets, so a word read from
// the bytevector can decode to an arbitrary bit pattern.  Only dereference
// words that really point into the heap.
bool heap_oop(oop w) {
  return w != nilObj && Universe::really_contains(w);
}

// Yield every closure method created by `m`, by locating the oop operands of
// the `push_new_closure*` family and keeping those that are methods.
template <typename F> void for_each_closure(methodOop m, F action) {
  for (CodeIterator it(m, 1); it.hp() < m->codes_end(); it.advance()) {
    Bytecodes::Format f = it.format();
    int opOff = (f == Bytecodes::BO) ? 1 : (f == Bytecodes::BBO ? 2 : -1);
    if (opOff < 0)
      continue;
    oop cand = it.oop_at(opOff);
    if (!heap_oop(cand) || !cand->is_method())
      continue;
    action(methodOop(cand));
  }
}

// Yield every method visible on `k`, including the class-side mixin, so the
// Alien constructors -- which are class-side methods -- are all reached.
template <typename F> void for_each_method(klassOop k, F action) {
  if (k == nilObj || !k->is_klass())
    return;
  if (objArrayOop own = k->klass_part()->methods()) {
    for (int i = own->length(); i >= 1; i--) {
      oop m = own->obj_at(i);
      if (m != nilObj && m->is_method())
        action(methodOop(m));
    }
  }
  mixinOop m = k->klass_part()->mixin();
  if (m == nilObj)
    return;
  if (objArrayOop mm = m->methods()) {
    for (int i = mm->length(); i >= 1; i--) {
      oop me = mm->obj_at(i);
      if (me != nilObj && me->is_method())
        action(methodOop(me));
    }
  }
  mixinOop cm = m->class_mixin();
  if (cm == nilObj)
    return;
  if (objArrayOop cmm = cm->methods()) {
    for (int i = cmm->length(); i >= 1; i--) {
      oop me = cmm->obj_at(i);
      if (me != nilObj && me->is_method())
        action(methodOop(me));
    }
  }
}

// True when `it` sits on a 1-argument send whose selector is `selector`.
bool is_send_of(CodeIterator& it, symbolOop selector) {
  int off = (it.format() == Bytecodes::BOO) ? 1 : -1;
  if (off < 0)
    return false;
  oop sel = it.oop_at(off);
  return heap_oop(sel) && sel->is_symbol() && symbolOop(sel) == selector;
}

// Rewrite a `push_succ_n` operand that feeds a 1-argument `selector` send.
// Both forms of the Alien size computation reach `primitiveNew:` this way:
//
//     (self primitiveNew: 8)          push_succ_n 7;  send primitiveNew:
//     (self primitiveNew: size + 4)   push_succ_n 3;  send +;  send primitiveNew:
//
// so at most one arithmetic send may sit between the literal and the target
// send; a bare literal still has to be the immediately preceding instruction.
// Every opcode involved has a fixed single-byte operand width, so rewriting the
// literal cannot invalidate a branch or failure-block offset.  Returns 1 when
// the operand was rewritten, 0 when it already held `want`.
template <typename A> int rewrite_push_before_send(methodOop m, symbolOop selector, A&& want, CodeIterator& b) {
  if (b.code() != Bytecodes::push_succ_n)
    return 0;
  u_char* site = b.hp();
  CodeIterator after(site);
  if (!after.advance())
    return 0;
  if (is_send_of(after, selector)) {
    // direct: the literal is the size itself
  } else if (after.format() == Bytecodes::BOO) {
    // indirect: `size + n`, i.e. one arithmetic send before the target
    CodeIterator middle(after);
    if (!middle.advance() || !is_send_of(middle, selector))
      return 0;
  } else {
    return 0;
  }
  int oldOperand = b.byte_at(1);
  if (oldOperand == want)
    return 0; // already patched, or natively correct
  site[1] = want;
  return 1;
}

} // namespace

// The frozen image's `MacOSXStatBuffer` was generated for a non-LP64 platform:
// its `struct stat` field offsets describe a 32-bit-ish layout.  On LP64 macOS
// `struct stat` is 144 bytes with `st_blksize` at 112 and `st_size` at 96, so
// `blockSize` reads `st_ctimespec.tv_sec` (a ~1.8e9 timestamp) and the image
// then allocates a multi-gigabyte read buffer.  Rewrite the `^k` accessors,
// which are all encoded as `push_succ_n (k-1); return_tos`, to the LP64 offsets.
int patch_statbuffer_offsets() {
  klassOop k = klassOop(Universe::find_global("MacOSXStatBuffer"));
  if (k == nilObj || !k->is_klass())
    return 0;

  struct {
    const char* selector;
    int old_value;
    int new_value;
  } table[] = {
    {"structureSize", 96, 144}, {"blockSizeOffset", 64, 112}, {"fileSizeOffset", 48, 96},
    {"gidOffset", 16, 20},      {"modeOffset", 8, 4},         {"uidOffset", 12, 16},
  };

  int patched = 0;
  for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
    methodOop mo = find_method(k, table[i].selector);
    if (mo == nilObj || mo == NULL)
      continue;
    u_char* codes = mo->codes();
    if (codes[0] != Bytecodes::push_succ_n) {
      lprintf("[image-compat] StatBuffer>>%s: unexpected opcode 0x%02x\n", table[i].selector, codes[0]);
      continue;
    }
    int current = codes[1] + 1;
    if (current != table[i].old_value) {
      // Already patched (or a value we do not recognise): leave it alone.
      continue;
    }
    codes[1] = (u_char)(table[i].new_value - 1);
    patched++;
  }

  if (patched > 0)
    lprintf("[image-compat] StatBuffer offsets -> LP64 macOS struct stat (%d site(s))\n", patched);
  return patched;
}

int patch_alien_ensure_loaded_size() {
  klassOop alien = klassOop(Universe::find_global("Alien"));
  if (alien == nilObj || !alien->is_klass())
    return 0;
  methodOop mo = find_method(alien, "ensureLoaded:");
  if (mo == nilObj || mo == NULL)
    return 0;

  // `4` is encoded as `push_succ_n 3` (pushes SMI(b+1)); the 64-bit size is
  // `push_succ_n 7`.  See BYTECODES.md section 4.1.
  symbolOop newSel = oopFactory::new_symbol("new:");
  int patched = 0;

  for_each_closure(mo, [&](methodOop blk) {
    CodeIterator b(blk, 1);
    while (b.hp() < blk->codes_end()) {
      patched += rewrite_push_before_send(blk, newSel, 7, b);
      b.advance();
    }
  });

  if (patched > 0)
    lprintf("[image-compat] Alien>>ensureLoaded: alien size 4 -> 8 bytes (%d site(s))\n", patched);
  return patched;
}

// Every Alien instance-creation method allocates its byteArray through
// `self primitiveNew: n`, where `n` is the physical size including the
// VM-private header.  The frozen image predates the LP64 header, so it encodes
//
//     (self primitiveNew: 8)          ; address alien: 4-byte size + 4-byte ptr
//     (self primitiveNew: size + 4)   ; inline alien:  4-byte size + payload
//
// while the LP64 VM header is `2 * oopSize` = 16 bytes.  Rewriting the operand
// makes the stale image allocate the header the C++ accessors now read, so the
// two files cannot disagree about layout.  Operands 7 (SMI 8) and 3 (SMI 4)
// both become 15 (SMI 16).
int patch_alien_allocation_sizes() {
  klassOop alien = klassOop(Universe::find_global("Alien"));
  if (alien == nilObj || !alien->is_klass())
    return 0;

  symbolOop primSel = oopFactory::new_symbol("primitiveNew:");
  int patched = 0;

  for_each_method(alien, [&](methodOop mo) {
    if (mo == nilObj || mo == NULL)
      return;
    CodeIterator b(mo, 1);
    while (b.hp() < mo->codes_end()) {
      patched += rewrite_push_before_send(mo, primSel, 15, b);
      b.advance();
    }
    // Constructors are frequently wrapped in a closure (e.g. `autoFreeAfter:`),
    // so the nested blocks need the same treatment.
    for_each_closure(mo, [&](methodOop blk) {
      CodeIterator cb(blk, 1);
      while (cb.hp() < blk->codes_end()) {
        patched += rewrite_push_before_send(blk, primSel, 15, cb);
        cb.advance();
      }
    });
  });

  if (patched > 0)
    lprintf("[image-compat] Alien header 8 -> 16 bytes: primitiveNew: (%d site(s))\n", patched);
  return patched;
}

namespace {

int g_prim_failure_sites = 0;

bool is_prim_failure_code(Bytecodes::Code c) {
  return c == Bytecodes::prim_call_failure || c == Bytecodes::predict_prim_call_failure ||
         c == Bytecodes::prim_call_self_failure || c == Bytecodes::prim_call_failure_lookup ||
         c == Bytecodes::predict_prim_call_failure_lookup || c == Bytecodes::prim_call_self_failure_lookup;
}

bool is_target_selector(symbolOop sel) {
  static const char* names[] = {
    "primitiveAt:put:", "at:put:", "classify:as:", "initializeClassifications", "vframe", "atAllPut:"};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    if (symbol_is(sel, names[i]))
      return true;
  return false;
}

void hex_bytes(char* out, u_char* p, int n) {
  static const char* H = "0123456789abcdef";
  int k = 0;
  for (int i = 0; i < n; i++) {
    out[k++] = H[(p[i] >> 4) & 0xf];
    out[k++] = H[p[i] & 0xf];
    out[k++] = ' ';
  }
  out[k] = 0;
}

void dump_one_method(methodOop m) {
  bool target = is_target_selector(m->selector());
  for (CodeIterator it(m, 1); it.hp() < m->codes_end(); it.advance()) {
    Bytecodes::Code c = it.code();
    if (!is_prim_failure_code(c))
      continue;
    g_prim_failure_sites++;
    if (!target)
      continue;

    u_char* p = it.hp();
    int total = int(m->codes_end() - p);
    int nBytes = total < 48 ? total : 48;

    // The VM's model (CodeIterator::next_hp, BLL): both operands are oopSize and
    // start at align(p+1).  Report the jump offset and entry point read that way,
    // and also read as 4-byte words, so a 32-bit vs 64-bit layout is visible.
    u_char* ops = it.aligned_oop(1) ? (u_char*)it.aligned_oop(1) : p;
    intptr_t as8_off = *(intptr_t*)(ops + oopSize);
    int32_t as4_off = *(int32_t*)(ops + oopSize);
    int32_t as4_off2 = *(int32_t*)(ops + 4);
    intptr_t as8_entry = *(intptr_t*)(ops);
    int32_t as4_entry = *(int32_t*)(ops);

    char sel[64];
    sel[0] = 0;
    {
      symbolOop s = m->selector();
      int len = s->length();
      if (len > 60)
        len = 60;
      for (int i = 0; i < len; i++)
        sel[i] = (char)s->byte_at(i + 1);
      sel[len] = 0;
    }

    lprintf("[site] %s bci=%d op=0x%02x total=%d off8=%lld off4b=%d\n", sel, it.bci(), (unsigned)c, total,
            (long long)as8_off, (int)as4_off2);
    if (!target)
      continue;

    char buf[200];
    hex_bytes(buf, p, nBytes);
    lprintf("[dump]   bytes: %s\n", buf);
    lprintf("[dump]   ops@%p entry8=0x%llx entry4=0x%x off8=%lld off4=%d off4b=%d\n", (void*)ops,
            (unsigned long long)as8_entry, (unsigned)as4_entry, (long long)as8_off, (int)as4_off, (int)as4_off2);
  }
}

void dump_one_method_thunk(methodOop m) {
  dump_one_method(m);
}

} // namespace

int dump_prim_failure_sites() {
  if (getenv("ST_DUMP_PRIM") == NULL)
    return 0;
  Universe::methods_do(&dump_one_method_thunk);
  lprintf("[image-compat] primitive-with-failure-block sites total = %d\n", g_prim_failure_sites);
  return g_prim_failure_sites;
}
