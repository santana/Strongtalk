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

// Set when the mock scheduler observes the test process stop for good (see
// launch_scheduler); read back through test_run_failed().
static bool testRunFailed = false;

bool test_run_failed() {
  return testRunFailed;
}

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
  // Allocate the process object directly from the Process class, mirroring the
  // canonical path in vm/prims/process_prims.cpp (processOopPrimitives::create):
  //   processOop process = processOop(receiver->primitive_allocate());
  //
  // Sending #new does NOT work. The image exposes process creation through the
  // `create:` primitive, which takes a block argument, so a zero-argument #new
  // send yields NULL -- which then faulted in processOopDesc::set_process()
  // (NULL - Mem_Tag + offsetof(_process) == 0xf) and made stest unusable.
  //
  // This process runs its own thread (launch_tests), so it only needs the oop
  // wrapper; no DeltaProcess body has to be created here. Scavenging is
  // suppressed because the process is still half-initialised at this point
  // (set_processObj()/set_process() run on the next lines).
  oop processKlass = Universe::find_global("Process");
  return processKlass->primitive_allocate(false);
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

// True while a process that suspended in this state should be handed the CPU
// again.  A real scheduler only resumes runnable processes; `stopped`,
// `completed`, `aborted` and the interpreter *error* states are terminal.
static bool state_is_resumable(ProcessState st) {
  switch (st) {
    case initialized:
    case running:
    case yielded:
    case preempted:
    case uncommon:
    case in_async_dll:
    case yielded_after_async_dll:
      return true;
    default:
      // stopped / completed / aborted / *_error / stack_overflow
      return false;
  }
}

// mock scheduler loop to allow test process->scheduler transfers and returns
int TestDeltaProcess::launch_scheduler(DeltaProcess* process) {
  process->suspend_at_creation();
  DeltaProcess::set_active(process);

  // Bounce the one test process back and forth as long as it is runnable,
  // exactly as a real scheduler would.  The state must be checked *before*
  // handing over the CPU as well as after: transfer_to() resumes the
  // destination first and only learns its state on the *next* suspension.  So
  // a process that stopped before this loop ever ran (e.g. an unhandled error
  // during startup) would still be resumed once -- re-entering
  // `Error>>defaultAction`, which re-raises the error.  A stopped process must
  // never be resumed, or the re-raise loop only ends in a stack overflow
  // (SIGILL).
  while (state_is_resumable(testProcess->state()))
    process->transfer_to(testProcess);

  // The test process stopped for good: an unhandled Smalltalk error moved it
  // to `stopped` (a clean finish never transfers back to the scheduler, so in
  // that case this loop is still blocked inside transfer_to() when the
  // launcher tears the VM down).  A real scheduler would reap the dead process
  // and run the next runnable one; the harness has only this process, so
  // report the error and end the run rather than resuming it forever.
  testRunFailed = true;
  {
    ResourceMark rm;
    _mystd->print("Unhandled error in the test process; state=");
    DeltaProcess::symbol_from_state(testProcess->state())->print_symbol_on(_mystd);
    _mystd->cr();
    // `ProcessorScheduler>>stopWithError:` stored the ProcessError on the
    // process object before calling the stop primitive; report it rather than
    // just the state symbol.  `ProcessError>>name` renders the message (e.g. a
    // ProcessExplicitError's `Error: <msg>`).
    processOop pobj = testProcess->processObj();
    if (pobj != nilObj) {
      oop err = Delta::call(pobj, oopFactory::new_symbol("processError"));
      if (err != nilObj) {
        // The generic oop printer truncates Strings at MaxElementPrintSize
        // characters (10 by default), too short for an error message; widen
        // it just for this report.
        int saved = MaxElementPrintSize;
        MaxElementPrintSize = 512;
        _mystd->print("Process error: ");
        oop desc = Delta::call(err, oopFactory::new_symbol("name"));
        (desc != nilObj ? desc : err)->print_value_on(_mystd);
        _mystd->cr();
        MaxElementPrintSize = saved;
      }
    }
  }
  os::signal_event(done);
  return 0;
}

void set_test_done_event(Event* e) {
  done = e;
}

void wait_for_test_done() {
  os::wait_for_event(done);
}