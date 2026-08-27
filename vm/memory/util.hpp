/* Copyright 1994, 1995 LongView Technologies L.L.C. $Revision: 1.15 $ */
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

#ifndef _UTIL_HPP
#define _UTIL_HPP

#include "oops/oopsHierarchy.hpp"
#include "topIncludes/types.hpp"

#include <cstdint>
#include <cstdlib>

// conveniences

extern int Indent;
void printIndent();

extern oop catchThisOne;

// utilities

extern "C" void copy_oops_up(oop* from, oop* to, int count);
extern "C" void set_oops(oop* to, int count, oop value = NULL);

char* copy_string(char* s);
char* copy_string(char* s, smi len);

char* copy_c_heap_string(char* s);

// copying oops must be accompanied by record_multistores for remembered set
void copy_oops_down(oop* from, oop* to, int count);
inline void copy_oops(oop* from, oop* to, int count) {
  copy_oops_up(from, to, count); }
inline void copy_oops_overlapping(oop* from, oop* to, int count) {
  if (from < to) copy_oops_down(from + count, to + count, count);
  else if (from > to) copy_oops_up(from, to, count);
}

inline void copy_words(int* from, int* to, int count) {
  copy_oops((oop*) from, (oop*) to, count);
}
inline void set_words(int* from, int count, int value = 0) {
  // set_oops((oop*) from, count, (oop) value);
  while(count--) {
    *from++ = value;
  }
}

inline int min(int a, int b)		{ return a < b ? a : b; }
inline int max(int a, int b)		{ return a > b ? a : b; }
inline int min(int a, int b, int c)	{ return a < b ? min(a, c) : min(b, c); }
inline int max(int a, int b, int c)	{ return a > b ? max(a, c) : max(b, c); }

#define between(p, low, high) ((void*)(p) >= (void*)(low) && (void*)(p) < (void*)(high))

inline void *align(void* p, int alignment) {
  intptr_t number = (intptr_t) p;
  int adjust = alignment - (number%alignment) % alignment;
  return (void*) (number + adjust);
}

// some useful constants

const int K = 1024;
const int M = K * K;

const int oopSize   = sizeof(oop);
const int floatSize = sizeof(double);

// Granularity for rounding byte content in byte-indexed objects (byteArray,
// symbol, doubleByteArray, doubleValueArray).  Must match oopSize so that
// the size computed for a loaded or newly-created object is consistent with
// the allocation formula.  On 64-bit this is 8, matching the original 32-bit
// design where it was 4.
const int image_oop_size = oopSize;

// Delta stack slot size. On AArch64 the delta stack slots are 16 bytes so
// that the stack pointer stays 16-byte aligned for every sp-based load/store
// (AAPCS64 requires SP alignment and macOS raises EXC_ARM_SP_ALIGN on any
// access that uses a misaligned sp). On x86 the slots are one word.
#ifdef DELTA_ASSEMBLER_BACKEND_AARCH64
const int slotSize  = 2 * oopSize;
#else
const int slotSize  = oopSize;
#endif
// Number of oop-sized words per delta stack slot (used to step stack scans).
const int oopsPerSlot = slotSize / oopSize;

inline int byte_size(void* from, void* to) {
  return (char*) to - (char*) from;
}

// If your compiler or lint supports a pragma informing it that a 
// variable is unused, redefine these appropriately
/*
#ifdef __GNUC__
  // GNU 2.6.2 can't disambiguate the overloaded Unused function
  #define Unused(x)	    
#else
  //#endif
  */

  inline void Unused(int x) { x, 0; }
  inline void Unused(void *x) { x, 0; }

#endif // _UTIL_HPP
