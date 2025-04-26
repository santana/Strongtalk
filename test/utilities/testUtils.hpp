#ifndef _TEST_UTILS_HPP
#define _TEST_UTILS_HPP

#define as_large_integer(value) \
  byteArrayPrimitives::largeIntegerFromSmallInteger(as_smiOop(value), reinterpret_cast<oop>(Universe::find_global("LargeInteger")))
#endif // _TEST_UTILS_HPP
