/*
Copyright (c) 2026, Gerardo Santana Gomez Garrido.

See image32.hpp for license and design notes.
*/

#include "memory/image32.hpp"
#include "memory/generation.hpp"
#include "memory/universe.hpp"
#include "oops/klass.hpp"
#include "oops/memOop.inline.hpp"
#include "oops/oop.inline.hpp"
#include "oops/methodOop.hpp"
#include "interpreter/bytecodes.hpp"
#include "utilities/lprintf.hpp"

namespace {

// ObjectClosure used to verify 64-bit object layout in old space.
class Image32VerifyClosure : public ObjectClosure {
public:
  int flagged = 0;

  void do_object(memOop obj) override {
    // Sanity: klass pointer should point into old space (klasses are tenured)
    klassOop k = obj->klass_field();
    if (k != NULL && !Universe::old_gen.contains((void*)k)) {
      lprintf("[image32] klass %p not in old space for obj %p\n", (void*)k, (void*)obj->addr());
      flagged++;
    }
    // Sanity: size should be >= header_size(2)
    int s = obj->size();
    if (s < 2) {
      lprintf("[image32] obj %p size %d < header_size(2)\n", (void*)obj->addr(), s);
      flagged++;
    }
  }
};

} // namespace

// Walk the old generation and verify 64-bit object layout consistency.
//
// For each object in old space, check:
//   - The object's klass pointer points into old space (klasses are tenured)
//   - The object's size is >= header_size(2)
//
// Returns the number of objects flagged for review.
int image32_verify_old_gen() {
  Image32VerifyClosure closure;
  Universe::old_gen.object_iterate(&closure);
  return closure.flagged;
}

// Placeholder for bytecode operand patching beyond imageCompat.cpp.
// Currently returns 0; imageCompat.cpp handles the known Alien patches.
// Additional patches can be added here as 32-bit-specific operands are
// discovered during boot debugging.
int image32_patch_bytecode_operands() {
  return 0;
}
