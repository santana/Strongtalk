/* Copyright 1994-2006 Sun Microsystems, Inc. All rights reserved.
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

#include "memory/error.hpp"
#include "memory/util.hpp"
#include "runtime/debug.hpp"
#include "runtime/os.hpp"
#include "utilities/lprintf.hpp" // temporary CODE_MEM layout dump

// Shared executable-code arena.
//
// Every region of generated/JIT machine code -- jump table (main table and
// block chunks), nmethod zone, PIC zone, interpreter, stub routines, generated
// primitives -- is carved from a single reservation so that any two code
// addresses are within +/-2GB of each other.
//
// On x86-64 the backend emits 32-bit PC-relative branches (E8 call / E9 jmp
// rel32) between code in different "owned" regions: jump table entries branch
// to nmethods and StubRoutines::compile_block_entry, nmethods call back into
// stub routines and interpreter entries, PIC stubs reference stub routines,
// and so on. If each region were its own independent mmap, ASLR could place
// them arbitrarily far apart and those relative branches would either fire the
// range guard in jumpTable.cpp (boot fatal) or, worse, silently mis-jump
// (the "first JIT'd method executes garbage" symptom of the Linux/macOS/
// Windows port). One arena keeps every relative displacement representable.
//
// The same arena removes the arm64 jump-table "address bucket" jitter noted
// in the port notes: the absolute ldr/br stubs no longer move between
// independent executable mappings.

static char* arena_base = NULL; // first byte of the arena reservation
static char* arena_cur = NULL; // next free byte inside the arena
static char* arena_end = NULL; // one past the end of the reservation

// Size of the arena reservation. Mirrors the sizes the allocators derive from
// the runtime flags (see Universe::current_sizes / memory/spaceSize.cpp):
//   nmethod zone: roundTo(CodeSize * K, page)
//   PIC zone:     roundTo(PICSize * K, page)
//   jump table:   JumpTableSize entries (24 bytes each -- the AArch64 stub
//                 layout is the larger of the two) plus runtime block chunks.
// A generous fixed headroom covers the interpreter, stub routines, generated
// primitives and block-closure chunks. The reservation only allocates virtual
// address space (pages are committed as they are touched), so over-reserving
// is cheap; nominal need is roughly 22 MB with the default flags and ~3 MB
// under the test rc file.
static int code_arena_capacity() {
  const int page = os::vm_page_size();
  const int jump_entry = 24; // worst-case jumpTableEntry size (AArch64 layout)
  const int zone = (CodeSize * K + page - 1) / page * page;
  const int pic = (PICSize * K + page - 1) / page * page;
  const int jump = JumpTableSize * jump_entry;
  const int headroom = 4 * M;
  int capacity = zone + pic + jump + headroom;
  if (capacity < 16 * M)
    capacity = 16 * M;
  return capacity;
}

char* os::code_arena_base() {
  return arena_base;
}

char* os::code_memory(int size) {
  if (size <= 0)
    fatal("os::code_memory: bad size");
  if (arena_base == NULL) {
    // First allocation: reserve the entire arena from the platform primitive.
    arena_base = os::exec_memory(code_arena_capacity());
    if (arena_base == NULL || arena_base == (char*)-1)
      fatal("os::code_memory: unable to reserve the executable code arena");
    arena_cur = arena_base;
    arena_end = arena_base + code_arena_capacity();
  }

  // 16-byte alignment suffices for every consumer: jump table entries are
  // aligned to oopSize and the nmethod/PIC heaps round the returned base up
  // to their own block size (their request already includes the +blockSize
  // slack that absorbs that rounding).
  const int align = 16;
  char* result = (char*)(((intptr_t)arena_cur + align - 1) & ~(intptr_t)(align - 1));
  if (result + size > arena_end)
    fatal1("os::code_memory: executable code arena exhausted (need %ld bytes)", size);
  arena_cur = result + size;
  lprintf("CODE_MEM layout: base=%p cur=%p size=%d result=%p\n", (void*)arena_base, (void*)result, size, (void*)result);
  return result;
}

void os::code_memory_free(void* p) {
  // The arena is a bump allocator; its regions live for the life of the VM,
  // so individual frees are not supported. (Jump-table chunks that are
  // released on nmethod invalidation simply keep their address space.)
  Unused(p);
}