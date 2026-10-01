/* Copyright 1994, 1995 LongView Technologies L.L.C. $Revision: 1.50 $ */
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

#ifndef _FRAME_HPP
#define _FRAME_HPP
#include "topIncludes/architecture.hpp"

#include "memory/allocation.hpp"

class CompiledIC;

// A frame represents a physical stack frame (an activation).  Frames can be
// C or Delta frames, and the Delta frames can be interpreted or compiled.
// In contrast, vframes represent source-level activations, so that one (Delta) frame
// can correspond to multiple deltaVFrames because of inlining.

// Layout of interpreter frame:
//    [locals + expr         ] * <- sp
//    [old frame pointer     ]   <- fp
//    [return pc             ]
//    [previous locals + expr]   <- sender sp

// Layout of deoptimized frame:
//    [&unpack_unoptimized_frame ]		// patched return address
//    [scrap area                ] * <- sp
//    [sender sp                 ]
//    [frame array               ]              // objArrayOop holding the deoptimized frames
//    [old frame                 ]   <- fp	// old frame may skip real frames deoptimized away.
//    [return pc                 ]

// On AArch64 the delta stack slots are 16 bytes (slotSize = 2*oopSize) so the
// offsets below ebp/above ebp are doubled with respect to the x86 layout; the
// link/return pair at fp[0]/fp[1] (pushed by enter as a 16-byte stp) is
// unchanged.
#ifdef DELTA_BACKEND_AARCH64
const int frame_temp_offset = -6; // For interpreter frames only
const int frame_hp_offset = -4; // For interpreter frames only
const int frame_receiver_offset = -2; // For interpreter frames only
const int frame_next_Delta_fp_offset = -2; // For entry frames only; see the Delta entry stubs (interpreterBackend)
const int frame_next_Delta_sp_offset = -4; // For entry frames only; see the Delta entry stubs (interpreterBackend)
#else
const int frame_temp_offset = -3; // For interpreter frames only
const int frame_hp_offset = -2; // For interpreter frames only
const int frame_receiver_offset = -1; // For interpreter frames only
const int frame_next_Delta_fp_offset = -1; // For entry frames only; see the Delta entry stubs (interpreterBackend)
const int frame_next_Delta_sp_offset = -2; // For entry frames only; see the Delta entry stubs (interpreterBackend)
#endif
const int frame_link_offset = 0;
const int frame_return_addr_offset = 1;
const int frame_arg_offset = 2;
const int frame_sender_sp_offset = 2;

const int frame_real_sender_sp_offset = -2; // For deoptimized frames only
const int frame_frame_array_offset = -1; // For deoptimized frames only

const int interpreted_frame_float_magic_offset = frame_temp_offset - oopsPerSlot;
const int compiled_frame_magic_oop_offset = -1;
const int minimum_size_for_deoptimized_frame = 4;

class frame : ValueObj {
private:
  oop* _sp; // stack pointer
  void** _fp; // frame pointer
  char* _pc; // program counter

public:
  // Constructors
  frame() {}
  frame(oop* sp, void* fp, char* pc) {
    _sp = sp;
    _fp = static_cast<void**>(fp);
    _pc = pc;
  }

  frame(oop* sp, void* fp) {
    _sp = sp;
    _fp = static_cast<void**>(fp);
    _pc = (char*)sp[-1];
  }

  // accessors for the instance variables
  oop* sp() const { return _sp; }
  void** fp() const { return _fp; }
  char* pc() const { return _pc; }

  // patching operations
  void patch_pc(char* pc); // patch the return address of the frame below.
  void patch_fp(void** fp); // patch the link of the frame below.

  void** addr_at(int index) const { return &fp()[index]; }
  void* at(int index) const { return *addr_at(index); }

private:
  void** link_addr() const { return addr_at(frame_link_offset); }
  char** return_addr_addr() const { return (char**)addr_at(frame_return_addr_offset); }

  // support for interpreter frames
#ifdef DELTA_BACKEND_AARCH64
  oop* receiver_addr() const { return (oop*)addr_at(frame_receiver_offset); }
  u_char** hp_addr() const { return (u_char**)addr_at(frame_hp_offset); }
  oop* arg_addr(int off) const { return (oop*)addr_at(frame_arg_offset + 2 * off); }
#else
  oop* receiver_addr() const { return (oop*)addr_at(frame_receiver_offset); }
  u_char** hp_addr() const { return (u_char**)addr_at(frame_hp_offset); }
  oop* arg_addr(int off) const { return (oop*)addr_at(frame_arg_offset + off); }
#endif

public:
  // returns the stack pointer of the calling frame
  oop* sender_sp() const { return (oop*)addr_at(frame_sender_sp_offset); }

#ifdef DELTA_BACKEND_AARCH64
  // Stack pointer of the frame that activated a *block*.
  //
  // A block is not activated by a plain call: _block_entry is reached from a
  // primitiveValue primitive, which first lets call_C save the return address
  // in a delta slot of its own and then has setupBlockValueFrame reserve
  // (nArgs+1) slots for the block's return bytecode to pop (interpreter.cpp
  // generate_primitiveValue / setupBlockValueFrame).  enter() then builds the
  // block frame on top of all of that, so the caller's sp sits
  //
  //     fp + 2*oopSize (enter's link/RA) + (nArgs+1)*slotSize (reserved)
  //         + slotSize (call_C's saved LR)
  //
  // above the block's fp, not the usual frame_sender_sp_offset.  Using the
  // ordinary sender_sp() here made the GC scan one uninitialized slot below the
  // caller's real expression stack and dereference a null-address oop.
  oop* block_activation_sender_sp() const;
#endif

  // Link
  void* link() const { return *link_addr(); }
  void set_link(void* addr) { *link_addr() = addr; }

  // Return address
  char* return_addr() const { return *return_addr_addr(); }
  void set_return_addr(char* addr) { *return_addr_addr() = addr; }

  // Receiver
  oop receiver() const { return *receiver_addr(); }
  void set_receiver(oop recv) { *receiver_addr() = recv; }

  // Temporaries
  oop temp(int offset) const { return *temp_addr(offset); }
  void set_temp(int offset, oop obj) { *temp_addr(offset) = obj; }
#ifdef DELTA_BACKEND_AARCH64
  oop* temp_addr(int offset) const { return (oop*)addr_at(frame_temp_offset - 2 * offset); }
#else
  oop* temp_addr(int offset) const { return (oop*)addr_at(frame_temp_offset - offset); }
#endif

  // Arguments
  oop arg(int offset) const { return *arg_addr(offset); }
  void set_arg(int offset, oop obj) { *arg_addr(offset) = obj; }

  // Expressions
#ifdef DELTA_BACKEND_AARCH64
  // delta stack slots are 16 bytes (slotSize = 2*oopSize) on AArch64, so the
  // oops live at even 8-byte indices
  oop expr(int index) const { return ((oop*)sp())[2 * index]; }
#else
  oop expr(int index) const { return ((oop*)sp())[index]; }
#endif

  // Hybrid Code Pointer (interpreted frames only); corresponds to "current PC", not return address
  u_char* hp() const;
  void set_hp(u_char* hp);

  // Returns the method for a valid hp() or NULL if frame not set up yet (interpreted frames only)
  // Used by the profiler which means we must check for
  // valid frame before using the hp value.
  methodOop method() const;

  // compiled code (compiled frames only)
  nmethod* code() const;

private:
  // Resolve hp() to its methodOop, or NULL if hp is not a valid hybrid code
  // pointer.  Shared by method() and is_interpreted_activation(); deliberately
  // does not assert on the frame kind and uses the bounds-checked object lookup.
  methodOop method_from_hp() const;
  // method_from_hp() but also accepting hp == the method's object base, which is
  // what convert_hcode_pointer() stores in the slot mid-mark-sweep.  frame::
  // sender() uses this so the convert and restore walks enumerate the same
  // frames; see the comment there.
  methodOop method_from_hp_or_base() const;

  // Float support
  inline bool has_interpreted_float_marker() const;
  bool oop_iterate_interpreted_float_frame(OopClosure* blk);
  bool follow_roots_interpreted_float_frame();

  inline bool has_compiled_float_marker() const;
  bool oop_iterate_compiled_float_frame(OopClosure* blk);
  bool follow_roots_compiled_float_frame();

public:
  // Accessors for (deoptimized frames only)
  objArrayOop* frame_array_addr() const;
  oop** real_sender_sp_addr() const;

  objArrayOop frame_array() const;
  void set_frame_array(objArrayOop a) { *frame_array_addr() = a; }

  oop* real_sender_sp() const { return *real_sender_sp_addr(); }
  void set_real_sender_sp(oop* addr) { *real_sender_sp_addr() = addr; }

  // returns the frame size in oops
  int frame_size() const { return sender_sp() - sp(); }

  // returns the the sending frame
  frame sender() const;
  // returns the the sending Delta frame, skipping any intermediate C frames
  // NB: receiver must not be first frame
  frame delta_sender() const;

  // tells whether there is another chunk of Delta stack above (entry frames only)
  bool has_next_Delta_fp() const;
  // returns the next C entry frame (entry frames only)
  void** next_Delta_fp() const;
  oop* next_Delta_sp() const;

  bool is_first_frame() const; // oldest frame? (has no sender)
  bool is_first_delta_frame() const; // same for Delta frame

  // testers
  bool is_interpreted_frame() const;
  // is_interpreted_frame() *and* a valid hybrid code pointer, i.e. the frame
  // really is a method activation rather than a C frame in an entry chunk that
  // happens to carry an interpreter return address.  Costs an object lookup,
  // so it is for the GC frame walks, not the send hot path.
  bool is_interpreted_activation() const;
  bool is_compiled_frame() const;
  bool is_delta_frame() const { return is_interpreted_frame() || is_compiled_frame(); }

  bool should_be_deoptimized() const;
  bool is_entry_frame() const; // Delta frame called from C?
  bool is_deoptimized_frame() const;

  // inline caches
  IC_Iterator* sender_ic_iterator() const; // sending IC (NULL if entry frame or if a perform rather than a send)
  IC_Iterator* current_ic_iterator() const; // current IC (will break if not at a send or perform)
  InterpretedIC* current_interpretedIC() const; // current IC in this frame; NULL if !is_interpreted_frame
  CompiledIC* current_compiledIC() const; // current IC in this frame; NULL if !is_compiled_frame

  // Iterators
  void oop_iterate(OopClosure* blk);
  void layout_iterate(FrameLayoutClosure* blk);

  // For debugging
private:
  char* print_name() const;

public:
  void verify() const;
  void print() const;

  // Prints the frame in a format useful when debugging deoptimization.
  void print_for_deoptimization(outputStream* st);

  // Garbage collection operations
  void follow_roots();
  void convert_hcode_pointer();
  void restore_hcode_pointer();

  // Returns the size of a number of interpreter frames in words.
  // This is used during deoptimization.
  static int interpreter_stack_size(int number_of_frames, int number_of_temporaries_and_locals) {
    return number_of_frames * interpreter_frame_size(0) + oopsPerSlot * number_of_temporaries_and_locals;
  }

  // Returns the word size of an interpreter frame.
  //
  // frame_temp_offset is expressed in oop indices, but on a backend whose delta
  // stack slot is wider than one oop (AArch64: slotSize = 2*oopSize) the span
  // from temp0 to the frame link covers twice as many *words*, and so does each
  // temporary.  Both terms must therefore scale by oopsPerSlot: leaving them
  // unscaled makes DeltaProcess::unpack_frame() advance current_sp by half a
  // frame per step on AArch64 (desynchronising it from the real stack and
  // yielding garbage vframes), and makes interpreter_stack_size() reserve half
  // the stack a new process needs.
  static int interpreter_frame_size(int locals) {
    return oopsPerSlot * (frame_return_addr_offset - frame_temp_offset + locals);
  }
};

// True if pc falls within the generated primitives' code buffer.  Kept out of
// frame.hpp's include graph (the primitives header pulls in heavy VM headers);
// implemented in prims/generatedPrimitives.cpp.
bool is_in_generated_primitives_code(char* pc);
#endif // _FRAME_HPP
