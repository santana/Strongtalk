#include "easyunit/test.h"
#include "memory/markSweep.hpp"
#include "memory/oopFactory.hpp"
#include "oops/klassOop.hpp"
#include "oops/oop.hpp"
#include "runtime/delta.hpp"
#include "memory/generation.inline.hpp"
#include "memory/universe.store.hpp"
#include "oops/oop.inline.hpp"
#include "oops/memOop.inline.hpp"

using namespace easyunit;

DECLARE(InterpretedICTest)
oop fixture;
END_DECLARE

SETUP(InterpretedICTest) {
  klassOop objectClass = klassOop(Universe::find_global("DoesNotUnderstandFixture"));
  fixture = objectClass->klass_part()->allocateObject();
}

TEARDOWN(InterpretedICTest){
  fixture = NULL;
  MarkSweep::collect();
}

TESTF(InterpretedICTest, noArgSendWithUnknownSelectorShouldInvokeDoesNotUnderstand)
{
  BlockScavenge bs;
  symbolOop selector = oopFactory::new_symbol("dnuTrigger1", 11);
  symbolOop returnedSelector = oopFactory::new_symbol("quack", 5);
  oop result = Delta::call(fixture, reinterpret_cast<oop>(selector));

  klassOop expectedKlass = klassOop(Universe::find_global("Message"));

  ASSERT_TRUE_M(result->is_mem(), "result should be object");
  ASSERT_EQUALS_M(expectedKlass, result->klass(), "wrong class returned");
  ASSERT_EQUALS_M(fixture, memOop(result)->raw_at(2), "message should contain receiver");
  ASSERT_EQUALS_M(reinterpret_cast<oop>(returnedSelector), memOop(result)->raw_at(3), "message should contain correct selector");
}
