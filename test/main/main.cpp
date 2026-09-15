#ifdef WIN32
//#include "processOop.hpp"
#include <windows.h>
#endif
//#include "handle.hpp"
#include "easyunit/testharness.h"
#include "memory/allocation.hpp"
#include "memory/handle.hpp"
#include "memory/iterator.hpp"
#include "memory/oopFactory.hpp"
#include "memory/universe.hpp"
#include "oops/associationOop.hpp"
#include "oops/klass.hpp"
#include "oops/methodOop.hpp"
#include "oops/mixinOop.hpp"
#include "oops/processOop.hpp"
#include "oops/symbolOop.hpp"
#include "runtime/arguments.hpp"
#include "runtime/delta.hpp"
#include "runtime/init.hpp"
#include "runtime/process.hpp"
#include "runtime/testProcess.hpp"
#include <unistd.h>
#include "utilities/ostream.hpp"
#include "memory/generation.inline.hpp"
#include "memory/universe.store.hpp"
#include "oops/oop.inline.hpp"
#include "oops/memOop.inline.hpp"

void ostream_init();

using namespace easyunit;
extern "C" void load_image();

static VMProcess* vmProcess;
static Thread* vmThread;

static int vmLoopLauncher(DeltaProcess* testProcess) {
  vmProcess->transfer_to(testProcess);
  vmProcess->loop();
  return 0;
}
void start_vm_process(TestDeltaProcess* testProcess) {
  int threadId;
  vmProcess = new VMProcess();
  DeltaProcess::initialize_async_dll_event();
  ::testProcess = testProcess;
  vmThread = os::create_thread((int (*)(void*))&vmLoopLauncher, testProcess, &threadId);
}
void stop_vm_process() {
  os::terminate_thread(vmThread);
}

int main(int argc, char* argv[]) {
  parse_arguments(argc, argv);
  init_globals();
  load_image();

  set_test_done_event(os::create_event(false));

  TestDeltaProcess testProcess;
  start_vm_process(&testProcess);
  wait_for_test_done();
  stop_vm_process();
}