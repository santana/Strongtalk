#ifndef _EASYUNIT_ALLOC_
#define _EASYUNIT_ALLOC_
#include <string.h>
#if defined(__OPENBSD__)
#include <sys/types.h>
#include <sys/malloc.h>
#else
#include <malloc.h>
#endif
#include <stdlib.h>

namespace easyunit {

  class CHeap {
   public:
    void* operator new(size_t size);
    void  operator delete(void* p);
  };

}
#endif
