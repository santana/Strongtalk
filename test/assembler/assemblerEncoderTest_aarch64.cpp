/*
Copyright (c) 2026, Gerardo Santana Gomez Garrido.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/* Test harness for the AArch64 (ARM64) encoder.
 *
 * Every test encodes one instruction (or a short sequence) with the AArch64
 * backend and checks the emitted little-endian words against golden bytes
 * produced by Apple clang (arm64-apple-macos11). The three "full sequence"
 * tests (ref_all, ref_pairs, ref_bitfield) re-emit entire assembly files
 * instruction-for-instruction, so the expected words are literally the bytes
 * of the corresponding clang-compiled objects (vm/../test ../../opencode refs).
 *
 * The harness is standalone: it does not link the rest of the VM. The
 * minimal runtime needed by the encoder (CodeBuffer, debug flags, error
 * reporting) is provided here; everything else is left unresolved and the
 * executable is linked with undefined symbols allowed.
 *
 * This file is inert without -DDELTA_ASSEMBLER_BACKEND_AARCH64 so that the
 * VM's stest build (which compiles the other encoder tests with the x86
 * backend) is unaffected.
 */

#ifdef DELTA_ASSEMBLER_BACKEND_AARCH64

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
#include <cstdint>

// ---------------------------------------------------------------------------
// minimal runtime for the standalone encoder
// ---------------------------------------------------------------------------

extern "C" bool CodeForP6 = false;
extern "C" bool EnableInt3 = true;
extern "C" bool EliminateJumpsToJumps = false;
extern "C" bool PrintJumpElimination = false;

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

char* StubRoutines::_call_inspector_entry;

extern "C" oop nilObj = NULL;
extern "C" oop trueObj = NULL;
extern "C" oop falseObj = NULL;

extern "C" char* byte_map_base = NULL;

extern "C" void** last_Delta_fp = NULL;
extern "C" oop* last_Delta_sp = NULL;

VirtualSpace::VirtualSpace() {
  _low_boundary = NULL;
  _high_boundary = NULL;
  _low_to_high = true;
  _low = NULL;
  _high = NULL;
}
VirtualSpace::~VirtualSpace() {}

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

static void check_words(const char* name, unsigned char* code, int len, const uint32_t* expected, int elen) {
  total++;
  bool ok = (len == elen * 4);
  if (ok) {
    for (int i = 0; i < elen; i++) {
      uint32_t w = 0;
      std::memcpy(&w, code + i * 4, 4);
      if (w != expected[i]) {
        ok = false;
        break;
      }
    }
  }
  if (ok) {
    std::printf("  ok  %s\n", name);
    return;
  }
  failures++;
  std::printf("FAIL %s (len %d, expected %d words)\n", name, len, elen);
  std::printf("  actual:   ");
  for (int i = 0; i < len; i++)
    std::printf("%02x ", code[i]);
  std::printf("\n  expected:");
  for (int i = 0; i < elen; i++)
    std::printf(" %08x", expected[i]);
  std::printf("\n");
}

#define TEST_BEGIN(name)                                                                                               \
  do {                                                                                                                 \
    const char* __test_name = (name);                                                                                  \
    std::memset(buf, 0, sizeof(buf));                                                                                  \
    CodeBuffer __cb((char*)buf, sizeof(buf));                                                                          \
    AArch64MacroAssembler __a(&__cb);

#define CHECK_WORDS(expected, elen) check_words(__test_name, buf, __cb.code_size(), (expected), (elen));

#define TEST_END                                                                                                       \
  }                                                                                                                    \
  while (0)                                                                                                            \
    ;

// ---------------------------------------------------------------------------
// single instruction tests
// ---------------------------------------------------------------------------

static void test_logical_shifted() {
  {
    static const uint32_t e[] = {0xAA020020}; // orr x0, x1, x2
    TEST_BEGIN("orr x0, x1, x2")
    __a.orr(x0, x1, x2);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xCA0700C5}; // eor x5, x6, x7
    TEST_BEGIN("eor x5, x6, x7")
    __a.eor(x5, x6, x7);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x8A0A0128}; // and x8, x9, x10
    TEST_BEGIN("and x8, x9, x10")
    __a.and_(x8, x9, x10);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xEA0D018B}; // ands x11, x12, x13
    TEST_BEGIN("ands x11, x12, x13")
    __a.ands(x11, x12, x13);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xEA02003F}; // tst x1, x2
    TEST_BEGIN("tst x1, x2")
    __a.tst(x1, x2);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x8A250083}; // bic x3, x4, x5
    TEST_BEGIN("bic x3, x4, x5")
    __a.bic(x3, x4, x5);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x8A250C83}; // bic x3, x4, x5, lsl #3
    TEST_BEGIN("bic x3, x4, x5, lsl #3")
    __a.bic(x3, x4, x5, LSL, 3);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xAA2800E6}; // orn x6, x7, x8
    TEST_BEGIN("orn x6, x7, x8")
    __a.orn(x6, x7, x8);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xAA6814E6}; // orn x6, x7, x8, lsr #5
    TEST_BEGIN("orn x6, x7, x8, lsr #5")
    __a.orn(x6, x7, x8, LSR, 5);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xCA2B0149}; // eon x9, x10, x11
    TEST_BEGIN("eon x9, x10, x11")
    __a.eon(x9, x10, x11);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x8A851883}; // and x3, x4, x5, asr #6
    TEST_BEGIN("and x3, x4, x5, asr #6")
    __a.and_(x3, x4, x5, ASR, 6);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xAAC51C83}; // orr x3, x4, x5, ror #7
    TEST_BEGIN("orr x3, x4, x5, ror #7")
    __a.orr(x3, x4, x5, ROR, 7);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x0A020020}; // and w0, w1, w2
    TEST_BEGIN("and w0, w1, w2")
    __a.and_(w0, w1, w2, LSL, 0, sz_32);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x2A020420}; // orr w0, w1, w2, lsl #1
    TEST_BEGIN("orr w0, w1, w2, lsl #1")
    __a.orr(w0, w1, w2, LSL, 1, sz_32);
    CHECK_WORDS(e, 1);
    TEST_END
  }
}

static void test_logical_imm() {
  {
    static const uint32_t e[] = {0x92401C20}; // and x0, x1, #0xff
    TEST_BEGIN("and x0, x1, #0xff")
    __a.and_(x0, x1, 0xffull);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x9200F062}; // and x2, x3, #0x5555555555555555
    TEST_BEGIN("and x2, x3, #0x5555555555555555")
    __a.and_(x2, x3, 0x5555555555555555ull);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x920104A4}; // and x4, x5, #0x8000000180000001
    TEST_BEGIN("and x4, x5, #0x8000000180000001")
    __a.and_(x4, x5, 0x8000000180000001ull);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x92407CE6}; // and x6, x7, #0x00000000ffffffff
    TEST_BEGIN("and x6, x7, #0x00000000ffffffff")
    __a.and_(x6, x7, 0xffffffffull);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xB2400128}; // orr x8, x9, #0x1
    TEST_BEGIN("orr x8, x9, #0x1")
    __a.orr(x8, x9, 0x1ull);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xD24C1D6A}; // eor x10, x11, #0x0ff0000000000000
    TEST_BEGIN("eor x10, x11, #0x0ff0000000000000")
    __a.eor(x10, x11, 0x0ff0000000000000ull);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xF240EDAC}; // ands x12, x13, #0xfffffffffffffff
    TEST_BEGIN("ands x12, x13, #0xfffffffffffffff")
    __a.ands(x12, x13, 0xfffffffffffffffull);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xF2401C1F}; // tst x0, #0xff
    TEST_BEGIN("tst x0, #0xff")
    __a.tst(x0, 0xffull);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x72001C1F}; // tst w0, #0xff
    TEST_BEGIN("tst w0, #0xff")
    __a.tst(w0, 0xffull, sz_32);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    // mov w0, #0: the 64-bit macro expands to orr x0, xzr, xzr (same register
    // number as w0, so the word is the X-register form)
    static const uint32_t e[] = {0xAA1F03E0};
    TEST_BEGIN("mov w0, #0 (64-bit orr)")
    __a.mov(w0, 0);
    CHECK_WORDS(e, 1);
    TEST_END
  }
}

static void test_addsub() {
  {
    static const uint32_t e[] = {0x9103FC20}; // add x0, x1, #0xff
    TEST_BEGIN("add x0, x1, #0xff")
    __a.add(x0, x1, 0xff);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x913FC062}; // add x2, x3, #0xff0
    TEST_BEGIN("add x2, x3, #0xff0")
    __a.add(x2, x3, 0xff0);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x914004A4}; // add x4, x5, #0x1000
    TEST_BEGIN("add x4, x5, #0x1000")
    __a.add(x4, x5, 0x1, 1);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x9143FCE6}; // add x6, x7, #0x0ff000
    TEST_BEGIN("add x6, x7, #0x0ff000")
    __a.add(x6, x7, 0xff, 1);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xD1048D28}; // sub x8, x9, #0x123
    TEST_BEGIN("sub x8, x9, #0x123")
    __a.sub(x8, x9, 0x123);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x9140056A}; // add x10, x11, #0x1, lsl #12
    TEST_BEGIN("add x10, x11, #0x1, lsl #12")
    __a.add(x10, x11, 0x1, 1);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xD14005AC}; // sub x12, x13, #0x1, lsl #12
    TEST_BEGIN("sub x12, x13, #0x1, lsl #12")
    __a.sub(x12, x13, 0x1, 1);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xF100401F}; // cmp x0, #0x10
    TEST_BEGIN("cmp x0, #0x10")
    __a.cmp(x0, 0x10);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xB100405F}; // cmn x2, #0x10
    TEST_BEGIN("cmn x2, #0x10")
    __a.cmn(x2, 0x10);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xCB0403E3}; // neg x3, x4
    TEST_BEGIN("neg x3, x4")
    __a.neg(x3, x4);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xCB0613E5}; // neg x5, x6, lsl #4
    TEST_BEGIN("neg x5, x6, lsl #4")
    __a.neg(x5, x6, LSL, 4);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x8B020020}; // add x0, x1, x2
    TEST_BEGIN("add x0, x1, x2")
    __a.add(x0, x1, x2);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x8B052083}; // add x3, x4, x5, lsl #8
    TEST_BEGIN("add x3, x4, x5, lsl #8")
    __a.add(x3, x4, x5, LSL, 8);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xCB4824E6}; // sub x6, x7, x8, lsr #9
    TEST_BEGIN("sub x6, x7, x8, lsr #9")
    __a.sub(x6, x7, x8, LSR, 9);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x8B8B2949}; // add x9, x10, x11, asr #10
    TEST_BEGIN("add x9, x10, x11, asr #10")
    __a.add(x9, x10, x11, ASR, 10);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xEB01001F}; // cmp x0, x1
    TEST_BEGIN("cmp x0, x1")
    __a.cmp(x0, x1);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xEB01141F}; // cmp x0, x1, lsl #5
    TEST_BEGIN("cmp x0, x1, lsl #5")
    __a.cmp(x0, x1, LSL, 5);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    // sp (x31) is not a valid source in the shifted-register encodings (it
    // decodes as xzr), so the macro layer materializes it through a scratch
    // GPR: add x17, sp, #0; cmp x17, x16.
    static const uint32_t e[] = {0x910003F1, 0xEB10023F}; // cmpl esp, x16
    TEST_BEGIN("cmpl esp, x16")
    __a.cmpl(esp, x16);
    CHECK_WORDS(e, 2);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x910003F0, 0xEB10001F}; // cmpl x0, esp
    TEST_BEGIN("cmpl x0, esp")
    __a.cmpl(x0, esp);
    CHECK_WORDS(e, 2);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x910003F0, 0x8B0B021F}; // addl esp, x11
    TEST_BEGIN("addl esp, x11")
    __a.addl(esp, x11);
    CHECK_WORDS(e, 2);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x910003F0, 0xCB0B021F}; // subl esp, x11
    TEST_BEGIN("subl esp, x11")
    __a.subl(esp, x11);
    CHECK_WORDS(e, 2);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xAB01001F}; // cmn x0, x1
    TEST_BEGIN("cmn x0, x1")
    __a.cmn(x0, x1);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xCB0103E0}; // neg x0, x1
    TEST_BEGIN("neg x0, x1")
    __a.neg(x0, x1);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x11000020}; // add w0, w1, #0
    TEST_BEGIN("add w0, w1, #0")
    __a.add(w0, w1, 0, 0, sz_32);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x7100001F}; // cmp w0, #0
    TEST_BEGIN("cmp w0, #0")
    __a.cmp(w0, 0, 0, sz_32);
    CHECK_WORDS(e, 1);
    TEST_END
  }
}

static void test_bitfield() {
  {
    static const uint32_t e[] = {0xD376D420}; // lsl x0, x1, #10
    TEST_BEGIN("lsl x0, x1, #10")
    __a.lsl(x0, x1, 10);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xD34BFC62}; // lsr x2, x3, #11
    TEST_BEGIN("lsr x2, x3, #11")
    __a.lsr(x2, x3, 11);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x934CFCA4}; // asr x4, x5, #12
    TEST_BEGIN("asr x4, x5, #12")
    __a.asr(x4, x5, 12);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xD340FC62}; // lsr x2, x3, #0
    TEST_BEGIN("lsr x2, x3, #0")
    __a.lsr(x2, x3, 0);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xD3442C20}; // ubfx x0, x1, #4, #8
    TEST_BEGIN("ubfx x0, x1, #4, #8")
    __a.ubfx(x0, x1, 4, 8);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x93453462}; // sbfx x2, x3, #5, #9
    TEST_BEGIN("sbfx x2, x3, #5, #9")
    __a.sbfx(x2, x3, 5, 9);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xD37B24E6}; // ubfiz x6, x7, #5, #10
    TEST_BEGIN("ubfiz x6, x7, #5, #10")
    __a.ubfiz(x6, x7, 5, 10);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x937D1820}; // sbfiz x0, x1, #3, #7
    TEST_BEGIN("sbfiz x0, x1, #3, #7")
    __a.sbfiz(x0, x1, 3, 7);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xB3782CE6}; // bfi x6, x7, #8, #12
    TEST_BEGIN("bfi x6, x7, #8, #12")
    __a.bfi(x6, x7, 8, 12);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xB3443528}; // bfxil x8, x9, #4, #10
    TEST_BEGIN("bfxil x8, x9, #4, #10")
    __a.bfxil(x8, x9, 4, 10);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x53001D28}; // uxtb w8, w9
    TEST_BEGIN("uxtb w8, w9")
    __a.uxtb(w8, w9);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x53003D6A}; // uxth w10, w11
    TEST_BEGIN("uxth w10, w11")
    __a.uxth(w10, w11);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x13001C62}; // sxtb w2, w3
    TEST_BEGIN("sxtb w2, w3")
    __a.sxtb(w2, w3);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x13003CA4}; // sxth w4, w5
    TEST_BEGIN("sxth w4, w5")
    __a.sxth(w4, w5);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x53165420}; // lsl w0, w1, #10
    TEST_BEGIN("lsl w0, w1, #10")
    __a.lsl(w0, w1, 10, sz_32);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x530B7C62}; // lsr w2, w3, #11
    TEST_BEGIN("lsr w2, w3, #11")
    __a.lsr(w2, w3, 11, sz_32);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x130C7D28}; // asr w8, w9, #12
    TEST_BEGIN("asr w8, w9, #12")
    __a.asr(w8, w9, 12, sz_32);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x53042CA4}; // ubfx w4, w5, #4, #8
    TEST_BEGIN("ubfx w4, w5, #4, #8")
    __a.ubfx(w4, w5, 4, 8, sz_32);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x1305356A}; // sbfx w10, w11, #5, #9
    TEST_BEGIN("sbfx w10, w11, #5, #9")
    __a.sbfx(w10, w11, 5, 9, sz_32);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x53001CE6}; // uxtb w6, w7
    TEST_BEGIN("uxtb w6, w7")
    __a.uxtb(w6, w7);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x13001DAC}; // sxtb w12, w13
    TEST_BEGIN("sxtb w12, w13")
    __a.sxtb(w12, w13);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x33182DEE}; // bfi w14, w15, #8, #12
    TEST_BEGIN("bfi w14, w15, #8, #12")
    __a.bfi(w14, w15, 8, 12, sz_32);
    CHECK_WORDS(e, 1);
    TEST_END
  }
}

static void test_dp2src_madd() {
  {
    static const uint32_t e[] = {0x9AC22020}; // lslv x0, x1, x2
    TEST_BEGIN("lslv x0, x1, x2")
    __a.lslv(x0, x1, x2);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x9AC52483}; // lsrv x3, x4, x5
    TEST_BEGIN("lsrv x3, x4, x5")
    __a.lsrv(x3, x4, x5);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x9AC828E6}; // asrv x6, x7, x8
    TEST_BEGIN("asrv x6, x7, x8")
    __a.asrv(x6, x7, x8);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x9ACB0949}; // udiv x9, x10, x11
    TEST_BEGIN("udiv x9, x10, x11")
    __a.udiv(x9, x10, x11);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x9ACE0DAC}; // sdiv x12, x13, x14
    TEST_BEGIN("sdiv x12, x13, x14")
    __a.sdiv(x12, x13, x14);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x9B020C20}; // madd x0, x1, x2, x3
    TEST_BEGIN("madd x0, x1, x2, x3")
    __a.madd(x0, x1, x2, x3);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x9B067CA4}; // mul x4, x5, x6
    TEST_BEGIN("mul x4, x5, x6")
    __a.mul(x4, x5, x6);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x1B020C20}; // madd w0, w1, w2, w3
    TEST_BEGIN("madd w0, w1, w2, w3")
    __a.madd(w0, w1, w2, w3, sz_32);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x1B067CA4}; // mul w4, w5, w6
    TEST_BEGIN("mul w4, w5, w6")
    __a.mul(w4, w5, w6, sz_32);
    CHECK_WORDS(e, 1);
    TEST_END
  }
}

static void test_mem() {
  {
    static const uint32_t e[] = {0xF9400020}; // ldr x0, [x1]
    TEST_BEGIN("ldr x0, [x1]")
    __a.ldr(x0, Address(x1));
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xF9400462}; // ldr x2, [x3, #8]
    TEST_BEGIN("ldr x2, [x3, #8]")
    __a.ldr(x2, Address(x3, 8));
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xF947F8A4}; // ldr x4, [x5, #0xff0]
    TEST_BEGIN("ldr x4, [x5, #0xff0]")
    __a.ldr(x4, Address(x5, 0xff0));
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xF94800E6}; // ldr x6, [x7, #0x1000]
    TEST_BEGIN("ldr x6, [x7, #0x1000]")
    __a.ldr(x6, Address(x7, 0x1000));
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xF9000128}; // str x8, [x9]
    TEST_BEGIN("str x8, [x9]")
    __a.str(x8, Address(x9));
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xF900096A}; // str x10, [x11, #16]
    TEST_BEGIN("str x10, [x11, #16]")
    __a.str(x10, Address(x11, 16));
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xB9400420}; // ldr w0, [x1, #4]
    TEST_BEGIN("ldr w0, [x1, #4]")
    __a.ldr_w(w0, Address(x1, 4));
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xB9000462}; // str w2, [x3, #4]
    TEST_BEGIN("str w2, [x3, #4]")
    __a.str_w(w2, Address(x3, 4));
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x394004A4}; // ldrb w4, [x5, #1]
    TEST_BEGIN("ldrb w4, [x5, #1]")
    __a.ldr_b(w4, Address(x5, 1));
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x390004E6}; // strb w6, [x7, #1]
    TEST_BEGIN("strb w6, [x7, #1]")
    __a.str_b(w6, Address(x7, 1));
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x79400528}; // ldrh w8, [x9, #2]
    TEST_BEGIN("ldrh w8, [x9, #2]")
    __a.ldr_h(w8, Address(x9, 2));
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x7900056A}; // strh w10, [x11, #2]
    TEST_BEGIN("strh w10, [x11, #2]")
    __a.str_h(w10, Address(x11, 2));
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xF85FC020}; // ldur x0, [x1, #-4]
    TEST_BEGIN("ldur x0, [x1, #-4]")
    __a.ldur(x0, Address(x1, -4));
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xF81F8062}; // stur x2, [x3, #-8]
    TEST_BEGIN("stur x2, [x3, #-8]")
    __a.stur(x2, Address(x3, -8));
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xF84FF0A4}; // ldur x4, [x5, #255]
    TEST_BEGIN("ldur x4, [x5, #255]")
    __a.ldur(x4, Address(x5, 255));
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xF81000E6}; // stur x6, [x7, #-256]
    TEST_BEGIN("stur x6, [x7, #-256]")
    __a.stur(x6, Address(x7, -256));
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xF85FCD28}; // ldr x8, [x9, #-4]!
    TEST_BEGIN("ldr x8, [x9, #-4]!")
    __a.ldr_pre(x8, x9, -4);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xF81F8D6A}; // str x10, [x11, #-8]!
    TEST_BEGIN("str x10, [x11, #-8]!")
    __a.str_pre(x10, x11, -8);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xF84105AC}; // ldr x12, [x13], #16
    TEST_BEGIN("ldr x12, [x13], #16")
    __a.ldr_post(x12, x13, 16);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xF81E05EE}; // str x14, [x15], #-32
    TEST_BEGIN("str x14, [x15], #-32")
    __a.str_post(x14, x15, -32);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xB85FC020}; // ldur w0, [x1, #-4]
    TEST_BEGIN("ldur w0, [x1, #-4]")
    __a.ldur(w0, Address(x1, -4), sz_32);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xB81F8062}; // stur w2, [x3, #-8]
    TEST_BEGIN("stur w2, [x3, #-8]")
    __a.stur(w2, Address(x3, -8), sz_32);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xF8626820}; // ldr x0, [x1, x2]
    TEST_BEGIN("ldr x0, [x1, x2]")
    __a.ldr(x0, Address(x1, x2, Address::no_scale));
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xF8657883}; // ldr x3, [x4, x5, lsl #3]
    TEST_BEGIN("ldr x3, [x4, x5, lsl #3]")
    __a.ldr(x3, Address(x4, x5, Address::times_8));
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xF82868E6}; // str x6, [x7, x8]
    TEST_BEGIN("str x6, [x7, x8]")
    __a.str(x6, Address(x7, x8, Address::no_scale));
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xF82B7949}; // str x9, [x10, x11, lsl #3]
    TEST_BEGIN("str x9, [x10, x11, lsl #3]")
    __a.str(x9, Address(x10, x11, Address::times_8));
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xB8627820}; // ldr w0, [x1, x2, lsl #2]
    TEST_BEGIN("ldr w0, [x1, x2, lsl #2]")
    __a.ldr_w(w0, Address(x1, x2, Address::times_4));
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xB8256883}; // str w3, [x4, x5]
    TEST_BEGIN("str w3, [x4, x5]")
    __a.str_w(w3, Address(x4, x5, Address::no_scale));
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x386868E6}; // ldrb w6, [x7, x8]
    TEST_BEGIN("ldrb w6, [x7, x8]")
    __a.ldr_b(w6, Address(x7, x8, Address::no_scale));
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x786B6949}; // ldrh w9, [x10, x11]
    TEST_BEGIN("ldrh w9, [x10, x11]")
    __a.ldr_h(w9, Address(x10, x11, Address::no_scale));
    CHECK_WORDS(e, 1);
    TEST_END
  }
}

static void test_ldp_stp() {
  {
    static const uint32_t e[] = {0xA9400440}; // ldp x0, x1, [x2]
    TEST_BEGIN("ldp x0, x1, [x2]")
    __a.ldp(x0, x1, x2, 0);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xA94210A3}; // ldp x3, x4, [x5, #32]
    TEST_BEGIN("ldp x3, x4, [x5, #32]")
    __a.ldp(x3, x4, x5, 32);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xA9001D06}; // stp x6, x7, [x8]
    TEST_BEGIN("stp x6, x7, [x8]")
    __a.stp(x6, x7, x8, 0);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xA93C2969}; // stp x9, x10, [x11, #-64]
    TEST_BEGIN("stp x9, x10, [x11, #-64]")
    __a.stp(x9, x10, x11, -64);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xA8C135CC}; // ldp x12, x13, [x14], #16
    TEST_BEGIN("ldp x12, x13, [x14], #16")
    __a.ldp_post(x12, x13, x14, 16);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xA9BF0440}; // stp x0, x1, [x2, #-16]!
    TEST_BEGIN("stp x0, x1, [x2, #-16]!")
    __a.stp_pre(x0, x1, x2, -16);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x29400440}; // ldp w0, w1, [x2]
    TEST_BEGIN("ldp w0, w1, [x2]")
    __a.ldp(w0, w1, x2, 0, sz_32);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x290110A3}; // stp w3, w4, [x5, #8]
    TEST_BEGIN("stp w3, w4, [x5, #8]")
    __a.stp(w3, w4, x5, 8, sz_32);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xA9C10440}; // ldp x0, x1, [x2, #16]!
    TEST_BEGIN("ldp x0, x1, [x2, #16]!")
    __a.ldp_pre(x0, x1, x2, 16);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xA8C10440}; // ldp x0, x1, [x2], #16
    TEST_BEGIN("ldp x0, x1, [x2], #16")
    __a.ldp_post(x0, x1, x2, 16);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xA97F8440}; // ldp x0, x1, [x2, #-8]
    TEST_BEGIN("ldp x0, x1, [x2, #-8]")
    __a.ldp(x0, x1, x2, -8);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xA94F8440}; // ldp x0, x1, [x2, #248]
    TEST_BEGIN("ldp x0, x1, [x2, #248]")
    __a.ldp(x0, x1, x2, 248);
    CHECK_WORDS(e, 1);
    TEST_END
  }
}

static void test_mov_wide() {
  {
    static const uint32_t e[] = {0xD2824680}; // movz x0, #0x1234
    TEST_BEGIN("movz x0, #0x1234")
    __a.movz(x0, 0x1234, 0);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xD2A24682}; // movz x2, #0x1234, lsl #16
    TEST_BEGIN("movz x2, #0x1234, lsl #16")
    __a.movz(x2, 0x1234, 1);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xD2C24684}; // movz x4, #0x1234, lsl #32
    TEST_BEGIN("movz x4, #0x1234, lsl #32")
    __a.movz(x4, 0x1234, 2);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xD2E24686}; // movz x6, #0x1234, lsl #48
    TEST_BEGIN("movz x6, #0x1234, lsl #48")
    __a.movz(x6, 0x1234, 3);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xF28ACF08}; // movk x8, #0x5678
    TEST_BEGIN("movk x8, #0x5678")
    __a.movk(x8, 0x5678, 0);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xF2AACF0A}; // movk x10, #0x5678, lsl #16
    TEST_BEGIN("movk x10, #0x5678, lsl #16")
    __a.movk(x10, 0x5678, 1);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x929579AC}; // movn x12, #0xabcd
    TEST_BEGIN("movn x12, #0xabcd")
    __a.movn(x12, 0xabcd, 0);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x52824680}; // movz w0, #0x1234
    TEST_BEGIN("movz w0, #0x1234")
    __a.movz(w0, 0x1234, 0, sz_32);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x72AACF01}; // movk w1, #0x5678, lsl #16
    TEST_BEGIN("movk w1, #0x5678, lsl #16")
    __a.movk(w1, 0x5678, 1, sz_32);
    CHECK_WORDS(e, 1);
    TEST_END
  }
}

static void test_macro_mov() {
  {
    static const uint32_t e[] = {0xAA1F03E0}; // mov x0, xzr (orr x0, xzr, xzr)
    TEST_BEGIN("mov x0, #0")
    __a.mov(x0, 0);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xAA0103E0}; // mov x0, x1
    TEST_BEGIN("mov x0, x1")
    __a.mov(x0, x1);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xB2401FE5}; // mov x5, #255 (orr x5, xzr, #0xff)
    TEST_BEGIN("mov x5, #255")
    __a.mov(x5, 0xff);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xD2A2468A, 0xF28ACF0A}; // mov x10, #0x12345678
    TEST_BEGIN("mov x10, #0x12345678")
    __a.mov(x10, 0x12345678);
    CHECK_WORDS(e, 2);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x929579AC}; // mov x12, #-0xabce
    TEST_BEGIN("mov x12, #-0xabce")
    __a.mov(x12, -0xabce);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xB24003E0}; // mov x0, #1 (orr x0, xzr, #1)
    TEST_BEGIN("mov x0, #1")
    __a.mov(x0, 0x1);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xD2805F42}; // mov x2, #0x2fa (movz)
    TEST_BEGIN("mov x2, #0x2fa")
    __a.mov(x2, 0x2fa);
    CHECK_WORDS(e, 1);
    TEST_END
  }
}

static void test_fp() {
  {
    static const uint32_t e[] = {0x1E622820}; // fadd d0, d1, d2
    TEST_BEGIN("fadd d0, d1, d2")
    __a.fadd(d0, d1, d2);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x1E653883}; // fsub d3, d4, d5
    TEST_BEGIN("fsub d3, d4, d5")
    __a.fsub(d3, d4, d5);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x1E6808E6}; // fmul d6, d7, d8
    TEST_BEGIN("fmul d6, d7, d8")
    __a.fmul(d6, d7, d8);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x1E6B1949}; // fdiv d9, d10, d11
    TEST_BEGIN("fdiv d9, d10, d11")
    __a.fdiv(d9, d10, d11);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x1E6D2180}; // fcmp d12, d13
    TEST_BEGIN("fcmp d12, d13")
    __a.fcmp(d12, d13);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x1E6021C8}; // fcmp d14, #0.0
    TEST_BEGIN("fcmp d14, #0.0")
    __a.fcmp0(d14);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x1E604020}; // fmov d0, d1
    TEST_BEGIN("fmov d0, d1")
    __a.fmov(d0, d1);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x1E60C062}; // fabs d2, d3
    TEST_BEGIN("fabs d2, d3")
    __a.fabs(d2, d3);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x1E6140A4}; // fneg d4, d5
    TEST_BEGIN("fneg d4, d5")
    __a.fneg(d4, d5);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x1E61C0E6}; // fsqrt d6, d7
    TEST_BEGIN("fsqrt d6, d7")
    __a.fsqrt(d6, d7);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x9E780020}; // fcvtzs x0, d1
    TEST_BEGIN("fcvtzs x0, d1")
    __a.fcvtzs(x0, d1, sz_64, sz_64);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x1E780062}; // fcvtzs w2, d3
    TEST_BEGIN("fcvtzs w2, d3")
    __a.fcvtzs(w2, d3, sz_64, sz_32);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x9E380020}; // fcvtzs x0, s1
    TEST_BEGIN("fcvtzs x0, s1")
    __a.fcvtzs(x0, d1, sz_32, sz_64);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x1E222820}; // fadd s0, s1, s2
    TEST_BEGIN("fadd s0, s1, s2")
    __a.fadd(d0, d1, d2, sz_32);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x1E2B1949}; // fdiv s9, s10, s11
    TEST_BEGIN("fdiv s9, s10, s11")
    __a.fdiv(d9, d10, d11, sz_32);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xFD400020}; // ldr d0, [x1]
    TEST_BEGIN("ldr d0, [x1]")
    __a.ldr(d0, Address(x1));
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xFD000862}; // str d2, [x3, #16]
    TEST_BEGIN("str d2, [x3, #16]")
    __a.str(d2, Address(x3, 16));
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xFC5F80A4}; // ldur d4, [x5, #-8]
    TEST_BEGIN("ldur d4, [x5, #-8]")
    __a.ldur(d4, Address(x5, -8));
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xFC4104E6}; // ldr d6, [x7], #16
    TEST_BEGIN("ldr d6, [x7], #16")
    __a.ldr_post(d6, x7, 16);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xBD400020}; // ldr s0, [x1]
    TEST_BEGIN("ldr s0, [x1]")
    __a.ldr_s(d0, Address(x1));
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xBD001062}; // str s2, [x3, #16]
    TEST_BEGIN("str s2, [x3, #16]")
    __a.str_s(d2, Address(x3, 16));
    CHECK_WORDS(e, 1);
    TEST_END
  }
}

static void test_push_pop() {
  {
    static const uint32_t e[] = {0xF81F0FEF}; // str x15, [sp, #-16]!
    TEST_BEGIN("push x15")
    __a.push(x15);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xF84107FE}; // ldr x30, [sp], #16
    TEST_BEGIN("pop x30")
    __a.pop(lr);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xA9BF07E0}; // stp x0, x1, [sp, #-16]!
    TEST_BEGIN("push x0 + x1 (pair)")
    __a.stp_pre(x0, x1, sp, -16);
    CHECK_WORDS(e, 1);
    TEST_END
  }
}

// ---------------------------------------------------------------------------
// x86-compatibility layer (the instruction set the interpreter generator uses)
// ---------------------------------------------------------------------------

static void test_compat_movl() {
  {
    static const uint32_t e[] = {0xD2824681}; // movz x1, #0x1234
    TEST_BEGIN("movl x1, #0x1234")
    __a.movl(x1, 0x1234);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xD2A24681, 0xF28ACF01}; // movz x1, #0x5678, lsl 16; movk x1, #0x1234
    TEST_BEGIN("movl x1, #0x12345678")
    __a.movl(x1, 0x12345678);
    CHECK_WORDS(e, 2);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x92800001}; // orr x1, xzr, #-1
    TEST_BEGIN("movl x1, #-1")
    __a.movl(x1, -1);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xAA0203E1}; // orr x1, xzr, x2
    TEST_BEGIN("movl x1, x2")
    __a.movl(x1, x2);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xF9400041}; // ldr x1, [x2]
    TEST_BEGIN("movl x1, [x2]")
    __a.movl(x1, Address(x2));
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xF9400441}; // ldr x1, [x2, #8]
    TEST_BEGIN("movl x1, [x2, #8]")
    __a.movl(x1, Address(x2, 8));
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xF9000022}; // str x2, [x1]
    TEST_BEGIN("movl [x1], x2")
    __a.movl(Address(x1), x2);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xF9000422}; // str x2, [x1, #8]
    TEST_BEGIN("movl [x1, #8], x2")
    __a.movl(Address(x1, 8), x2);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xAA1F03F0, 0xF9000050}; // movz x1, #0; str x1, [x2]
    TEST_BEGIN("movl [x2], #0")
    __a.movl(Address(x2), 0);
    CHECK_WORDS(e, 2);
    TEST_END
  }
}

static void test_compat_push_pop() {
  {
    static const uint32_t e[] = {0xF81F0FE1}; // str x1, [sp, #-16]!
    TEST_BEGIN("pushl x1")
    __a.pushl(x1);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xF84107E1}; // ldr x1, [sp], #16
    TEST_BEGIN("popl x1")
    __a.popl(x1);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {
      0xA9BF3FED, // stp x13, x15, [sp, #-16]!
      0xA9BF33EB, // stp x11, x12, [sp, #-16]!
      0xA9BF3BFB, // stp x27, x14, [sp, #-16]!
    };
    TEST_BEGIN("pushad")
    __a.pushad();
    CHECK_WORDS(e, 3);
    TEST_END
  }
  {
    static const uint32_t e[] = {
      0xA8C13BFB, // ldp x27, x14, [sp], #16
      0xA8C133EB, // ldp x11, x12, [sp], #16
      0xA8C13FED, // ldp x13, x15, [sp], #16
    };
    TEST_BEGIN("popad")
    __a.popad();
    CHECK_WORDS(e, 3);
    TEST_END
  }
}

static void test_compat_arith() {
  {
    static const uint32_t e[] = {0x91000421}; // add x1, x1, #1
    TEST_BEGIN("addl x1, #1")
    __a.addl(x1, 1);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x913FFC21}; // add x1, x1, #4095
    TEST_BEGIN("addl x1, #4095")
    __a.addl(x1, 4095);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xD1004021}; // sub x1, x1, #16
    TEST_BEGIN("addl x1, #-16")
    __a.addl(x1, -16);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xB27103F0, 0x8B100021}; // movz x16, #0x8000; add x1, x1, x16
    TEST_BEGIN("addl x1, #0x8000")
    __a.addl(x1, 0x8000);
    CHECK_WORDS(e, 2);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xD1002021}; // sub x1, x1, #8
    TEST_BEGIN("subl x1, #8")
    __a.subl(x1, 8);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x91000421}; // add x1, x1, #1
    TEST_BEGIN("incl x1")
    __a.incl(x1);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xD1000421}; // sub x1, x1, #1
    TEST_BEGIN("decl x1")
    __a.decl(x1);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xCB0103E1}; // sub x1, xzr, x1
    TEST_BEGIN("negl x1")
    __a.negl(x1);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xAA2103E1}; // orn x1, xzr, x1
    TEST_BEGIN("notl x1")
    __a.notl(x1);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x9B027C21}; // mul x1, x1, x2
    TEST_BEGIN("imull x1, x2")
    __a.imull(x1, x2);
    CHECK_WORDS(e, 1);
    TEST_END
  }
}

static void test_compat_lea_cmp() {
  {
    static const uint32_t e[] = {0x91001041}; // add x1, x2, #4
    TEST_BEGIN("leal x1, [x2, #4]")
    __a.leal(x1, Address(x2, 4));
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x8B030041}; // add x1, x2, x3
    TEST_BEGIN("leal x1, [x2 + x3]")
    __a.leal(x1, Address(x2, x3, Address::times_1));
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x8B030C41}; // add x1, x2, x3, lsl #3
    TEST_BEGIN("leal x1, [x2 + x3*8]")
    __a.leal(x1, Address(x2, x3, Address::times_8));
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    // leal absolute: ldr x1, [pc, #8]; b .+12 (skips the .quad literal).
    static const uint32_t e[] = {0x58000041, 0x14000003, 0x00001000, 0x00000000};
    TEST_BEGIN("leal x1, [absolute]")
    __a.leal(x1, Address(noreg, noreg, Address::no_scale, 0x1000, relocInfo::none));
    CHECK_WORDS(e, 4);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xEB02003F}; // subs xzr, x1, x2
    TEST_BEGIN("cmpl x1, x2")
    __a.cmpl(x1, x2);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xF100003F}; // cmp x1, #0
    TEST_BEGIN("cmpl x1, #0")
    __a.cmpl(x1, 0);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xF100203F}; // cmp x1, #8
    TEST_BEGIN("cmpl x1, #8")
    __a.cmpl(x1, 8);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xEA02003F}; // ands xzr, x1, x2
    TEST_BEGIN("testl x1, x2")
    __a.testl(x1, x2);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xF240003F}; // ands xzr, x1, #1
    TEST_BEGIN("testl x1, #1")
    __a.testl(x1, 1);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xF240003F}; // ands xzr, x1, #1
    TEST_BEGIN("testb x1, #1")
    __a.testb(x1, 1);
    CHECK_WORDS(e, 1);
    TEST_END
  }
}

static void test_compat_jcc() {
  {
    // 14 branches all jumping to the same target (imm19 = 14..1),
    // encodings verified against clang (ref_compat.s)
    static const uint32_t e[] = {
      0x540001C0, // b.eq  +0x38 (equal)
      0x540001A1, // b.ne  +0x34 (notEqual)
      0x5400018B, // b.lt  +0x30 (less)
      0x5400016D, // b.le  +0x2c (lessEqual)
      0x5400014C, // b.gt  +0x28 (greater)
      0x5400012A, // b.ge  +0x24 (greaterEqual)
      0x54000108, // b.hi  +0x20 (above)
      0x540000E9, // b.ls  +0x1c (belowEqual)
      0x540000C2, // b.hs  +0x18 (aboveEqual)
      0x540000A3, // b.lo  +0x14 (below)
      0x54000084, // b.mi  +0x10 (negative)
      0x54000065, // b.pl  +0x0c (positive)
      0x54000046, // b.vs  +0x08 (overflow)
      0x54000027, // b.vc  +0x04 (noOverflow)
    };
    TEST_BEGIN("jcc x86 aliases")
    Label L;
    __a.jcc(equal, L);
    __a.jcc(notEqual, L);
    __a.jcc(less, L);
    __a.jcc(lessEqual, L);
    __a.jcc(greater, L);
    __a.jcc(greaterEqual, L);
    __a.jcc(above, L);
    __a.jcc(belowEqual, L);
    __a.jcc(aboveEqual, L);
    __a.jcc(below, L);
    __a.jcc(negative, L);
    __a.jcc(positive, L);
    __a.jcc(overflow, L);
    __a.jcc(noOverflow, L);
    __a.bind(L);
    CHECK_WORDS(e, 14);
    TEST_END
  }
}

static void test_compat_dispatch_frame() {
  {
    static const uint32_t e[] = {0xF8627830, 0xD61F0200}; // ldr x16, [x1, x2, lsl #3]; br x16
    TEST_BEGIN("jmp [x1 + x2*8] (dispatch)")
    __a.jmp(Address(x1, x2, Address::times_8));
    CHECK_WORDS(e, 2);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xF8627830, 0xD63F0200}; // ldr x16, [x1, x2, lsl #3]; blr x16
    TEST_BEGIN("call [x1 + x2*8]")
    __a.call(Address(x1, x2, Address::times_8));
    CHECK_WORDS(e, 2);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xF9400030, 0xD61F0200}; // ldr x16, [x1]; br x16
    TEST_BEGIN("jmp [x1]")
    __a.jmp(Address(x1));
    CHECK_WORDS(e, 2);
    TEST_END
  }
  {
    // base + index*8 + disp (the interpreter's arg_addr/temp_addr form)
    static const uint32_t e[] = {
      0x8B0B0E70, // add x16, x19, x11, lsl #3   (fp + index*8)
      0xF9400210, // ldr x16, [x16, #0]          (disp = 0)
      0xD61F0200, // br x16
    };
    TEST_BEGIN("jmp [fp + index*8 + disp]")
    __a.jmp(Address(x19, x11, Address::times_8, 0));
    CHECK_WORDS(e, 3);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x8B0301B0, 0xF9400A00}; // add x16, x13, x3; ldr x0, [x16, #16]
    TEST_BEGIN("movl x0, [obj + smi*1 + disp] (field_addr)")
    __a.movl(x0, Address(x13, x3, Address::times_1, 16));
    CHECK_WORDS(e, 2);
    TEST_END
  }
  {
    static const uint32_t e[] = {
      0xA9BF7BFD, // stp x29, x30, [sp, #-16]!
      0x910003FD, // mov x29, sp
    };
    TEST_BEGIN("enter")
    __a.enter();
    CHECK_WORDS(e, 2);
    TEST_END
  }
  {
    static const uint32_t e[] = {
      0x910003BF, // mov sp, x29
      0xA8C17BFD, // ldp x29, x30, [sp], #16
    };
    TEST_BEGIN("leave")
    __a.leave();
    CHECK_WORDS(e, 2);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xD65F03C0}; // ret
    TEST_BEGIN("ret")
    __a.ret();
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xD65F03C0}; // ret (imm ignored)
    TEST_BEGIN("ret 8")
    __a.ret(8);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xD4200000}; // brk #0
    TEST_BEGIN("hlt")
    __a.hlt();
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xD4200000}; // brk #0
    TEST_BEGIN("int3")
    __a.int3();
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xD503201F}; // nop
    TEST_BEGIN("ic_info placeholder")
    Label L;
    __a.ic_info(L, 0);
    CHECK_WORDS(e, 1);
    TEST_END
  }
}

static void test_compat_misc() {
  {
    static const uint32_t e[] = {
      0xF9400050, // ldr x16, [x2]
      0x91000610, // add x16, x16, #1
      0xF9000050, // str x16, [x2]
    };
    TEST_BEGIN("incl [x2]")
    __a.incl(Address(x2));
    CHECK_WORDS(e, 3);
    TEST_END
  }
  {
    static const uint32_t e[] = {
      0xF9400050, // ldr x16, [x2]
      0x91000A10, // add x16, x16, #2
      0xF9000050, // str x16, [x2]
    };
    TEST_BEGIN("addl [x2], #2")
    __a.addl(Address(x2), 2);
    CHECK_WORDS(e, 3);
    TEST_END
  }
  {
    static const uint32_t e[] = {
      0xD2FFFFF0, // movz x16, #0xffff, lsl 48 (sign-extended 0xbadbabe0)
      0xF2DFFFF0, // movk x16, #0xffff, lsl 32
      0xF2B75B70, // movk x16, #0xbadb, lsl 16
      0xF2957C10, // movk x16, #0xabe0
      0xF81F0FF0, // str x16, [sp, #-16]!
    };
    TEST_BEGIN("pushl #imm (magic marker)")
    __a.pushl(0xbadbabe0);
    CHECK_WORDS(e, 5);
    TEST_END
  }
  {
    // subl(reg, Address)
    static const uint32_t e[] = {
      0xF9400071, // ldr x17, [x3] (scratch avoids clobbering x16)
      0xCB110210, // sub x16, x16, x17
    };
    TEST_BEGIN("subl x16, [x3]")
    __a.subl(x16, Address(x3));
    CHECK_WORDS(e, 2);
    TEST_END
  }
  {
    // shll(reg) with count in ecx (x11): lslv
    static const uint32_t e[] = {0x9ACB2042}; // lsl x2, x2, x11
    TEST_BEGIN("shll x2 (count in ecx)")
    __a.shll(x2);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x9ACB2042, 0x9ACB2842}; // lsl x2,x2,x11; asr x2,x2,x11
    TEST_BEGIN("shll x2 + sarl x2 (variable)")
    __a.shll(x2);
    __a.sarl(x2);
    CHECK_WORDS(e, 2);
    TEST_END
  }
}

// ---------------------------------------------------------------------------
// new FP encoders (encodings verified against clang: ref_float.s / ref_float2.s)
// ---------------------------------------------------------------------------

static void test_fp_new_encoders() {
  {
    static const uint32_t e[] = {0x9E620020}; // scvtf d0, x1
    TEST_BEGIN("scvtf d0, x1")
    __a.scvtf(d0, x1);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x1E620020}; // scvtf d0, w1
    TEST_BEGIN("scvtf d0, w1")
    __a.scvtf(d0, x1, sz_64, sz_32);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x9E220020}; // scvtf s0, x1
    TEST_BEGIN("scvtf s0, x1")
    __a.scvtf(d0, x1, sz_32, sz_64);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x1E220020}; // scvtf s0, w1
    TEST_BEGIN("scvtf s0, w1")
    __a.scvtf(d0, x1, sz_32, sz_32);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x9E6703E0}; // fmov d0, xzr
    TEST_BEGIN("fmov d0, xzr")
    __a.fmov(d0, xzr);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x9E6700A4}; // fmov d4, x5
    TEST_BEGIN("fmov d4, x5")
    __a.fmov(d4, x5);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x9A82B020}; // csel x0, x1, x2, lt
    TEST_BEGIN("csel x0, x1, x2, lt")
    __a.csel(x0, x1, x2, LT);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x9A8860E6}; // csel x6, x7, x8, vs
    TEST_BEGIN("csel x6, x7, x8, vs")
    __a.csel(x6, x7, x8, VS);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x1A8BB149}; // csel w9, w10, w11, lt
    TEST_BEGIN("csel w9, w10, w11, lt")
    __a.csel(x9, x10, x11, LT, sz_32);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xB27803F1}; // orr x17, xzr, #0x100
    TEST_BEGIN("orr x17, xzr, #0x100")
    __a.mov(x17, 0x0100);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xB27203F1}; // orr x17, xzr, #0x4000
    TEST_BEGIN("orr x17, xzr, #0x4000")
    __a.mov(x17, 0x4000);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xD288A011}; // movz x17, #0x4500
    TEST_BEGIN("movz x17, #0x4500 (non-logical imm)")
    __a.mov(x17, 0x4500);
    CHECK_WORDS(e, 1);
    TEST_END
  }
}

// ---------------------------------------------------------------------------
// x87-style float stack (st(i) -> d(8 + depth - 1 - i); push_float/pop_float
// only adjust the depth, so tests set up the stack without emitting code)
// ---------------------------------------------------------------------------

static void test_compat_float() {
  {
    static const uint32_t e[] = {0xFD4005A8}; // ldr d8, [x13, #8]   (fld_d at depth 0)
    TEST_BEGIN("fld_d [eax, #8]")
    __a.fld_d(Address(eax, 8));
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0xFD0005A9}; // str d9, [x13, #8]   (fstp_d at depth 2)
    TEST_BEGIN("fstp_d [eax, #8]")
    __a.push_float();
    __a.push_float();
    __a.fstp_d(Address(eax, 8));
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {
      0xB9400030, // ldr w16, [x1]
      0x1E620208, // scvtf d8, w16
    };
    TEST_BEGIN("fild_s [x1]")
    __a.fild_s(Address(x1));
    CHECK_WORDS(e, 2);
    TEST_END
  }
  {
    static const uint32_t e[] = {
      0x8B020C30, // add x16, x1, x2, lsl #3
      0xFD400A08, // ldr d8, [x16, #16]
    };
    TEST_BEGIN("fld_d [x1 + x2*8 + 16]")
    __a.fld_d(Address(x1, x2, Address::times_8, 16));
    CHECK_WORDS(e, 2);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x9E6703E8}; // fmov d8, xzr
    TEST_BEGIN("fldz")
    __a.fldz();
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x1E6E1008}; // fmov d8, #1.0
    TEST_BEGIN("fld1")
    __a.fld1();
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {}; // fpop emits nothing
    TEST_BEGIN("fpop (depth-only)")
    __a.push_float();
    __a.fpop();
    CHECK_WORDS(e, 0);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x1E60C108}; // fabs d8, d8
    TEST_BEGIN("fabs")
    __a.push_float();
    __a.fabs();
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x1E614108}; // fneg d8, d8
    TEST_BEGIN("fchs")
    __a.push_float();
    __a.fchs();
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x1E61C108}; // fsqrt d8, d8
    TEST_BEGIN("fsqrt")
    __a.push_float();
    __a.fsqrt();
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x1E680908}; // fmul d8, d8, d8
    TEST_BEGIN("fmul(0) (squared)")
    __a.push_float();
    __a.fmul(0);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x1E692908}; // fadd d8, d8, d9
    TEST_BEGIN("faddp")
    __a.push_float();
    __a.push_float();
    __a.faddp();
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x1E693908}; // fsub d8, d8, d9
    TEST_BEGIN("fsubp")
    __a.push_float();
    __a.push_float();
    __a.fsubp();
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x1E690908}; // fmul d8, d8, d9
    TEST_BEGIN("fmulp")
    __a.push_float();
    __a.push_float();
    __a.fmulp();
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x1E691908}; // fdiv d8, d8, d9
    TEST_BEGIN("fdivp")
    __a.push_float();
    __a.push_float();
    __a.fdivp();
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {
      0x1E60413F, // fmov d31, d9
      0x1E604109, // fmov d9, d8
      0x1E6043E8, // fmov d8, d31
    };
    TEST_BEGIN("fxch")
    __a.push_float();
    __a.push_float();
    __a.fxch();
    CHECK_WORDS(e, 3);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x1E602108}; // fcmp d8, #0.0
    TEST_BEGIN("ftst")
    __a.push_float();
    __a.ftst();
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x1E692100}; // fcmp d8, d9
    TEST_BEGIN("fcompp")
    __a.push_float();
    __a.push_float();
    __a.fcompp();
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {
      0xB27803F1, // orr x17, xzr, #0x100
      0x9A9F422D, // csel x13, x17, xzr, mi
      0xB27203F1, // orr x17, xzr, #0x4000
      0x9A8D022D, // csel x13, x17, x13, eq
      0xD288A011, // movz x17, #0x4500
      0x9A8D622D, // csel x13, x17, x13, vs
    };
    TEST_BEGIN("fnstsw_ax")
    __a.push_float();
    __a.fnstsw_ax();
    CHECK_WORDS(e, 6);
    TEST_END
  }
  {
    // fprem: fmov args, align sp, call fmod, restore, store back. The fmod
    // literal is an absolute address, so it is zeroed before comparing.
    static const uint32_t e[] = {
      0x1E604120, // fmov d0, d9
      0x1E604101, // fmov d1, d8
      0x910003F0, // mov x16, sp
      0xF27D021F, // tst x16, #8
      0x540000C0, // b.eq +0x10
      0xD2E82990, // movz x16, #0x414C, lsl48 (stack_pad_marker)
      0xF2C8E9D0, // movk x16, #0x474E, lsl32
      0xF2A82890, // movk x16, #0x4144, lsl16
      0xF2888010, // movk x16, #0x4400
      0xF81F8FF0, // str x16, [sp, #-8]!
      0x58000050, // ldr x16, [pc, #8]
      0xD63F0200, // blr x16
      0x00000000, // .quad <fmod>  (zeroed before comparing)
      0x00000000,
      0xF94003F0, // ldr x16, [sp]
      0xD2E82991, // movz x17, #0x414C, lsl48
      0xF2C8E9D1, // movk x17, #0x474E, lsl32
      0xF2A82891, // movk x17, #0x4144, lsl16
      0xF2888011, // movk x17, #0x4400
      0xEB11021F, // cmp x16, x17
      0x54000041, // b.ne +0x8
      0x910023FF, // add sp, sp, #8
      0x1E604008, // fmov d8, d0
    };
    TEST_BEGIN("fprem (st0 mod st1)")
    __a.push_float();
    __a.push_float();
    __a.fprem();
    std::memset(buf + 12 * 4, 0, 8); // zero the fmod literal
    CHECK_WORDS(e, 23);
    TEST_END
  }
}

static void test_fpu_mask_and_cond() {
  int mask;
  Condition cond;
  total++;
  bool ok = true;
  AArch64MacroAssembler::fpu_mask_and_cond_for(equal, mask, cond);
  ok &= (mask == 0x4000 && cond == notZero);
  AArch64MacroAssembler::fpu_mask_and_cond_for(notEqual, mask, cond);
  ok &= (mask == 0x4000 && cond == zero);
  AArch64MacroAssembler::fpu_mask_and_cond_for(less, mask, cond);
  ok &= (mask == 0x0100 && cond == notZero);
  AArch64MacroAssembler::fpu_mask_and_cond_for(lessEqual, mask, cond);
  ok &= (mask == 0x4500 && cond == notZero);
  AArch64MacroAssembler::fpu_mask_and_cond_for(greater, mask, cond);
  ok &= (mask == 0x4500 && cond == zero);
  AArch64MacroAssembler::fpu_mask_and_cond_for(greaterEqual, mask, cond);
  ok &= (mask == 0x0100 && cond == zero);
  if (ok) {
    std::printf("  ok  fpu_mask_and_cond_for\n");
  } else {
    failures++;
    std::printf("FAIL fpu_mask_and_cond_for\n");
  }
}

// ---------------------------------------------------------------------------
// full sequences (golden bytes from clang)
// ---------------------------------------------------------------------------

static void test_ref_bitfield_sequence() {
  static const uint32_t e[] = {
    0xd376d420, 0xd34bfc62, 0xd3442ca4, 0xd37b24e6, 0x53001d28, 0x53003d6a, 0x934cfdac,
    0x934535ee, 0x937d1820, 0x13001c62, 0x13003ca4, 0xb3782ce6, 0xb3443528, 0x53165420,
    0x530b7c62, 0x53042ca4, 0x53001ce6, 0x130c7d28, 0x1305356a, 0x13001dac, 0x33182dee,
  };
  TEST_BEGIN("ref_bitfield.s sequence")
  __a.lsl(x0, x1, 10);
  __a.lsr(x2, x3, 11);
  __a.ubfx(x4, x5, 4, 8);
  __a.ubfiz(x6, x7, 5, 10);
  __a.uxtb(w8, w9);
  __a.uxth(w10, w11);
  __a.asr(x12, x13, 12);
  __a.sbfx(x14, x15, 5, 9);
  __a.sbfiz(x0, x1, 3, 7);
  __a.sxtb(w2, w3);
  __a.sxth(w4, w5);
  __a.bfi(x6, x7, 8, 12);
  __a.bfxil(x8, x9, 4, 10);
  __a.lsl(w0, w1, 10, sz_32);
  __a.lsr(w2, w3, 11, sz_32);
  __a.ubfx(w4, w5, 4, 8, sz_32);
  __a.uxtb(w6, w7);
  __a.asr(w8, w9, 12, sz_32);
  __a.sbfx(w10, w11, 5, 9, sz_32);
  __a.sxtb(w12, w13);
  __a.bfi(w14, w15, 8, 12, sz_32);
  CHECK_WORDS(e, 21);
  TEST_END
}

static void test_ref_pairs_sequence() {
  static const uint32_t e[] = {
    0xa9400440, 0xa9001d06, 0xa9c10440, 0xa9bf1d06, 0xa8c10440, 0xa8bf1d06, 0x29400440, 0x29001d06,
    0x29c20440, 0x29be1d06, 0x28c20440, 0x28be1d06, 0xa97f8440, 0xa9009d06, 0xa94f8440, 0xa9301d06,
  };
  TEST_BEGIN("ref_pairs.s sequence")
  __a.ldp(x0, x1, x2, 0);
  __a.stp(x6, x7, x8, 0);
  __a.ldp_pre(x0, x1, x2, 16);
  __a.stp_pre(x6, x7, x8, -16);
  __a.ldp_post(x0, x1, x2, 16);
  __a.stp_post(x6, x7, x8, -16);
  __a.ldp(w0, w1, x2, 0, sz_32);
  __a.stp(w6, w7, x8, 0, sz_32);
  __a.ldp_pre(w0, w1, x2, 16, sz_32);
  __a.stp_pre(w6, w7, x8, -16, sz_32);
  __a.ldp_post(w0, w1, x2, 16, sz_32);
  __a.stp_post(w6, w7, x8, -16, sz_32);
  __a.ldp(x0, x1, x2, -8);
  __a.stp(x6, x7, x8, 8);
  __a.ldp(x0, x1, x2, 248);
  __a.stp(x6, x7, x8, -256);
  CHECK_WORDS(e, 16);
  TEST_END
}

static void test_ref_all_sequence() {
  static const uint32_t e[] = {
    0xaa0103e0, 0xaa040062, 0xca0700c5, 0x8a0a0128, 0xea0d018b, 0xea02003f, 0x8a250083, 0x8a250c83, 0xaa2800e6,
    0xaa6814e6, 0xca2b0149, 0x8a851883, 0xaac51c83, 0x92401c20, 0x9200f062, 0x920104a4, 0x92407ce6, 0xb2400128,
    0xd24c1d6a, 0xf240edac, 0xf2401c1f, 0x9103fc20, 0x913fc062, 0x914004a4, 0x9143fce6, 0xd1048d28, 0x9140056a,
    0xd14005ac, 0x9143fdee, 0xf100401f, 0xf140043f, 0xb100405f, 0xcb0403e3, 0xcb0613e5, 0x8b020020, 0x8b052083,
    0xcb4824e6, 0x8b8b2949, 0x8b0efdac, 0xeb020020, 0xeb01001f, 0xeb01141f, 0xab01001f, 0xcb0103e0, 0xd376d420,
    0xd34bfc62, 0x934cfca4, 0xaa0103e0, 0xd340fc62, 0xd3442c20, 0x93453462, 0x9ac22020, 0x9ac52483, 0x9ac828e6,
    0x9acb0949, 0x9ace0dac, 0x9b020c20, 0x9b067ca4, 0x1b020c20, 0x1b067ca4, 0xf9400020, 0xf9400462, 0xf947f8a4,
    0xf94800e6, 0xf9000128, 0xf900096a, 0xb9400420, 0xb9000462, 0x394004a4, 0x390004e6, 0x79400528, 0x7900056a,
    0xf85fc020, 0xf81f8062, 0xf84ff0a4, 0xf81000e6, 0xf85fcd28, 0xf81f8d6a, 0xf84105ac, 0xf81e05ee, 0xb85fc020,
    0xb81f8062, 0xf8626820, 0xf8657883, 0xf82868e6, 0xf82b7949, 0xb8627820, 0xb8256883, 0x386868e6, 0x786b6949,
    0xa9400440, 0xa94210a3, 0xa9001d06, 0xa93c2969, 0xa8c135cc, 0xa9bf0440, 0x29400440, 0x290110a3, 0x1400001e,
    0x9400001c, 0x54000340, 0x54000321, 0x54000308, 0x540002e9, 0x540002c3, 0x540002a2, 0x5400028a, 0x5400026b,
    0x5400024c, 0x5400022d, 0x54000204, 0x540001e5, 0x540001c6, 0x540001a7, 0xb4000160, 0xb5000141, 0x36280102,
    0xb7f800e3, 0x100000a4, 0xd503201f, 0xd65f03c0, 0xd61f00a0, 0xd63f00c0, 0xd61f03e0, 0xd503201f, 0xd503201f,
    0xd503201f, 0xd503201f, 0xd503201f, 0x58000060, 0x5c000080, 0x14000005, 0x55667788, 0x11223344, 0x00000000,
    0x00000000, 0xd503201f, 0x1e622820, 0x1e653883, 0x1e6808e6, 0x1e6b1949, 0x1e6d2180, 0x1e6021c8, 0x1e604020,
    0x1e60c062, 0x1e6140a4, 0x1e61c0e6, 0x9e780020, 0x1e780062, 0x1e222820, 0x1e253883, 0x1e2808e6, 0x1e2b1949,
    0x1e2d2180, 0x1e204020, 0x1e20c062, 0x1e2140a4, 0x1e21c0e6, 0x9e380020, 0xfd400020, 0xfd000862, 0xfc5f80a4,
    0xfc4104e6, 0xbd400020, 0xbd001062, 0xd2824680, 0xd2a24682, 0xd2c24684, 0xd2e24686, 0xf28acf08, 0xf2aacf0a,
    0x929579ac, 0x52824680, 0x72aacf01,
  };
  TEST_BEGIN("ref_all.s sequence")
  // moves / logical shifted
  __a.mov(x0, x1);
  __a.orr(x2, x3, x4);
  __a.eor(x5, x6, x7);
  __a.and_(x8, x9, x10);
  __a.ands(x11, x12, x13);
  __a.tst(x1, x2);
  __a.bic(x3, x4, x5);
  __a.bic(x3, x4, x5, LSL, 3);
  __a.orn(x6, x7, x8);
  __a.orn(x6, x7, x8, LSR, 5);
  __a.eon(x9, x10, x11);
  __a.and_(x3, x4, x5, ASR, 6);
  __a.orr(x3, x4, x5, ROR, 7);
  // logical immediate
  __a.and_(x0, x1, 0xffull);
  __a.and_(x2, x3, 0x5555555555555555ull);
  __a.and_(x4, x5, 0x8000000180000001ull);
  __a.and_(x6, x7, 0xffffffffull);
  __a.orr(x8, x9, 0x1ull);
  __a.eor(x10, x11, 0x0ff0000000000000ull);
  __a.ands(x12, x13, 0xfffffffffffffffull);
  __a.tst(x0, 0xffull);
  // add/sub immediate
  __a.add(x0, x1, 0xff);
  __a.add(x2, x3, 0xff0);
  __a.add(x4, x5, 0x1, 1);
  __a.add(x6, x7, 0xff, 1);
  __a.sub(x8, x9, 0x123);
  __a.add(x10, x11, 0x1, 1);
  __a.sub(x12, x13, 0x1, 1);
  __a.add(x14, x15, 0xff, 1);
  __a.cmp(x0, 0x10);
  __a.cmp(x1, 0x1, 1);
  __a.cmn(x2, 0x10);
  __a.neg(x3, x4);
  __a.neg(x5, x6, LSL, 4);
  // add/sub shifted register
  __a.add(x0, x1, x2);
  __a.add(x3, x4, x5, LSL, 8);
  __a.sub(x6, x7, x8, LSR, 9);
  __a.add(x9, x10, x11, ASR, 10);
  __a.add(x12, x13, x14, LSL, 63);
  __a.subs(x0, x1, x2);
  __a.cmp(x0, x1);
  __a.cmp(x0, x1, LSL, 5);
  __a.cmn(x0, x1);
  __a.neg(x0, x1);
  // bitfield
  __a.lsl(x0, x1, 10);
  __a.lsr(x2, x3, 11);
  __a.asr(x4, x5, 12);
  __a.mov(x0, x1);
  __a.lsr(x2, x3, 0);
  __a.ubfx(x0, x1, 4, 8);
  __a.sbfx(x2, x3, 5, 9);
  // dp-2src
  __a.lslv(x0, x1, x2);
  __a.lsrv(x3, x4, x5);
  __a.asrv(x6, x7, x8);
  __a.udiv(x9, x10, x11);
  __a.sdiv(x12, x13, x14);
  // madd/mul
  __a.madd(x0, x1, x2, x3);
  __a.mul(x4, x5, x6);
  __a.madd(w0, w1, w2, w3, sz_32);
  __a.mul(w4, w5, w6, sz_32);
  // mem: unsigned imm
  __a.ldr(x0, Address(x1));
  __a.ldr(x2, Address(x3, 8));
  __a.ldr(x4, Address(x5, 0xff0));
  __a.ldr(x6, Address(x7, 0x1000));
  __a.str(x8, Address(x9));
  __a.str(x10, Address(x11, 16));
  __a.ldr_w(w0, Address(x1, 4));
  __a.str_w(w2, Address(x3, 4));
  __a.ldr_b(w4, Address(x5, 1));
  __a.str_b(w6, Address(x7, 1));
  __a.ldr_h(w8, Address(x9, 2));
  __a.str_h(w10, Address(x11, 2));
  // mem: unscaled / pre-post
  __a.ldur(x0, Address(x1, -4));
  __a.stur(x2, Address(x3, -8));
  __a.ldur(x4, Address(x5, 255));
  __a.stur(x6, Address(x7, -256));
  __a.ldr_pre(x8, x9, -4);
  __a.str_pre(x10, x11, -8);
  __a.ldr_post(x12, x13, 16);
  __a.str_post(x14, x15, -32);
  __a.ldur(w0, Address(x1, -4), sz_32);
  __a.stur(w2, Address(x3, -8), sz_32);
  // mem: register offset
  __a.ldr(x0, Address(x1, x2, Address::no_scale));
  __a.ldr(x3, Address(x4, x5, Address::times_8));
  __a.str(x6, Address(x7, x8, Address::no_scale));
  __a.str(x9, Address(x10, x11, Address::times_8));
  __a.ldr_w(w0, Address(x1, x2, Address::times_4));
  __a.str_w(w3, Address(x4, x5, Address::no_scale));
  __a.ldr_b(w6, Address(x7, x8, Address::no_scale));
  __a.ldr_h(w9, Address(x10, x11, Address::no_scale));
  // ldp/stp
  __a.ldp(x0, x1, x2, 0);
  __a.ldp(x3, x4, x5, 32);
  __a.stp(x6, x7, x8, 0);
  __a.stp(x9, x10, x11, -64);
  __a.ldp_post(x12, x13, x14, 16);
  __a.stp_pre(x0, x1, x2, -16);
  __a.ldp(w0, w1, x2, 0, sz_32);
  __a.stp(w3, w4, x5, 8, sz_32);
  // branches
  Label L1, L2, L3, L4, L5, L6;
  __a.b(L1);
  __a.bl(L2);
  __a.b(EQ, L3);
  __a.b(NE, L3);
  __a.b(HI, L3);
  __a.b(LS, L3);
  __a.b(LO, L3);
  __a.b(HS, L3);
  __a.b(GE, L3);
  __a.b(LT, L3);
  __a.b(GT, L3);
  __a.b(LE, L3);
  __a.b(MI, L3);
  __a.b(PL, L3);
  __a.b(VS, L3);
  __a.b(VC, L3);
  __a.cbz(x0, L4);
  __a.cbnz(x1, L4);
  __a.tbz(x2, 5, L5);
  __a.tbnz(x3, 63, L5);
  __a.adr(x4, L6);
  __a.nop();
  __a.ret();
  __a.br(x5);
  __a.blr(x6);
  __a.bind(L6);
  __a.br(xzr);
  __a.bind(L5);
  __a.nop();
  __a.bind(L4);
  __a.nop();
  __a.bind(L3);
  __a.nop();
  __a.bind(L2);
  __a.nop();
  __a.bind(L1);
  __a.nop();
  // literal
  Label L7, L8, L9;
  __a.ldr(x0, L7);
  __a.ldr(d0, L8);
  __a.b(L9);
  __a.bind(L7);
  __a.emit_quad_data(0x1122334455667788ll, relocInfo::none);
  __a.bind(L8);
  __a.emit_quad_data(0, relocInfo::none);
  __a.bind(L9);
  __a.nop();
  // fp scalar
  __a.fadd(d0, d1, d2);
  __a.fsub(d3, d4, d5);
  __a.fmul(d6, d7, d8);
  __a.fdiv(d9, d10, d11);
  __a.fcmp(d12, d13);
  __a.fcmp0(d14);
  __a.fmov(d0, d1);
  __a.fabs(d2, d3);
  __a.fneg(d4, d5);
  __a.fsqrt(d6, d7);
  __a.fcvtzs(x0, d1, sz_64, sz_64);
  __a.fcvtzs(w2, d3, sz_64, sz_32);
  __a.fadd(d0, d1, d2, sz_32);
  __a.fsub(d3, d4, d5, sz_32);
  __a.fmul(d6, d7, d8, sz_32);
  __a.fdiv(d9, d10, d11, sz_32);
  __a.fcmp(d12, d13, sz_32);
  __a.fmov(d0, d1, sz_32);
  __a.fabs(d2, d3, sz_32);
  __a.fneg(d4, d5, sz_32);
  __a.fsqrt(d6, d7, sz_32);
  __a.fcvtzs(x0, d1, sz_32, sz_64);
  // fp mem
  __a.ldr(d0, Address(x1));
  __a.str(d2, Address(x3, 16));
  __a.ldur(d4, Address(x5, -8));
  __a.ldr_post(d6, x7, 16);
  __a.ldr_s(d0, Address(x1));
  __a.str_s(d2, Address(x3, 16));
  // mov wide
  __a.movz(x0, 0x1234, 0);
  __a.movz(x2, 0x1234, 1);
  __a.movz(x4, 0x1234, 2);
  __a.movz(x6, 0x1234, 3);
  __a.movk(x8, 0x5678, 0);
  __a.movk(x10, 0x5678, 1);
  __a.movn(x12, 0xabcd, 0);
  __a.movz(w0, 0x1234, 0, sz_32);
  __a.movk(w1, 0x5678, 1, sz_32);
  CHECK_WORDS(e, 174);
  TEST_END
}

// ---------------------------------------------------------------------------
// label fixup tests
// ---------------------------------------------------------------------------

static void test_new_instructions() {
  {
    static const uint32_t e[] = {0x9B417E11}; // smulh x17, x16, x1
    TEST_BEGIN("smulh")
    __a.smulh(x17, x16, x1);
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x9B517E0C, 0x9B01C22C}; // smulh x12, x16, x17; msub x12, x17, x1, x16
    TEST_BEGIN("msub")
    __a.smulh(x12, x16, x17);
    __a.msub(x12, x17, x1, x16);
    CHECK_WORDS(e, 2);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x9A9F17F0, 0x9A9F07F1}; // cset x16, eq; cset x17, ne
    TEST_BEGIN("cset")
    __a.cset(x16, EQ);
    __a.cset(x17, NE);
    CHECK_WORDS(e, 2);
    TEST_END
  }
  {
    static const uint32_t e[] = {0x937FFDAC}; // asr x12, x13, #63 (cdq)
    TEST_BEGIN("cdq")
    __a.cdq();
    CHECK_WORDS(e, 1);
    TEST_END
  }
  {
    // eax(x13) := eax * x1; EQ set iff no overflow
    static const uint32_t e[] = {
      0xAA0D03F0, // mov x16, x13
      0x9B417E11, // smulh x17, x16, x1
      0x9B017E10, // mul x16, x16, x1
      0xAA1003ED, // mov x13, x16
      0x937FFDB0, // asr x16, x13, #63
      0xEB10023F, // cmp x17, x16
    };
    TEST_BEGIN("imull(Register) overflow check")
    __a.imull(x1);
    CHECK_WORDS(e, 6);
    TEST_END
  }
  {
    // eax(x13) := edx:eax / x1 (signed), edx(x12) := remainder; EQ iff no overflow
    static const uint32_t e[] = {
      0xAA0D03F0, // mov x16, x13
      0x9AC10E11, // sdiv x17, x16, x1
      0x9B01C22C, // msub x12, x17, x1, x16
      0xAA1103ED, // mov x13, x17
      0xB24103F0, // orr x16, xzr, #0x8000000000000000
      0xEB10023F, // cmp x17, x16
      0x9A9F17F0, // cset x16, eq
      0xB100043F, // cmn x1, #1
      0x9A9F17F1, // cset x17, eq
      0x8A110210, // and x16, x16, x17
      0xF100021F, // cmp x16, #0
    };
    TEST_BEGIN("idivl(Register) overflow check")
    __a.idivl(x1);
    CHECK_WORDS(e, 11);
    TEST_END
  }
  {
    // memory-operand doubles: st(0) op double [x0, #8] (depth 1 -> d8)
    static const uint32_t e[] = {
      0xFD400410, // ldr d16, [x0, #8]
      0x1E702908, // fadd d8, d8, d16
    };
    TEST_BEGIN("fadd_d")
    __a.push_float();
    __a.fadd_d(Address(x0, 8));
    CHECK_WORDS(e, 2);
    TEST_END
  }
  {
    static const uint32_t e[] = {
      0xFD400410, // ldr d16, [x0, #8]
      0x1E703908, // fsub d8, d8, d16
      0xFD400410, // ldr d16, [x0, #8]
      0x1E700908, // fmul d8, d8, d16
      0xFD400410, // ldr d16, [x0, #8]
      0x1E701908, // fdiv d8, d8, d16
    };
    TEST_BEGIN("fsub_d/fmul_d/fdiv_d")
    __a.push_float();
    __a.fsub_d(Address(x0, 8));
    __a.fmul_d(Address(x0, 8));
    __a.fdiv_d(Address(x0, 8));
    CHECK_WORDS(e, 6);
    TEST_END
  }
  {
    // base-class pushl/popl
    static const uint32_t e[] = {
      0xF81F8FE0, // str x0, [sp, #-8]!
      0xF84087F0, // ldr x16, [sp], #8
    };
    TEST_BEGIN("base pushl/popl")
    CodeBuffer cb((char*)buf, sizeof(buf));
    AArch64Assembler a(&cb);
    std::memset(buf, 0, sizeof(buf));
    a.pushl(x0);
    a.popl(x16);
    check_words("base pushl/popl", buf, cb.code_size(), e, 2);
    TEST_END
  }
  {
    // popl(Address): pop into x16, store
    static const uint32_t e[] = {
      0xF84107F0, // ldr x16, [sp], #16
      0xF9000030, // str x16, [x1]
    };
    TEST_BEGIN("popl(Address)")
    __a.popl(Address(x1, 0));
    CHECK_WORDS(e, 2);
    TEST_END
  }
  {
    // decl(Address)
    static const uint32_t e[] = {
      0xF9400030, // ldr x16, [x1]
      0xD1000610, // sub x16, x16, #1
      0xF9000030, // str x16, [x1]
    };
    TEST_BEGIN("decl(Address)")
    __a.decl(Address(x1, 0));
    CHECK_WORDS(e, 3);
    TEST_END
  }
  {
    // cmpl(Register, oop): mov x16, oop; cmp
    static const uint32_t e[] = {
      0xB27403F0, // orr x16, xzr, #0x1000
      0xEB10001F, // cmp x0, x16
    };
    TEST_BEGIN("cmpl(Register, oop)")
    __a.cmpl(x0, (oop)0x1000);
    CHECK_WORDS(e, 2);
    TEST_END
  }
  {
    // orl(Register, Address)
    static const uint32_t e[] = {
      0xF9400030, // ldr x16, [x1]
      0xAA100000, // orr x0, x0, x16
    };
    TEST_BEGIN("orl(Register, Address)")
    __a.orl(x0, Address(x1, 0));
    CHECK_WORDS(e, 2);
    TEST_END
  }
  {
    // inline_oop: literal load + oop quad
    static const uint32_t e[] = {
      0x58000050, // ldr x16, [pc, #8]
      0x14000003, // b .+12 (skip the literal)
      0x00001000, // .quad 0x1000
      0x00000000,
    };
    TEST_BEGIN("inline_oop")
    __a.inline_oop((oop)0x1000);
    CHECK_WORDS(e, 4);
    TEST_END
  }
}

static void test_label_chains() {
  {
    // multiple backward/forward branches to the same label (chain fixup)
    static const uint32_t e[] = {
      0x14000002, // b   L
      0x14000001, // b   L
      0xD503201F, // L:  nop
    };
    TEST_BEGIN("chained forward branches to same label")
    Label L;
    __a.b(L);
    __a.b(L);
    __a.bind(L);
    __a.nop();
    CHECK_WORDS(e, 3);
    TEST_END
  }
  {
    // backward branch
    static const uint32_t e[] = {
      0xD503201F, // L:  nop
      0xD503201F, //     nop
      0x17FFFFFE, //     b L
    };
    TEST_BEGIN("backward branch")
    Label L;
    __a.bind(L);
    __a.nop();
    __a.nop();
    __a.b(L);
    CHECK_WORDS(e, 3);
    TEST_END
  }
  {
    // conditional + unconditional branches to the same label
    static const uint32_t e[] = {
      0x54000040, // b.eq L
      0x14000001, // b   L
      0xD503201F, // L:  nop
    };
    TEST_BEGIN("cond + uncond branches to same label")
    Label L;
    __a.b(EQ, L);
    __a.b(L);
    __a.bind(L);
    __a.nop();
    CHECK_WORDS(e, 3);
    TEST_END
  }
  {
    // cbz / cbnz to the same label
    static const uint32_t e[] = {
      0xB4000040, // cbz x0, L
      0xB5000021, // cbnz x1, L
      0xD503201F, // L:  nop
    };
    TEST_BEGIN("cbz + cbnz to same label")
    Label L;
    __a.cbz(x0, L);
    __a.cbnz(x1, L);
    __a.bind(L);
    __a.nop();
    CHECK_WORDS(e, 3);
    TEST_END
  }
  {
    // tbz / tbnz to the same label
    static const uint32_t e[] = {
      0x36300042, // tbz x2, #6, L
      0xB7300022, // tbnz x2, #6, L
      0xD503201F, // L:  nop
    };
    TEST_BEGIN("tbz + tbnz to same label")
    Label L;
    __a.tbz(x2, 6, L);
    __a.tbnz(x2, 6, L);
    __a.bind(L);
    __a.nop();
    CHECK_WORDS(e, 3);
    TEST_END
  }
  {
    // adr to a forward label
    static const uint32_t e[] = {
      0x10000044, // adr x4, L
      0x10000024, // adr x4, L
      0xD503201F, // L:  nop
    };
    TEST_BEGIN("adr chain to same label")
    Label L;
    __a.adr(x4, L);
    __a.adr(x4, L);
    __a.bind(L);
    __a.nop();
    CHECK_WORDS(e, 3);
    TEST_END
  }
  {
    // literal loads to the same data
    static const uint32_t e[] = {
      0x58000040, // ldr x0, L
      0x58000021, // ldr x1, L
      0x00000000, // L:  .quad 0
      0x00000000,
    };
    TEST_BEGIN("literal chain to same label")
    Label L;
    __a.ldr(x0, L);
    __a.ldr(x1, L);
    __a.bind(L);
    __a.emit_quad_data(0, relocInfo::none);
    CHECK_WORDS(e, 4);
    TEST_END
  }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
  std::printf("AArch64 encoder tests\n");

  test_logical_shifted();
  test_logical_imm();
  test_addsub();
  test_bitfield();
  test_dp2src_madd();
  test_mem();
  test_ldp_stp();
  test_mov_wide();
  test_macro_mov();
  test_fp();
  test_push_pop();
  test_compat_movl();
  test_compat_push_pop();
  test_compat_arith();
  test_compat_lea_cmp();
  test_compat_jcc();
  test_compat_dispatch_frame();
  test_compat_misc();

  test_fp_new_encoders();
  test_compat_float();
  test_fpu_mask_and_cond();

  test_ref_bitfield_sequence();
  test_ref_pairs_sequence();
  test_ref_all_sequence();

  test_new_instructions();
  test_label_chains();

  std::printf("\n%d tests, %d failures\n", total, failures);
  return failures == 0 ? 0 : 1;
}

#endif // DELTA_ASSEMBLER_BACKEND_AARCH64
