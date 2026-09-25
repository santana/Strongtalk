/* Copyright 1994 - 1996 LongView Technologies L.L.C. $Revision: 1.35 $ */
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

#ifdef DELTA_COMPILER

#include "code/nmethod.hpp"
#include "code/pcDesc.hpp"
#include "code/relocInfo.hpp"
#include "disasm/disassembler.hpp"
#include "prims/prim.hpp"
#include "memory/generation.inline.hpp"
#include "memory/universe.store.hpp"
#include "oops/oop.inline.hpp"
#include "oops/memOop.inline.hpp"

#include "capstone/capstone.h"
#include <cstdarg>
#include <cstdio>

#define MAX_HEXBUF_SIZE 256
#define MAX_OUTBUF_SIZE 256

static csh capstone_handle = 0;
static bool capstone_initialized = false;
static bool capstone_valid = false;
static bool capstone_warned = false;

static void initialize(outputStream* st);
static bool literalWord(nmethod* nm, char* pc, outputStream* st);
static char tohex(unsigned char c);
static char* bintohex(char* data, int bytes);

static void printRelocInfo(relocIterator* iter, outputStream* st);
static void printRelocInfo(nmethod* nm, char* pc, int lendis, outputStream* st);
static void printPcDescInfo(nmethod* nm, char* pc, outputStream* st);

static void disasm(char* begin, char* end, nmethod* nm, outputStream* st);

// Capstone 5 resolves its allocations through cs_opt_mem set with
// cs_option(CS_OPT_MEM) before the first cs_open(); the libc allocator is all
// this debug facility needs.
static void* cs_alloc(size_t size) {
  return malloc(size);
}
static void* cs_calloc(size_t count, size_t size) {
  return calloc(count, size);
}
static void* cs_realloc(void* ptr, size_t size) {
  return realloc(ptr, size);
}
static void cs_free(void* ptr) {
  free(ptr);
}

static void initialize(outputStream* st) {
  cs_opt_mem mem;
  mem.malloc = cs_alloc;
  mem.calloc = cs_calloc;
  mem.realloc = cs_realloc;
  mem.free = cs_free;
  mem.vsnprintf = vsnprintf;
  if (cs_option(0, CS_OPT_MEM, (size_t)&mem) != CS_ERR_OK) {
    if (st != NULL)
      st->print_cr("INFO: capstone memory setup failed!");
    return;
  }
#ifdef DELTA_BACKEND_AARCH64
  cs_err err = cs_open(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN, &capstone_handle);
#else
  cs_err err = cs_open(CS_ARCH_X86, CS_MODE_64, &capstone_handle);
#endif
  if (err == CS_ERR_OK) {
    capstone_valid = true;
  } else if (st != NULL) {
    st->print_cr("INFO: capstone init failed: %s", cs_strerror(err));
  }
}

static char tohex(unsigned char c) {
  char* digits = "0123456789ABCDEF";
  if (c > 0xf)
    return '?';
  return digits[c];
}

static char* bintohex(char* data, int bytes) {
  static char buf[MAX_HEXBUF_SIZE];

  char* p = buf;
  while (bytes--) {
    *p++ = tohex((*data & 0xF0) >> 4);
    *p++ = tohex(*data & 0x0F);
    data++;
  }
  *p = '\0';
  return buf;
}

static void printRelocInfo(relocIterator* iter, outputStream* st) {
  primitive_desc* pd;
  char* target;
  int* addr;

  st->print("[reloc @ ");
  addr = iter->word_addr();
  switch (iter->type()) {
    case relocInfo::none:
      st->print("none");
      break;

    case relocInfo::oop_type:
      st->print("%p, embedded oop, ", addr);
      (*iter->oop_addr())->print_value();
      break;

    case relocInfo::ic_type:
      st->print("%p, inline cache", addr);
      break;

    case relocInfo::prim_type:
      st->print("%p, primitive call, ", addr);
#ifdef DELTA_BACKEND_AARCH64
      target = *(char**)addr; // .quad literal holds the absolute target
#else
      target = (char*)(*addr + (intptr_t)addr + 4); // 4-byte rel32 displacement
#endif
      pd = primitives::lookup((fntype)target);
      if (pd != NULL) {
        st->print("(%s)", pd->name());
      } else {
        st->print("runtime routine");
      }
      break;

    case relocInfo::runtime_call_type:
      st->print("%p, runtime call", addr);
      break;

    case relocInfo::external_word_type:
      st->print("%p, external word", addr);
      break;

    case relocInfo::internal_word_type:
      st->print("%p, internal word", addr);
      break;

    case relocInfo::uncommon_type:
      st->print("%p, uncommon trap ", addr);
      if (iter->wasUncommonTrapExecuted())
        st->print(" (taken)");
      else
        st->print(" (not taken)");
      break;

    case relocInfo::dll_type:
      st->print("%p, dll", addr);
      break;

    default:
      st->print("???");
      break;
  }
  st->print("]");
}

static void printRelocInfo(nmethod* nm, char* pc, int lendis, outputStream* st) {
  relocIterator iter(nm);
  char* addr;

  while (iter.next()) {
    addr = (char*)iter.word_addr();
    if (addr > pc && addr < (pc + lendis)) {
      printRelocInfo(&iter, st);
      break;
    }
  }
}

static void printPcDescInfo(nmethod* nm, char* pc, outputStream* st) {
  PcDesc* pcs;

  pcs = nm->containingPcDesc(pc, NULL);
  if (pcs) {
    st->print("bc = %03ld ", pcs->byteCode);
    if (pcs->is_prologue()) {
      st->print("prologue ");
    } else if (pcs->is_epilogue()) {
      st->print("epilogue ");
    }
  }
}

#ifdef DELTA_BACKEND_AARCH64
// On AArch64, code frequently embeds 8-byte literal words carrying the absolute
// address of oops/primitive/runtime targets (the pool behind the ldr/br/blr
// sequences emitted by load_absolute_address/call/jmp). Emit those as .quad
// data instead of trying to decode them as instructions.
static bool literalWord(nmethod* nm, char* pc, outputStream* st) {
  if (nm == NULL || ((uintptr_t)pc & 7) != 0)
    return false;
  relocIterator iter(nm);
  while (iter.next()) {
    if ((char*)iter.word_addr() == pc) {
      st->print("%p .quad %-20s  0x%llx", pc, bintohex(pc, sizeof(intptr_t)), (unsigned long long)*(intptr_t*)pc);
      st->cr();
      return true;
    }
  }
  return false;
}
#endif

static void disasm(char* begin, char* end, nmethod* nm, outputStream* st) {
  if (!capstone_initialized) {
    capstone_initialized = true;
    initialize(st);
  }
  if (!capstone_valid) {
    if (!capstone_warned) {
      capstone_warned = true;
      st->print_cr("INFO: no disassembler available!");
    }
    return;
  }

  cs_insn* insn = cs_malloc(capstone_handle);
  if (insn == NULL) {
    st->print_cr("INFO: capstone cs_malloc failed!");
    return;
  }

  const uint8_t* code = (const uint8_t*)begin;
  size_t remaining = (size_t)(end - begin);

  while (remaining > 0) {
    char* start = (char*)(intptr_t)code;
#ifdef DELTA_BACKEND_AARCH64
    if (literalWord(nm, start, st)) {
      code += sizeof(intptr_t);
      remaining -= sizeof(intptr_t);
      continue;
    }
#endif
    uint64_t address = (uint64_t)(intptr_t)code;
    if (cs_disasm_iter(capstone_handle, &code, &remaining, &address, insn)) {
      int lendis = (int)insn->size;
      if (lendis <= 0)
        break;
      static char buf[MAX_OUTBUF_SIZE];
      snprintf(buf, sizeof(buf), "%s %s", insn->mnemonic, insn->op_str);
      st->print("%p %-20s    %-40s", start, bintohex(start, lendis), buf);
      if (nm) {
        st->print("; ");
        printPcDescInfo(nm, start, st);
        printRelocInfo(nm, start, lendis, st);
      }
      st->cr();
    } else {
      // Undecodable bytes (or trailing data): print them raw and advance by the
      // smallest instruction unit so the dump can never stall.
#ifdef DELTA_BACKEND_AARCH64
      static const int unit = (int)sizeof(int32_t);
#else
      static const int unit = 1;
#endif
      int skip = (int)remaining;
      if (skip > unit)
        skip = unit;
      st->print("%p .byte %-20s      %-40s", start, bintohex(start, skip), "<undecodable>");
      st->cr();
      code += skip;
      remaining -= skip;
    }
  }
  cs_free(insn, 1);
}

void Disassembler::decode(nmethod* nm, outputStream* st) {
  disasm(nm->insts(), nm->instsEnd(), nm, st);
}

void Disassembler::decode(char* begin, char* end, outputStream* st) {
  disasm(begin, end, NULL, st);
}

#endif