//#include "handle.hpp"
#include "easyunit/test.h"
#include "memory/handle.hpp"
#include "memory/markSweep.hpp"
#include "oops/associationKlass.hpp"
#include "memory/universe.store.hpp"
#include "oops/oop.inline.hpp"
#include "oops/memOop.inline.hpp"

using namespace easyunit;

DECLARE(AssociationKlassTests)
END_DECLARE

SETUP(AssociationKlassTests) {}

TEARDOWN(AssociationKlassTests) {
  MarkSweep::collect();
}

TESTF(AssociationKlassTests, shouldAllocateTenured) {
  HandleMark mark;
  Handle objectClass(Universe::find_global("GlobalAssociation"));
  oop assoc = ((associationKlass*)objectClass.as_klass()->klass_part())->allocateObject(true);
  ASSERT_TRUE(Universe::old_gen.contains(assoc));
}

TESTF(AssociationKlassTests, allocateShouldFailWhenAllowedAndNoSpace) {
  HandleMark mark;
  Handle objectClass(Universe::find_global("GlobalAssociation"));
  {
    OldSpaceMark oldMark(Universe::old_gen.top_mark()._space);
    int freeSpace = Universe::old_gen.free();
    Universe::allocate_tenured(freeSpace / oopSize - 1);
    ASSERT_TRUE(Universe::old_gen.free() < 5 * oopSize);
    ASSERT_EQUALS((intptr_t)NULL,
                  (intptr_t)((associationKlass*)objectClass.as_klass()->klass_part())->allocateObject(false));
  }
}

TESTF(AssociationKlassTests, allocateShouldNotFailWhenNotAllowedAndNoSpace) {
  HandleMark mark;
  Handle objectClass(Universe::find_global("GlobalAssociation"));
  {
    OldSpaceMark oldMark(Universe::old_gen.top_mark()._space);
    int freeSpace = Universe::old_gen.free();
    Universe::allocate_tenured(freeSpace / oopSize - 1);
    ASSERT_TRUE(Universe::old_gen.free() < 5 * oopSize);
    ASSERT_TRUE(
      Universe::old_gen.contains(((associationKlass*)objectClass.as_klass()->klass_part())->allocateObject(true)));
  }
}
