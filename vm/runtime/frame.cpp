/* Copyright 1994 - 1996 LongView Technologies L.L.C. $Revision: 1.73 $ */
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

#include "code/nmethod.hpp"
#include "code/compiledIC.hpp"
#include "code/compiledPIC.hpp"
#include "code/stubRoutines.hpp"
#include "interpreter/floats.hpp"
#include "interpreter/interpretedIC.hpp"
#include "interpreter/interpreter.hpp"
#include "memory/iterator.hpp"
#include "memory/markSweep.hpp"
#include "oops/blockOop.hpp"
#include "oops/memOop.hpp"
#include "oops/methodOop.hpp"
#include "oops/objArrayOop.hpp"
#include "oops/symbolOop.hpp"
#include "oops/oop.hpp"
#include "runtime/frame.hpp"
#include "runtime/vframe.hpp"
#include "topIncludes/std_includes.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/ostream.hpp"
#include "memory/generation.inline.hpp"
#include "oops/oop.inline.hpp"

u_char* frame::hp() const {
  // Lars, please check -- assertion fails
  //assert(is_interpreted_frame(), "must be interpreted");
  return *hp_addr();
}

void frame::set_hp(u_char* hp) {
  //assert(is_interpreted_frame(), "must be interpreted");
  *hp_addr() = hp;
}

void frame::patch_pc(char* pc) {
  char** pc_addr = (char**)sp() - 1;
  *pc_addr = pc;
}

objArrayOop* frame::frame_array_addr() const {
  assert(frame_size() >= minimum_size_for_deoptimized_frame, "Compiler frame is too small for deoptimization");
  //assert(is_deoptimized_frame(), "must be deoptimized frame");
  return (objArrayOop*)addr_at(frame_frame_array_offset);
}

objArrayOop frame::frame_array() const {
  objArrayOop result = *frame_array_addr();
  assert(result->is_objArray(), "must be objArray");
  return result;
}

oop** frame::real_sender_sp_addr() const {
  //assert(is_deoptimized_frame(), "must be deoptimized frame");
  return (oop**)addr_at(frame_real_sender_sp_offset);
}

void frame::patch_fp(void** fp) {
  frame previous(NULL, ((int*)sp()) - frame_sender_sp_offset, NULL);
  previous.set_link(fp);
}

#ifdef DELTA_BACKEND_AARCH64
oop* frame::block_activation_sender_sp() const {
  methodOop m = method_from_hp();
  int nArgs = (m != NULL) ? m->number_of_arguments() : 0;
  // See frame.hpp: call_C's saved-LR slot plus the (nArgs+1) slots reserved by
  // setupBlockValueFrame sit between the block's fp and the caller's sp.
  return (oop*)((char*)addr_at(frame_sender_sp_offset) + slotSize + (nArgs + 1) * slotSize);
}
#endif

methodOop frame::method_from_hp_or_base() const {
  // Resolve hp() to its methodOop when it addresses a byte inside a method's
  // bytecodes (method_from_hp()), and *also* when it holds the method's object
  // base -- which is what convert_hcode_pointer() leaves in the slot for the
  // duration of a mark-sweep cycle.  Both denote the same frame's method, and
  // frame::sender() has to give the same answer for both or the convert and
  // restore walks enumerate different frames.
  oop* h = (oop*)hp();
  if (!Universe::old_gen.contains(h))
    return NULL;
  oop* start = Universe::object_start_checked(h);
  if (start == NULL || !(*start)->is_mark())
    return NULL;
  memOop obj = as_memOop(start);
  if (!obj->is_method())
    return NULL;
  methodOop m = methodOop(obj);
  // Either an interior bytecode address, or the object base itself (offset 0).
  if (h == (oop*)obj || (h >= (oop*)m->codes() && h < (oop*)m->codes_end()))
    return m;
  return NULL;
}

methodOop frame::method_from_hp() const {
  // Resolve hp() to its methodOop, or NULL if hp is not a valid hybrid code
  // pointer.  Deliberately uses the *checked* lookup: a frame slot we have not
  // yet established to be a real activation can hold an arbitrary value, and
  // the plain object_start() would assert or run off the end of a space.
  oop* h = (oop*)hp();
  if (!Universe::old_gen.contains(h))
    return NULL;
  oop* start = Universe::object_start_checked(h);
  if (start == NULL)
    return NULL;
  // object_start_checked() returns the *raw* base address of the object (its
  // callers wrap the return value in as_memOop()), so `*start` is the first
  // word of the object, i.e. its header.  A header is the object's klass, which
  // is a markOop (tag Mark_Tag == 3) and therefore NOT a memOop, so is_mark() is
  // the predicate here; is_mem() is false for every legitimate header.  A garbage
  // word can still carry the mark tag, which is why the codes-containment test
  // below is needed as well.
  if (!(*start)->is_mark())
    return NULL;
  memOop obj = as_memOop(start);
  if (!obj->is_method())
    return NULL;
  methodOop m = methodOop(obj);
  // A genuine hybrid pointer addresses a byte *inside* the method's bytecodes.
  // This containment test is what makes the check reliable: a garbage slot can
  // land in the old generation and, via the card table, resolve to a word that
  // merely carries a plausible tag.  Requiring hp to be within
  // [codes(), codes_end()) rejects those.
  if (hp() < m->codes() || hp() >= m->codes_end())
    return NULL;
  return m;
}

methodOop frame::method() const {
  assert(is_interpreted_frame(), "must be interpreter frame");
  // First we will check the interpreter frame is valid by checking the frame size.
  // The interpreter guarantees hp is valid if the frame is at least 4 in size.
  // (return address, link, receiver, hcode pointer)
  if (frame_size() < minimum_size_for_deoptimized_frame)
    return NULL;

  return method_from_hp();
}

nmethod* frame::code() const {
  assert(is_compiled_frame(), "no code");
  return findNMethod(pc());
}

bool frame::is_interpreted_frame() const {
  // The interpreter's C-call glue can also live in the generated primitives
  // code buffer (e.g. the scavenge/allocate stubs); frames created from such a
  // call site still describe an interpreted method activation, so classify them
  // as interpreted too.
  //
  // This is deliberately just a pc-range test: it is on the send hot path
  // (current_interpretedIC) and during image load the card table that
  // object_start() relies on is not yet built, so hp cannot be resolved here.
  // Code that walks frames for the GC must use is_interpreted_activation(),
  // which additionally requires a valid hybrid code pointer.
  return Interpreter::contains(pc()) || is_in_generated_primitives_code(pc());
}

bool frame::is_interpreted_activation() const {
  if (!is_interpreted_frame())
    return false;
  // The pc test is necessary but *not* sufficient.  frame::sender()
  // deliberately returns the top C frame of a chunk when the current frame is
  // an entry frame, and that C frame is built with the two-argument
  // constructor, so its pc is taken from sp[-1] and can land inside the
  // interpreter even though the frame is not an activation at all.  On x86-64
  // such a frame exposes next_Delta_fp/next_Delta_sp in the very slots an
  // interpreted frame uses for receiver/hp, so believing it makes the GC
  // resolve a bogus address (convert_hcode_pointer -> Universe::object_start)
  // and makes oop_iterate/follow_roots scan the wrong words.
  //
  // A genuine interpreted activation always carries a valid hybrid code
  // pointer: hp() addresses the bytecodes of a methodOop in the old
  // generation.  Requiring that separates the two cases, and it keeps the
  // 17e13c1 behaviour (real activations reached from a call site in the
  // generated primitives buffer still count) because those do have a valid hp.
  if (frame_size() < minimum_size_for_deoptimized_frame)
    return false;

  return method_from_hp() != NULL;
}

bool frame::is_compiled_frame() const {
#ifdef DELTA_COMPILER
  return Universe::code->contains(pc());
#else
  return false;
#endif
}

bool frame::is_deoptimized_frame() const {
  return pc() == StubRoutines::unpack_unoptimized_frames();
}

IC_Iterator* frame::sender_ic_iterator() const {
  return is_entry_frame() ? NULL : sender().current_ic_iterator();
}

IC_Iterator* frame::current_ic_iterator() const {

  if (is_interpreted_activation()) {
    InterpretedIC* ic = current_interpretedIC();
    if (ic && !Bytecodes::is_send_code(ic->send_code()))
      return NULL;
    return ic ? new InterpretedIC_Iterator(ic) : NULL;
  }

  if (is_compiled_frame()) {
    CompiledIC* ic = current_compiledIC();
    return ic->inlineCache() ? new CompiledIC_Iterator(ic) : NULL; // a perform, not a send
  }

  // entry or deoptimized frame
  return NULL;
}

InterpretedIC* frame::current_interpretedIC() const {

  // is_interpreted_activation(), not is_interpreted_frame(): method() returns
  // NULL for a frame that is not a real activation (e.g. the C frame of an
  // entry chunk that happens to carry an interpreter return address), and the
  // bci/codes lookups below dereference the method unconditionally.
  if (is_interpreted_activation()) {
    methodOop m = method();
    int bci = m->bci_from(hp());
    u_char* codeptr = m->codes(bci);
    if (Bytecodes::is_send_code(Bytecodes::Code(*codeptr))) {
      InterpretedIC* ic = as_InterpretedIC((char*)hp());
      assert(ic->send_code_addr() == codeptr, "found wrong ic");
      return ic;
    } else {
      return NULL; // perform, dll call, etc.
    }
  }

  return NULL; // doesn't have InterpretedIC
}

CompiledIC* frame::current_compiledIC() const {
  return is_compiled_frame()
           ? CompiledIC_from_return_addr(pc()) // may fail if current frame isn't at a send -- caller must know
           : NULL;
}

bool frame::is_entry_frame() const {
  return pc() == StubRoutines::return_from_Delta();
}

bool frame::has_next_Delta_fp() const {
  return at(frame_next_Delta_fp_offset) != 0;
}

void** frame::next_Delta_fp() const {
  return (void**)at(frame_next_Delta_fp_offset);
}

oop* frame::next_Delta_sp() const {
  return (oop*)at(frame_next_Delta_sp_offset);
}

bool frame::is_first_frame() const {
  return is_entry_frame() && !has_next_Delta_fp();
}

bool frame::is_first_delta_frame() const {
  // last Delta frame isn't necessarily is_first_frame(), so check is a bit more complicated
  // [I don't understand why, but the first Delta frame of a process isn't an entry frame  -Urs 2/96]
  frame s;
  for (s = sender(); !(s.is_delta_frame() || s.is_first_frame()); s = s.sender())
    ;
  return s.is_first_frame();
}

char* frame::print_name() const {
  if (is_interpreted_frame())
    return "interpreted";
  if (is_compiled_frame())
    return "compiled";
  if (is_deoptimized_frame())
    return "deoptimized";
  return "C";
}

void frame::print() const {
  mystd->print("[%s frame: fp = %#lx, sp = %#lx, pc = %#lx", print_name(), fp(), sp(), pc());
  if (is_compiled_frame()) {
    mystd->print(", nm = %#x", findNMethod(pc()));
  } else if (is_interpreted_frame()) {
    mystd->print(", hp = %#x, method = %#x", hp(), method());
  }
  mystd->print_cr("]");

  if (PrintLongFrames) {
    for (oop* p = sp(); p < (oop*)fp(); p++)
      mystd->print_cr("  - 0x%lx: 0x%lx", p, *p);
  }
}

static void print_context_chain(contextOop con, outputStream* st) {
  if (con) {
    // Print out the contexts chain
    st->print("    context ");
    con->print_value_on(st);
    while (con->has_outer_context()) {
      con = con->outer_context();
      st->print(" -> ");
      con->print_value_on(st);
    }
    st->cr();
  }
}

void frame::print_for_deoptimization(outputStream* st) {
  ResourceMark rm;
  st->print(" - ");
  if (is_interpreted_activation()) {
    st->print("I ");
    interpretedVFrame* vf = (interpretedVFrame*)vframe::new_vframe(this);
    vf->method()->print_value_on(st);
    if (ActivationShowBCI) {
      st->print(" bci=%d ", vf->bci());
    }
    mystd->print_cr(" @ 0x%lx", fp());
    print_context_chain(vf->interpreter_context(), st);
    if (ActivationShowExpressionStack) {
      GrowableArray<oop>* stack = vf->expression_stack();
      for (int index = 0; index < stack->length(); index++) {
        st->print("    %3d: ", index);
        stack->at(index)->print_value_on(st);
        st->cr();
      }
    }
    return;
  }

  if (is_compiled_frame()) {
    st->print("C ");
    compiledVFrame* vf = (compiledVFrame*)vframe::new_vframe(this);
    assert(vf->is_compiled_frame(), "should be compiled vframe");
    vf->code()->print_value_on(st);
    mystd->print_cr(" @ 0x%lx", fp());

    while (true) {
      st->print("    ");
      vf->method()->print_value_on(st);
      mystd->print_cr(" @ %d", vf->scope()->offset());
      print_context_chain(vf->compiled_context(), st);
      if (vf->is_top())
        break;
      vf = (compiledVFrame*)vf->sender();
      assert(vf->is_compiled_frame(), "should be compiled vframe");
    }
    return;
  }

  if (is_deoptimized_frame()) {
    st->print("D ");
    frame_array()->print_value();
    mystd->print_cr(" @ 0x%lx", fp());

    deoptimizedVFrame* vf = (deoptimizedVFrame*)vframe::new_vframe(this);
    assert(vf->is_deoptimized_frame(), "should be deoptimized vframe");
    while (true) {
      st->print("    ");
      vf->method()->print_value_on(st);
      mystd->cr();
      print_context_chain(vf->deoptimized_context(), st);
      if (vf->is_top())
        break;
      vf = (deoptimizedVFrame*)vf->sender();
      assert(vf->is_deoptimized_frame(), "should be deoptimized vframe");
    }
    return;
  }

  st->print("E foreign frame @ 0x%lx", fp());
}

void frame::layout_iterate(FrameLayoutClosure* blk) {
  if (is_interpreted_activation()) {
    oop* eos = temp_addr(0);
    for (oop* p = sp(); p <= eos; p++)
      blk->do_stack(eos - p, p);
    blk->do_hp(hp_addr());
    blk->do_receiver(receiver_addr());
    blk->do_link(link_addr());
    blk->do_return_addr(return_addr_addr());
  }
}

bool frame::has_interpreted_float_marker() const {
  return oop(at(interpreted_frame_float_magic_offset)) == Floats::magic_value();
}

bool frame::has_compiled_float_marker() const {
  return oop(at(compiled_frame_magic_oop_offset)) == Floats::magic_value();
}

bool frame::oop_iterate_interpreted_float_frame(OopClosure* blk) {
  methodOop m = methodOopDesc::methodOop_from_hcode(hp());
  // Return if this activation has no floats (the marker is conservative)
  if (!m->has_float_temporaries())
    return false;

  // Everything at or below the float section (declared float temporaries plus
  // the float expression stack reserved by the float_allocate bytecode) is raw
  // float data that the interpreter never initializes, so it must not be
  // scanned as oops. Only the initialized stack temporaries above the float
  // section - [float_section_start .. temp0] - hold oops, one per delta slot.
  for (oop* q = (oop*)addr_at(m->float_section_start_offset()); q <= temp_addr(0); q += oopsPerSlot) {
    blk->do_oop(q);
  }

  // The receiver
  blk->do_oop(receiver_addr());

  return true;
}

bool frame::oop_iterate_compiled_float_frame(OopClosure* blk) {
  warning("oop_iterate_compiled_float_frame not implemented");
  return false;
}

void frame::oop_iterate(OopClosure* blk) {
  if (is_interpreted_activation()) {
    if (has_interpreted_float_marker() && oop_iterate_interpreted_float_frame(blk))
      return;

    // lprintf("Frame: fp = %#lx, sp = %#lx]\n", fp(), sp());
    for (oop* p = sp(); p <= temp_addr(0); p += oopsPerSlot) {
      blk->do_oop(p);
    }
    // lprintf("\t{%#lx}: ", receiver_addr());
    // (*receiver_addr())->short_print();
    // lprintf("\n");
    blk->do_oop(receiver_addr());
    return;
  }

  if (is_compiled_frame()) {
    if (has_compiled_float_marker() && oop_iterate_compiled_float_frame(blk))
      return;

    // All oops are [sp..fp[
    for (oop* p = sp(); p < (oop*)fp(); p++) {
      blk->do_oop(p);
    }
    return;
  }

  if (is_entry_frame()) {
    // Need to iterate over the arguments passed to the frame called by the entry frame,
    // but not the rest of the cruft (esi, edi, last sp and last fp) - slr 09/08.
    for (oop* p = sp(); p < (oop*)fp() - 4 * oopsPerSlot; p += oopsPerSlot) {
      blk->do_oop(p);
    }
    return;
  }

  if (is_deoptimized_frame()) {
    // Expression stack
    oop* end = (oop*)fp() + frame_real_sender_sp_offset;
    // All oops are [sp..end[
    for (oop* p = sp(); p < end; p++) {
      blk->do_oop(p);
    }
    blk->do_oop((oop*)frame_array_addr());
    return;
  }
}

bool frame::follow_roots_interpreted_float_frame() {
  methodOop m = methodOop(hp());
  assert(m->is_method(), "must be method");
  // Return if this activation has no floats (the marker is conservative)
  if (!m->has_float_temporaries())
    return false;

  // See oop_iterate_interpreted_float_frame: the float region holds raw double
  // data never initialized by the interpreter and must be skipped. The
  // initialized stack temporaries are one oop per delta slot.
  for (oop* q = (oop*)addr_at(m->float_section_start_offset()); q <= temp_addr(0); q += oopsPerSlot) {
    MarkSweep::follow_root(q);
  }

  // The receiver
  MarkSweep::follow_root(receiver_addr());

  return true;
}

bool frame::follow_roots_compiled_float_frame() {
  warning("follow_roots_compiled_float_frame not implemented");
  return true;
}

void frame::follow_roots() {
  if (is_interpreted_activation()) {
    if (has_interpreted_float_marker() && follow_roots_interpreted_float_frame())
      return;

    // Follow the roots of the frame
    for (oop* p = sp(); p <= temp_addr(0); p += oopsPerSlot) {
      MarkSweep::follow_root(p);
    }
    MarkSweep::follow_root((oop*)hp_addr());
    MarkSweep::follow_root(receiver_addr());
    return;
  }

  // Processes::follow_roots() runs from mark_sweep_phase1(), inside the window
  // opened by Processes::convert_hcode_pointers(), which has just rewritten
  // this frame's hp slot from an interior bytecode address into the method's
  // tagged oop.  is_interpreted_activation() is value-based and is therefore
  // false for exactly those frames (the object base lies below codes(), so
  // method_from_hp()'s containment test fails), so the temporaries and the
  // receiver of a converted frame are *not* traced here.  convert_hcode_pointer()
  // marks them while the hybrid code pointer is still valid, i.e. before
  // Universe::oops_do() reverses the heap's headers; see the comment there.
  //
  // Only the hp slot is left to do here: it now holds the method's object base,
  // a real oop, and following it here is what keeps restore_hcode_pointer()
  // adding its offset to the post-compaction address.
  if (is_interpreted_frame()) {
    MarkSweep::follow_root((oop*)hp_addr());
    return;
  }

  if (is_compiled_frame()) {
    if (has_compiled_float_marker() && follow_roots_compiled_float_frame())
      return;

    for (oop* p = sp(); p < (oop*)fp(); p++)
      MarkSweep::follow_root(p);
    return;
  }

  if (is_entry_frame()) {
    //for (oop* p = sp(); p < (oop*)fp(); p++) {
    // %hack
    //   MarkSweep::follow_root(p);
    //}
    // Should be the following to match oop_iterate() (used by scavenge) slr - 11/09
    // %TODO how to test?
    for (oop* p = sp(); p < (oop*)fp() - 4 * oopsPerSlot; p += oopsPerSlot) {
      MarkSweep::follow_root(p);
    }
    return;
  }

  if (is_deoptimized_frame()) {
    // Expression stack
    oop* end = (oop*)fp() + frame_real_sender_sp_offset;
    for (oop* p = sp(); p < end; p++)
      MarkSweep::follow_root(p);
    MarkSweep::follow_root((oop*)frame_array_addr());
    return;
  }
}

void frame::convert_hcode_pointer() {
  if (!is_interpreted_activation()) {
    // Keep the offset FIFO balanced.  restore_hcode_pointer() consumes exactly
    // one entry per frame visited by the matching restore walk and cannot
    // re-derive this decision for itself: by the time it runs, the activation
    // test fails for the frames this pass *did* convert (see below), so it
    // cannot be used to tell a converted frame from an untouched one.
    MarkSweep::set_hcode_pending_base(NULL);
    MarkSweep::add_hcode_offset(-1);
    return;
  }
  // Adjust hcode pointer to object start
  u_char* h = hp();
  u_char* obj = (u_char*)as_memOop(Universe::object_start((oop*)h));
  set_hp(obj);
  // Save the offset, together with the base it was measured from so that
  // restore_hcode_pointer() can verify this exact frame still holds that base.
  MarkSweep::set_hcode_pending_base(obj);
  MarkSweep::add_hcode_offset(h - obj);

  // Mark the rest of this frame *here*, while the hybrid code pointer is still
  // valid.  Once set_hp() above has run, is_interpreted_activation() no longer
  // recognises the frame, so frame::follow_roots() cannot reach its temporaries
  // or its receiver later in phase1 -- and moving this after
  // Universe::oops_do(&follow_root) is not an option either, because that pass
  // reverses every marked object's header into its marking pointer, after which
  // method_from_hp()'s is_mark() test fails for all of them.  Leaving the frame
  // unmarked here is what produced "VM Error: klass 0x... isn't a klass" from
  // InterpretedIC::inline_cache_miss: compaction moved the receiver and the
  // temporaries without updating the frame.
  methodOop m = method();
  if (has_interpreted_float_marker() && m != NULL && m->has_float_temporaries()) {
    // Everything at or below the float section is raw float data that the
    // interpreter never initializes, so it must not be scanned as oops; the
    // oop-carrying temporaries sit between it and temp0.
    for (oop* q = (oop*)addr_at(m->float_section_start_offset()); q <= temp_addr(0); q += oopsPerSlot) {
      MarkSweep::follow_root(q);
    }
  } else {
    for (oop* p = sp(); p <= temp_addr(0); p += oopsPerSlot) {
      MarkSweep::follow_root(p);
    }
  }
  MarkSweep::follow_root(receiver_addr());
  // if (WizardMode) lprintf("[0x%lx+%d]\n", obj, h - obj);
}

void frame::restore_hcode_pointer() {
  // Do NOT re-test is_interpreted_activation() here.  convert_hcode_pointer()
  // has already rewritten this frame's hp slot to the method's *object base*,
  // which lies below codes(), so method_from_hp()'s [codes(), codes_end())
  // containment test fails and the test now reports "not an activation" for
  // precisely the frames that were converted.  Re-testing therefore left
  // MarkSweep::hcode_pos stranded mid-queue, so the next mark-sweep replayed
  // stale offsets onto the wrong frames and corrupted bytecode pointers.  The
  // sentinel recorded by convert is the authoritative answer.
  void* base = NULL;
  int offset = MarkSweep::next_hcode_offset(&base);
  if (offset < 0)
    return;
  if (getenv("ST_TRACE_RESTORE") != NULL && base != NULL)
    lprintf("RESTORE fp=%p base=%p offset=%d hp=%p\n", (void*)fp(), base, offset, (void*)hp());
  // Readjust hcode pointer
  u_char* obj = hp();
  // Verify the round trip.  The offset is relative to the method object base,
  // and by now the slot holds that method's *post-compaction* base (frame::
  // follow_roots() followed hp_addr() as a real oop precisely so compaction
  // would update it), so `base` -- the pre-compaction base convert recorded --
  // is expected to differ from `obj` and is not itself an error.  What must
  // hold is that obj is a method and obj+offset lands inside its bytecodes.
  // A failure here means the offset was replayed onto a frame that is not the
  // one it was measured from, i.e. the convert and restore walks disagree.
  // Diagnosed rather than fatal: the real bug is upstream, so report it and
  // carry on to keep the process alive long enough to be inspected.
  if (base != NULL) {
    methodOop m = method_from_hp_or_base();
    if (m == NULL) {
      warning("restore_hcode_pointer: hp %p (recorded base %p) does not "
              "resolve to a method; offset %d will be misapplied",
              obj, base, offset);
    } else if ((u_char*)obj + offset < m->codes() || (u_char*)obj + offset >= m->codes_end()) {
      warning("restore_hcode_pointer: hp %p + offset %d is outside "
              "[codes %p, codes_end %p) of the method it resolves to",
              obj, offset, m->codes(), m->codes_end());
    }
  }
  // if (WizardMode) lprintf("[0x%lx+%d]\n", obj, offset);
  set_hp(obj + offset);
}

class VerifyOopClosure : public OopClosure {
public:
  frame* fr;
  void do_oop(oop* o) {
    oop obj = *o;
    if (!obj->verify()) {
      lprintf("Verify failed in frame:\n");
      fr->print();
    }
  }
};

void frame::verify() const {
  if (fp() == NULL)
    fatal("fp cannot be NULL");
  if (sp() == NULL)
    fatal("sp cannot be NULL");
  VerifyOopClosure blk;
  blk.fr = (frame*)this;
  ((frame*)this)->oop_iterate(&blk);
}

frame frame::sender() const {
  frame result;
  if (is_entry_frame()) {
    // Delta frame called from C; skip all C frames and return top C
    // frame of that chunk as the sender
    assert(has_next_Delta_fp(), "next Delta fp must be non zero");
    assert(next_Delta_fp() > _fp, "must be above this frame on stack");
    result = frame(next_Delta_sp(), next_Delta_fp());
  } else if (is_deoptimized_frame()) {
    result = frame(real_sender_sp(), link(), return_addr());
  } else {
#ifdef DELTA_BACKEND_AARCH64
    // A block activation consumed extra delta slots (call_C's saved LR plus the
    // slots setupBlockValueFrame reserved for the block's return), so the
    // caller's sp is not at the usual sender_sp offset.
    //
    // This test must NOT go through is_interpreted_activation().  That predicate
    // is value-based on hp(), and convert_hcode_pointer() deliberately rewrites
    // hp() to the method's object base -- which lies below codes(), so
    // method_from_hp()'s [codes(), codes_end()) containment test then fails and
    // is_interpreted_activation() reports false for exactly the frames that
    // were converted.  Asking it here therefore made frame::sender() pick
    // sender_sp() instead of block_activation_sender_sp() during the restore
    // walk for block activations the convert walk had handled correctly, so the
    // two walks enumerated *different* frames in the same order and the
    // hcode-offset FIFO stayed balanced by count while replaying each offset
    // onto the wrong frame.
    //
    // The block-ness of a frame does not change across the mark-sweep cycle, so
    // the GC walks use the mark-phase-tolerant resolution below instead.  It
    // deliberately does NOT apply to the general frame::sender() path: the heap
    // lookup is far too expensive to repeat on every sender() during a normal
    // frame walk (and re-enters the GC's own object_start_checked()), so only
    // the GC enables it.  See MarkSweep::in_hcode_walk().
    if (MarkSweep::in_hcode_walk()) {
      methodOop m = method_from_hp_or_base();
      if (m != NULL && m->is_blockMethod()) {
        result = frame(block_activation_sender_sp(), link(), return_addr());
        return result;
      }
    }
#endif
    result = frame(sender_sp(), link(), return_addr());
  }
  return result;
}

frame frame::delta_sender() const {
  frame s;
  for (s = sender(); !s.is_delta_frame(); s = s.sender())
    ;
  return s;
}

bool frame::should_be_deoptimized() const {
  if (!is_compiled_frame())
    return false;
  nmethod* nm = code();
  if (TraceApplyChange) {
    mystd->print("checking (%s) ", nm->is_marked_for_deoptimization() ? "true" : "false");
    nm->print_value_on(mystd);
    mystd->cr();
  }
  return nm->is_marked_for_deoptimization();
}
