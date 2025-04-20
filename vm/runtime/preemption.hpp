#ifndef _PREEMPTION_HPP
#define _PREEMPTION_HPP

#include "runtime/task.hpp"

class ProcessPreemptionTask : public PeriodicTask {
public:
  ProcessPreemptionTask() : PeriodicTask(1) {};
  void task();
};

#endif // _PREEMPTION_HPP
