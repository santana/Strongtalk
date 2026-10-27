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

// Global 32-bit address -> 64-bit oop mapping, populated by bootstrap during
// read-in.  Used by image32_translate_fields() to rewrite raw 32-bit pointers
// in object bodies to their 64-bit equivalents.
static int* g_addr32_keys = NULL;
static oop* g_addr32_table = NULL;
static int g_addr32_count = 0;

void image32_set_addr32_map(int* keys, oop* table, int count) {
  g_addr32_keys = keys;
  g_addr32_table = table;
  g_addr32_count = count;
}

// Look up a 32-bit heap address in the global map.  Returns NULL if not found.
static oop lookup_addr32(int addr32) {
  for (int i = 0; i < g_addr32_count; i++) {
    if (g_addr32_keys[i] == addr32)
      return g_addr32_table[i];
  }
  return NULL;
}

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
    for (int i = hdr; i < non_idx && i < s; i++) {
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
          lprintf("[image32] bad field obj#%d klass=%s size=%d non_idx=%d "
                  "field[%d]=%p tag=%d\n",
                  count, kname, s, non_idx, i, (void*)(intptr_t)f, (int)((intptr_t)f & 3));
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
  // Klass field may be a raw 32-bit pointer from the 32-bit image.
  // If upper 32 bits are non-zero, it's already a valid 64-bit pointer;
  // otherwise look up the raw 32-bit address in the addr32 map.
  if ((intptr_t)k >> 32 == 0) {
    k = klassOop(lookup_addr32((int)(intptr_t)k));
    if (k == NULL)
      return 0; // Can't resolve klass; skip this object
    obj->raw_at_put(memOopDesc::klass_byte_offset() / oopSize, k);
  }
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
    // Raw 32-bit values stored as 64-bit have zero upper 32 bits.
    // Skip anything that already looks like a 64-bit pointer.
    // Check this BEFORE calling any methods on f, since f may be a raw
    // 32-bit heap address that is not a valid object pointer.
    if (raw >> 32 != 0)
      continue;
    // f is either nil, smi, or raw 32-bit pointer - look up in addr32_map
    oop wide = lookup_addr32((int)raw);
    if (wide != NULL) {
      obj->raw_at_put(i, wide);
      translated++;
      lprintf("[image32] translated obj=%p field[%d]=0x%x -> %p\n", (void*)obj, i, (uint32_t)raw, (void*)wide);
    } else if ((raw & 3) == Int_Tag) {
      // Smi: 30-bit value stored as (v << 2) | 0 on 32-bit.  Widen to 62-bit.
      int32_t v32 = (int32_t)(raw >> 2);
      obj->raw_at_put(i, as_smiOop(v32));
    } else {
      lprintf("[image32] untranslatable field raw=0x%x obj=%p klass=%s\n", (uint32_t)raw, (void*)obj,
              k->klass_part()->delta_name() ? k->klass_part()->delta_name() : "(null)");
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
      // Raw 32-bit values stored as 64-bit have zero upper 32 bits.
      if ((intptr_t)f >> 32 != 0)
        continue;
      uint32_t w32 = (uint32_t)raw;
      // First check if this raw 32-bit address is in our map.
      oop wide = lookup_addr32((int)w32);
      if (wide != NULL) {
        obj->raw_at_put(i, wide);
        translated++;
        lprintf("[image32] translated array field[%d]=0x%x -> %p\n", i, w32, (void*)wide);
      } else if ((w32 & 3) == Int_Tag) {
        int32_t v32 = (int32_t)(w32 >> 2);
        obj->raw_at_put(i, as_smiOop(v32));
      } else {
        lprintf("[image32] unknown array field tag raw=0x%x\n", w32);
      }
      translated++;
    }
  }

  return translated;
}

// Walk the bootstrap oop table and translate 32-bit raw oop fields to 64-bit
// tagged oops.  Called during bootstrap after all objects have been read
// but before any interpreted sends execute.  Only objects read from the 32-bit
// image are translated -- this avoids corrupting Delta frames and other
// heap objects that were not read from the image.
//
// Returns the total number of fields translated.
int image32_translate_all_fields(oop* oop_table, int oop_count) {
  int total = 0;
  for (int i = 0; i < oop_count; i++) {
    oop obj = oop_table[i];
    if (obj != NULL && obj->is_mem())
      total += image32_translate_fields(as_memOop(obj));
  }
  lprintf("[image32] translated %d fields across %d objects\n", total, oop_count);
  return total;
}
