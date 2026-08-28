#ifndef _TEST_PROCESS_HPP
#define _TEST_PROCESS_HPP

extern void addTestToProcesses();
extern void removeTestFromProcesses();

class AddTestProcess : public ValueObj {
public:
  AddTestProcess() { addTestToProcesses(); }
  ~AddTestProcess() { removeTestFromProcesses(); }
};
#endif // _TEST_PROCESS_HPP
