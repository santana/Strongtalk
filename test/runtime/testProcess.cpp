#include "easyunit/testharness.h"
#include "memory/allocation.hpp"
#include "memory/handle.hpp"
#include "memory/oopFactory.hpp"
#include "memory/universe.hpp"
#include "memory/generation.inline.hpp"
#include "memory/universe.store.hpp"
#include "oops/associationOop.hpp"
#include "oops/processOop.hpp"
#include "oops/symbolOop.hpp"
#include "oops/oop.inline.hpp"
#include "oops/memOop.inline.hpp"
#include "runtime/delta.hpp"
#include "runtime/process.hpp"
#include "runtime/testProcess.hpp"
#include "utilities/ostream.hpp"

using namespace easyunit;

extern "C" void load_image();

static Event* done;

TestDeltaProcess* testProcess = NULL;

static int addTestProcessDepth = 0;

void addTestToProcesses() {
  if (addTestProcessDepth++ == 0)
    testProcess->addToProcesses();
}

void removeTestFromProcesses() {
  assert(addTestProcessDepth > 0, "unbalanced removeTestFromProcesses");
  if (--addTestProcessDepth == 0)
    testProcess->removeFromProcesses();
}

void TestDeltaProcess::removeFromProcesses() {
  Processes::remove(this);
}

oop newProcess() {
  return Delta::call(Universe::find_global("Process"), reinterpret_cast<oop>(oopFactory::new_symbol("new")));
}

void TestDeltaProcess::addToProcesses() {
  oop process = newProcess();
  assert(process->is_process(), "Should be process");
  set_processObj(processOop(process));
  processOop(process)->set_process(this);
  Processes::add(this);
}

TestDeltaProcess::TestDeltaProcess() : DeltaProcess(NULL, NULL, false) {
  int ignore;
  Processes::remove(this);
  // no launch_delta thread is created by the base class; we run our own
  _thread = os::create_thread((int (*)(void*))&launch_tests, this, &ignore);
  _stack_limit = (char*)os::stack_limit(_thread);

  oop process = newProcess();
  assert(process->is_process(), "Should be process");
  set_processObj(processOop(process));
  processOop(process)->set_process(this);
}
TestDeltaProcess::TestDeltaProcess(fn launchfn) : DeltaProcess(NULL, NULL, false) {
  int ignore;
  Processes::remove(this);
  // no launch_delta thread is created by the base class; we run our own
  _thread = os::create_thread((osfn)launchfn, this, &ignore);
  _stack_limit = (char*)os::stack_limit(_thread);

  oop process = newProcess();
  assert(process->is_process(), "Should be process");
  set_processObj(processOop(process));
  processOop(process)->set_process(this);
}

TestDeltaProcess::~TestDeltaProcess() {
  set_processObj(processOop(newProcess()));
}
void setProcessRefs(DeltaProcess* process, processOop processObj) {
  processObj->set_process(process);
  process->set_processObj(processObj);
}

void initializeSmalltalkEnvironment() {
  AddTestProcess ap;
  PersistentHandle _new(reinterpret_cast<oop>(oopFactory::new_symbol("new")));
  PersistentHandle initialize(reinterpret_cast<oop>(oopFactory::new_symbol("initialize")));
  PersistentHandle runBase(reinterpret_cast<oop>(oopFactory::new_symbol("runBaseClassInitializers")));
  PersistentHandle processorScheduler(Universe::find_global("ProcessorScheduler"));
  PersistentHandle smalltalk(Universe::find_global("Smalltalk"));
  PersistentHandle systemInitializer(Universe::find_global("SystemInitializer"));
  PersistentHandle processor(Delta::call(processorScheduler.as_oop(), _new.as_oop()));

  associationOop processorAssoc = Universe::find_global_association("Processor");
  processorAssoc->set_value(processor.as_oop());

  DeltaProcess* scheduler = new TestDeltaProcess(&TestDeltaProcess::launch_scheduler);
  DeltaProcess::set_scheduler(scheduler);

  Delta::call(processor.as_oop(), initialize.as_oop());
  Delta::call(systemInitializer.as_oop(), runBase.as_oop());
  Delta::call(smalltalk.as_oop(), initialize.as_oop());
}

int TestDeltaProcess::launch_tests(DeltaProcess* process) {
  process->suspend_at_creation();
  DeltaProcess::set_active(process);
  initializeSmalltalkEnvironment();
  TestRegistry::runAndPrint();
  os::signal_event(done);
  return 0;
}

// mock scheduler loop to allow test process->scheduler transfers and returns
int TestDeltaProcess::launch_scheduler(DeltaProcess* process) {
  process->suspend_at_creation();
  DeltaProcess::set_active(process);
  while (true) {
    // No async DLLs are ever pending in the test harness, so
    // wait_for_async_dll would never grant the CPU back to the test process;
    // hand control over on every round-trip instead.
    process->transfer_to(testProcess);
  }
  return 0;
}

void set_test_done_event(Event* e) {
  done = e;
}

void wait_for_test_done() {
  os::wait_for_event(done);
}