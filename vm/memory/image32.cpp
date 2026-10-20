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
#include "oops/memOop.hpp"
#include "oops/smiOop.hpp"
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
    int hdr = k->klass_part()->oop_header_size();
    // Check instVar oop fields (hdr .. non_idx).  Field 0 is the mark word,
    // field 1 is the klass pointer (already validated above).
    for (int i = hdr; i < non_idx; i++) {
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

// Translate 32-bit raw oop fields in an object body to 64-bit tagged oops.
//
// The 32-bit image stores instVar fields as raw 32-bit values: smis are
// stored as 30-bit values (already zero-extended), and object references
// are stored as raw 32-bit heap addresses. On 64-bit, all oop fields hold
// tagged 64-bit pointers: smis are (value << 2) | Int_Tag, heap objects
// are tagged pointers with Mem_Tag.
//
// This function walks object-body oop fields (from hdr to non_indexable_size)
// and widens any raw 32-bit value to its 64-bit tagged equivalent.  It does
// not touch indexable data (raw bytes for byte containers, bytecodes for
// methods) since those are handled elsewhere.
//
// Returns the number of fields translated.
int image32_translate_fields(memOop obj) {
  klassOop k = obj->klass_field();
  if (k == NULL)
    return 0;
  int hdr = k->klass_part()->oop_header_size();
  int non_idx = k->klass_part()->non_indexable_size();
  int s = obj->size();
  int translated = 0;

  // Header + instVar oop fields.
  for (int i = hdr; i < non_idx && i < s; i++) {
    oop f = obj->raw_at(i);
    if (f == NULL)
      continue;
    intptr_t raw = (intptr_t)f;
    // Already a valid tagged oop (nil, smi, or heap pointer into old space).
    if (f->is_nil() || f->is_smi() || (f->is_mem() && Universe::old_gen.contains((void*)f)))
      continue;
    // Raw 32-bit value.  Reconstruct the original 32-bit word and retag.
    uint32_t w32 = (uint32_t)raw;
    if ((w32 & 3) == Int_Tag) {
      // Smi: 30-bit value stored as (v << 2) | 0 on 32-bit.  Widen to 62-bit.
      int32_t v32 = (int32_t)(w32 >> 2);
      obj->raw_at_put(i, as_smiOop(v32));
    } else if ((w32 & 3) == Mem_Tag) {
      // Heap object: raw 32-bit address.  Treat as a raw heap pointer into
      // old space.
      oop wide = as_memOop((void*)(intptr_t)w32);
      if (Universe::old_gen.contains((void*)wide))
        obj->raw_at_put(i, wide);
      else
        lprintf("[image32] untranslatable field raw=0x%x\n", w32);
    } else {
      lprintf("[image32] unknown field tag raw=0x%x\n", w32);
    }
    translated++;
  }

  // ObjArray indexable fields are also oops.
  if (obj->is_objArray()) {
    for (int i = non_idx; i < s; i++) {
      oop f = obj->raw_at(i);
      if (f == NULL)
        continue;
      intptr_t raw = (intptr_t)f;
      if (f->is_nil() || f->is_smi() || (f->is_mem() && Universe::old_gen.contains((void*)f)))
        continue;
      uint32_t w32 = (uint32_t)raw;
      if ((w32 & 3) == Int_Tag) {
        int32_t v32 = (int32_t)(w32 >> 2);
        obj->raw_at_put(i, as_smiOop(v32));
      } else if ((w32 & 3) == Mem_Tag) {
        oop wide = as_memOop((void*)(intptr_t)w32);
        if (Universe::old_gen.contains((void*)wide))
          obj->raw_at_put(i, wide);
        else
          lprintf("[image32] untranslatable array field raw=0x%x\n", w32);
      } else {
        lprintf("[image32] unknown array field tag raw=0x%x\n", w32);
      }
      translated++;
    }
  }

  return translated;
}

// Walk the old generation and translate 32-bit raw oop fields to 64-bit
// tagged oops.  Called during bootstrap after all objects have been read
// but before any interpreted sends execute.
//
// Returns the total number of fields translated.
int image32_translate_all_fields() {
  class TranslateClosure : public ObjectClosure {
  public:
    int total = 0;
    void do_object(memOop obj) override { total += image32_translate_fields(obj); }
  } closure;
  Universe::old_gen.object_iterate(&closure);
  return closure.total;
}
