/* Copyright 1994, 1995 LongView Technologies L.L.C. $Revision: 1.1 $ */
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

#ifndef _MARK_SWEEP_HPP
#define _MARK_SWEEP_HPP

#include "memory/allocation.hpp"

template <class E> class GrowableArray;

// MarkSweep takes care of garbage collection
class OopRelocations;
class MarkSweep : AllStatic {
public:
  static oop collect(oop p = NULL);

  // Call backs
  static void follow_root(oop* p);
  static void reverse_and_push(oop* p);
  static void reverse_and_follow(oop* p);

  static void add_hcode_offset(int offset);
  // Records the base the *next* add_hcode_offset() call's offset was measured
  // from.  frame::convert_hcode_pointer() sets this to the method object base it
  // just stored in the frame's hp slot, so next_hcode_offset() can verify on
  // replay that the slot still holds that base.
  static void set_hcode_pending_base(void* base);
  // Pops the next offset.  When base is non-NULL it receives the method object
  // base that offset was measured from (NULL for a skipped frame), so the caller
  // can verify the frame's hp slot still holds that base before replaying it;
  // see frame::restore_hcode_pointer().
  static int next_hcode_offset(void** base = NULL);

private:
  // the traversal stack used during phase1.
  static GrowableArray<memOop>* stack;
  // the hcode pointer offsets saved before and
  // and retrieved after the garbage collection.
  static GrowableArray<intptr_t>* hcode_offsets;
  static int hcode_pos;
  // The object base each hcode offset was measured from, parallel to
  // hcode_offsets.  -1 offsets store NULL.
  static GrowableArray<void*>* hcode_bases;
  static int hcode_base_pos;
  // Base for the next add_hcode_offset() call; see set_hcode_pending_base().
  static void* hcode_pending_base;
  // resource area for non-aligned oops requiring relocation (eg. in nmethods)
  static OopRelocations* oopRelocations;

private:
  static void mark_sweep_phase1(oop* p);
  static void mark_sweep_phase2();
  static void mark_sweep_phase3();

  static inline memOop reverse(oop* p);

  static void allocate();
  static void deallocate();
  static void trace(char* msg);
};
#endif // _MARK_SWEEP_HPP
