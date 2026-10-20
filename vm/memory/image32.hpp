/*
Copyright (c) 2026, Gerardo Santana Gomez Garrido.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following
   disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
   following disclaimer in the documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE
*/

// Temporary 32-bit image reader bridge for Plan A.
//
// `strongtalk.bst` was written by a 32-bit VM (4-byte oops, 8-byte object header:
// mark + klass). The VM is now 64-bit-only (8-byte oops, 16-byte header).
//
// This module isolates all 32-bit image-reading knowledge. It is intended to be
// deleted once a native 64-bit image exists (see PLAN-64BIT-IMAGE.md Plan A,
// Phases 5-6).
//
// The bootstrap in vm/runtime/bootstrap.cpp reads the image textually and
// allocates objects via Universe::allocate_tenured(size), where size is a word
// count. For most objects the word count is architecture-independent (same
// number of fields), so the bootstrap works unchanged. The places where 32-bit
// vs 64-bit differs:
//
//   - Byte containers (byteArray, symbol, doubleByteArray): data bytes are
//     packed into oops; 4 bytes per oop on 32-bit, 8 bytes per oop on 64-bit.
//     Already handled in bootstrap.cpp for types 'd', 'g', 'e'.
//
//   - Object header size: mark(8) + klass(8) = 16 bytes on 64-bit vs
//     mark(4) + klass(4) = 8 bytes on 32-bit. header_size() returns 2 words on
//     both architectures, so this is transparent.
//
//   - klass _non_indexable_size: a field count, same on both architectures.
//
//   - methodOop bytecodes: oop operands are 4 bytes in the 32-bit image, 8 bytes
//     in 64-bit. Already handled by bootstrap.cpp's relocation logic.
//
//   - Bytecode operands that encode 32-bit size values (e.g. Alien's
//     `push_succ_n 3` → SMI 4, should be `push_succ_n 7` → SMI 8). Handled by
//     vm/runtime/imageCompat.cpp patches applied after load.
//
// This module adds a post-load verification and fix-up pass: after bootstrap
// completes and imageCompat patches are applied, walk the old generation and
// validate that every object's size and header are consistent with the 64-bit
// layout. Log any inconsistencies for debugging.

#ifndef _IMAGE32_HPP
#define _IMAGE32_HPP

#include "oops/oop.hpp"
#include "memory/universe.hpp"

// Walk the old generation and verify 64-bit object layout consistency.
// Returns the number of objects found with potential 32-bit remnants.
// Intended as a debugging aid during the bridge phase; removed in Phase 6.
int image32_verify_old_gen();

// Patch any 32-bit-specific bytecode operands not already handled by
// imageCompat.cpp. Returns the number of patches applied.
int image32_patch_bytecode_operands();

// Translate 32-bit raw oop fields in a single object to 64-bit tagged oops.
// Called during bootstrap after each object is read from the 32-bit image.
// Returns the number of fields translated.
int image32_translate_fields(memOop obj);

// Set the 32-bit address -> 64-bit oop mapping from bootstrap.
// Called before the bootstrap object is destroyed so image32_translate_all_fields()
// can look up 32-bit heap addresses.
void image32_set_addr32_map(int* keys, oop* table, int count);

// Walk the bootstrap oop table and translate 32-bit raw oop fields to 64-bit
// tagged oops.  Called during bootstrap after all objects have been read
// but before any interpreted sends execute.  Returns the total number of
// fields translated.
int image32_translate_all_fields(oop* oop_table, int oop_count);

#endif // _IMAGE32_HPP
