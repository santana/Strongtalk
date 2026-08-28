#include "easyunit/test.h"
#include "memory/oopFactory.hpp"
#include "memory/vmSymbols.hpp"
#include "prims/prim.hpp"
#include "oops/smiOop.hpp"
#include "topIncludes/std_includes.hpp"
#include "memory/universe.store.hpp"
#include "oops/oop.inline.hpp"
#include "oops/memOop.inline.hpp"

using namespace easyunit;

typedef oop (__stdcall * smifntype)(smiOop, smiOop);

extern "C" int expansion_count;
DECLARE(SmiPrimsTests)
  smifntype smiQuo;
  symbolOop quoSymbol;
END_DECLARE

SETUP(SmiPrimsTests) {
  quoSymbol = oopFactory::new_symbol("primitiveQuo:ifFail:");
  primitive_desc* prim = primitives::lookup(quoSymbol);
  smiQuo = smifntype(prim->fn());
}

TEARDOWN(SmiPrimsTests){
}

TESTF(SmiPrimsTests, quoShouldReturnDivideReceiverByArgument) {
  ASSERT_EQUALS(5, smiOop(smiQuo(as_smiOop(2), as_smiOop(10)))->value());
}

TESTF(SmiPrimsTests, quoShouldReturnReceiverHasWrongTypeWhenNotSMI) {
  oop result = smiQuo(as_smiOop(2), smiOop(quoSymbol));
  ASSERT_EQUALS((intptr_t)markSymbol(vmSymbols::receiver_has_wrong_type()), (intptr_t)result);
}
