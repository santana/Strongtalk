/*
Copyright (c) 2026, Gerardo Santana Gomez Garrido.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/* Test harness for the x86 encoder (64-bit only; 32-bit support was dropped).
 *
 * The source is compiled once against the 64-bit encoder. Every test
 * encodes a single instruction (or a short instruction sequence) and checks
 * the emitted bytes against the expected x86 encoding, so the golden bytes
 * below are the ground truth for the encoder.
 *
 * The harness is standalone: it does not link the rest of the VM. The
 * minimal runtime needed by the encoder (CodeBuffer, debug flags, error
 * reporting) is provided here. All symbols that the encoder references but
 * never calls at run time (Universe globals, print_reg, store_check, the
 * ostream, ...) are intentionally left unresolved; like the VM's own build
 * this links with -undefined dynamic_lookup (macOS) / --unresolved-symbols
 * =ignore-all (Linux) so those references never need to be satisfied.
 *
 * To keep label fixup deterministic, EliminateJumpsToJumps is disabled; bind()
 * then immediately patches the displacement instead of deferring it.
 */

#if defined(DELTA_BACKEND_AARCH64)
// x86-only test; the AArch64 backend has its own encoder tests in
// assemblerEncoderTest_aarch64.cpp.
int main() {
  return 0;
}
#else

#include "asm/assembler.hpp"
#include "asm/codeBuffer.hpp"
#include "code/stubRoutines.hpp"
#include "memory/universe.hpp"
#include "runtime/runtime.hpp"
#include "utilities/ostream.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <capstone/capstone.h>
#include <string>

// Capstone-backed decode of the emitted bytes: the authoritative semantic
// check. The raw byte arrays below are a secondary pin on the exact encoding.
static std::string decode_all(const unsigned char* code, size_t len) {
  cs_opt_mem mem;
  mem.malloc = malloc;
  mem.calloc = calloc;
  mem.realloc = realloc;
  mem.free = free;
  mem.vsnprintf = vsnprintf;
  if (cs_option(0, CS_OPT_MEM, (size_t)&mem) != CS_ERR_OK)
    return "<capstone mem setup failed>";
  csh h;
  if (cs_open(CS_ARCH_X86, CS_MODE_64, &h) != CS_ERR_OK)
    return "<capstone open failed>";

  std::string s;
  size_t off = 0;
  while (off < len) {
    cs_insn* insn;
    size_t cnt = cs_disasm(h, code + off, len - off, off, 1, &insn);
    if (cnt != 1) {
      char tmp[64];
      std::snprintf(tmp, sizeof(tmp), "<undecoded @%zu>", off);
      s += tmp;
      off++;
      continue;
    }
    s += insn[0].mnemonic;
    if (insn[0].op_str[0] != '\0') {
      s += ' ';
      s += insn[0].op_str;
    }
    off += insn[0].size;
    if (off < len)
      s += " ; ";
  }
  cs_close(&h);
  return s;
}

// ---------------------------------------------------------------------------
// minimal runtime for the standalone encoder
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

// NativeTest (normally in vm/code/nativeInstruction.cpp); only its verify()
// is referenced by the encoder (through the inline nativeTest_at helper).
void NativeTest::verify() {}

// outputStream printing (normally in vm/utilities/ostream.cpp); only needed
// if the encoder's own print helpers were ever called (they are not).
outputStream* _mystd;
void outputStream::print(const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  std::vfprintf(stdout, format, ap);
  va_end(ap);
}
void outputStream::print_cr(const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  std::vfprintf(stdout, format, ap);
  va_end(ap);
  std::fprintf(stdout, "\n");
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

static int failures = 0;
static int total = 0;

static int g_dump_mode = 0; // -d: regenerate the canonical decode strings

static void check_bytes(const char* name, unsigned char* code, int len, const unsigned char* expected, int elen,
                        const char* expected_decode) {
  std::string actual_decode = decode_all(code, len);
  if (g_dump_mode) {
    std::printf("%s\t", name);
    for (int i = 0; i < len; i++)
      std::printf("%02x", code[i]);
    std::printf("\t%s\n", actual_decode.c_str());
    return;
  }
  total++;
  bool ok = (len == elen);
  if (ok) {
    for (int i = 0; i < len; i++) {
      if (code[i] != expected[i]) {
        ok = false;
        break;
      }
    }
  }
  bool decode_ok = (actual_decode == expected_decode);
  if (ok && decode_ok) {
    std::printf("  ok  %s\n", name);
    return;
  }
  failures++;
  std::printf("FAIL %s\n", name);
  if (!ok) {
    std::printf("  raw len %d expected %d\n  actual:   ", len, elen);
    for (int i = 0; i < len; i++)
      std::printf("%02x ", code[i]);
    std::printf("\n  expected: ");
    for (int i = 0; i < elen; i++)
      std::printf("%02x ", expected[i]);
    std::printf("\n");
  }
  if (!decode_ok) {
    std::printf("  decode actual:   %s\n", actual_decode.c_str());
    std::printf("  decode expected: %s\n", expected_decode);
  }
}

#define TEST_BEGIN(name)                                                                                               \
  do {                                                                                                                 \
    const char* __test_name = (name);                                                                                  \
    std::memset(buf, 0, sizeof(buf));                                                                                  \
    CodeBuffer __cb((char*)buf, sizeof(buf));                                                                          \
    X86MacroAssembler __a(&__cb);

#define CHECK_HEX(expected, len, decoded) check_bytes(__test_name, buf, __cb.code_size(), (expected), (len), (decoded));

#define TEST_END                                                                                                       \
  }                                                                                                                    \
  while (0)                                                                                                            \
    ;

// ---------------------------------------------------------------------------
// tests
// ---------------------------------------------------------------------------

static void test_moves() {
  {
    static const unsigned char e[] = {
      0x48,
      0x8b,
      0xc3,
    };
    TEST_BEGIN("movl eax, ebx")
    __a.movl(eax, ebx);
    CHECK_HEX(e, sizeof(e), "mov rax, rbx");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xB8, 0x78, 0x56, 0x34, 0x12};
    TEST_BEGIN("movl eax, 0x12345678")
    __a.movl(eax, 0x12345678);
    CHECK_HEX(e, sizeof(e), "mov eax, 0x12345678");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xB8, 0x0A, 0x00, 0x00, 0x00};
    TEST_BEGIN("movl eax, 0x0A (imm8, no sign-extension)")
    __a.movl(eax, 0x0A);
    CHECK_HEX(e, sizeof(e), "mov eax, 0xa");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x48,
      0x8b,
      0x03,
    };
    TEST_BEGIN("movl eax, [ebx]")
    __a.movl(eax, Address(ebx));
    CHECK_HEX(e, sizeof(e), "mov rax, qword ptr [rbx]");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x48,
      0x8b,
      0x43,
      0x04,
    };
    TEST_BEGIN("movl eax, [ebx+4]")
    __a.movl(eax, Address(ebx, 4));
    CHECK_HEX(e, sizeof(e), "mov rax, qword ptr [rbx + 4]");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x48,
      0x8b,
      0x45,
      0x00,
    };
    TEST_BEGIN("movl eax, [ebp] (disp8 even with disp 0)")
    __a.movl(eax, Address(ebp));
    CHECK_HEX(e, sizeof(e), "mov rax, qword ptr [rbp]");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x48,
      0x8b,
      0x45,
      0x10,
    };
    TEST_BEGIN("movl eax, [ebp+16]")
    __a.movl(eax, Address(ebp, 16));
    CHECK_HEX(e, sizeof(e), "mov rax, qword ptr [rbp + 0x10]");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x48,
      0x8b,
      0x45,
      0xfc,
    };
    TEST_BEGIN("movl eax, [ebp-4]")
    __a.movl(eax, Address(ebp, -4));
    CHECK_HEX(e, sizeof(e), "mov rax, qword ptr [rbp - 4]");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x48,
      0x8b,
      0x04,
      0x24,
    };
    TEST_BEGIN("movl eax, [esp] (SIB)")
    __a.movl(eax, Address(esp));
    CHECK_HEX(e, sizeof(e), "mov rax, qword ptr [rsp]");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x48, 0x8b, 0x44, 0x24, 0x04,
    };
    TEST_BEGIN("movl eax, [esp+4] (SIB + disp8)")
    __a.movl(eax, Address(esp, 4));
    CHECK_HEX(e, sizeof(e), "mov rax, qword ptr [rsp + 4]");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x48,
      0x8b,
      0x04,
      0x8b,
    };
    TEST_BEGIN("movl eax, [ebx+ecx*4] (SIB, no disp)")
    __a.movl(eax, Address(ebx, ecx, Address::times_4, 0));
    CHECK_HEX(e, sizeof(e), "mov rax, qword ptr [rbx + rcx*4]");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x48, 0x8b, 0x44, 0x8b, 0x08,
    };
    TEST_BEGIN("movl eax, [ebx+ecx*4+8]")
    __a.movl(eax, Address(ebx, ecx, Address::times_4, 8));
    CHECK_HEX(e, sizeof(e), "mov rax, qword ptr [rbx + rcx*4 + 8]");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x48, 0x8b, 0x94, 0x8b, 0x00, 0x01, 0x00, 0x00,
    };
    TEST_BEGIN("movl edx, [ebx+ecx*4+0x100] (disp32)")
    __a.movl(edx, Address(ebx, ecx, Address::times_4, 0x100));
    CHECK_HEX(e, sizeof(e), "mov rdx, qword ptr [rbx + rcx*4 + 0x100]");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x48,
      0x8b,
      0x04,
      0x94,
    };
    TEST_BEGIN("movl eax, [esp+edx*4]")
    __a.movl(eax, Address(esp, edx, Address::times_4, 0));
    CHECK_HEX(e, sizeof(e), "mov rax, qword ptr [rsp + rdx*4]");
    TEST_END
  }
  {
    // On a 64-bit build this is RIP-relative (mod=00 r/m=101 includes the
    // disp32 disp field).
    static const unsigned char e[] = {
      0x48, 0x8b, 0x05, 0x78, 0x56, 0x34, 0x12,
    };
    TEST_BEGIN("movl eax, [0x12345678] (mod=00 r/m=101)")
    __a.movl(eax, Address(0x12345678, relocInfo::none));
    CHECK_HEX(e, sizeof(e), "mov rax, qword ptr [rip + 0x12345678]");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x48,
      0x89,
      0x43,
      0x04,
    };
    TEST_BEGIN("movl [ebx+4], eax")
    __a.movl(Address(ebx, 4), eax);
    CHECK_HEX(e, sizeof(e), "mov qword ptr [rbx + 4], rax");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x41, 0xba, 0x2a, 0x00, 0x00, 0x00, 0x4c, 0x89, 0x53, 0x04,
    };
    TEST_BEGIN("movl [ebx+4], 0x2A")
    __a.movl(Address(ebx, 4), 0x2A);
    CHECK_HEX(e, sizeof(e), "mov r10d, 0x2a ; mov qword ptr [rbx + 4], r10");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x8A, 0x03};
    TEST_BEGIN("movb eax, [ebx]")
    __a.movb(eax, Address(ebx));
    CHECK_HEX(e, sizeof(e), "mov al, byte ptr [rbx]");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xC6, 0x43, 0x01, 0x2A};
    TEST_BEGIN("movb [ebx+1], 0x2A")
    __a.movb(Address(ebx, 1), 0x2A);
    CHECK_HEX(e, sizeof(e), "mov byte ptr [rbx + 1], 0x2a");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x88, 0x03};
    TEST_BEGIN("movb [ebx], eax")
    __a.movb(Address(ebx), eax);
    CHECK_HEX(e, sizeof(e), "mov byte ptr [rbx], al");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x66, 0x8B, 0x43, 0x02};
    TEST_BEGIN("movw eax, [ebx+2]")
    __a.movw(eax, Address(ebx, 2));
    CHECK_HEX(e, sizeof(e), "mov ax, word ptr [rbx + 2]");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x66, 0x89, 0x43, 0x02};
    TEST_BEGIN("movw [ebx+2], eax")
    __a.movw(Address(ebx, 2), eax);
    CHECK_HEX(e, sizeof(e), "mov word ptr [rbx + 2], ax");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x0F, 0xBE, 0xC3};
    TEST_BEGIN("movsxb eax, ebx")
    __a.movsxb(eax, ebx);
    CHECK_HEX(e, sizeof(e), "movsx eax, bl");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x0F, 0xBF, 0xC3};
    TEST_BEGIN("movsxw eax, ebx")
    __a.movsxw(eax, ebx);
    CHECK_HEX(e, sizeof(e), "movsx eax, bx");
    TEST_END
  }
}

static void test_arith() {
  {
    static const unsigned char e[] = {
      0x48,
      0x03,
      0xc3,
    };
    TEST_BEGIN("addl eax, ebx")
    __a.addl(eax, ebx);
    CHECK_HEX(e, sizeof(e), "add rax, rbx");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x48,
      0x83,
      0xc0,
      0x0a,
    };
    TEST_BEGIN("addl eax, 0x0A (sign-extended imm8)")
    __a.addl(eax, 0x0A);
    CHECK_HEX(e, sizeof(e), "add rax, 0xa");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x48, 0x81, 0xc0, 0x78, 0x56, 0x34, 0x12,
    };
    TEST_BEGIN("addl eax, 0x12345678")
    __a.addl(eax, 0x12345678);
    CHECK_HEX(e, sizeof(e), "add rax, 0x12345678");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x81, 0x43, 0x04, 0x01, 0x00, 0x00, 0x00};
    TEST_BEGIN("addl [ebx+4], 0x1 (always imm32)")
    __a.addl(Address(ebx, 4), 0x1);
    CHECK_HEX(e, sizeof(e), "add dword ptr [rbx + 4], 1");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x48,
      0x2b,
      0xc3,
    };
    TEST_BEGIN("subl eax, ebx")
    __a.subl(eax, ebx);
    CHECK_HEX(e, sizeof(e), "sub rax, rbx");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x48, 0x81, 0xe8, 0x34, 0x12, 0x00, 0x00,
    };
    TEST_BEGIN("subl eax, 0x1234")
    __a.subl(eax, 0x1234);
    CHECK_HEX(e, sizeof(e), "sub rax, 0x1234");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x48,
      0x23,
      0xd1,
    };
    TEST_BEGIN("andl edx, ecx")
    __a.andl(edx, ecx);
    CHECK_HEX(e, sizeof(e), "and rdx, rcx");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x48,
      0x0b,
      0xd1,
    };
    TEST_BEGIN("orl edx, ecx")
    __a.orl(edx, ecx);
    CHECK_HEX(e, sizeof(e), "or rdx, rcx");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x48,
      0x33,
      0xc0,
    };
    TEST_BEGIN("xorl eax, eax")
    __a.xorl(eax, eax);
    CHECK_HEX(e, sizeof(e), "xor rax, rax");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x48,
      0x83,
      0xf0,
      0x0a,
    };
    TEST_BEGIN("xorl eax, 0x0A")
    __a.xorl(eax, 0x0A);
    CHECK_HEX(e, sizeof(e), "xor rax, 0xa");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x48,
      0x3b,
      0xc3,
    };
    TEST_BEGIN("cmpl eax, ebx")
    __a.cmpl(eax, ebx);
    CHECK_HEX(e, sizeof(e), "cmp rax, rbx");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x83, 0xF8, 0x0A};
    TEST_BEGIN("cmpl eax, 0x0A")
    __a.cmpl(eax, 0x0A);
    CHECK_HEX(e, sizeof(e), "cmp eax, 0xa");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x81, 0xF8, 0x78, 0x56, 0x34, 0x12};
    TEST_BEGIN("cmpl eax, 0x12345678")
    __a.cmpl(eax, 0x12345678);
    CHECK_HEX(e, sizeof(e), "cmp eax, 0x12345678");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x81, 0x3B, 0x01, 0x00, 0x00, 0x00};
    TEST_BEGIN("cmpl [ebx], 0x1")
    __a.cmpl(Address(ebx), 0x1);
    CHECK_HEX(e, sizeof(e), "cmp dword ptr [rbx], 1");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x13, 0xC3};
    TEST_BEGIN("adcl eax, ebx")
    __a.adcl(eax, ebx);
    CHECK_HEX(e, sizeof(e), "adc eax, ebx");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x83, 0xD0, 0x01};
    TEST_BEGIN("adcl eax, 0x1")
    __a.adcl(eax, 0x1);
    CHECK_HEX(e, sizeof(e), "adc eax, 1");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x1B, 0xC3};
    TEST_BEGIN("sbbl eax, ebx")
    __a.sbbl(eax, ebx);
    CHECK_HEX(e, sizeof(e), "sbb eax, ebx");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x85, 0xC2};
    TEST_BEGIN("testl eax, edx")
    __a.testl(eax, edx);
    CHECK_HEX(e, sizeof(e), "test edx, eax");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xF7, 0xC2, 0x01, 0x00, 0x00, 0x00};
    TEST_BEGIN("testl edx, 0x1")
    __a.testl(edx, 0x1);
    CHECK_HEX(e, sizeof(e), "test edx, 1");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xA9, 0x00, 0x00, 0x00, 0x80};
    TEST_BEGIN("testl eax, 0x80000000 (short form)")
    __a.testl(eax, 0x80000000);
    CHECK_HEX(e, sizeof(e), "test eax, 0x80000000");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xF6, 0xC0, 0x7F};
    TEST_BEGIN("test eax, 0x7F (testb)")
    __a.test(eax, 0x7F);
    CHECK_HEX(e, sizeof(e), "test al, 0x7f");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xF7, 0xC4, 0x01, 0x00, 0x00, 0x00};
    TEST_BEGIN("test esp, 0x1 (no byte register)")
    __a.test(esp, 0x1);
    CHECK_HEX(e, sizeof(e), "test esp, 1");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xFF, 0x03};
    TEST_BEGIN("incl [ebx]")
    __a.incl(Address(ebx));
    CHECK_HEX(e, sizeof(e), "inc dword ptr [rbx]");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xFF, 0x0B};
    TEST_BEGIN("decl [ebx]")
    __a.decl(Address(ebx));
    CHECK_HEX(e, sizeof(e), "dec dword ptr [rbx]");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x48,
      0xf7,
      0xd8,
    };
    TEST_BEGIN("negl eax")
    __a.negl(eax);
    CHECK_HEX(e, sizeof(e), "neg rax");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xF7, 0xD0};
    TEST_BEGIN("notl eax")
    __a.notl(eax);
    CHECK_HEX(e, sizeof(e), "not eax");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xF7, 0xE3};
    TEST_BEGIN("mull ebx")
    __a.mull(ebx);
    CHECK_HEX(e, sizeof(e), "mul ebx");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x48,
      0x0f,
      0xaf,
      0xc3,
    };
    TEST_BEGIN("imull ebx")
    __a.imull(ebx);
    CHECK_HEX(e, sizeof(e), "imul rax, rbx");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x48,
      0x0f,
      0xaf,
      0xc3,
    };
    TEST_BEGIN("imull eax, ebx")
    __a.imull(eax, ebx);
    CHECK_HEX(e, sizeof(e), "imul rax, rbx");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x48,
      0x6b,
      0xc3,
      0x02,
    };
    TEST_BEGIN("imull eax, ebx, 2 (imm8)")
    __a.imull(eax, ebx, 2);
    CHECK_HEX(e, sizeof(e), "imul rax, rbx, 2");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x48, 0x69, 0xc3, 0x00, 0x01, 0x00, 0x00,
    };
    TEST_BEGIN("imull eax, ebx, 0x100 (imm32)")
    __a.imull(eax, ebx, 0x100);
    CHECK_HEX(e, sizeof(e), "imul rax, rbx, 0x100");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x48,
      0xf7,
      0xfb,
    };
    TEST_BEGIN("idivl ebx")
    __a.idivl(ebx);
    CHECK_HEX(e, sizeof(e), "idiv rbx");
    TEST_END
  }
}

static void test_shifts() {
  {
    static const unsigned char e[] = {
      0x48,
      0xd1,
      0xe3,
    };
    TEST_BEGIN("shll ebx, 1")
    __a.shll(ebx, 1);
    CHECK_HEX(e, sizeof(e), "shl rbx, 1");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x48,
      0xc1,
      0xe3,
      0x03,
    };
    TEST_BEGIN("shll ebx, 3")
    __a.shll(ebx, 3);
    CHECK_HEX(e, sizeof(e), "shl rbx, 3");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x48,
      0xd3,
      0xe3,
    };
    TEST_BEGIN("shll ebx (by cl)")
    __a.shll(ebx);
    CHECK_HEX(e, sizeof(e), "shl rbx, cl");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x48,
      0xd1,
      0xe8,
    };
    TEST_BEGIN("shrl eax, 1")
    __a.shrl(eax, 1);
    CHECK_HEX(e, sizeof(e), "shr rax, 1");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x48,
      0xc1,
      0xe8,
      0x02,
    };
    TEST_BEGIN("shrl eax, 2")
    __a.shrl(eax, 2);
    CHECK_HEX(e, sizeof(e), "shr rax, 2");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x48,
      0xd3,
      0xe8,
    };
    TEST_BEGIN("shrl eax (by cl)")
    __a.shrl(eax);
    CHECK_HEX(e, sizeof(e), "shr rax, cl");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x48,
      0xd1,
      0xf8,
    };
    TEST_BEGIN("sarl eax, 1")
    __a.sarl(eax, 1);
    CHECK_HEX(e, sizeof(e), "sar rax, 1");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x48,
      0xc1,
      0xf8,
      0x02,
    };
    TEST_BEGIN("sarl eax, 2")
    __a.sarl(eax, 2);
    CHECK_HEX(e, sizeof(e), "sar rax, 2");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x48,
      0xd3,
      0xf9,
    };
    TEST_BEGIN("sarl ecx (by cl)")
    __a.sarl(ecx);
    CHECK_HEX(e, sizeof(e), "sar rcx, cl");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xD1, 0xD2};
    TEST_BEGIN("rcll edx, 1")
    __a.rcll(edx, 1);
    CHECK_HEX(e, sizeof(e), "rcl edx, 1");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xC1, 0xD2, 0x03};
    TEST_BEGIN("rcll edx, 3")
    __a.rcll(edx, 3);
    CHECK_HEX(e, sizeof(e), "rcl edx, 3");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x0F, 0xA5, 0xCA};
    TEST_BEGIN("shldl edx, ecx")
    __a.shldl(edx, ecx);
    CHECK_HEX(e, sizeof(e), "shld edx, ecx, cl");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x0F, 0xAD, 0xCA};
    TEST_BEGIN("shrdl edx, ecx")
    __a.shrdl(edx, ecx);
    CHECK_HEX(e, sizeof(e), "shrd edx, ecx, cl");
    TEST_END
  }
}

static void test_stack() {
  {
    static const unsigned char e[] = {
      0x48,
      0x53,
    };
    TEST_BEGIN("pushl ebx")
    __a.pushl(ebx);
    CHECK_HEX(e, sizeof(e), "push rbx");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x68, 0x34, 0x12, 0x00, 0x00};
    TEST_BEGIN("pushl 0x1234")
    __a.pushl(0x1234);
    CHECK_HEX(e, sizeof(e), "push 0x1234");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x48,
      0xff,
      0x73,
      0x04,
    };
    TEST_BEGIN("pushl [ebx+4]")
    __a.pushl(Address(ebx, 4));
    CHECK_HEX(e, sizeof(e), "push qword ptr [rbx + 4]");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x48,
      0x58,
    };
    TEST_BEGIN("popl eax")
    __a.popl(eax);
    CHECK_HEX(e, sizeof(e), "pop rax");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x48,
      0x8f,
      0x03,
    };
    TEST_BEGIN("popl [ebx]")
    __a.popl(Address(ebx));
    CHECK_HEX(e, sizeof(e), "pop qword ptr [rbx]");
    TEST_END
  }
}

static void test_control_flow() {
  {
    static const unsigned char e[] = {0xFF, 0xD3};
    TEST_BEGIN("call ebx")
    __a.call(ebx);
    CHECK_HEX(e, sizeof(e), "call rbx");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xFF, 0x53, 0x04};
    TEST_BEGIN("call [ebx+4]")
    __a.call(Address(ebx, 4));
    CHECK_HEX(e, sizeof(e), "call qword ptr [rbx + 4]");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xFF, 0xE3};
    TEST_BEGIN("jmp ebx")
    __a.jmp(ebx);
    CHECK_HEX(e, sizeof(e), "jmp rbx");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xFF, 0x23};
    TEST_BEGIN("jmp [ebx]")
    __a.jmp(Address(ebx));
    CHECK_HEX(e, sizeof(e), "jmp qword ptr [rbx]");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xC3};
    TEST_BEGIN("ret")
    __a.ret();
    CHECK_HEX(e, sizeof(e), "ret");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xC2, 0x10, 0x00};
    TEST_BEGIN("ret 0x10")
    __a.ret(0x10);
    CHECK_HEX(e, sizeof(e), "ret 0x10");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x90};
    TEST_BEGIN("nop")
    __a.nop();
    CHECK_HEX(e, sizeof(e), "nop");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xF4};
    TEST_BEGIN("hlt")
    __a.hlt();
    CHECK_HEX(e, sizeof(e), "hlt");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xCC};
    TEST_BEGIN("int3")
    __a.int3();
    CHECK_HEX(e, sizeof(e), "int3");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x48,
      0x99,
    };
    TEST_BEGIN("cdq")
    __a.cdq();
    CHECK_HEX(e, sizeof(e), "cqo");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x90, 0x90, 0x90, 0x90};
    TEST_BEGIN("align(4) after 1 byte")
    __a.nop();
    __a.align(4);
    CHECK_HEX(e, sizeof(e), "nop ; nop ; nop ; nop");
    TEST_END
  }
}

static void test_fpu() {
  {
    static const unsigned char e[] = {0xD9, 0xE8};
    TEST_BEGIN("fld1")
    __a.fld1();
    CHECK_HEX(e, sizeof(e), "fld1");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xD9, 0xEE};
    TEST_BEGIN("fldz")
    __a.fldz();
    CHECK_HEX(e, sizeof(e), "fldz");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xD9, 0x03};
    TEST_BEGIN("fld_s [ebx]")
    __a.fld_s(Address(ebx));
    CHECK_HEX(e, sizeof(e), "fld dword ptr [rbx]");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xDD, 0x03};
    TEST_BEGIN("fld_d [ebx]")
    __a.fld_d(Address(ebx));
    CHECK_HEX(e, sizeof(e), "fld qword ptr [rbx]");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xD9, 0x1B};
    TEST_BEGIN("fstp_s [ebx]")
    __a.fstp_s(Address(ebx));
    CHECK_HEX(e, sizeof(e), "fstp dword ptr [rbx]");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xDD, 0x1B};
    TEST_BEGIN("fstp_d [ebx]")
    __a.fstp_d(Address(ebx));
    CHECK_HEX(e, sizeof(e), "fstp qword ptr [rbx]");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xDB, 0x03};
    TEST_BEGIN("fild_s [ebx]")
    __a.fild_s(Address(ebx));
    CHECK_HEX(e, sizeof(e), "fild dword ptr [rbx]");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xDF, 0x2B};
    TEST_BEGIN("fild_d [ebx]")
    __a.fild_d(Address(ebx));
    CHECK_HEX(e, sizeof(e), "fild qword ptr [rbx]");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xDB, 0x1B};
    TEST_BEGIN("fistp_s [ebx]")
    __a.fistp_s(Address(ebx));
    CHECK_HEX(e, sizeof(e), "fistp dword ptr [rbx]");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xDF, 0x3B};
    TEST_BEGIN("fistp_d [ebx]")
    __a.fistp_d(Address(ebx));
    CHECK_HEX(e, sizeof(e), "fistp qword ptr [rbx]");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xD9, 0xE1};
    TEST_BEGIN("fabs")
    __a.fabs();
    CHECK_HEX(e, sizeof(e), "fabs");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xD9, 0xE0};
    TEST_BEGIN("fchs")
    __a.fchs();
    CHECK_HEX(e, sizeof(e), "fchs");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xDC, 0x03};
    TEST_BEGIN("fadd_d [ebx]")
    __a.fadd_d(Address(ebx));
    CHECK_HEX(e, sizeof(e), "fadd qword ptr [rbx]");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xDC, 0x23};
    TEST_BEGIN("fsub_d [ebx]")
    __a.fsub_d(Address(ebx));
    CHECK_HEX(e, sizeof(e), "fsub qword ptr [rbx]");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xDC, 0x0B};
    TEST_BEGIN("fmul_d [ebx]")
    __a.fmul_d(Address(ebx));
    CHECK_HEX(e, sizeof(e), "fmul qword ptr [rbx]");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xDC, 0x33};
    TEST_BEGIN("fdiv_d [ebx]")
    __a.fdiv_d(Address(ebx));
    CHECK_HEX(e, sizeof(e), "fdiv qword ptr [rbx]");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xDC, 0xC1};
    TEST_BEGIN("fadd(1)")
    __a.fadd(1);
    CHECK_HEX(e, sizeof(e), "fadd st(1), st(0)");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xDC, 0xE9};
    TEST_BEGIN("fsub(1)")
    __a.fsub(1);
    CHECK_HEX(e, sizeof(e), "fsub st(1), st(0)");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xDC, 0xC9};
    TEST_BEGIN("fmul(1)")
    __a.fmul(1);
    CHECK_HEX(e, sizeof(e), "fmul st(1), st(0)");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xDC, 0xF9};
    TEST_BEGIN("fdiv(1)")
    __a.fdiv(1);
    CHECK_HEX(e, sizeof(e), "fdiv st(1), st(0)");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xDE, 0xC1};
    TEST_BEGIN("faddp(1)")
    __a.faddp(1);
    CHECK_HEX(e, sizeof(e), "faddp st(1)");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xDE, 0xE9};
    TEST_BEGIN("fsubp(1)")
    __a.fsubp(1);
    CHECK_HEX(e, sizeof(e), "fsubp st(1)");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xDE, 0xE1};
    TEST_BEGIN("fsubrp(1)")
    __a.fsubrp(1);
    CHECK_HEX(e, sizeof(e), "fsubrp st(1)");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xDE, 0xC9};
    TEST_BEGIN("fmulp(1)")
    __a.fmulp(1);
    CHECK_HEX(e, sizeof(e), "fmulp st(1)");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xDE, 0xF9};
    TEST_BEGIN("fdivp(1)")
    __a.fdivp(1);
    CHECK_HEX(e, sizeof(e), "fdivp st(1)");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xD9, 0xF8};
    TEST_BEGIN("fprem")
    __a.fprem();
    CHECK_HEX(e, sizeof(e), "fprem");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xD9, 0xF5};
    TEST_BEGIN("fprem1")
    __a.fprem1();
    CHECK_HEX(e, sizeof(e), "fprem1");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xD9, 0xC9};
    TEST_BEGIN("fxch(1)")
    __a.fxch(1);
    CHECK_HEX(e, sizeof(e), "fxch st(1)");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xD9, 0xF7};
    TEST_BEGIN("fincstp")
    __a.fincstp();
    CHECK_HEX(e, sizeof(e), "fincstp");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xDD, 0xC0};
    TEST_BEGIN("ffree(0)")
    __a.ffree(0);
    CHECK_HEX(e, sizeof(e), "ffree st(0)");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xD9, 0xE4};
    TEST_BEGIN("ftst")
    __a.ftst();
    CHECK_HEX(e, sizeof(e), "ftst");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xDE, 0xD9};
    TEST_BEGIN("fcompp")
    __a.fcompp();
    CHECK_HEX(e, sizeof(e), "fcompp");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xDF, 0xE0};
    TEST_BEGIN("fnstsw_ax")
    __a.fnstsw_ax();
    CHECK_HEX(e, sizeof(e), "fnstsw ax");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x9B};
    TEST_BEGIN("fwait")
    __a.fwait();
    CHECK_HEX(e, sizeof(e), "wait");
    TEST_END
  }
}

static void test_labels() {
  {
    static const unsigned char e[] = {0x90, 0x90, 0x74, 0xFC};
    TEST_BEGIN("backward jcc short (jz back over 2 bytes)")
    Label L;
    __a.bind(L);
    __a.nop();
    __a.nop();
    __a.jcc(X86Assembler::zero, L);
    CHECK_HEX(e, sizeof(e), "nop ; nop ; je 0");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x0F, 0x85, 0x01, 0x00, 0x00, 0x00, 0x90};
    TEST_BEGIN("forward jcc long")
    Label L;
    __a.jcc(X86Assembler::notEqual, L);
    __a.nop();
    __a.bind(L);
    CHECK_HEX(e, sizeof(e), "jne 7 ; nop");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xE9, 0x01, 0x00, 0x00, 0x00, 0x90};
    TEST_BEGIN("forward jmp")
    Label L;
    __a.jmp(L);
    __a.nop();
    __a.bind(L);
    CHECK_HEX(e, sizeof(e), "jmp 6 ; nop");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x90, 0xEB, 0xFD};
    TEST_BEGIN("backward jmp short")
    Label L;
    __a.bind(L);
    __a.nop();
    __a.jmp(L);
    CHECK_HEX(e, sizeof(e), "nop ; jmp 0");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x90, 0xE8, 0xFA, 0xFF, 0xFF, 0xFF};
    TEST_BEGIN("backward call")
    Label L;
    __a.bind(L);
    __a.nop();
    __a.call(L);
    CHECK_HEX(e, sizeof(e), "nop ; call 0");
    TEST_END
  }
  {
    static const unsigned char e[] = {0xE8, 0x01, 0x00, 0x00, 0x00, 0x90};
    TEST_BEGIN("forward call")
    Label L;
    __a.call(L);
    __a.nop();
    __a.bind(L);
    CHECK_HEX(e, sizeof(e), "call 6 ; nop");
    TEST_END
  }
}

static void test_x86_64() {
  {
    static const unsigned char e[] = {0x48, 0x8B, 0xC3};
    TEST_BEGIN("movq eax, ebx")
    __a.movq(eax, ebx);
    CHECK_HEX(e, sizeof(e), "mov rax, rbx");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x48, 0x8B, 0x43, 0x08};
    TEST_BEGIN("movq eax, [ebx+8]")
    __a.movq(eax, Address(ebx, 8));
    CHECK_HEX(e, sizeof(e), "mov rax, qword ptr [rbx + 8]");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x48, 0x89, 0x43, 0x08};
    TEST_BEGIN("movq [ebx+8], eax")
    __a.movq(Address(ebx, 8), eax);
    CHECK_HEX(e, sizeof(e), "mov qword ptr [rbx + 8], rax");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x48, 0xC7, 0x43, 0x08, 0x2A, 0x00, 0x00, 0x00};
    TEST_BEGIN("movq [ebx+8], 0x2A")
    __a.movq(Address(ebx, 8), 0x2A);
    CHECK_HEX(e, sizeof(e), "mov qword ptr [rbx + 8], 0x2a");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x48, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
    TEST_BEGIN("movq eax, 0x1122334455667788 (movabs)")
    __a.movq(eax, (intptr_t)0x1122334455667788LL);
    CHECK_HEX(e, sizeof(e), "movabs rax, 0x1122334455667788");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x48, 0x03, 0xC3};
    TEST_BEGIN("addq eax, ebx")
    __a.addq(eax, ebx);
    CHECK_HEX(e, sizeof(e), "add rax, rbx");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x48, 0x83, 0xC0, 0x0A};
    TEST_BEGIN("addq eax, 0x0A")
    __a.addq(eax, 0x0A);
    CHECK_HEX(e, sizeof(e), "add rax, 0xa");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x48, 0x81, 0xC0, 0x78, 0x56, 0x34, 0x12};
    TEST_BEGIN("addq eax, 0x12345678")
    __a.addq(eax, 0x12345678);
    CHECK_HEX(e, sizeof(e), "add rax, 0x12345678");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x48, 0x2B, 0xC3};
    TEST_BEGIN("subq eax, ebx")
    __a.subq(eax, ebx);
    CHECK_HEX(e, sizeof(e), "sub rax, rbx");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x48, 0x3B, 0xC3};
    TEST_BEGIN("cmpq eax, ebx")
    __a.cmpq(eax, ebx);
    CHECK_HEX(e, sizeof(e), "cmp rax, rbx");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x48, 0x23, 0xC3};
    TEST_BEGIN("andq eax, ebx")
    __a.andq(eax, ebx);
    CHECK_HEX(e, sizeof(e), "and rax, rbx");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x48, 0x0B, 0xC3};
    TEST_BEGIN("orq eax, ebx")
    __a.orq(eax, ebx);
    CHECK_HEX(e, sizeof(e), "or rax, rbx");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x48, 0x33, 0xC3};
    TEST_BEGIN("xorq eax, ebx")
    __a.xorq(eax, ebx);
    CHECK_HEX(e, sizeof(e), "xor rax, rbx");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x48, 0x85, 0xC3};
    TEST_BEGIN("testq eax, ebx")
    __a.testq(eax, ebx);
    CHECK_HEX(e, sizeof(e), "test rbx, rax");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x48, 0xF7, 0xD8};
    TEST_BEGIN("negq eax")
    __a.negq(eax);
    CHECK_HEX(e, sizeof(e), "neg rax");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x48, 0xF7, 0xD0};
    TEST_BEGIN("notq eax")
    __a.notq(eax);
    CHECK_HEX(e, sizeof(e), "not rax");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x48, 0x53};
    TEST_BEGIN("pushq ebx")
    __a.pushq(ebx);
    CHECK_HEX(e, sizeof(e), "push rbx");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x48, 0x58};
    TEST_BEGIN("popq eax")
    __a.popq(eax);
    CHECK_HEX(e, sizeof(e), "pop rax");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x48, 0xFF, 0xC0};
    TEST_BEGIN("incq eax")
    __a.incq(eax);
    CHECK_HEX(e, sizeof(e), "inc rax");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x48, 0xFF, 0xC8};
    TEST_BEGIN("decq eax")
    __a.decq(eax);
    CHECK_HEX(e, sizeof(e), "dec rax");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x48, 0x99};
    TEST_BEGIN("cqo")
    __a.cqo();
    CHECK_HEX(e, sizeof(e), "cqo");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x48, 0x63, 0xC3};
    TEST_BEGIN("movsxq eax, ebx")
    __a.movsxq(eax, ebx);
    CHECK_HEX(e, sizeof(e), "movsxd rax, ebx");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x48, 0x8D, 0x43, 0x08};
    TEST_BEGIN("leaq eax, [ebx+8]")
    __a.leaq(eax, Address(ebx, 8));
    CHECK_HEX(e, sizeof(e), "lea rax, [rbx + 8]");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x48, 0xC1, 0xE0, 0x02};
    TEST_BEGIN("shlq eax, 2")
    __a.shlq(eax, 2);
    CHECK_HEX(e, sizeof(e), "shl rax, 2");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x48, 0xC1, 0xF8, 0x02};
    TEST_BEGIN("sarq eax, 2")
    __a.sarq(eax, 2);
    CHECK_HEX(e, sizeof(e), "sar rax, 2");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x48, 0xC1, 0xE8, 0x02};
    TEST_BEGIN("shrq eax, 2")
    __a.shrq(eax, 2);
    CHECK_HEX(e, sizeof(e), "shr rax, 2");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x48, 0x0F, 0xAF, 0xC3};
    TEST_BEGIN("imulq eax, ebx")
    __a.imulq(eax, ebx);
    CHECK_HEX(e, sizeof(e), "imul rax, rbx");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x48, 0x6B, 0xC3, 0x02};
    TEST_BEGIN("imulq eax, ebx, 2")
    __a.imulq(eax, ebx, 2);
    CHECK_HEX(e, sizeof(e), "imul rax, rbx, 2");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x48, 0xF7, 0xFB};
    TEST_BEGIN("idivq ebx")
    __a.idivq(ebx);
    CHECK_HEX(e, sizeof(e), "idiv rbx");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x48, 0xF7, 0xE3};
    TEST_BEGIN("mulq ebx")
    __a.mulq(ebx);
    CHECK_HEX(e, sizeof(e), "mul rbx");
    TEST_END
  }

  // extended registers (REX.R/REX.B)
  {
    static const unsigned char e[] = {
      0x4d,
      0x8b,
      0xc1,
    };
    TEST_BEGIN("movl r8d, r9d (REX.R/B)")
    __a.movl(r8, r9);
    CHECK_HEX(e, sizeof(e), "mov r8, r9");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x4D, 0x8B, 0xC1};
    TEST_BEGIN("movq r8, r9 (REX.W+R/B)")
    __a.movq(r8, r9);
    CHECK_HEX(e, sizeof(e), "mov r8, r9");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x49, 0x8B, 0x40, 0x08};
    TEST_BEGIN("movq rax, [r8+8] (REX.B)")
    __a.movq(eax, Address(r8, 8));
    CHECK_HEX(e, sizeof(e), "mov rax, qword ptr [r8 + 8]");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x4c,
      0x8b,
      0x03,
    };
    TEST_BEGIN("movl r8d, [ebx] (REX.R)")
    __a.movl(r8, Address(ebx));
    CHECK_HEX(e, sizeof(e), "mov r8, qword ptr [rbx]");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x41, 0xB8, 0x34, 0x12, 0x00, 0x00};
    TEST_BEGIN("movl r8d, 0x1234 (REX.B)")
    __a.movl(r8, 0x1234);
    CHECK_HEX(e, sizeof(e), "mov r8d, 0x1234");
    TEST_END
  }

  // word-size-dependent forms (must use the 64-bit encodings)
  {
    static const unsigned char e[] = {
      0x48,
      0xff,
      0xc0,
    };
    TEST_BEGIN("incl eax (64-bit encoding)")
    __a.incl(eax);
    CHECK_HEX(e, sizeof(e), "inc rax");
    TEST_END
  }
  {
    static const unsigned char e[] = {
      0x48,
      0xff,
      0xc8,
    };
    TEST_BEGIN("decl eax (64-bit encoding)")
    __a.decl(eax);
    CHECK_HEX(e, sizeof(e), "dec rax");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x48, 0x55, 0x48, 0x8B, 0xEC};
    TEST_BEGIN("enter (64-bit)")
    __a.enter();
    CHECK_HEX(e, sizeof(e), "push rbp ; mov rbp, rsp");
    TEST_END
  }
  {
    static const unsigned char e[] = {0x48, 0x8B, 0xE5, 0x48, 0x5D};
    TEST_BEGIN("leave (64-bit)")
    __a.leave();
    CHECK_HEX(e, sizeof(e), "mov rsp, rbp ; pop rbp");
    TEST_END
  }
}

int main(int argc, char** argv) {
  g_dump_mode = (argc > 1 && std::strcmp(argv[1], "-d") == 0);
  std::printf("x86 encoder test (64-bit)\n");

  test_moves();
  test_arith();
  test_shifts();
  test_stack();
  test_control_flow();
  test_fpu();
  test_labels();
  test_x86_64();

  if (!g_dump_mode)
    std::printf("\n%d of %d tests passed\n", total - failures, total);
  return failures == 0 ? 0 : 1;
}

#endif // !defined(DELTA_BACKEND_AARCH64)
