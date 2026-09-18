/* Copyright 1994, 1995 LongView Technologies L.L.C. $Revision: 1.12 $ */
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

#ifndef DLL_HPP
#define DLL_HPP

// Support routines for Dynamic Link Libraries (DLLs)

#include "code/nativeInstruction.hpp"
#include "memory/allocation.hpp"
#include "runtime/os.hpp"

#include <cstdint>

class InterpretedDLL_Cache : public ValueObj {
private:
  symbolOop _dll_name;
  symbolOop _funct_name;
  dll_func _entry_point;
  char _number_of_arguments;
  // Do not add more instance variables! Layout must correspond to DLL call in bytecodes!

public:
  symbolOop dll_name() const { return _dll_name; }
  symbolOop funct_name() const { return _funct_name; }
  dll_func entry_point() const { return _entry_point; }
  int number_of_arguments() const { return _number_of_arguments; }
  bool async() const;
  void set_entry_point(dll_func f) { _entry_point = f; }

  // Debugging
  void verify();
  void print();
};

// Layout of CompiledDLL_Caches
//
// mov  edx, entry_point	<- mov edx
// test eax, dll_name		<- test 1
// test eax, function_name	<- test 2
// call sync_DLL/async_DLL	<- call
// ...				<- this

class CompiledDLL_Cache : public NativeCall {
#ifdef DELTA_BACKEND_AARCH64
  // AArch64 layout (see CodeGenerator::aDLLNode): three 16-byte literal pools
  // precede the 20-byte NativeCall, all addressed relative to the call's return
  // address (this):
  //
  //   ldr x12,[pc,#8]; b .+12; .quad entry           entry_point  (R-68..R-52)
  //   ldr x16,[pc,#8]; b .+12; .quad dll_name        dll_name     (R-52..R-36)
  //   ldr x16,[pc,#8]; b .+12; .quad function_name   function_name(R-36..R-20)
  //   ldr x16,[pc,#8]; b .+12; .quad target; blr x16 call         (R-20..R)
private:
  enum Layout_constants {
    entry_point_offset = -60,  // .quad entry (the value the compiler loads into edx)
    dll_name_offset = -44,     // .quad dll_name
    function_name_offset = -28 // .quad function_name
  };

public:
  symbolOop dll_name() { return (symbolOop)*(oop*)addr_at(dll_name_offset); }
  symbolOop function_name() { return (symbolOop)*(oop*)addr_at(function_name_offset); }
  dll_func entry_point() { return (dll_func)*(intptr_t*)addr_at(entry_point_offset); }
  bool async() const;
  void set_entry_point(dll_func f) { *(intptr_t*)addr_at(entry_point_offset) = intptr_t(f); }

  // Debugging
  void verify();
  void print();
#else
private:
  enum Layout_constants {
    test_2_instruction_offset = -NativeCall::instruction_size - NativeTest::instruction_size,
    test_1_instruction_offset = test_2_instruction_offset - NativeTest::instruction_size,
    mov_edx_instruction_offset = test_1_instruction_offset - NativeMov::instruction_size,
  };

  NativeMov* mov_at(int offset) { return nativeMov_at(addr_at(offset)); }
  NativeTest* test_at(int offset) { return nativeTest_at(addr_at(offset)); }

public:
  symbolOop dll_name() { return symbolOop(test_at(test_1_instruction_offset)->data()); }
  symbolOop function_name() { return symbolOop(test_at(test_2_instruction_offset)->data()); }
  dll_func entry_point() { return (dll_func)mov_at(mov_edx_instruction_offset)->data(); }
  bool async() const;
  void set_entry_point(dll_func f) { mov_at(mov_edx_instruction_offset)->set_data(intptr_t(f)); }

  // Debugging
  void verify();
  void print();
#endif

  // Creation
  friend CompiledDLL_Cache* compiledDLL_Cache_from_return_address(char* return_address);
  friend CompiledDLL_Cache* compiledDLL_Cache_from_relocInfo(char* displacement_address);
};

inline CompiledDLL_Cache* compiledDLL_Cache_from_return_address(char* return_address) {
  CompiledDLL_Cache* cache = (CompiledDLL_Cache*)(nativeCall_from_return_address(return_address));
#ifdef ASSERT
  cache->verify();
#endif
  return cache;
}

inline CompiledDLL_Cache* compiledDLL_Cache_from_relocInfo(char* displacement_address) {
  return (CompiledDLL_Cache*)nativeCall_from_relocInfo(displacement_address);
}

class DLLs : AllStatic {
public:
  // Lookup
  static dll_func lookup(symbolOop name, DLL* library);
  static DLL* load(symbolOop name);
  static bool unload(DLL* library);

  static dll_func lookup_fail(symbolOop dll_name, symbolOop function_name);
  static dll_func lookup(symbolOop dll_name, symbolOop function_name);
  static dll_func lookup_and_patch_InterpretedDLL_Cache();
  static dll_func lookup_and_patch_CompiledDLL_Cache();

  // Support for asynchronous DLL calls
  static void enter_async_call(DeltaProcess** addr); // called before each asynchronous DLL call
  static void exit_async_call(DeltaProcess** addr); // called after each asynchronous DLL call
  static void exit_sync_call(DeltaProcess** addr); // called after each synchronous DLL call
};
#endif // DLL_HPP
