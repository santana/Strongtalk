#include "easyunit/test.h"
#include "memory/markSweep.hpp"
#include "oops/klassOop.hpp"
#include "memory/universe.store.hpp"
#include "oops/oop.inline.hpp"
#include "oops/memOop.inline.hpp"

using namespace easyunit;

extern "C" oop* eden_top;
extern "C" oop* eden_end;

DECLARE(ProxyKlassTests)
  klassOop theClass;
  oop* oldEdenTop;
END_DECLARE

SETUP(ProxyKlassTests) {
  theClass = klassOop(Universe::find_global("ExternalProxy"));
  oldEdenTop = eden_top;
}

TEARDOWN(ProxyKlassTests){
  eden_top = oldEdenTop;
  MarkSweep::collect();
}

TESTF(ProxyKlassTests, allocateShouldFailWhenAllowedAndNoSpace) {
  eden_top = eden_end;
  ASSERT_EQUALS((intptr_t)NULL, (intptr_t)(theClass->klass_part()->allocateObject(false)));
}

TESTF(ProxyKlassTests, allocateShouldAllocateTenuredWhenRequired) {
  ASSERT_TRUE(Universe::old_gen.contains(theClass->klass_part()->allocateObject(false, true)));
}

TESTF(ProxyKlassTests, allocateShouldNotFailWhenNotAllowedAndNoSpace) {
  eden_top = eden_end;
  ASSERT_TRUE(Universe::new_gen.eden()->free() < 4 * oopSize);
  ASSERT_TRUE(Universe::new_gen.contains(theClass->klass_part()->allocateObject(true)));
}
