/* Copyright 1994, 1995 LongView Technologies L.L.C. $Revision: 1.99 $ */
/* Copyright (c) 2006, Sun Microsystems, Inc.
All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
following conditions are met:

    * Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following
	  disclaimer in the documentation and/or other materials provided with the distribution.
    * Neither the name of Sun Microsystems nor the names of its contributors may be used to endorse or promote products derived
	  from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT
NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE


*/

#include "compiler/costModel.hpp"
#include "interpreter/codeIterator.hpp"
#include "interpreter/interpretedIC.hpp"
#include "interpreter/methodIterator.hpp"
#include "interpreter/methodPrinter.hpp"
#include "interpreter/prettyPrinter.hpp"
#include "memory/oopFactory.hpp"
#include "memory/universe.hpp"
#include "memory/universe.store.hpp"
#include "memory/vmSymbols.hpp"
#include "oops/associationOop.hpp"
#include "oops/blockOop.hpp"
#include "oops/klassOop.hpp"
#include "oops/methodOop.hpp"
#include "oops/mixinOop.hpp"
#include "oops/symbolOop.hpp"
#include "prims/dll.hpp"
#include "prims/prim.hpp"
#include "runtime/bootstrap.hpp"
#include "topIncludes/std_includes.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/ostream.hpp"
#include "memory/generation.inline.hpp"
#include "oops/oop.inline.hpp"

void methodOopDesc::decay_invocation_count(double decay_factor) {
  double new_count = (double)invocation_count() / decay_factor;
  set_invocation_count((int)new_count);

  // Take care of the block methods
  CodeIterator c(this);
  do {
    switch (c.code()) {
      case Bytecodes::push_new_closure_tos_0: // fall through
      case Bytecodes::push_new_closure_tos_1: // fall through
      case Bytecodes::push_new_closure_tos_2: // fall through
      case Bytecodes::push_new_closure_context_0: // fall through
      case Bytecodes::push_new_closure_context_1: // fall through
      case Bytecodes::push_new_closure_context_2: {
        methodOop block_method = methodOop(c.oop_at(1));
        assert(block_method->is_method(), "must be method");
        block_method->decay_invocation_count(decay_factor);
      } break;
      case Bytecodes::push_new_closure_tos_n: // fall through
      case Bytecodes::push_new_closure_context_n: {
        methodOop block_method = methodOop(c.oop_at(2));
        assert(block_method->is_method(), "must be method");
        block_method->decay_invocation_count(decay_factor);
      } break;
      default:
        break; // not a block-creating bytecode
    }
  } while (c.advance());
}

void methodOopDesc::inc_sharing_count() {
  if (sharing_count() < _sharing_count_max) {
    set_sharing_count(sharing_count() + 1);
  }
}

void methodOopDesc::dec_sharing_count() {
  if (sharing_count() > 0) {
    set_sharing_count(sharing_count() - 1);
  }
}

// Per-instruction state captured while decoding the compact 32-bit code stream
// of a method.  The 64-bit layout (inline oops 8-byte aligned) is computed
// iteratively: if a byte-form branch distance no longer fits a single byte the
// branch is upgraded to its word form, which grows the instruction, so all
// following positions are recomputed until the layout is stable.
struct bootstrap_instr {
  Bytecodes::Code code; // opcode (possibly upgraded to a word form)
  Bytecodes::Format fmt; // current format of code
  int w32, n32; // 32-bit positions, 1-based
  int w64, n64; // 64-bit positions, 0-based codes offsets
  int nargs;
  u_char args[3]; // non-offset argument bytes
  int noops;
  intptr_t oops[2]; // inline oop slot values (real oops/small ints)
  bool oop_is_offset[2]; // oop slot holds a raw offset to relocate
  u_char trailing;
  bool has_trailing;
  int noff; // number of offsets (0, 1, or 2 for jump_loop)
  int off[2]; // raw 32-bit offset values
  int new_off[2]; // relocated offsets (final layout)
  int off_arg[2]; // byte form: arg index holding each offset (-1)
  int off_slot[2]; // word form: oop slot holding each offset (-1)
  int off_mode[2]; // 0 = next + off; 1 = w - off (while); 2 = jump_loop end
  int bs_start, bs_count; // BBS data
};

// Maps a 32-bit instruction position back to its 64-bit position.
static int bootstrap_w64_of(int w32, const int* rec_w32, const int* rec_w64, int n) {
  for (int i = 0; i < n; i++)
    if (rec_w32[i] == w32)
      return rec_w64[i];
  fatal("bootstrap: branch target not found");
  return 0;
}

// Recomputes the 64-bit distance of the j'th offset of 'it'.  The image stores
// forward branch distances referenced from the next instruction and while-test
// distances referenced from the opcode; the jump_loop end distance is
// referenced from the cond jump destination.
static int bootstrap_instr_offset(const bootstrap_instr* it, int j, const int* rec_w32, const int* rec_w64, int n) {
  if (it->off_mode[j] == 1) {
    int target32 = it->w32 - it->off[j]; // backward from the opcode
    int target64 = bootstrap_w64_of(target32, rec_w32, rec_w64, n);
    return it->w64 - target64;
  }
  if (it->off_mode[j] == 2) { // jump_loop end offset
    int end32 = it->n32 + it->off[0] + it->off[1];
    int end64 = bootstrap_w64_of(end32, rec_w32, rec_w64, n);
    int cond32 = it->n32 + it->off[0];
    int cond64 = bootstrap_w64_of(cond32, rec_w32, rec_w64, n);
    return end64 - cond64; // referenced from the cond destination
  }
  int target32 = it->n32 + it->off[j]; // forward from the next instruction
  int target64 = bootstrap_w64_of(target32, rec_w32, rec_w64, n);
  return target64 - it->n64;
}

// Upgrades a byte-form branch that overflows a single byte to its word form,
// moving the offset from an argument byte into an inline oop slot.
static void bootstrap_upgrade(bootstrap_instr* it) {
  switch (it->code) {
    case Bytecodes::ifTrue_byte:
      it->code = Bytecodes::ifTrue_word;
      it->nargs = 1;
      goto bl1;
    case Bytecodes::ifFalse_byte:
      it->code = Bytecodes::ifFalse_word;
      it->nargs = 1;
      goto bl1;
    case Bytecodes::and_byte:
      it->code = Bytecodes::and_word;
      it->nargs = 0;
      goto bl1;
    case Bytecodes::or_byte:
      it->code = Bytecodes::or_word;
      it->nargs = 0;
      goto bl1;
    case Bytecodes::whileTrue_byte:
      it->code = Bytecodes::whileTrue_word;
      it->nargs = 0;
      goto bl1;
    case Bytecodes::whileFalse_byte:
      it->code = Bytecodes::whileFalse_word;
      it->nargs = 0;
      goto bl1;
    case Bytecodes::jump_else_byte:
      it->code = Bytecodes::jump_else_word;
      it->nargs = 0;
      goto bl1;
    case Bytecodes::jump_loop_byte:
      it->code = Bytecodes::jump_loop_word;
      it->nargs = 0;
      it->noops = 2;
      it->oop_is_offset[0] = true;
      it->oop_is_offset[1] = true;
      it->off_arg[0] = -1;
      it->off_arg[1] = -1;
      it->off_slot[0] = 1;
      it->off_slot[1] = 0; // cond at slot 1, end at slot 0
      goto done;
    default:
      ShouldNotReachHere();
    bl1:
      it->noops = 1;
      it->oop_is_offset[0] = true;
      it->off_arg[0] = -1;
      it->off_slot[0] = 0;
    done:
      it->fmt = Bytecodes::format(it->code);
  }
}

// Computes the 64-bit byte offset of the first inline oop of an instruction
// whose opcode sits at byte offset 'w' within the codes area, along with the
// offset of the next instruction.  These reproduce the positions the
// interpreter computes via skip_words/advance_aligned (inline oops are
// 8-byte aligned and each oop occupies oopSize bytes).
static void oop_instr_offsets(Bytecodes::Format f, int w, int& base, int& next) {
  switch (f) {
    case Bytecodes::BO:
    case Bytecodes::BL:
      base = (w + 16) & -oopSize;
      base -= oopSize;
      next = base + oopSize;
      break;
    case Bytecodes::BBO:
    case Bytecodes::BBL:
      base = (w + 17) & -oopSize;
      base -= oopSize;
      next = base + oopSize;
      break;
    case Bytecodes::BOO:
    case Bytecodes::BLO:
    case Bytecodes::BOL:
    case Bytecodes::BLL:
      base = (w + 24) & -oopSize;
      base -= 2 * oopSize;
      next = base + 2 * oopSize;
      break;
    case Bytecodes::BBOO:
    case Bytecodes::BBLO:
      base = (w + 25) & -oopSize;
      base -= 2 * oopSize;
      next = base + 2 * oopSize;
      break;
    case Bytecodes::BLB:
      base = (w + 1) & -oopSize;
      next = base + oopSize + 1;
      break;
    case Bytecodes::BOOLB:
      base = (w + 32) & -oopSize;
      base -= 3 * oopSize;
      next = base + 3 * oopSize + 1;
      break;
    default:
      ShouldNotReachHere();
  }
}

void methodOopDesc::bootstrap_object(bootstrap* st) {
  memOopDesc::bootstrap_header(st);
  st->read_oop((oop*)&addr()->_debugInfo);
  st->read_oop((oop*)&addr()->_selector_or_method);
  set_counters(0, 0);
  st->read_oop((oop*)&addr()->_size_and_flags);

  // The image stores code in the compact layout used by the 32-bit VM: byte
  // items advance the stream by 1, oop items by 4.  In this VM oops are 8
  // bytes and the interpreter 8-byte aligns inline oops, so each instruction
  // must be expanded in place.  The stream is decoded once into per
  // instruction records; the 64-bit layout is then computed iteratively,
  // upgrading byte-form branches that overflow, and finally the codes area is
  // written with the relocated branch and failure-block offsets.
  int max = size_of_codes() * 4 + 8;
  int* rec_w32 = NEW_C_HEAP_ARRAY(int, max);
  int* rec_w64 = NEW_C_HEAP_ARRAY(int, max);
  bootstrap_instr* instrs = NEW_C_HEAP_ARRAY(bootstrap_instr, max);
  u_char* bs_data = NEW_C_HEAP_ARRAY(u_char, size_of_codes() * 4 + 1);
  int n_instrs = 0, bs_total = 0;

  int index = 1; // 32-bit stream position
  while (index <= size_of_codes() * 4) {
    if (!st->is_byte())
      fatal("expected bytecode");
    Bytecodes::Code code = Bytecodes::Code((u_char)st->read_byte());
    index++;
    int w32 = index - 1; // 32-bit position of the opcode
    Bytecodes::Format f = Bytecodes::format(code);

    bootstrap_instr* it = &instrs[n_instrs++];
    memset(it, 0, sizeof(*it));
    it->code = code;
    it->fmt = f;
    it->w32 = w32;
    it->off_arg[0] = it->off_arg[1] = -1;
    it->off_slot[0] = it->off_slot[1] = -1;

    int args = 0, oops = 0, trailing = 0;
    switch (f) {
      case Bytecodes::B:
        break;
      case Bytecodes::BB:
        args = 1;
        break;
      case Bytecodes::BBB:
        args = 2;
        break;
      case Bytecodes::BBBB:
        args = 3;
        break;
      case Bytecodes::BBS:
        args = 1;
        break;
      case Bytecodes::BBO:
      case Bytecodes::BBL:
        args = 1;
        oops = 1;
        break;
      case Bytecodes::BO:
      case Bytecodes::BL:
        oops = 1;
        break;
      case Bytecodes::BLB:
        oops = 1;
        trailing = 1;
        break;
      case Bytecodes::BOO:
      case Bytecodes::BLO:
      case Bytecodes::BOL:
      case Bytecodes::BLL:
        oops = 2;
        break;
      case Bytecodes::BBOO:
      case Bytecodes::BBLO:
        args = 1;
        oops = 2;
        break;
      case Bytecodes::BOOLB:
        oops = 3;
        trailing = 1;
        break;
      default:
        fatal("undefined bytecode in method");
    }
    it->nargs = args;
    it->noops = oops;
    it->has_trailing = (trailing != 0);

    // Inline word slots that hold raw 32-bit offsets (branch distances,
    // primitive failure-block sizes) rather than oops.  The offsets must be
    // relocated for the 64-bit layout.
    bool slot_is_offset[2] = {false, false};
    switch (code) {
      case Bytecodes::jump_loop_word:
        slot_is_offset[0] = true;
        slot_is_offset[1] = true;
        break;
      case Bytecodes::ifTrue_word:
      case Bytecodes::ifFalse_word:
      case Bytecodes::and_word:
      case Bytecodes::or_word:
      case Bytecodes::whileTrue_word:
      case Bytecodes::whileFalse_word:
      case Bytecodes::jump_else_word:
        slot_is_offset[0] = true;
        break;
      case Bytecodes::prim_call_failure_lookup:
      case Bytecodes::predict_prim_call_failure_lookup:
      case Bytecodes::prim_call_self_failure_lookup:
      case Bytecodes::prim_call_failure:
      case Bytecodes::predict_prim_call_failure:
      case Bytecodes::prim_call_self_failure:
        slot_is_offset[1] = true;
        break;
      default:
        break;
    }
    for (int i = 0; i < oops; i++)
      it->oop_is_offset[i] = slot_is_offset[i];

    // Where each branch offset lives and how it is referenced.
    switch (code) {
      case Bytecodes::ifTrue_byte:
      case Bytecodes::ifFalse_byte:
        it->noff = 1;
        it->off_arg[0] = 1;
        it->off_mode[0] = 0;
        break;
      case Bytecodes::and_byte:
      case Bytecodes::or_byte:
      case Bytecodes::jump_else_byte:
        it->noff = 1;
        it->off_arg[0] = 0;
        it->off_mode[0] = 0;
        break;
      case Bytecodes::whileTrue_byte:
      case Bytecodes::whileFalse_byte:
        it->noff = 1;
        it->off_arg[0] = 0;
        it->off_mode[0] = 1;
        break;
      case Bytecodes::jump_loop_byte:
        it->noff = 2;
        it->off_arg[0] = 1;
        it->off_mode[0] = 0; // cond
        it->off_arg[1] = 0;
        it->off_mode[1] = 2; // end
        break;
      case Bytecodes::ifTrue_word:
      case Bytecodes::ifFalse_word:
      case Bytecodes::and_word:
      case Bytecodes::or_word:
      case Bytecodes::jump_else_word:
        it->noff = 1;
        it->off_slot[0] = 0;
        it->off_mode[0] = 0;
        break;
      case Bytecodes::whileTrue_word:
      case Bytecodes::whileFalse_word:
        it->noff = 1;
        it->off_slot[0] = 0;
        it->off_mode[0] = 1;
        break;
      case Bytecodes::jump_loop_word:
        it->noff = 2;
        it->off_slot[0] = 1;
        it->off_mode[0] = 0; // cond
        it->off_slot[1] = 0;
        it->off_mode[1] = 2; // end
        break;
      case Bytecodes::prim_call_failure_lookup:
      case Bytecodes::predict_prim_call_failure_lookup:
      case Bytecodes::prim_call_self_failure_lookup:
      case Bytecodes::prim_call_failure:
      case Bytecodes::predict_prim_call_failure:
      case Bytecodes::prim_call_self_failure:
        it->noff = 1;
        it->off_slot[0] = 1;
        it->off_mode[0] = 0;
        break;
      default:
        break;
    }

    // argument bytes
    for (int i = 0; i < args; i++) {
      if (!st->is_byte())
        fatal("expected byte");
      it->args[i] = st->read_byte();
      index++;
    }
    for (int j = 0; j < it->noff; j++)
      if (it->off_arg[j] >= 0)
        it->off[j] = it->args[it->off_arg[j]];

    // inline oops / raw inline words
    if (oops) {
      for (int s = 0; s < oops; s++) {
        // skip the 32-bit alignment padding preceding each oop: '4'-marked
        // items whose value is 0xff.  The stream then either holds a raw
        // 32-bit oop word (each byte stored as a '4'-marked item, e.g. instVar
        // indices and control offsets) or a '5'-encoded object.
        while (st->is_byte()) {
          u_char v = (u_char)st->read_byte();
          index++;
          if (v != 0xff) {
            int32_t word = v;
            for (int j = 1; j < 4; j++) {
              if (!st->is_byte())
                fatal("expected byte in raw inline oop word");
              word |= ((int32_t)((u_char)st->read_byte()) << (8 * j));
              index++;
            }
            if (slot_is_offset[s]) {
              int k = (it->off_slot[0] == s) ? 0 : 1;
              it->off[k] = (int)(int32_t)word; // raw 32-bit offset
            } else {
              if ((word & 3) != Int_Tag) {
                fatal("raw inline oop word must be a small integer");
              }
              it->oops[s] = (intptr_t)as_smiOop(word >> 2);
            }
            goto next_oop;
          }
        }
        {
          oop o;
          st->read_oop(&o);
          index += 4;
          if (slot_is_offset[s]) {
            int k = (it->off_slot[0] == s) ? 0 : 1;
            if (o->is_smi())
              it->off[k] = smiOop(o)->value();
            else
              fatal("inline offset is not a small integer");
          } else {
            it->oops[s] = (intptr_t)o;
          }
        }
      next_oop:;
      }
      for (int i = 0; i < trailing; i++) {
        if (!st->is_byte())
          fatal("expected byte");
        it->trailing = st->read_byte();
        it->has_trailing = true;
        index++;
      }
    }

    if (f == Bytecodes::BBS) {
      int count = it->args[0];
      if (count == 0)
        count = 256;
      it->bs_count = count;
      it->bs_start = bs_total;
      for (int i = 0; i < count; i++) {
        if (!st->is_byte())
          fatal("expected byte");
        bs_data[bs_total++] = st->read_byte();
        index++;
      }
    }

    it->n32 = index;
    rec_w32[n_instrs - 1] = w32;
    rec_w64[n_instrs - 1] = 0; // filled during layout
  }

  // Iteratively compute the 64-bit layout.  A byte-form branch whose distance
  // no longer fits a byte is upgraded to its word form, growing the
  // instruction; this loop terminates because each upgrade only grows the
  // layout and only shrinks the set of byte-form branches.
  bool upgraded = true;
  int w64 = 0;
  while (upgraded) {
    upgraded = false;
    w64 = 0;
    for (int i = 0; i < n_instrs; i++) {
      bootstrap_instr* it = &instrs[i];
      it->w64 = w64;
      if (it->noops > 0) {
        int base, next;
        oop_instr_offsets(it->fmt, w64, base, next);
        it->n64 = next;
      } else {
        int extra = 1 + it->nargs + (it->has_trailing ? 1 : 0);
        if (it->fmt == Bytecodes::BBS)
          extra += it->bs_count;
        it->n64 = w64 + extra;
      }
      rec_w64[i] = it->w64;
      w64 = it->n64;
    }
    for (int i = 0; i < n_instrs; i++) {
      bootstrap_instr* it = &instrs[i];
      if (it->noff == 0)
        continue;
      bool overflows = false;
      for (int j = 0; j < it->noff; j++) {
        it->new_off[j] = bootstrap_instr_offset(it, j, rec_w32, rec_w64, n_instrs);
        if (it->off_arg[j] >= 0 && (it->new_off[j] < 0 || it->new_off[j] > 255))
          overflows = true;
      }
      if (overflows) {
        bootstrap_upgrade(it);
        upgraded = true;
      }
    }
  }
  // The 64-bit layout of some methods (mostly those with branch upgrades) no
  // longer fits the codes area sized for the 32-bit image.  Reallocate the
  // method with enough room, copying the header fields and registering the
  // replacement so get_object returns the new oop.
  methodOop m = this;
  if (w64 > size_of_codes() * oopSize) {
    int new_codes = (w64 + oopSize - 1) / oopSize;
    int new_size = methodOopDesc::header_size() + new_codes;
    methodOop nm = as_methodOop(Universe::allocate_tenured(new_size));
    for (int i = 0; i < methodOopDesc::header_size(); i++)
      nm->raw_at_put(i, raw_at(i));
    nm->set_size_and_flags(new_codes, nofArgs(), flags());
    st->set_oop_replacement(nm);
    m = nm;
  }

  // Zero the codes area so the padding between an opcode and its 8-byte
  // aligned inline oops is well defined.
  for (int i = 0; i < m->size_of_codes() * oopSize; i++)
    m->codes()[i] = 0;

  // Write the final layout: opcodes, argument bytes, inline oops, and the
  // relocated branch and failure-block offsets.
  for (int i = 0; i < n_instrs; i++) {
    bootstrap_instr* it = &instrs[i];
    int w64 = it->w64;
    m->byte_at_put(w64 + 1, it->code);
    for (int a = 0; a < it->nargs; a++) {
      int v = it->args[a];
      for (int j = 0; j < it->noff; j++)
        if (it->off_arg[j] == a)
          v = it->new_off[j];
      if (v < 0 || v > 255) {
        fatal("byte branch offset does not fit the 64-bit layout");
      }
      m->byte_at_put(w64 + 2 + a, (u_char)v);
    }
    if (it->noops > 0) {
      int base, next;
      oop_instr_offsets(it->fmt, w64, base, next);
      for (int s = 0; s < it->noops; s++) {
        intptr_t v;
        if (it->oop_is_offset[s]) {
          int j = (it->off_slot[0] == s) ? 0 : 1;
          v = it->new_off[j];
        } else {
          v = it->oops[s];
        }
        *(intptr_t*)m->codes(base + s * oopSize + 1) = v;
      }
      if (it->has_trailing)
        m->byte_at_put(base + it->noops * oopSize + 1, it->trailing);
    } else if (it->fmt == Bytecodes::BBS) {
      for (int k = 0; k < it->bs_count; k++)
        m->byte_at_put(w64 + 3 + k, bs_data[it->bs_start + k]);
    }
  }

  FreeHeap(rec_w32);
  FreeHeap(rec_w64);
  FreeHeap(instrs);
  FreeHeap(bs_data);
}
int methodOopDesc::next_bci_from(u_char* hp) const {
  // Computes the next bci
  // hp is the interpreter 'ip' kept in the activation
  // pointing to the next code to execute.

  // Fist the next bci is computed. Note the first index is 1.
  return (hp - (u_char*)addr()) - sizeof(methodOopDesc) + 1;
}

int methodOopDesc::bci_from(u_char* hp) const {
  // We find the current bci by searching from the beginning
  return find_bci_from(next_bci_from(hp));
}

int methodOopDesc::number_of_arguments() const {
  assert(is_blockMethod() || selector()->number_of_arguments() == nofArgs(), "just checking");
  return nofArgs();
}

int methodOopDesc::number_of_stack_temporaries() const {
  int n = 1; // temporary 0 is always there
  u_char b0 =
    *codes(1); // if there's more than one temporary there's an allocate temp or allocate float at the beginning
  switch (b0) {
    case Bytecodes::allocate_temp_1:
      n += 1;
      break;
    case Bytecodes::allocate_temp_2:
      n += 2;
      break;
    case Bytecodes::allocate_temp_3:
      n += 3;
      break;
    case Bytecodes::allocate_temp_n: {
      u_char b1 = *codes(2);
      n += ((b1 == 0) ? 256 : b1);
    } break;
    case Bytecodes::float_allocate: {
      // One additional temp (temp1) for Floats::magic + additional
      // temps allocated in pairs to match to match one float temp.
      u_char b1 = *codes(2);
      n += 1 + b1 * 2;
    } break;
  }
  return n;
}

int methodOopDesc::float_offset(int float_no) const {
  assert(0 <= float_no && float_no < number_of_float_temporaries(), "float_no out of range");
  return float_section_start_offset() - float_no * floatSize / oopSize - 1;
}

symbolOop methodOopDesc::enclosing_method_selector() const {
  assert(is_blockMethod(), "must be block method");
  methodOop m = parent();
  while (m->is_blockMethod())
    m = m->parent();
  return m->selector();
}

void methodOopDesc::print_value_for(klassOop receiver_klass, outputStream* st) {
  outputStream* s = st ? st : mystd;
  if (is_blockMethod()) {
    s->print("[] in ");
    enclosing_method_selector()->print_symbol_on(s);
  } else {
    selector()->print_symbol_on(s);
  }
  klassOop holder = receiver_klass->klass_part()->lookup_method_holder_for(this);
  if (holder) {
    s->print(" in ");
    holder->klass_part()->print_name_on(s);
  }
}

void methodOopDesc::print_codes() {
  ResourceMark rm;
  selector()->print_symbol_on(mystd);
  mystd->cr();
  MethodPrinterClosure closure = MethodPrinterClosure(mystd);
  MethodIterator mi(this, &closure);
  mystd->cr();
}

void methodOopDesc::pretty_print() {
  ResourceMark rm;
  prettyPrinter::print(this);
}

symbolOop methodOopDesc::selector() const {
  if (selector_or_method()->is_symbol())
    return symbolOop(selector_or_method());
  return vmSymbols::selector_for_blockMethod();
}

methodOop methodOopDesc::parent() const {
  oop t = selector_or_method();
  return t->is_method() ? methodOop(t) : NULL;
}

methodOop methodOopDesc::home() const {
  methodOop m = methodOop(this);
  while (m->is_blockMethod())
    m = m->parent();
  return m;
}

byteArrayOop methodOopDesc::source() {
  return oopFactory::new_symbol("<no source>");
}

objArrayOop methodOopDesc::tempInfo() {
  return debugInfo();
}

class methodStream {
public:
  GrowableArray<oop>* result;

  methodStream() { result = new GrowableArray<oop>(1000); }

  void put_byte(int byte) {
    result->append(trueObj);
    result->append(as_smiOop(byte));
  }

  void put_word(int word) {
    char* p = (char*)&word;
    put_byte(p[0]);
    put_byte(p[1]);
    put_byte(p[2]);
    put_byte(p[3]);
  }

  void put_oop(oop obj) {
    result->append(falseObj);
    result->append(obj);
  }

  void align(u_char* hp) {
    u_char* end = (u_char*)(((intptr_t)hp + 3) & (~3));
    while (hp < end) {
      put_byte(255);
      hp++;
    }
  }
};

objArrayOop methodOopDesc::fileout_body() {
  // Convert sends into canonical form
  // Do not uncustomize since we need the mixin to do that.
  BlockScavenge bs;
  ResourceMark rm;
  methodStream out;

  CodeIterator c(this);
  do {
    if (Bytecodes::send_type(c.code()) != Bytecodes::no_send) {
      // Send
      Bytecodes::Code original = Bytecodes::original_send_code_for(c.code());
      out.put_byte(original);
      if (Bytecodes::format(original) == Bytecodes::BBOO) {
        out.put_byte(c.byte_at(1));
        out.align(c.hp() + 2);
      } else {
        out.align(c.hp() + 1);
      }
      out.put_oop(c.ic()->selector());
      out.put_oop(smiOop_zero);
    } else if (c.is_primitive_call()) {
      // Primitive call
      Bytecodes::Code original = Bytecodes::original_primitive_call_code_for(c.code());
      out.put_byte(original);
      out.align(c.hp() + 1);
      if (c.code() == Bytecodes::prim_call || c.code() == Bytecodes::prim_call_failure ||
          c.code() == Bytecodes::prim_call_self || c.code() == Bytecodes::prim_call_self_failure) {
        primitive_desc* pdesc = primitives::lookup((fntype)c.word_at(1));
        out.put_oop(pdesc->selector());
      } else {
        out.put_oop(c.oop_at(1));
      }
      if (Bytecodes::format(original) == Bytecodes::BOL) {
        out.put_word(c.word_at(5));
      }
    } else if (c.is_dll_call()) {
      // DLL call
      InterpretedDLL_Cache* ic = c.dll_cache();
      out.put_byte(c.code());
      out.align(c.hp() + 1);
      out.put_oop(ic->dll_name());
      out.put_oop(ic->funct_name());
      out.put_oop(smiOop_zero);
      out.put_byte(ic->number_of_arguments());
    } else {
      // Otherwise
      out.put_byte(c.code());
      switch (c.format()) {
        case Bytecodes::B:
          break;
        case Bytecodes::BB:
          out.put_byte(c.byte_at(1));
          break;
        case Bytecodes::BBB:
          out.put_byte(c.byte_at(1));
          out.put_byte(c.byte_at(2));
          break;
        case Bytecodes::BBBB:
          out.put_byte(c.byte_at(1));
          out.put_byte(c.byte_at(2));
          out.put_byte(c.byte_at(3));
          break;
        case Bytecodes::BBO:
          out.put_byte(c.byte_at(1));
          out.align(c.hp() + 2);
          out.put_oop(c.oop_at(2));
          break;
        case Bytecodes::BBL:
          out.put_byte(c.byte_at(1));
          out.align(c.hp() + 2);
          out.put_word(c.word_at(2));
          break;
        case Bytecodes::BO:
          out.align(c.hp() + 1);
          out.put_oop(c.oop_at(1));
          break;
        case Bytecodes::BOL:
          out.put_oop(c.oop_at(1));
          out.align(c.hp() + 2);
          out.put_word(c.word_at(5));
          break;
        case Bytecodes::BLL:
          out.align(c.hp() + 1);
          out.put_word(c.word_at(1));
          out.put_word(c.word_at(5));
          break;
        case Bytecodes::BL:
          out.align(c.hp() + 1);
          out.put_word(c.word_at(1));
          break;
        case Bytecodes::BBS: {
          int length = c.byte_at(1) == 0 ? 256 : c.byte_at(1);
          out.put_byte(length);
          for (int index = 0; index < length; index++) {
            out.put_byte(c.byte_at(2 + index));
          }
          break;
        }
        default:
          mystd->print_cr("Format unknown %s", Bytecodes::format_as_string(c.format()));
          fatal("aborting");
      }
    }
  } while (c.advance());
  return oopFactory::new_objArray(out.result);
}

methodOopDesc::Method_Inlining_Info methodOopDesc::method_inlining_info() const {
  if (is_blockMethod())
    return normal_inline;
  Method_Inlining_Info info = Method_Inlining_Info(get_unsigned_bitfield(flags(), methodInfoFlags, methodInfoSize));
  return info;
}

void methodOopDesc::set_method_inlining_info(Method_Inlining_Info info) {
  if (is_blockMethod())
    return;
  set_flags(set_unsigned_bitfield(flags(), methodInfoFlags, methodInfoSize, info));
}

methodOopDesc::Block_Info methodOopDesc::block_info() const {
  assert(is_blockMethod(), "must be a block");
  return Block_Info(get_unsigned_bitfield(flags(), blockInfoFlags, blockInfoSize));
}

bool methodOopDesc::in_context_allocation(int bci) const {
  CodeIterator c(methodOop(this), bci);
  return c.code_type() == Bytecodes::new_context;
}

class BlockFinderClosure : public SpecializedMethodClosure {
public:
  bool hasBlock;
  BlockFinderClosure() { hasBlock = false; }
  void allocate_closure(AllocationType type, int nofArgs, methodOop meth) { hasBlock = true; }
};

bool methodOopDesc::hasNestedBlocks() const {
  // should be a bit in the methodOop -- fix this, Robert (delete class above)
  BlockFinderClosure cl;
  MethodIterator it(methodOop(this), &cl);
  return cl.hasBlock;
}

// The following two functions map context numbers (as used in the interpreter
// to access temps in enclosing scopes) to source-level lexical distances, and
// vice versa.
// Definitions: context no = number of indirections through contexts needed to
//			     access temporary (0 -> temp is in current context)
//		lex. dist. = difference in nesting levels between two scopes;
//			     e.g., distance between a scope and its immediately
//			     enclosing scope is 1

int methodOopDesc::lexicalDistance(int contextNo) {
  methodOop m = this;
  int c = -1;
  int d = -1;
  while (c < contextNo) {
    if (m->allocatesInterpretedContext())
      c++;
    m = m->parent();
    d++;
  };
  return d;
}

int methodOopDesc::contextNo(int lexicalDistance) {
  methodOop m = this;
  int c = -1;
  int d = -1;
  while (d < lexicalDistance) {
    if (m->allocatesInterpretedContext())
      c++;
    m = m->parent();
    d++;
  }
  return c;
}

int methodOopDesc::context_chain_length() const {
  int length = 0;
  for (methodOop method = methodOop(this); method; method = method->parent()) {
    if (method->allocatesInterpretedContext())
      length++;
  }
  return length;
}

void methodOopDesc::clear_inline_caches() {
  // %skim the cream:
  //    if the method is not customized it has never been executed.
  if (!is_customized())
    return;

  CodeIterator c(this);
  do {
    InterpretedIC* ic = c.ic();
    if (ic) {
      ic->clear();
    } else {
      // Call it for blocks
      switch (c.code()) {
        case Bytecodes::push_new_closure_tos_0: // fall through
        case Bytecodes::push_new_closure_tos_1: // fall through
        case Bytecodes::push_new_closure_tos_2: // fall through
        case Bytecodes::push_new_closure_context_0: // fall through
        case Bytecodes::push_new_closure_context_1: // fall through
        case Bytecodes::push_new_closure_context_2: {
          methodOop block_method = methodOop(c.oop_at(1));
          assert(block_method->is_method(), "must be method");
          block_method->clear_inline_caches();
        } break;
        case Bytecodes::push_new_closure_tos_n: // fall through
        case Bytecodes::push_new_closure_context_n: {
          methodOop block_method = methodOop(c.oop_at(2));
          assert(block_method->is_method(), "must be method");
          block_method->clear_inline_caches();
        } break;
        default:
          break; // not a block-creating bytecode
      }
    }
  } while (c.advance());
}

void methodOopDesc::cleanup_inline_caches() {
  // %skim the cream:
  //    if the method is not customized it has never been executed.
  if (!is_customized())
    return;

  CodeIterator c(this);
  do {
    InterpretedIC* ic = c.ic();
    if (ic) {
      ic->cleanup();
    } else {
      methodOop bm = c.block_method();
      if (bm) {
        bm->cleanup_inline_caches();
      }
    }
  } while (c.advance());
}

bool methodOopDesc::was_never_executed() {
  // skim the cream: if the method is not customized it has never been ecexuted.
  if (!is_customized())
    return true;

  // return true if method looks like it was never executed
  if (invocation_count() != 0 || sharing_count() != 0)
    return false;
  CodeIterator c(this);
  do {
    InterpretedIC* ic = c.ic();
    if (ic && !ic->is_empty())
      return false;
  } while (c.advance());
  return true;
}

int methodOopDesc::estimated_inline_cost(klassOop receiverKlass) {
  // the result of this calculation should be cached in the method; 8 bits are enough
  CodeIterator c(this);
  int cost = 0;
  do {
    cost += CostModel::cost_for(c.code());
    switch (c.code()) {
      case Bytecodes::push_new_closure_context_0:
      case Bytecodes::push_new_closure_context_1:
      case Bytecodes::push_new_closure_context_2:
      case Bytecodes::push_new_closure_tos_0:
      case Bytecodes::push_new_closure_tos_1:
      case Bytecodes::push_new_closure_tos_2: {
        methodOop m = methodOop(c.oop_at(1));
        assert(m->is_method(), "must be method");
        cost += m->estimated_inline_cost(receiverKlass);
        break;
      }
      case Bytecodes::push_new_closure_tos_n:
      case Bytecodes::push_new_closure_context_n: {
        methodOop m = methodOop(c.oop_at(2));
        assert(m->is_method(), "must be method");
        cost += m->estimated_inline_cost(receiverKlass);
        break;
      }
      default:
        break; // not a block-creating bytecode
    }
    extern bool SuperSendsAreAlwaysInlined;
    if (Bytecodes::is_super_send(c.code()) && SuperSendsAreAlwaysInlined && receiverKlass) {
      klassOop mh = receiverKlass->klass_part()->lookup_method_holder_for(this);
      // TODO: the following is wrong. A super send may use a different selector than
      // the containing method. It's bad style, but legal. Need to lookup the selector
      // for the send, not the containing method's selector. slr 13/04/2010
      methodOop superMethod = mh ? lookupCache::compile_time_super_lookup(mh, selector()) : NULL;
      if (superMethod)
        cost += superMethod->estimated_inline_cost(receiverKlass);
    }
  } while (c.advance());
  return cost;
}

int methodOopDesc::find_bci_from(int nbci) const {
  CodeIterator c(methodOop(this));
  int prev_bci = 1;
  do {
    if (c.bci() == nbci)
      return prev_bci;
    prev_bci = c.bci();
  } while (c.advance());
  return -1;
}

int methodOopDesc::next_bci(int bci) const {
  CodeIterator c(methodOop(this), bci);
  c.advance();
  return c.bci();
}

class ExpressionStackMapper : public MethodClosure {
private:
  GrowableArray<intptr_t>* mapping;
  int target_bci;

  void map_push() { map_push(bci()); }

  void map_push(int b) {
    // lprintf("push(%d)", bci);
    if (b >= target_bci) {
      abort();
    } else {
      mapping->push(b);
    }
  }

  void map_pop() {
    if (bci() >= target_bci) {
      abort();
    } else {
      // lprintf("pop(%d)", bci());
      mapping->pop();
    }
  }

  void map_send(bool has_receiver, int number_of_arguments) {
    if (has_receiver)
      map_pop();
    for (int i = 0; i < number_of_arguments; i++)
      map_pop();
    map_push();
  }

public:
  ExpressionStackMapper(GrowableArray<intptr_t>* mapping, int target_bci) {
    this->mapping = mapping;
    this->target_bci = target_bci;
  }

  void push_self() { map_push(); }
  void push_tos() { map_push(); }
  void push_literal(oop obj) { map_push(); }
  void push_argument(int no) { map_push(); }
  void push_temporary(int no) { map_push(); }
  void push_temporary(int no, int context) { map_push(); }
  void push_instVar(int offset) { map_push(); }
  void push_instVar_name(symbolOop name) { map_push(); }
  void push_classVar(associationOop assoc) { map_push(); }
  void push_classVar_name(symbolOop name) { map_push(); }

  void push_global(associationOop obj) { map_push(); }

  void pop() { map_pop(); }

  void normal_send(InterpretedIC* ic) { map_send(true, ic->selector()->number_of_arguments()); }
  void self_send(InterpretedIC* ic) { map_send(false, ic->selector()->number_of_arguments()); }
  void super_send(InterpretedIC* ic) { map_send(false, ic->selector()->number_of_arguments()); }

  void double_equal() { map_send(true, 1); }
  void double_not_equal() { map_send(true, 1); }

  void method_return(int nofArgs) { map_pop(); }
  void nonlocal_return(int nofArgs) { map_pop(); }

  void allocate_closure(AllocationType type, int nofArgs, methodOop meth) {
    if (type == tos_as_scope)
      map_pop();
    map_push();
  }

  // nodes
  void if_node(IfNode* node);
  void cond_node(CondNode* node);
  void while_node(WhileNode* node);
  void primitive_call_node(PrimitiveCallNode* node);
  void dll_call_node(DLLCallNode* node);

  // call backs to ignore
  void allocate_temporaries(int nofTemps) {}
  void store_temporary(int no) {}
  void store_temporary(int no, int context) {}
  void store_instVar(int offset) {}
  void store_instVar_name(symbolOop name) {}
  void store_classVar(associationOop assoc) {}
  void store_classVar_name(symbolOop name) {}

  void store_global(associationOop obj) {}
  void allocate_context(int nofTemps, bool forMethod = false) {}
  void set_self_via_context() {}
  void copy_self_into_context() {}
  void copy_argument_into_context(int argNo, int no) {}
  void zap_scope() {}
  void predict_prim_call(primitive_desc* pdesc, int failure_start) {}
  void float_allocate(int nofFloatTemps, int nofFloatExprs) {}
  void float_floatify(Floats::Function f, int tof) { map_pop(); }
  void float_move(int tof, int from) {}
  void float_set(int tof, doubleOop value) {}
  void float_nullary(Floats::Function f, int tof) {}
  void float_unary(Floats::Function f, int tof) {}
  void float_binary(Floats::Function f, int tof) {}
  void float_unaryToOop(Floats::Function f, int tof) { map_push(); }
  void float_binaryToOop(Floats::Function f, int tof) { map_push(); }
};

void ExpressionStackMapper::if_node(IfNode* node) {
  if (node->includes(target_bci)) {
    if (node->then_code()->includes(target_bci)) {
      map_pop();
      MethodIterator i(node->then_code(), this);
    } else if (node->else_code() && node->else_code()->includes(target_bci)) {
      map_pop();
      MethodIterator i(node->else_code(), this);
    }
    abort();
  } else {
    map_pop();
    if (node->produces_result())
      map_push(node->begin_bci());
  }
}

void ExpressionStackMapper::cond_node(CondNode* node) {
  if (node->includes(target_bci)) {
    if (node->expr_code()->includes(target_bci)) {
      map_pop();
      MethodIterator i(node->expr_code(), this);
    }
    abort();
  } else {
    map_pop();
    map_push(node->begin_bci());
  }
}

void ExpressionStackMapper::while_node(WhileNode* node) {
  if (node->includes(target_bci)) {
    if (node->expr_code()->includes(target_bci))
      MethodIterator i(node->expr_code(), this);
    else if (node->body_code() && node->body_code()->includes(target_bci))
      MethodIterator i(node->body_code(), this);
    abort();
  }
}

void ExpressionStackMapper::primitive_call_node(PrimitiveCallNode* node) {
  int nofArgsToPop = node->number_of_parameters();
  for (int i = 0; i < nofArgsToPop; i++)
    map_pop();

  map_push();
  if (node->failure_code() && node->failure_code()->includes(target_bci)) {
    MethodIterator i(node->failure_code(), this);
  }
}

void ExpressionStackMapper::dll_call_node(DLLCallNode* node) {
  int nargs = node->nofArgs();
  for (int index = 0; index < nargs; index++)
    map_pop();
}

GrowableArray<intptr_t>* methodOopDesc::expression_stack_mapping(int bci) {
  GrowableArray<intptr_t>* mapping = new GrowableArray<intptr_t>(10);
  ExpressionStackMapper blk(mapping, bci);
  MethodIterator i(this, &blk);

  // reverse the mapping so the top of the expression stack is first
  // %todo:
  //    move reverse to GrowableArray

  GrowableArray<intptr_t>* result = new GrowableArray<intptr_t>(mapping->length());
  for (int index = mapping->length() - 1; index >= 0; index--) {
    result->push(mapping->at(index));
  }
  return result;
}

static void lookup_primitive_and_patch(u_char* p, u_char byte) {
  assert((intptr_t)p % 4 == 0, "first instruction supposed to be aligned");
  *p = byte; // patch byte
  p += 4; // advance to primitive name
  //(*(symbolOop*)p)->print_symbol_on();
  *(int*)p = (intptr_t)primitives::lookup(*(symbolOop*)p)->fn();
}

bool methodOopDesc::is_primitiveMethod() const {
  char b = *codes();
  switch (*codes()) {
    case Bytecodes::predict_prim_call:
      return true;
    case Bytecodes::predict_prim_call_failure:
      return true;
    case Bytecodes::predict_prim_call_lookup:
      lookup_primitive_and_patch(codes(), Bytecodes::predict_prim_call);
      return true;
    case Bytecodes::predict_prim_call_failure_lookup:
      lookup_primitive_and_patch(codes(), Bytecodes::predict_prim_call_failure);
      return true;
    default:
      return false;
  }
}

Bytecodes::Code methodOopDesc::special_primitive_code() const {
  assert(is_special_primitiveMethod(), "should only be called for special primitive methods");
  Bytecodes::Code code = Bytecodes::Code(*codes(2));
  assert(Bytecodes::send_type(code) == Bytecodes::predicted_send, "code or bytecode table inconsistent");
  return code;
}

methodOop methodOopDesc::methodOop_from_hcode(u_char* hp) {
  methodOop method = methodOop(as_memOop(Universe::object_start((oop*)hp)));
  assert(method->is_method(), "must be method");
  assert(method->codes() <= hp && hp < method->codes() + method->size_of_codes() * sizeof(oop),
         "h-code pointer not contained in method");
  return method;
}

int methodOopDesc::end_bci() const {
  int last_entry = this->size_of_codes() * oopSize;
  for (int index = 0; index < 4; index++)
    if (byte_at(last_entry - index) != Bytecodes::halt)
      return last_entry + 1 - index;
  fatal("should never reach the point");
  return 0;
}

InterpretedIC* methodOopDesc::ic_at(int bci) const {
  CodeIterator iterator(methodOop(this), bci);
  return iterator.ic();
}

methodOop methodOopDesc::block_method_at(int bci) {
  CodeIterator c(methodOop(this), bci);
  switch (c.code()) {
    case Bytecodes::push_new_closure_tos_0: // fall through
    case Bytecodes::push_new_closure_tos_1: // fall through
    case Bytecodes::push_new_closure_tos_2: // fall through
    case Bytecodes::push_new_closure_context_0: // fall through
    case Bytecodes::push_new_closure_context_1: // fall through
    case Bytecodes::push_new_closure_context_2: {
      methodOop block_method = methodOop(c.oop_at(1));
      assert(block_method->is_method(), "must be method");
      return block_method;
    } break;
    case Bytecodes::push_new_closure_tos_n: // fall through
    case Bytecodes::push_new_closure_context_n: {
      methodOop block_method = methodOop(c.oop_at(2));
      assert(block_method->is_method(), "must be method");
      return block_method;
    } break;
    default:
      return NULL; // not a block-creating bytecode
  }
}

int methodOopDesc::bci_for_block_method(methodOop inner) {
  CodeIterator c(this);
  do {
    if (inner == block_method_at(c.bci()))
      return c.bci();
  } while (c.advance());
  ShouldNotReachHere();
  return 0;
}

void methodOopDesc::print_inlining_database_on(outputStream* st) {
  if (is_blockMethod()) {
    methodOop o = parent();
    o->print_inlining_database_on(st);
    st->print(" %d", o->bci_for_block_method(this));
  } else {
    selector()->print_symbol_on(st);
  }
}

// ContextMethodIterator is used in number_of_context_temporaries to
// get information about context allocation
class ContextMethodIterator : public SpecializedMethodClosure {
private:
  enum {
    sentinel = -1
  };
  int count;
  bool _self_in_context;

public:
  ContextMethodIterator() {
    count = sentinel;
    _self_in_context = false;
  }

  bool self_in_context() { return _self_in_context; }

  int number_of_context_temporaries() {
    assert(count != sentinel, "number_of_context_temporaries not set");
    return count;
  }

  void allocate_context(int nofTemps, bool forMethod) {
    assert(count == sentinel, "make sure it is not called more than one");
    count = nofTemps;
  }

  void copy_self_into_context() { _self_in_context = true; }
};

int methodOopDesc::number_of_context_temporaries(bool* self_in_context) {
  // Use this for debugging only
  assert(allocatesInterpretedContext(), "can only be called if method allocates context");
  ContextMethodIterator blk;
  MethodIterator i(this, &blk);
  if (self_in_context)
    *self_in_context = blk.self_in_context();
  return blk.number_of_context_temporaries();
}

void methodOopDesc::customize_for(klassOop klass, mixinOop mixin) {
  assert(!is_customized() || klass != mixin->primary_invocation(), "should not recustomize to the same class");

  CodeIterator c(this);
  do {
    InterpretedIC* ic = c.ic();
    if (ic)
      ic->clear_without_deallocation_pic();
    switch (c.code()) {

      case Bytecodes::push_classVar_name:
      case Bytecodes::store_classVar_pop_name:
      case Bytecodes::store_classVar_name:
        c.customize_class_var_code(klass);
        break;

      case Bytecodes::push_classVar:
      case Bytecodes::store_classVar_pop:
      case Bytecodes::store_classVar:
        c.recustomize_class_var_code(mixin->primary_invocation(), klass);
        break;

      case Bytecodes::push_instVar_name:
      case Bytecodes::store_instVar_pop_name:
      case Bytecodes::store_instVar_name:
      case Bytecodes::return_instVar_name:
        c.customize_inst_var_code(klass);
        break;

      case Bytecodes::push_instVar:
      case Bytecodes::store_instVar_pop:
      case Bytecodes::store_instVar:
      case Bytecodes::return_instVar:
        c.recustomize_inst_var_code(mixin->primary_invocation(), klass);
        break;

      case Bytecodes::push_new_closure_tos_0: // fall through
      case Bytecodes::push_new_closure_tos_1: // fall through
      case Bytecodes::push_new_closure_tos_2: // fall through
      case Bytecodes::push_new_closure_context_0: // fall through
      case Bytecodes::push_new_closure_context_1: // fall through
      case Bytecodes::push_new_closure_context_2: {
        methodOop block_method = methodOop(c.oop_at(1));
        assert(block_method->is_method(), "must be method");
        block_method->customize_for(klass, mixin);
      } break;
      case Bytecodes::push_new_closure_tos_n: // fall through
      case Bytecodes::push_new_closure_context_n: {
        methodOop block_method = methodOop(c.oop_at(2));
        assert(block_method->is_method(), "must be method");
        block_method->customize_for(klass, mixin);
      } break;
      default:
        break; // no customization needed for this bytecode
    }
  } while (c.advance());
  // set customized flag

  int new_flags = addNth(flags(), isCustomizedFlag);
  set_size_and_flags(size_of_codes(), nofArgs(), new_flags);
}

void methodOopDesc::uncustomize_for(mixinOop mixin) {
  // Skim the cream
  if (!is_customized())
    return;

  klassOop klass = mixin->primary_invocation();
  assert(klass->is_klass(), "primary invocation muyst be present");

  CodeIterator c(this);
  do {
    InterpretedIC* ic = c.ic();
    if (ic)
      ic->clear_without_deallocation_pic();
    switch (c.code()) {
      case Bytecodes::push_classVar:
      case Bytecodes::store_classVar_pop:
      case Bytecodes::store_classVar:
        c.uncustomize_class_var_code(mixin->primary_invocation());
        break;

      case Bytecodes::push_instVar:
      case Bytecodes::store_instVar_pop:
      case Bytecodes::store_instVar:
      case Bytecodes::return_instVar:
        c.uncustomize_inst_var_code(mixin->primary_invocation());
        break;

      case Bytecodes::push_new_closure_tos_0: // fall through
      case Bytecodes::push_new_closure_tos_1: // fall through
      case Bytecodes::push_new_closure_tos_2: // fall through
      case Bytecodes::push_new_closure_context_0: // fall through
      case Bytecodes::push_new_closure_context_1: // fall through
      case Bytecodes::push_new_closure_context_2: {
        methodOop block_method = methodOop(c.oop_at(1));
        assert(block_method->is_method(), "must be method");
        block_method->uncustomize_for(mixin);
      } break;
      case Bytecodes::push_new_closure_tos_n: // fall through
      case Bytecodes::push_new_closure_context_n: {
        methodOop block_method = methodOop(c.oop_at(2));
        assert(block_method->is_method(), "must be method");
        block_method->uncustomize_for(mixin);
      } break;
      default:
        break; // no uncustomization needed for this bytecode
    }
  } while (c.advance());
  // set customized flag
  int new_flags = subNth(flags(), isCustomizedFlag);
  set_size_and_flags(size_of_codes(), nofArgs(), new_flags);
}

methodOop methodOopDesc::copy_for_customization() const {
  // Copy this method
  int len = size();
  oop* clone = Universe::allocate_tenured(len);
  oop* to = clone;
  oop* from = (oop*)addr();
  oop* end = to + len;
  while (to < end)
    *to++ = *from++;

  // Do the deep copy
  methodOop new_method = methodOop(as_memOop(clone));
  CodeIterator c(new_method);
  do {
    switch (c.code()) {
      case Bytecodes::push_new_closure_tos_0: // fall through
      case Bytecodes::push_new_closure_tos_1: // fall through
      case Bytecodes::push_new_closure_tos_2: // fall through
      case Bytecodes::push_new_closure_context_0: // fall through
      case Bytecodes::push_new_closure_context_1: // fall through
      case Bytecodes::push_new_closure_context_2: {
        methodOop block_method = methodOop(c.oop_at(1));
        assert(block_method->is_method(), "must be method");
        methodOop new_block_method = block_method->copy_for_customization();
        new_block_method->set_selector_or_method(new_method);
        Universe::store(c.aligned_oop(1), new_block_method);
      } break;
      case Bytecodes::push_new_closure_tos_n: // fall through
      case Bytecodes::push_new_closure_context_n: {
        methodOop block_method = methodOop(c.oop_at(2));
        assert(block_method->is_method(), "must be method");
        methodOop new_block_method = block_method->copy_for_customization();
        new_block_method->set_selector_or_method(new_method);
        Universe::store(c.aligned_oop(2), new_block_method);
      } break;
      default:
        break; // not a block-creating bytecode
    }
  } while (c.advance());
  return new_method;
}

void methodOopDesc::verify_context(contextOop con) {
  // Check if we should expect a context
  if (!activation_has_context()) {
    warning("Activation has no context (0x%lx).", con);
  }
  // Check the static vs. dynamic chain length
  if (context_chain_length() != con->chain_length()) {
    warning("Wrong context chain length (got %d expected %d)", con->chain_length(), context_chain_length());
  }
  // Check the context has no forward reference
  if (con->unoptimized_context() != NULL) {
    warning("Context is optimized (0x%lx).", con);
  }
}

// Traverses over the method including the blocks inside
class TransitiveMethodClosure : public MethodClosure {
public:
  void if_node(IfNode* node);
  void cond_node(CondNode* node);
  void while_node(WhileNode* node);
  void primitive_call_node(PrimitiveCallNode* node);
  void dll_call_node(DLLCallNode* node);

public:
  virtual void inlined_send(symbolOop selector) {}

public:
  void allocate_temporaries(int nofTemps) {}
  void push_self() {}
  void push_tos() {}
  void push_literal(oop obj) {}
  void push_argument(int no) {}
  void push_temporary(int no) {}
  void push_temporary(int no, int context) {}
  void push_instVar(int offset) {}
  void push_instVar_name(symbolOop name) {}
  void push_classVar(associationOop assoc) {}
  void push_classVar_name(symbolOop name) {}
  void push_global(associationOop obj) {}
  void store_temporary(int no) {}
  void store_temporary(int no, int context) {}
  void store_instVar(int offset) {}
  void store_instVar_name(symbolOop name) {}
  void store_classVar(associationOop assoc) {}
  void store_classVar_name(symbolOop name) {}
  void store_global(associationOop obj) {}
  void pop() {}
  void normal_send(InterpretedIC* ic) {}
  void self_send(InterpretedIC* ic) {}
  void super_send(InterpretedIC* ic) {}
  void double_equal() {}
  void double_not_equal() {}
  void method_return(int nofArgs) {}
  void nonlocal_return(int nofArgs) {}
  void allocate_closure(AllocationType type, int nofArgs, methodOop meth);
  void allocate_context(int nofTemps, bool forMethod) {}
  void set_self_via_context() {}
  void copy_self_into_context() {}
  void copy_argument_into_context(int argNo, int no) {}
  void zap_scope() {}
  void predict_prim_call(primitive_desc* pdesc, int failure_start) {}
  void float_allocate(int nofFloatTemps, int nofFloatExprs) {}
  void float_floatify(Floats::Function f, int fno) {}
  void float_move(int fno, int from) {}
  void float_set(int fno, doubleOop value) {}
  void float_nullary(Floats::Function f, int fno) {}
  void float_unary(Floats::Function f, int fno) {}
  void float_binary(Floats::Function f, int fno) {}
  void float_unaryToOop(Floats::Function f, int fno) {}
  void float_binaryToOop(Floats::Function f, int fno) {}
};

void TransitiveMethodClosure::allocate_closure(AllocationType type, int nofArgs, methodOop meth) {
  MethodIterator iter(meth, this);
}

void TransitiveMethodClosure::if_node(IfNode* node) {
  inlined_send(node->selector());
  MethodIterator iter(node->then_code(), this);
  if (node->else_code() != NULL) {
    MethodIterator iter(node->else_code(), this);
  }
}

void TransitiveMethodClosure::cond_node(CondNode* node) {
  inlined_send(node->selector());
  MethodIterator iter(node->expr_code(), this);
}

void TransitiveMethodClosure::while_node(WhileNode* node) {
  inlined_send(node->selector());
  MethodIterator iter(node->expr_code(), this);
  if (node->body_code() != NULL) {
    MethodIterator iter(node->body_code(), this);
  }
}

void TransitiveMethodClosure::primitive_call_node(PrimitiveCallNode* node) {
  inlined_send(node->name());
  if (node->failure_code() != NULL) {
    MethodIterator iter(node->failure_code(), this);
  }
}

void TransitiveMethodClosure::dll_call_node(DLLCallNode* node) {
  inlined_send(node->function_name());
  if (node->failure_code() != NULL) {
    MethodIterator iter(node->failure_code(), this);
  }
}

class ReferencedInstVarNamesClosure : public TransitiveMethodClosure {
private:
  mixinOop mixin;

  void collect(int offset) {
    symbolOop name = mixin->primary_invocation()->klass_part()->inst_var_name_at(offset);
    if (name)
      result->append(name);
  }

  void collect(symbolOop name) { result->append(name); }

public:
  void push_instVar(int offset) { collect(offset); }
  void push_instVar_name(symbolOop name) { collect(name); }
  void store_instVar(int offset) { collect(offset); }
  void store_instVar_name(symbolOop name) { collect(name); }

public:
  ReferencedInstVarNamesClosure(int size, mixinOop mixin) {
    this->result = new GrowableArray<oop>(size);
    this->mixin = mixin;
  }
  GrowableArray<oop>* result;
};

objArrayOop methodOopDesc::referenced_instance_variable_names(mixinOop mixin) const {
  ResourceMark rm;
  ReferencedInstVarNamesClosure blk(20, mixin);
  MethodIterator(methodOop(this), &blk);
  return oopFactory::new_objArray(blk.result);
}

class ReferencedClassVarNamesClosure : public TransitiveMethodClosure {
private:
  void collect(symbolOop name) { result->append(name); }

public:
  void push_classVar(associationOop assoc) { collect(assoc->key()); }
  void push_classVar_name(symbolOop name) { collect(name); }

  void store_classVar(associationOop assoc) { collect(assoc->key()); }
  void store_classVar_name(symbolOop name) { collect(name); }

public:
  ReferencedClassVarNamesClosure(int size) { result = new GrowableArray<oop>(size); }
  GrowableArray<oop>* result;
};

objArrayOop methodOopDesc::referenced_class_variable_names() const {
  ResourceMark rm;
  ReferencedClassVarNamesClosure blk(20);
  MethodIterator(methodOop(this), &blk);
  return oopFactory::new_objArray(blk.result);
}

class ReferencedGlobalsClosure : public TransitiveMethodClosure {
private:
  void collect(symbolOop selector) { result->append(selector); }

public:
  void push_global(associationOop obj) { collect(obj->key()); }
  void store_global(associationOop obj) { collect(obj->key()); }

public:
  ReferencedGlobalsClosure(int size) { result = new GrowableArray<oop>(size); }
  GrowableArray<oop>* result;
};

objArrayOop methodOopDesc::referenced_global_names() const {
  ResourceMark rm;
  ReferencedGlobalsClosure blk(20);
  MethodIterator(methodOop(this), &blk);
  return oopFactory::new_objArray(blk.result);
}

class SendersClosure : public TransitiveMethodClosure {
private:
  void collect(symbolOop selector) { result->append(selector); }

  void float_op(Floats::Function f) {
    if (Floats::has_selector_for(f)) {
      collect(Floats::selector_for(f));
    }
  }

public:
  void inlined_send(symbolOop selector) { collect(selector); }
  void normal_send(InterpretedIC* ic) { collect(ic->selector()); }
  void self_send(InterpretedIC* ic) { collect(ic->selector()); }
  void super_send(InterpretedIC* ic) { collect(ic->selector()); }
  void double_equal() { collect(vmSymbols::double_equal()); }
  void double_not_equal() { collect(vmSymbols::double_tilde()); }

  void float_floatify(Floats::Function f, int fno) { float_op(f); }
  void float_nullary(Floats::Function f, int fno) { float_op(f); }
  void float_unary(Floats::Function f, int fno) { float_op(f); }
  void float_binary(Floats::Function f, int fno) { float_op(f); }
  void float_unaryToOop(Floats::Function f, int fno) { float_op(f); }
  void float_binaryToOop(Floats::Function f, int fno) { float_op(f); }

public:
  SendersClosure(int size) { result = new GrowableArray<oop>(size); }
  GrowableArray<oop>* result;
};

objArrayOop methodOopDesc::senders() const {
  ResourceMark rm;
  SendersClosure blk(20);
  MethodIterator(methodOop(this), &blk);
  return oopFactory::new_objArray(blk.result);
}

symbolOop selectorFrom(oop method_or_selector) {
  if (method_or_selector == NULL)
    return NULL;
  if (method_or_selector->is_symbol())
    return symbolOop(method_or_selector);
  if (!method_or_selector->is_method())
    return NULL;

  methodOop method = methodOop(method_or_selector);
  if (method->is_blockMethod())
    return selectorFrom(methodOop(method->selector_or_method()));
  return method->selector();
}

void stopInSelector(const char* name, methodOop method) {
  int len = strlen(name);
  symbolOop selector = selectorFrom(method);
  if (selector == NULL)
    warning("Selector was NULL!");
  else if (selector->length() == len && strncmp(name, selector->chars(), len) == 0) {
    TraceCanonicalContext = true;
    //method->pretty_print();
    //method->print_codes();
    breakpoint();
  }
}

bool StopInSelector::ignored = false;

symbolOop className(klassOop klass) {
  const int class_name_index = 9;

  if (!klass->is_klass())
    return NULL;
  symbolOop selector = symbolOop(klass->instVarAt(class_name_index));
  if (selector->is_symbol())
    return selector;
  if (!selector->is_klass())
    return NULL;
  return className(klassOop(selector));
}
bool selcmp(const char* name, symbolOop selector) {
  int len = strlen(name);
  if (selector == NULL && name == NULL)
    return true;
  if (selector == NULL || !selector->is_symbol())
    return false;

  return ((selector->length() == len && strncmp(name, selector->chars(), len) == 0));
}
bool shouldStop(const char* name, oop method_or_selector, const char* class_name, klassOop klass) {
  return selcmp(name, selectorFrom(method_or_selector)) && selcmp(class_name, className(klass));
}

StopInSelector::StopInSelector(const char* class_name, const char* name, klassOop klass, oop method_or_selector,
                               bool& fl, bool stop) :
  enable(shouldStop(name, method_or_selector, class_name, klass)), oldFlag(enable ? fl : ignored, true), stop(stop) {
  if (enable && stop)
    breakpoint();
}
