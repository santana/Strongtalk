/* Copyright 1994 - 1996, LongView Technologies, L.L.C. $Revision: 1.4 $ */
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

#ifndef _NATIVE_INSTRUCTION_HPP
#define _NATIVE_INSTRUCTION_HPP
#include "topIncludes/architecture.hpp"

#include "memory/allocation.hpp"
#include "oops/oopsHierarchy.hpp"

// The base class for differnt kinds of native instruction abstractions.
// Provides the primitive operations to manipulate code relative to this.

class NativeInstruction : ValueObj {
protected:
  char* addr_at(int offset) const { return (char*)this + offset; }

  char char_at(int offset) const { return *addr_at(offset); }
  int long_at(int offset) const { return *(int*)addr_at(offset); }
  oop oop_at(int offset) const { return *(oop*)addr_at(offset); }

  void set_char_at(int offset, char c) { *addr_at(offset) = c; }
  void set_long_at(int offset, int i) { *(int*)addr_at(offset) = i; }
  void set_oop_at(int offset, oop o) { *(oop*)addr_at(offset) = o; }
};

// An abstraction for accessing/manipulating native call imm32 instructions.
// (used to manipulate inline caches, primitive & dll calls, etc.)

class NativeCall : public NativeInstruction {
public:
#ifdef DELTA_BACKEND_AARCH64
  // AArch64 "call to absolute address" is emitted as:
  //   ldr x16, [pc, #8]   (4 bytes)
  //   b .+12              (4 bytes)
  //   .quad target        (8 bytes)   <-- patchable absolute target literal
  //   blr x16             (4 bytes)
  // The NativeCall 'this' points at the return address (the instruction after
  // the blr); the patchable literal lives 12 bytes before it and holds an
  // absolute address. Unlike x86 the target is absolute, not relative.
  enum AArch64_specific_constants {
    instruction_code = 0x58000050, // ldr x16, [pc, #8] (exact emitted opcode)
    instruction_size = 20,
    instruction_offset = -20,
    literal_offset = -12, // '.quad target' is 12 bytes before the return addr
    displacement_offset = -12, // nativeCall_from_relocInfo reloc points at the literal
    return_address_offset = 0,
  };

  char* instruction_address() const { return addr_at(instruction_offset); }
  char* next_instruction_address() const { return addr_at(return_address_offset); }
  // The literal stores an absolute pointer on AArch64.
  intptr_t displacement() const { return *(intptr_t*)(addr_at(literal_offset)); }
  char* return_address() const { return addr_at(return_address_offset); }
  char* destination() const { return (char*)displacement(); }
  void set_destination(char* dest) { *(intptr_t*)(addr_at(literal_offset)) = (intptr_t)dest; }

  void verify();
  void print();
#else
  enum Intel_specific_constants {
    instruction_code = 0xE8,
    instruction_size = 5,
    instruction_offset = -5,
    displacement_offset = -4,
    return_address_offset = 0,
  };

  char* instruction_address() const { return addr_at(instruction_offset); }
  char* next_instruction_address() const { return addr_at(return_address_offset); }
  int displacement() const { return long_at(displacement_offset); }
  char* return_address() const { return addr_at(return_address_offset); }
  char* destination() const { return return_address() + displacement(); }
  void set_destination(char* dest) { set_long_at(displacement_offset, dest - return_address()); }

  void verify();
  void print();
#endif

  // Creation
  friend NativeCall* nativeCall_at(char* address);

  friend NativeCall* nativeCall_from_return_address(char* return_address);

  friend NativeCall* nativeCall_from_relocInfo(char* displacement_address);
};

inline NativeCall* nativeCall_at(char* address) {
  NativeCall* call = (NativeCall*)(address - NativeCall::instruction_offset);
#ifdef ASSERT
  call->verify();
#endif
  return call;
}

inline NativeCall* nativeCall_from_return_address(char* return_address) {
  NativeCall* call = (NativeCall*)(return_address - NativeCall::return_address_offset);
#ifdef ASSERT
  call->verify();
#endif
  return call;
}

inline NativeCall* nativeCall_from_relocInfo(char* displacement_address) {
  NativeCall* call = (NativeCall*)(displacement_address - NativeCall::displacement_offset);
#ifdef ASSERT
  call->verify();
#endif
  return call;
}

// An abstraction for accessing/manipulating native mov reg, imm32 instructions.
// (used to manipulate inlined 32bit data dll calls, etc.)

class NativeMov : public NativeInstruction {
public:
  enum Intel_specific_constants {
    instruction_code = 0xB8,
    instruction_size = 5,
    instruction_offset = 0,
    data_offset = 1,
    next_instruction_offset = 5,
    register_mask = 0x07,
  };

  char* instruction_address() const { return addr_at(instruction_offset); }
  char* next_instruction_address() const { return addr_at(next_instruction_offset); }
  intptr_t data() const { return long_at(data_offset); }
  void set_data(intptr_t x) { set_long_at(data_offset, x); }

  void verify();
  void print();

  // Creation
  friend NativeMov* nativeMov_at(char* address);
};

inline NativeMov* nativeMov_at(char* address) {
  NativeMov* test = (NativeMov*)(address - NativeMov::instruction_offset);
#ifdef ASSERT
  test->verify();
#endif
  return test;
}

// An abstraction for accessing/manipulating native test eax, imm32 instructions.
// (used to manipulate inlined 32bit data for NLRs, dll calls, etc.)

class NativeTest : public NativeInstruction {
public:
#ifdef DELTA_BACKEND_AARCH64
  // On AArch64 there is no `test eax, imm32`; the IC info is a 4-byte
  // hint-encoded NOP (see MacroAssembler::ic_info) located AT the return
  // address, so the info word is at offset 0 and the next instruction four
  // bytes later.
  enum AArch64_specific_constants {
    instruction_size = 4,
    instruction_offset = 0,
    data_offset = 0,
    next_instruction_offset = 4,
  };
#else
  enum Intel_specific_constants {
    instruction_code = 0xA9,
    instruction_size = 5,
    instruction_offset = 0,
    data_offset = 1,
    next_instruction_offset = 5,
  };
#endif

  char* instruction_address() const { return addr_at(instruction_offset); }
  char* next_instruction_address() const { return addr_at(next_instruction_offset); }
  intptr_t data() const { return long_at(data_offset); }
  void set_data(intptr_t x) { set_long_at(data_offset, x); }

  void verify();
  void print();

  // Creation
  friend NativeTest* nativeTest_at(char* address);
};

inline NativeTest* nativeTest_at(char* address) {
  NativeTest* test = (NativeTest*)(address - NativeTest::instruction_offset);
#ifdef ASSERT
  test->verify();
#endif
  return test;
}

// An abstraction for accessing/manipulating native test eax, imm32 instructions that serve as IC info.

class IC_Info : public NativeTest {
public:
#ifdef DELTA_BACKEND_AARCH64
  // The info word is a hint-encoded NOP carrying the (up to 6) flag bits in
  // hint-imm positions 5..11; MacroAssembler::ic_info remaps imm values
  // 0x07..0x0f (+9) to dodge the PAC pointer-auth aliases, mirror that here.
  static int dec_flags(long v) {
    long f = (v >> 5) & 0x3f;
    return f >= 16 ? (int)(f - 9) : (int)f;
  }
  static long enc_flags(int flags) {
    long f = flags & 0x3f;
    return f >= 7 ? f + 9 : f;
  }
  enum AArch64_specific_constants {
    info_offset = 0,
    number_of_flags = 8, // for decode_ic_info (stubRoutines.cpp); NLR is register-homed on AArch64
    flags_mask = (1 << number_of_flags) - 1, // x86 reader uses this; kept for parity
  };

  char* NLR_target() const { return instruction_address(); } // NLR home is a register on AArch64
  int flags() const { return dec_flags(long_at(info_offset)); }
  void set_flags(int flags) {
    long w = long_at(info_offset);
    set_long_at(info_offset, (w & ~0xfe0L) | (enc_flags(flags) << 5));
  }
#else
  enum Intel_specific_constants {
    info_offset = data_offset,
    number_of_flags = 8,
    flags_mask = (1 << number_of_flags) - 1,
  };

  char* NLR_target() const { return instruction_address() + (data() >> number_of_flags); }
  int flags() const { return data() & flags_mask; }
  void set_flags(int flags) { set_data((data() & ~flags_mask) | (flags & flags_mask)); }
#endif

  // Creation
  friend IC_Info* ic_info_at(char* address);
};

inline IC_Info* ic_info_at(char* address) {
  return (IC_Info*)nativeTest_at(address);
}
#endif // _NATIVE_INSTRUCTION_HPP
