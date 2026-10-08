/*
Copyright (c) 2026, Gerardo Santana Gomez Garrido.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/* Test harness for the Capstone-backed disassembler (vm/disasm).
 *
 * The decode half of the round trip is the real production code
 * (vm/disasm/disassembler.cpp linked verbatim); the encode half is the
 * backend assembler. Every test encodes a short instruction sequence, feeds
 * the emitted bytes to Disassembler::decode() and checks that the printed
 * disassembly contains the expected mnemonic/text (the exact text is produced
 * by Capstone 5, so the expectations below mirror its actual output).
 *
 * The two halves are decoded directly (decode(begin, end, &stream)); the
 * nmethod-annotation path (PcDesc/reloc info) is compiled in but never
 * exercised, so all of its symbols may stay unresolved.
 *
 * The harness is standalone: it does not link the rest of the VM. The
 * minimal runtime needed by the encoder and the decoder (CodeBuffer, debug
 * flags, error reporting, the outputStream) is provided here; everything else
 * is left unresolved and the executable is linked with undefined symbols
 * allowed (see the Makefile).
 *
 * Like the encoder tests, the binary picks its backend explicitly with
 * -DDELTA_BACKEND_X86_64 / -DDELTA_BACKEND_AARCH64; one compile produces a
 * test binary for the given backend.
 */

#include "asm/assembler.hpp"
#include "asm/codeBuffer.hpp"
#include "code/stubRoutines.hpp"
#include "disasm/disassembler.hpp"
#include "memory/universe.hpp"
#include "memory/universe.store.hpp"
#include "oops/memOop.inline.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/runtime.hpp"
#include "utilities/ostream.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <cstdint>

// ---------------------------------------------------------------------------
// minimal runtime for the standalone harness
// ---------------------------------------------------------------------------

// debug flags (declared extern "C" in vm/runtime/debug.hpp)
extern "C" bool CodeForP6 = false;
extern "C" bool EnableInt3 = true;
extern "C" bool EliminateJumpsToJumps = false;
extern "C" bool PrintJumpElimination = false;

// error reporting (normally in vm/memory/error.cpp)
extern "C" void breakpoint() {
  std::abort();
}
extern "C" void error_breakpoint() {
  std::abort();
}

void report_assertion_failure(char* code, char* file, int line, char* msg) {
  std::fprintf(stderr, "assertion failure: %s\n%s, %d\n", msg, file, line);
  std::abort();
}
void report_fatal(char* file, int line, char* fmt, ...) {
  std::fprintf(stderr, "fatal: %s, %d\n", file, line);
  std::abort();
}
void report_should_not_call(char* file, int line) {
  std::fprintf(stderr, "ShouldNotCall %s, %d\n", file, line);
  std::abort();
}
void report_should_not_reach_here(char* file, int line) {
  std::fprintf(stderr, "ShouldNotReachHere %s, %d\n", file, line);
  std::abort();
}
void report_subclass_responsibility(char* file, int line) {
  std::fprintf(stderr, "SubclassResponsibility %s, %d\n", file, line);
  std::abort();
}
void report_unimplemented(char* file, int line) {
  std::fprintf(stderr, "Unimplemented %s, %d\n", file, line);
  std::abort();
}

// CodeBuffer: the encoder only uses instsStart/instsEnd/instsOverflow and
// relocation bookkeeping; the full VM implementation lives in codeBuffer.cpp.
CodeBuffer::CodeBuffer(char* code_start, int code_size) {
  instsStart = code_start;
  instsEnd = code_start;
  instsOverflow = code_start + code_size;
  locsStart = NULL;
  locsEnd = NULL;
  locsOverflow = NULL;
  last_reloc_offset = 0;
  _decode_begin = NULL;
}

void CodeBuffer::set_code_end(char* end) {
  instsEnd = end;
}

void CodeBuffer::relocate(char* at, relocInfo::relocType rtype) {
  // relocation records are irrelevant for encoding-only tests
}

void CodeBuffer::decode() {}
char* CodeBuffer::decode_begin() {
  return NULL;
}

void CodeBuffer::print() {}
void PrintableResourceObj::print_short() {}

void NativeTest::verify() {}

// outputStream printing (normally in vm/utilities/ostream.cpp): the decoder
// prints every decoded instruction through the stream, so print/print_cr/cr
// capture into a shared buffer the tests then inspect.
outputStream* _mystd;

outputStream::outputStream(int width) {
  _indentation = 0;
  _width = width;
  _position = 0;
}

static char capture[8192];
static size_t capture_len;

static void capture_append(const char* text, int n) {
  if (n <= 0)
    return;
  if (capture_len + (size_t)n >= sizeof(capture))
    n = (int)(sizeof(capture) - capture_len - 1);
  std::memcpy(capture + capture_len, text, n);
  capture_len += n;
  capture[capture_len] = '\0';
  std::fwrite(text, 1, n, stdout);
}

void outputStream::print(const char* format, ...) {
  char tmp[512];
  va_list ap;
  va_start(ap, format);
  int n = std::vsnprintf(tmp, sizeof(tmp), format, ap);
  va_end(ap);
  capture_append(tmp, n);
}
void outputStream::print_cr(const char* format, ...) {
  char tmp[512];
  va_list ap;
  va_start(ap, format);
  int n = std::vsnprintf(tmp, sizeof(tmp), format, ap);
  va_end(ap);
  capture_append(tmp, n);
  capture_append("\n", 1);
}
void outputStream::cr() {
  capture_append("\n", 1);
}
// put/sp define the key function (`put`), which makes the compiler emit the
// outputStream vtable here (the encoder tests never instantiate an
// outputStream, so they never need one).
void outputStream::put(char c) {
  capture_append(&c, 1);
}
void outputStream::sp() {
  capture_append(" ", 1);
}

// StubRoutines entry points (normally in vm/code/stubRoutines.cpp)
char* StubRoutines::_call_inspector_entry;

// Universe state (normally in vm/memory/universe.cpp). The encoder only takes
// the address of these globals (from print_reg/store_check/etc.), which are
// never called by the tests, so they can be zeroed out. The generation
// classes need non-trivial vtables/constructors, hence the stubs below.
extern "C" oop nilObj = NULL;
extern "C" oop trueObj = NULL;
extern "C" oop falseObj = NULL;

// card table base (vm/runtime/runtime.hpp)
extern "C" char* byte_map_base = NULL;

// last Delta frame (vm/runtime/process.hpp)
extern "C" void** last_Delta_fp = NULL;
extern "C" oop* last_Delta_sp = NULL;
extern "C" char* last_Delta_pc = NULL;

// virtual memory (normally in vm/runtime/virtualspace.cpp) - constructors of
// the Universe generations pull these in
VirtualSpace::VirtualSpace() {
  _low_boundary = NULL;
  _high_boundary = NULL;
  _low_to_high = true;
  _low = NULL;
  _high = NULL;
}
VirtualSpace::~VirtualSpace() {}

// spaces and generations (normally in vm/memory/space.cpp / generation.cpp)
extern "C" oop* eden_bottom = NULL;
extern "C" oop* eden_top = NULL;
extern "C" oop* eden_end = NULL;
edenSpace::edenSpace() {}
survivorSpace::survivorSpace() {}
void newSpace::verify() {}
int newGeneration::capacity() {
  return 0;
}
int newGeneration::used() {
  return 0;
}
int newGeneration::free() {
  return 0;
}
int oldGeneration::capacity() {
  return 0;
}
int oldGeneration::used() {
  return 0;
}
int oldGeneration::free() {
  return 0;
}
bool oldGeneration::contains(void* p) {
  return false;
}

newGeneration Universe::new_gen;
oldGeneration Universe::old_gen;

// ---------------------------------------------------------------------------
// test driver
// ---------------------------------------------------------------------------

static unsigned char buf[4096];
static outputStream stream;

static int failures = 0;
static int total = 0;

static void check_find(const char* name, const char* expected) {
  total++;
  capture[capture_len] = '\0';
  bool ok = std::strstr(capture, expected) != NULL;
  if (ok) {
    std::printf("  ok  %-28s found \"%s\"\n", name, expected);
    return;
  }
  failures++;
  std::printf("FAIL %-28s missing \"%s\"\n", name, expected);
  std::printf("  disassembly:\n%s\n", capture_len ? capture : "<empty>");
}

#define DECODE_BEGIN()                                                                                                 \
  do {                                                                                                                 \
    static const char* __name = __func__;                                                                              \
    (void)__name;                                                                                                      \
    capture_len = 0;                                                                                                   \
    capture[0] = '\0';                                                                                                 \
    Disassembler::decode((char*)buf, __cb.code_end(), &stream);

#define CHECK_FIND(expected) check_find(__name, (expected));

#define DECODE_END                                                                                                     \
  }                                                                                                                    \
  while (0)                                                                                                            \
    ;

#define TEST_BEGIN(name)                                                                                               \
  do {                                                                                                                 \
    std::memset(buf, 0, sizeof(buf));                                                                                  \
    CodeBuffer __cb((char*)buf, sizeof(buf));                                                                          \
    typeof_mac __a(&__cb);

#define TEST_END                                                                                                       \
  }                                                                                                                    \
  while (0)                                                                                                            \
    ;

// ---------------------------------------------------------------------------
// instruction-sequence tests (encoded with the backend assembler, text
// asserted as Capstone 5 prints it)
// ---------------------------------------------------------------------------

#ifdef DELTA_BACKEND_AARCH64

#define typeof_mac AArch64MacroAssembler

static void test_aarch64_decodes() {
  {
    TEST_BEGIN("ret")
    __a.ret();
    DECODE_BEGIN()
    CHECK_FIND("ret")
    DECODE_END
    TEST_END
  }
  {
    TEST_BEGIN("mov x0, #0")
    __a.mov(x0, 0);
    DECODE_BEGIN()
    CHECK_FIND("mov x0, xzr")
    DECODE_END
    TEST_END
  }
  {
    TEST_BEGIN("add x0, x1, #0xff")
    __a.add(x0, x1, 0xff);
    DECODE_BEGIN()
    CHECK_FIND("add x0, x1, #0xff")
    DECODE_END
    TEST_END
  }
  {
    TEST_BEGIN("nop")
    __a.nop();
    DECODE_BEGIN()
    CHECK_FIND("nop")
    DECODE_END
    TEST_END
  }
  {
    TEST_BEGIN("int3 (brk #0)")
    __a.int3();
    DECODE_BEGIN()
    CHECK_FIND("brk #0")
    DECODE_END
    TEST_END
  }
  {
    TEST_BEGIN("movz x0, #0x1234")
    __a.movz(x0, 0x1234, 0);
    DECODE_BEGIN()
    CHECK_FIND("mov x0, #0x1234")
    DECODE_END
    TEST_END
  }
  {
    TEST_BEGIN("multi-instruction sequence")
    __a.ret();
    __a.mov(x0, 0);
    __a.nop();
    DECODE_BEGIN()
    CHECK_FIND("ret")
    CHECK_FIND("mov x0, xzr")
    CHECK_FIND("nop")
    DECODE_END
    TEST_END
  }
}

#else // DELTA_BACKEND_X86_64

#define typeof_mac MacroAssembler

static void test_x86_decodes() {
  {
    TEST_BEGIN("mov eax, 0x1234")
    __a.movl(eax, 0x1234);
    DECODE_BEGIN()
    CHECK_FIND("mov eax, 0x1234")
    DECODE_END
    TEST_END
  }
  {
    TEST_BEGIN("mov rax, rax")
    __a.movq(eax, eax);
    DECODE_BEGIN()
    CHECK_FIND("mov rax, rax")
    DECODE_END
    TEST_END
  }
  {
    TEST_BEGIN("ret")
    __a.ret();
    DECODE_BEGIN()
    CHECK_FIND("ret")
    DECODE_END
    TEST_END
  }
  {
    TEST_BEGIN("inc (rax)")
    __a.incl(eax);
    DECODE_BEGIN()
    CHECK_FIND("inc rax")
    DECODE_END
    TEST_END
  }
  {
    TEST_BEGIN("cmp eax, 0")
    __a.cmpl(eax, 0);
    DECODE_BEGIN()
    CHECK_FIND("cmp eax, 0")
    DECODE_END
    TEST_END
  }
  {
    TEST_BEGIN("nop")
    __a.nop();
    DECODE_BEGIN()
    CHECK_FIND("nop")
    DECODE_END
    TEST_END
  }
  {
    TEST_BEGIN("enter")
    __a.enter();
    DECODE_BEGIN()
    CHECK_FIND("push rbp")
    CHECK_FIND("mov rbp, rsp")
    DECODE_END
    TEST_END
  }
  {
    TEST_BEGIN("multi-instruction sequence")
    __a.ret();
    __a.pushl(eax);
    __a.incl(eax);
    DECODE_BEGIN()
    CHECK_FIND("ret")
    CHECK_FIND("push rax")
    CHECK_FIND("inc rax")
    DECODE_END
    TEST_END
  }
}

#endif

int main() {
#ifdef DELTA_BACKEND_AARCH64
  std::printf("AArch64 disassembler round-trip test\n");
  test_aarch64_decodes();
#else
  std::printf("x86-64 disassembler round-trip test\n");
  test_x86_decodes();
#endif

  std::printf("\n%d of %d tests passed\n", total - failures, total);
  return failures == 0 ? 0 : 1;
}