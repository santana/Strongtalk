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
  int count = 0;

  void do_object(memOop obj) override {
    count++;
    klassOop k = obj->klass_field();
    bool k_in_old = k != NULL && Universe::old_gen.contains((void*)k);
    int s = obj->size();
    bool bad = !k_in_old || s < 2;
    if (bad) {
      flagged++;
    }
    lprintf("[image32] obj #%d addr=%p klass=%p k_in_old=%d size=%d bad=%d %s%s\n", count, (void*)obj->addr(), (void*)k,
            k_in_old ? 1 : 0, s, bad ? 1 : 0, obj->is_method() ? "METHOD " : "", obj->is_symbol() ? "SYMBOL " : "");
    if (k != NULL) {
      lprintf("[image32]   klass name='%s' non_indexable_size=%d\n",
              k->klass_part()->delta_name() != NULL ? k->klass_part()->delta_name() : "(null)",
              k->klass_part()->non_indexable_size());
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
