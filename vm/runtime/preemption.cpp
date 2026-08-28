#include "runtime/debug.hpp"
#include "runtime/preemption.hpp"
#include "runtime/process.hpp"

void ProcessPreemptionTask::task() {
  if (EnableProcessPreemption)
    DeltaProcess::preempt_active();
}

void preemption_init() {
  ProcessPreemptionTask* task = new ProcessPreemptionTask;
  task->enroll();
}
