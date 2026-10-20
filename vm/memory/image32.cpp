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
// Only checks oop fields (0 .. non_indexable_size), not indexable data
// (raw bytes for byte containers, bytecodes for methods).
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
    if (k == NULL)
      return;
    int non_idx = k->klass_part()->non_indexable_size();
    // Check all oop fields: header (0 .. hdr) and instVars (hdr .. non_idx).
    for (int i = 0; i < non_idx; i++) {
      oop f = obj->raw_at(i);
      if (f == NULL)
        continue;
      bool f_valid = f->is_nil() || f->is_smi() || (f->is_mem() && Universe::old_gen.contains((void*)f));
      if (!f_valid) {
        flagged++;
        const char* kname = k->klass_part()->delta_name() ? k->klass_part()->delta_name() : "(null)";
        lprintf("[image32] bad field obj#%d klass=%s size=%d non_idx=%d field[%d]=%p tag=%d\n", count, kname, s,
                non_idx, i, (void*)(intptr_t)f, (int)((intptr_t)f & 3));
        if (flagged > 100)
          return; // don't flood
      }
    }
    // For objArray, indexable fields are also oops.
    if (obj->is_objArray()) {
      for (int i = non_idx; i < s; i++) {
        oop f = obj->raw_at(i);
        if (f == NULL)
          continue;
        bool f_valid = f->is_nil() || f->is_smi() || (f->is_mem() && Universe::old_gen.contains((void*)f));
        if (!f_valid) {
          flagged++;
          const char* kname = k->klass_part()->delta_name() ? k->klass_part()->delta_name() : "(null)";
          lprintf("[image32] bad field obj#%d klass=%s size=%d non_idx=%d field[%d]=%p tag=%d\n", count, kname, s,
                  non_idx, i, (void*)(intptr_t)f, (int)((intptr_t)f & 3));
          if (flagged > 100)
            return;
        }
      }
    }
  }
};

} // namespace

// Walk the old generation and verify 64-bit object layout consistency.
//
// For each object in old space, check:
//   - The object's klass pointer points into old space (klasses are tenured)
//   - The object's size is >= header_size(2)
//   - Each oop field (0 .. non_indexable_size) is a valid tagged oop
//
// Returns the number of objects/fields flagged for review.
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
