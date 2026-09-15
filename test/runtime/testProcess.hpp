#ifndef _TEST_PROCESS_HPP
#define _TEST_PROCESS_HPP

#include "memory/allocation.hpp"
#include "runtime/process.hpp"

extern void addTestToProcesses();
extern void removeTestFromProcesses();

typedef int (*osfn)(void*);
typedef int (*fn)(DeltaProcess*);

// A fake DeltaProcess used to run the tests. It is there to allow VM
// operations to be executed on the VMProcess thread. It lives in the test
// library (not the test main) so the test .so/.dll is self-contained; the
// Windows PE linker cannot resolve symbols back into the executable the way
// ELF can at runtime.
class TestDeltaProcess : public DeltaProcess {
private:
  static int launch_tests(DeltaProcess* process);

public:
  TestDeltaProcess();
  TestDeltaProcess(fn launchfn);
  ~TestDeltaProcess();
  void addToProcesses();
  void removeFromProcesses();
  void deoptimized_wrt_marked_nmethods() {}
  bool has_stack() const { return false; }

  static int launch_scheduler(DeltaProcess* process);
};

extern TestDeltaProcess* testProcess;

void initializeSmalltalkEnvironment();
void set_test_done_event(Event* e);
void wait_for_test_done();

class AddTestProcess : public ValueObj {
public:
  AddTestProcess() { addTestToProcesses(); }
  ~AddTestProcess() { removeTestFromProcesses(); }
};
#endif // _TEST_PROCESS_HPP