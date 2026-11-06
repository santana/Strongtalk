/*
Copyright (c) 2026, Gerardo Santana Gomez Garrido.

See imageWriter.hpp for license and design notes.
*/

#include "memory/imageWriter.hpp"
#include "memory/universe.hpp"
#include "memory/universe.store.hpp"
#include "oops/associationOop.hpp"
#include "oops/byteArrayOop.hpp"
#include "oops/dByteArrayOop.hpp"
#include "oops/dValueArrayOop.hpp"
#include "oops/doubleOop.hpp"
#include "oops/klass.hpp"
#include "oops/klassOop.hpp"
#include "oops/memOop.inline.hpp"
#include "oops/methodOop.hpp"
#include "oops/mixinOop.hpp"
#include "oops/objArrayOop.hpp"
#include "oops/oop.inline.hpp"
#include "oops/processOop.hpp"
#include "oops/symbolOop.hpp"
#include "interpreter/bytecodes.hpp"
#include "interpreter/codeIterator.hpp"
#include "utilities/lprintf.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

// Native 64-bit bootstrap-text image format
// ------------------------------------------
// The stream starts with the version integer 200 + Bytecodes::version().
// It is followed by the 26 bootstrap roots in the exact order the reader
// expects, each encoded as a normal object value.  Objects are emitted
// inline depth-first: the first time an object is referenced it is written
// out in full, subsequent references are '3' + index.  The index of an object
// is assigned when its header is emitted, which is also the moment the
// reader allocates (and registers) it, so references to already-emitted
// objects -- including self references and cycles -- always resolve.
//
// Integers use the little-endian base-128 encoding of bootstrap::get_integer.

static void write_integer(FILE* fp, int value) {
  assert(value >= 0, "write_integer expects a non-negative value");
  u_char b = (u_char)(value % 128);
  if (value < 128) {
    putc(b, fp);
  } else {
    putc(b | 0x80, fp);
    write_integer(fp, value / 128);
  }
}

static inline void put_char(FILE* fp, char c) {
  putc((u_char)c, fp);
}

static inline memOop as_mem(oop obj) {
  return (memOop)obj;
}

// Layout of the inline oop slots of an instruction whose opcode is at byte
// offset 'w' in the codes area.  Mirrors methodOop.cpp's oop_instr_offsets()
// so the writer and the reader agree on where each inline slot lives.
static void oop_slot_layout(Bytecodes::Format f, int w, int& base, int& next) {
  switch (f) {
    case Bytecodes::BO:
    case Bytecodes::BL:
      base = (w + 16) & -oopSize;
      base -= oopSize;
      next = base + oopSize;
      break;
    case Bytecodes::BBO:
    case Bytecodes::BBL:
      base = (w + 17) & -oopSize;
      base -= oopSize;
      next = base + oopSize;
      break;
    case Bytecodes::BOO:
    case Bytecodes::BLO:
    case Bytecodes::BOL:
    case Bytecodes::BLL:
      base = (w + 24) & -oopSize;
      base -= 2 * oopSize;
      next = base + 2 * oopSize;
      break;
    case Bytecodes::BBOO:
    case Bytecodes::BBLO:
      base = (w + 25) & -oopSize;
      base -= 2 * oopSize;
      next = base + 2 * oopSize;
      break;
    case Bytecodes::BLB:
      base = (w + 1) & -oopSize;
      next = base + oopSize + 1;
      break;
    case Bytecodes::BOOLB:
      base = (w + 32) & -oopSize;
      base -= 3 * oopSize;
      next = base + 3 * oopSize + 1;
      break;
    default:
      base = next = w;
      break;
  }
}

static int argument_bytes(Bytecodes::Format f) {
  switch (f) {
    case Bytecodes::BB:
    case Bytecodes::BBS:
    case Bytecodes::BBO:
    case Bytecodes::BBL:
    case Bytecodes::BBOO:
    case Bytecodes::BBLO:
      return 1;
    case Bytecodes::BBB:
      return 2;
    case Bytecodes::BBBB:
      return 3;
    default:
      return 0;
  }
}

static int inline_oop_slots(Bytecodes::Format f) {
  switch (f) {
    case Bytecodes::BO:
    case Bytecodes::BL:
    case Bytecodes::BBO:
    case Bytecodes::BBL:
    case Bytecodes::BLB:
      return 1;
    case Bytecodes::BOO:
    case Bytecodes::BLO:
    case Bytecodes::BOL:
    case Bytecodes::BLL:
    case Bytecodes::BBOO:
    case Bytecodes::BBLO:
      return 2;
    case Bytecodes::BOOLB:
      return 3;
    default:
      return 0;
  }
}

static bool format_has_trailing_byte(Bytecodes::Format f) {
  return f == Bytecodes::BLB || f == Bytecodes::BOOLB;
}

// Inline slots that hold a raw offset (branch distances, primitive failure
// block sizes) instead of an oop.
static bool slot_holds_offset(Bytecodes::Code code, int slot) {
  switch (code) {
    case Bytecodes::jump_loop_word:
      return slot < 2;
    case Bytecodes::ifTrue_word:
    case Bytecodes::ifFalse_word:
    case Bytecodes::and_word:
    case Bytecodes::or_word:
    case Bytecodes::whileTrue_word:
    case Bytecodes::whileFalse_word:
    case Bytecodes::jump_else_word:
      return slot == 0;
    case Bytecodes::prim_call_failure_lookup:
    case Bytecodes::predict_prim_call_failure_lookup:
    case Bytecodes::prim_call_self_failure_lookup:
    case Bytecodes::prim_call_failure:
    case Bytecodes::predict_prim_call_failure:
    case Bytecodes::prim_call_self_failure:
      return slot == 1;
    default:
      return false;
  }
}

static bool is_dll_entry_slot(Bytecodes::Code code, int slot) {
  return slot == 2 && (code == Bytecodes::dll_call_sync || code == Bytecodes::dll_call_async);
}

class ImageWriter {
  FILE* _fp;
  std::map<oop, int> _index; // tagged oop -> stream index
  int _next_index;
  bool _failed;

  void fail(const char* what, oop obj) {
    lprintf("image writer: %s (oop %#lx)\n", what, (intptr_t)obj);
    _failed = true;
  }

  void write_smi_value(intptr_t v) {
    if (v < 0) {
      put_char(_fp, '-');
      write_integer(_fp, (int)(-v));
    } else {
      put_char(_fp, '0');
      write_integer(_fp, (int)v);
    }
  }

  void write_raw_int(int v) {
    put_char(_fp, 'I');
    write_smi_value(v);
  }

  char klass_type_char(Klass::Format f) {
    switch (f) {
      case Klass::klass_klass:
        return 'A';
      case Klass::smi_klass:
        return 'B';
      case Klass::mem_klass:
        return 'C';
      case Klass::byteArray_klass:
        return 'D';
      case Klass::doubleByteArray_klass:
        return 'E';
      case Klass::objArray_klass:
        return 'F';
      case Klass::symbol_klass:
        return 'G';
      case Klass::double_klass:
        return 'H';
      case Klass::association_klass:
        return 'I';
      case Klass::method_klass:
        return 'J';
      case Klass::blockClosure_klass:
        return 'K';
      case Klass::context_klass:
        return 'L';
      case Klass::proxy_klass:
        return 'M';
      case Klass::mixin_klass:
        return 'N';
      case Klass::weakArray_klass:
        return 'O';
      case Klass::process_klass:
        return 'P';
      case Klass::doubleValueArray_klass:
        return 'Q';
      case Klass::vframe_klass:
        return 'R';
      default:
        return 0;
    }
  }

  char instance_type_char(Klass::Format f) {
    switch (f) {
      case Klass::mem_klass:
        return 'c';
      case Klass::byteArray_klass:
        return 'd';
      case Klass::doubleByteArray_klass:
        return 'e';
      case Klass::objArray_klass:
        return 'f';
      case Klass::symbol_klass:
        return 'g';
      case Klass::double_klass:
        return 'h';
      case Klass::association_klass:
        return 'i';
      case Klass::method_klass:
        return 'j';
      case Klass::mixin_klass:
        return 'n';
      case Klass::process_klass:
        return 'p';
      case Klass::doubleValueArray_klass:
        return 'q';
      // blockClosure, context, proxy, weakArray and vframe instances carry
      // process-specific state (code pointers, stack contents, foreign
      // addresses) and are not supported by the reader.  They are not part of
      // the frozen graph; if one is ever reachable the writer fails instead of
      // producing an image that cannot be loaded.
      default:
        return 0;
    }
  }

  char type_char(oop obj) {
    if (obj->is_klass())
      return klass_type_char(klassOop(obj)->klass_part()->format());
    return instance_type_char(as_mem(obj)->klass_field()->klass_part()->format());
  }

  // Write an object field: smis and already-emitted objects are written as
  // values; a new object is defined inline.
  void write_value(oop obj) {
    if (obj == NULL) {
      put_char(_fp, '?');
      return;
    }
    if (obj->is_smi()) {
      write_smi_value(smiOop(obj)->value());
      return;
    }
    std::map<oop, int>::iterator i = _index.find(obj);
    if (i != _index.end()) {
      put_char(_fp, '3');
      write_integer(_fp, i->second);
      return;
    }
    write_object(obj);
  }

  void write_klass_object(oop obj) {
    memOop m = (memOop)obj;
    write_value(m->raw_at(3)); // _non_indexable_size
    write_value(m->raw_at(4)); // _has_untagged_contents
    write_value(m->raw_at(1)); // klass (the metaclass)
    write_value(m->raw_at(5)); // _classVars
    write_value(m->raw_at(6)); // _methods
    write_value(m->raw_at(7)); // _superKlass
    write_value(m->raw_at(8)); // _mixin
    int ni = as_mem(obj)->size();
    for (int i = klassOopDesc::header_size(); i < ni; i++)
      write_value(m->raw_at(i));
  }

  void write_mem_object(oop obj) {
    memOop m = (memOop)obj;
    Klass* k = as_mem(obj)->klass_field()->klass_part();
    write_value(m->raw_at(1));
    int ni = k->non_indexable_size();
    for (int i = k->oop_header_size(); i < ni; i++)
      write_value(m->raw_at(i));
  }

  void write_byte_array(oop obj) {
    memOop m = (memOop)obj;
    Klass* k = as_mem(obj)->klass_field()->klass_part();
    int ni = k->non_indexable_size();
    write_value(m->raw_at(1));
    for (int i = k->oop_header_size(); i < ni; i++)
      write_value(m->raw_at(i));
    byteArrayOop a = byteArrayOop(obj);
    int len = a->length();
    write_smi_value(len);
    for (int i = 1; i <= len; i++)
      put_char(_fp, a->byte_at(i));
  }

  void write_double_byte_array(oop obj) {
    memOop m = (memOop)obj;
    Klass* k = as_mem(obj)->klass_field()->klass_part();
    int ni = k->non_indexable_size();
    write_value(m->raw_at(1));
    for (int i = k->oop_header_size(); i < ni; i++)
      write_value(m->raw_at(i));
    doubleByteArrayOop a = doubleByteArrayOop(obj);
    int len = a->length();
    write_smi_value(len);
    for (int i = 1; i <= len; i++)
      write_integer(_fp, (int)a->doubleByte_at(i));
  }

  void write_double_value_array(oop obj) {
    memOop m = (memOop)obj;
    Klass* k = as_mem(obj)->klass_field()->klass_part();
    int ni = k->non_indexable_size();
    write_value(m->raw_at(1));
    for (int i = k->oop_header_size(); i < ni; i++)
      write_value(m->raw_at(i));
    doubleValueArrayOop a = doubleValueArrayOop(obj);
    int len = a->length();
    write_smi_value(len);
    for (int i = 1; i <= len; i++) {
      double d = a->double_at(i);
      fwrite(&d, sizeof(d), 1, _fp);
    }
  }

  void write_obj_array(oop obj) {
    memOop m = (memOop)obj;
    Klass* k = as_mem(obj)->klass_field()->klass_part();
    int ni = k->non_indexable_size();
    write_value(m->raw_at(1));
    for (int i = k->oop_header_size(); i < ni; i++)
      write_value(m->raw_at(i));
    objArrayOop a = objArrayOop(obj);
    int len = a->length();
    write_smi_value(len);
    for (int i = 1; i <= len; i++)
      write_value(a->obj_at(i));
  }

  void write_double_object(oop obj) {
    memOop m = (memOop)obj;
    write_value(m->raw_at(1));
    double d = doubleOop(obj)->value();
    fwrite(&d, sizeof(d), 1, _fp);
  }

  void write_association(oop obj) {
    memOop m = (memOop)obj;
    write_value(m->raw_at(1));
    write_value(m->raw_at(associationOopDesc::key_offset()));
    write_value(m->raw_at(associationOopDesc::value_offset()));
    write_value(m->raw_at(associationOopDesc::is_constant_offset()));
  }

  void write_mixin(oop obj) {
    memOop m = (memOop)obj;
    write_value(m->raw_at(1));
    for (int i = 2; i < mixinOopDesc::header_size(); i++)
      write_value(m->raw_at(i));
    Klass* k = as_mem(obj)->klass_field()->klass_part();
    int ni = k->non_indexable_size();
    for (int i = mixinOopDesc::header_size(); i < ni; i++)
      write_value(m->raw_at(i));
  }

  void write_process(oop obj) {
    memOop m = (memOop)obj;
    Klass* k = as_mem(obj)->klass_field()->klass_part();
    write_value(m->raw_at(1));
    // The process pointer is process-specific; the reader installs NULL.
    for (int i = processOopDesc::header_size(); i < k->non_indexable_size(); i++)
      write_value(m->raw_at(i));
  }

  void write_method(oop obj) {
    methodOop m = methodOop(obj);
    memOop d = (memOop)obj;
    write_value(d->raw_at(1)); // klass
    write_value(d->raw_at(2)); // _debugInfo
    write_value(d->raw_at(3)); // _selector_or_method
    // _counters is reset by the reader.
    write_value(d->raw_at(5)); // _size_and_flags
    write_codes(m);
  }

  void write_codes(methodOop m);

  void write_object(oop obj) {
    char tc = type_char(obj);
    if (tc == 0) {
      fail("unsupported object format", obj);
      return;
    }
    int index = _next_index++;
    _index[obj] = index;

    int size = as_mem(obj)->size();
    put_char(_fp, tc);
    write_integer(_fp, size);

    if (tc >= 'A' && tc <= 'R') {
      write_klass_object(obj);
      return;
    }
    switch (tc) {
      case 'c':
        write_mem_object(obj);
        break;
      case 'd':
      case 'g':
        write_byte_array(obj);
        break;
      case 'e':
        write_double_byte_array(obj);
        break;
      case 'f':
        write_obj_array(obj);
        break;
      case 'h':
        write_double_object(obj);
        break;
      case 'i':
        write_association(obj);
        break;
      case 'j':
        write_method(obj);
        break;
      case 'n':
        write_mixin(obj);
        break;
      case 'p':
        write_process(obj);
        break;
      case 'q':
        write_double_value_array(obj);
        break;
      default:
        fail("cannot serialize object", obj);
        break;
    }
  }

public:
  explicit ImageWriter(FILE* fp) : _fp(fp), _next_index(0), _failed(false) {}

  bool failed() const { return _failed; }

  void write_root(oop obj) { write_value(obj); }

  int objects_written() const { return _next_index; }
};

// The in-memory codes area is already in the native 64-bit layout.  Emit the
// instruction count followed by each instruction's raw bytes and structured
// inline slots, so the reader can rebuild the exact same area without any
// relocation.  Inline slots holding raw offsets (and the process-specific DLL
// entry cell) are written as 'I' + integer; other slots are normal object
// values (including '?' for an empty inline cache).
void ImageWriter::write_codes(methodOop m) {
  u_char* codes = m->codes();
  int n = 0;
  for (CodeIterator it(m, 1); it.hp() < m->codes_end(); it.advance())
    n++;
  write_integer(_fp, n);

  for (CodeIterator it(m, 1); it.hp() < m->codes_end(); it.advance()) {
    int w = (int)(it.hp() - codes);
    Bytecodes::Code code = it.code();
    Bytecodes::Format f = it.format();

    put_char(_fp, (char)(u_char)code);

    int args = argument_bytes(f);
    for (int i = 0; i < args; i++)
      put_char(_fp, (char)it.byte_at(1 + i));

    int oops = inline_oop_slots(f);
    int next;
    if (oops > 0) {
      int base;
      oop_slot_layout(f, w, base, next);
      for (int s = 0; s < oops; s++) {
        intptr_t* slot = (intptr_t*)(codes + base + s * oopSize);
        if (slot_holds_offset(code, s)) {
          write_raw_int((int)*slot);
        } else if (is_dll_entry_slot(code, s)) {
          write_raw_int(0); // resolve the entry point at runtime
        } else {
          write_value((oop)*slot);
        }
      }
      if (format_has_trailing_byte(f))
        put_char(_fp, (char)codes[next - 1]);
    } else if (f == Bytecodes::BBS) {
      int count = it.byte_at(1);
      if (count == 0)
        count = 256;
      for (int i = 0; i < count; i++)
        put_char(_fp, (char)it.byte_at(2 + i));
    }
  }
}

// The first object in the stream is the (meta)class of smiKlass.  The reader
// discards it, but it is referenced by smiKlass's klass field, so emitting it
// first keeps the stream identical in shape to the original image.
static klassOop first_root() {
  return klassOop(memOop((oop)::smiKlassObj)->klass_field());
}

int image_writer_save(const char* path) {
  FILE* fp = fopen(path, "wb");
  if (fp == NULL) {
    lprintf("image writer: cannot open %s for writing\n", path);
    return -1;
  }

  // Version 200 marks the native 64-bit format; the low part is the bytecode
  // version the reader verifies.
  write_integer(fp, 200 + Bytecodes::version());

  oop roots[] = {
    first_root(), // 0
    ::smiKlassObj, // 1
    Universe::memOopKlassObj(), // 2
    Universe::objArrayKlassObj(), // 3
    Universe::byteArrayKlassObj(), // 4
    ::symbolKlassObj, // 5
    ::doubleKlassObj, // 6
    Universe::methodKlassObj(), // 7
    Universe::associationKlassObj(), // 8
    ::zeroArgumentBlockKlassObj, // 9
    ::oneArgumentBlockKlassObj, // 10
    ::twoArgumentBlockKlassObj, // 11
    ::threeArgumentBlockKlassObj, // 12
    ::fourArgumentBlockKlassObj, // 13
    ::fiveArgumentBlockKlassObj, // 14
    ::sixArgumentBlockKlassObj, // 15
    ::sevenArgumentBlockKlassObj, // 16
    ::eightArgumentBlockKlassObj, // 17
    ::nineArgumentBlockKlassObj, // 18
    ::contextKlassObj, // 19
    nilObj, // 20
    trueObj, // 21
    falseObj, // 22
    Universe::vframeKlassObj(), // 23
    Universe::asciiCharacters(), // 24
    Universe::systemDictionaryObj(), // 25
  };

  ImageWriter writer(fp);
  for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++)
    writer.write_root(roots[i]);

  bool failed = writer.failed() || ferror(fp);
  if (fclose(fp) != 0)
    failed = true;

  if (failed) {
    lprintf("image writer: failed to write a complete image to %s\n", path);
    return -1;
  }
  return writer.objects_written();
}
