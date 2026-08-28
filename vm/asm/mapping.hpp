/*
Copyright (c) 2026, Gerardo Santana Gomez Garrido.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

// This is the single entry point for the mapping. It is split into a
// backend-independent interface (the Mapping class and the global Location
// constants below) and one implementation per target architecture
// (asm/mapping_<arch>.hpp/.cpp, see mapping_x86.* for the x86 backend).
// All code that wants to talk about machine-specific locations should go
// through "asm/mapping.hpp" and not reference the backend files directly.
//
// The backend header must provide the architecture-specific constants that
// the Mapping interface depends on: nofRegisters, nofLocalRegisters,
// nofArgRegisters, first_temp_offset, first_float_offset and the register
// constants (self_reg, temp1, ...). The default backend is x86. To build
// for another backend, define the corresponding DELTA_ASSEMBLER_BACKEND_*
// macro and add an implementation under vm/asm/ following the pattern of
// the x86 one.

#ifndef _MAPPING_HPP
#define _MAPPING_HPP

#ifdef DELTA_COMPILER

#include "asm/location.hpp"
#include "asm/assembler.hpp"
#include "compiler/scope.hpp"
#include "memory/allocation.hpp"

#if defined(DELTA_ASSEMBLER_BACKEND_AARCH64)
#include "asm/mapping_aarch64.hpp"
#else
// default backend: x86
#include "asm/mapping_x86.hpp"
#endif

// Mapping specifies the architecture specific constants and
// code sequences that are valid machine-independently.

class Mapping : AllStatic {
private:
  static Location _localRegisters[nofLocalRegisters + 1]; // the list of local registers
  static int _localRegisterIndex[nofRegisters + 1]; // the inverse of localRegisters[]

public:
  // initialization
  static void initialize();

  // register allocation
  static Location localRegister(int i); // the i.th local register (i = 0 .. nofLocalRegisters-1)
  static int localRegisterIndex(Location l); // the index of local register l (localRegisterIndex(localRegister(i)) = i)

  // parameter passing
  static Location incomingArg(int i, int nofArgs); // incoming argument (excluding receiver; i >= 0, 0 = first arg)
  static Location outgoingArg(int i, int nofArgs); // outgoing argument (excluding receiver; i >= 0, 0 = first arg)

  // stack allocation
  static Location localTemporary(int i); // the i.th local temporary (i >= 0)
  static int
  localTemporaryIndex(Location l); // the index of the local temporary l (localTemporaryIndex(localTemporary(i)) = i)
  static Location floatTemporary(int scope_id, int i); // the i.th float temporary within a scope (i >= 0)

  // context temporaries
  static int contextOffset(int tempNo); // the byte offset of temp from the contextOop
  static Location contextTemporary(int contextNo, int i, int scope_id); // the i.th context temporary (i >= 0)
  static Location* new_contextTemporary(int contextNo, int i, int scope_id); // ditto, but allocated in resource area

  // conversion functions
  static Location asLocation(Register reg) { return Location::registerLocation(reg.number()); }
  static Register asRegister(Location loc) { return Register(loc.number(), ' '); }

  // predicates
  static bool isTemporaryRegister(const Location loc) { return false; } // fix this
  static bool isTrashedRegister(const Location loc) { return true; } // fix this
  static bool isLocalRegister(const Location loc) { return _localRegisterIndex[loc.number()] != -1; }

  static bool isNormalTemporary(Location loc);
  static bool isFloatTemporary(Location loc);

  // helper functions for code generation
  //
  // needsStoreCheck determines whether a store check is needed when storing into a context location
  // (e.g., no storeCheck is needed when initializing individual context fields because there's one
  // store check after context creation).
  static void load(Location src, Register dst);
  static void store(Register src, Location dst, Register temp1, Register temp2, bool needsStoreCheck);
  static void storeO(oop obj, Location dst, Register temp1, Register temp2, bool needsStoreCheck);

  // helper functions for float code
  static void fload(Location src, Register base);
  static void fstore(Location dst, Register base);
};

// calls
const Location selfLoc = Mapping::asLocation(self_reg); // incoming receiver location (in prologue of callee)
const Location receiverLoc = Mapping::asLocation(receiver_reg); // outgoing receiver location (before call)
const Location resultLoc = Mapping::asLocation(result_reg); // outgoing result location (before exit)
const Location frameLoc = Mapping::asLocation(frame_reg); // activation frame pointer

// non-local returns (make sure to adjust the corresponding constants in interpreter_asm.asm when changing these)
const Location NLRResultLoc = Mapping::asLocation(NLR_result_reg); // result being returned
const Location NLRHomeLoc = Mapping::asLocation(NLR_home_reg); // frame ptr of home frame (stack)
const Location NLRHomeIdLoc = Mapping::asLocation(NLR_homeId_reg); // scope id of home scope (inlining)

#endif // DELTA_COMPILER
#endif // _MAPPING_HPP
