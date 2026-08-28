/* Copyright 1994 - 1996 LongView Technologies L.L.C. $Revision: 1.50 $ */
/* Copyright (c) 2006, Sun Microsystems, Inc.
 All rights reserved.
 
 Redistribution and use in source and binary forms, with or without modification, are permitted provided that the 
 following conditions are met:
 
 * Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
 * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following 
 disclaimer in the documentation and/or other materials provided with the distribution.
 * Neither the name of Sun Microsystems nor the names of its contributors may be used to endorse or promote products derived 
 from this software without specific prior written permission.
 
 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT 
 NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL 
 THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES 
 (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE 
 OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE
 
 
 */
#ifndef __OS_DARWIN__
#define __OS_DARWIN__
#ifdef __APPLE__
#define _XOPEN_SOURCE
#include "memory/allocation.hpp"
#include "runtime/os.hpp"
#include "runtime/debug.hpp"
#include "utilities/growableArray.hpp"
#include <pthread.h>
#include <unistd.h>
#include <semaphore.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/mman.h>
#include <time.h>
#include <stdio.h>
#include <dlfcn.h>
#include <signal.h>
#include <ucontext.h>
#include <errno.h>

void os_dump_context2(ucontext_t* context) {
#ifdef DELTA_ASSEMBLER_BACKEND_AARCH64
#ifdef __APPLE__
  mcontext_t mcontext = context->uc_mcontext;
  printf("\n");
  for (int r = 0; r < 29; r++)
    if (r == 11 || r == 12 || r == 13 || r == 14 || r == 15 || r == 27)
      printf("x%d=0x%llx ", r, mcontext->__ss.__x[r]);
  printf("fp=0x%llx lr=0x%llx sp=0x%llx pc=0x%llx\n", mcontext->__ss.__fp, mcontext->__ss.__lr, mcontext->__ss.__sp,
         mcontext->__ss.__pc);
#endif
#endif
#if defined(__APPLE__) && !defined(DELTA_ASSEMBLER_BACKEND_AARCH64)
  _STRUCT_MCONTEXT* mcontext = context->uc_mcontext;
  printf("\nrax=0x%llx rbx=0x%llx rcx=0x%llx rdx=0x%llx\n", mcontext->__ss.__rax, mcontext->__ss.__rbx,
         mcontext->__ss.__rcx, mcontext->__ss.__rdx);
  printf("rsi=0x%llx rdi=0x%llx rbp=0x%llx rsp=0x%llx\n", mcontext->__ss.__rsi, mcontext->__ss.__rdi,
         mcontext->__ss.__rbp, mcontext->__ss.__rsp);
  printf("r8 =0x%llx r9 =0x%llx r10=0x%llx r11=0x%llx\n", mcontext->__ss.__r8, mcontext->__ss.__r9,
         mcontext->__ss.__r10, mcontext->__ss.__r11);
  printf("r12=0x%llx r13=0x%llx r14=0x%llx r15=0x%llx\n", mcontext->__ss.__r12, mcontext->__ss.__r13,
         mcontext->__ss.__r14, mcontext->__ss.__r15);
  printf("rip=0x%llx\n", mcontext->__ss.__rip);
#endif
}
void os_dump_context() {
  //	ucontext_t context;
  //	getcontext(&context);
  //	os_dump_context2(&context);
}

static int main_thread_id;
class Lock {
private:
  pthread_mutex_t* mutex;

public:
  Lock(pthread_mutex_t* mutex) : mutex(mutex) { pthread_mutex_lock(mutex); }
  ~Lock() { pthread_mutex_unlock(mutex); }
};

static int _argc;
static char** _argv;

int os::argc() {
  return _argc;
}

char** os::argv() {
  return _argv;
}

void os::set_args(int argc, char* argv[]) {
  _argc = argc;
  _argv = argv;
}

class Event : public CHeapObj {
private:
  bool _signalled;
  pthread_mutex_t mutex;
  pthread_cond_t notifier;

public:
  void inline signal() {
    Lock mark(&mutex);
    _signalled = true;
    pthread_cond_signal(&notifier);
  }
  void inline reset() {
    Lock mark(&mutex);
    _signalled = false;
    pthread_cond_signal(&notifier);
  }
  void inline waitFor() {
    Lock mark(&mutex);
    while (!_signalled)
      pthread_cond_wait(&notifier, &mutex);
  }
  Event(bool state) {
    _signalled = state;
    int result;
    usleep(1000); // pause for signal on other thread to exit
    result = pthread_mutex_init(&mutex, NULL);
    if (result) {
      fatal1("Failed to init Event mutex: %d", result);
    }
    result = pthread_cond_init(&notifier, NULL);
    if (result) {
      fatal1("Failed to init Event condition variable: %d", result);
    }
  }
  ~Event() {
    int result;
    result = pthread_mutex_unlock(&mutex);
    if (result) {
      warning("Failed to unlock Event mutex: %d", result);
    }
    result = pthread_mutex_destroy(&mutex);
    if (result) {
      warning("Failed to destroy Event mutex: %d", result);
    }
    result = pthread_cond_destroy(&notifier);
    if (result) {
      warning("Failed to destroy Event condition variable: %d", result);
    }
  }
};

class Thread : CHeapObj {
public:
  static Thread* find(pthread_t threadId) {
    for (int index = 0; index < _threads->length(); index++) {
      Thread* candidate = _threads->at(index);
      if (candidate == NULL)
        continue;
      if (pthread_equal(threadId, candidate->_threadId))
        return candidate;
    }
    return NULL;
  }
  void suspend() { suspendEvent.waitFor(); }
  void resume() { suspendEvent.signal(); }

private:
  Event suspendEvent;
  static GrowableArray<Thread*>* _threads;
  pthread_t _threadId;
  //	clockid_t _clockId;
  int _thread_index;
  void* stackLimit;

  static void init() {
    ThreadCritical lock;
    _threads = new (true) GrowableArray<Thread*>(10, true);
  }
  Thread(pthread_t threadId, void* stackLimit) : _threadId(threadId), suspendEvent(false), stackLimit(stackLimit) {
    ThreadCritical lock;
    //		pthread_getcpuclockid(_threadId, &_clockId);
    _thread_index = _threads->length();
    _threads->push(this);
  };
  ~Thread() {
    ThreadCritical lock;
    _threads->at_put(_thread_index, NULL);
  }
  double get_cpu_time() {
    struct timespec cpu;
    //		clock_gettime(_clockId, &cpu);
    //		return ((double)cpu.tv_sec) + ((double)cpu.tv_nsec)/1000000000.0;
    return 0;
  }
  friend class os;
};

GrowableArray<Thread*>* Thread::_threads = NULL;
static Thread* main_thread;

extern void intercept_for_single_step();

// No references in VM
int os::getenv(char* name, char* buffer, int len) {
  return 0;
}

// 1 reference (lprintf.cpp)
bool os::move_file(char* from, char* to) {
  return false;
}

// 1 reference (inliningdb.cpp)
bool os::check_directory(char* dir_name) {
  return false;
}

// 1 reference (memory/util.cpp)
void os::breakpoint() {
#if defined(__aarch64__)
  __builtin_trap();
#else
  asm("int3");
#endif
}

// 1 reference process.cpp
Thread* os::starting_thread(int* id_addr) {
  *id_addr = main_thread->_thread_index;
  return main_thread;
}

typedef struct {
  int (*main)(void* parameter);
  void* parameter;
  char* stackLimit;
} thread_args_t;

static Event* threadCreated = NULL;

#define STACK_SIZE ThreadStackSize* K

char* calcStackLimit() {
  char* stackptr;
#if defined(__aarch64__)
  __asm__ volatile("mov %0, sp" : "=r"(stackptr));
#else
  asm("movq %%rsp, %0;" : "=r"(stackptr));
#endif
  stackptr = (char*)align(stackptr, os::vm_page_size());

  int stackHeadroom = 2 * os::vm_page_size();
  return stackptr - STACK_SIZE + stackHeadroom;
}
void* mainWrapper(void* args) {
  thread_args_t* targs = (thread_args_t*)args;
  targs->stackLimit = calcStackLimit();

  int (*threadMain)(void*) = targs->main;
  void* parameter = targs->parameter;
  int* result = (int*)malloc(sizeof(int));
  threadCreated->signal();
  *result = threadMain(parameter);
  return (void*)result;
}

Thread* os::create_thread(int threadStart(void* parameter), void* parameter, int* id_addr) {
  pthread_t threadId;
  thread_args_t threadArgs;
  {
    ThreadCritical tc;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, STACK_SIZE);

    threadCreated->reset();
    threadArgs.main = threadStart;
    threadArgs.parameter = parameter;

    int status = pthread_create(&threadId, &attr, &mainWrapper, &threadArgs);
    if (status != 0) {
      fatal1("Unable to create thread. status = %d", status);
    }
  }
  threadCreated->waitFor();
  Thread* thread = new Thread(threadId, threadArgs.stackLimit);
  *id_addr = thread->_thread_index;
  return thread;
}

void* os::stack_limit(Thread* thread) {
  return thread->stackLimit;
}
// 1 reference process.cpp
void os::terminate_thread(Thread* thread) {
  int result = pthread_cancel(thread->_threadId);
  if (result)
    warning("Error cancelling thread: %d", result);
  usleep(1000);
}

// 1 reference process.cpp
void os::delete_event(Event* event) {
  delete event;
}

// 1 reference process.cpp
Event* os::create_event(bool initial_state) {
  return new Event(initial_state);
}

tms processTimes;

// 2 references - prims/system_prims.cpp, timer.cpp
int os::updateTimes() {
  return times(&processTimes) != (clock_t)-1;
}

// 2 references - prims/system_prims.cpp, timer.cpp
double os::userTime() {
  return ((double)processTimes.tms_utime) / CLOCKS_PER_SEC;
}

// 2 references - prims/system_prims.cpp, timer.cpp
double os::systemTime() {
  return ((double)processTimes.tms_stime) / CLOCKS_PER_SEC;
}

// 1 reference - process.cpp
double os::user_time_for(Thread* thread) {
  //Hack warning - assume half time is spent in kernel, half in user code
  return thread->get_cpu_time() / 2;
}

// 1 reference - process.cpp
double os::system_time_for(Thread* thread) {
  //Hack warning - assume half time is spent in kernel, half in user code
  return thread->get_cpu_time() / 2;
}

static int has_performance_count = 0;
static long_int initial_performance_count(0, 0);
static long_int performance_frequency(0, 0);

// 2 references - memory/error.cpp, evaluator.cpp
void os::fatalExit(int num) {
  exit(num);
}

class DLLLoadError {};

class DLL : CHeapObj {
private:
  char* _name;
  void* _handle;

  DLL(char* name) {
    _handle = dlopen(name, RTLD_LAZY);
    checkHandle(_handle, "could not find library: %s");
    _name = (char*)malloc(strlen(name) + 1);
    strcpy(_name, name);
  }
  void checkHandle(void* handle, const char* format) {
    if (handle == NULL) {
      char* message = (char*)malloc(200);
      snprintf(message, 200, format, dlerror());
      assert(handle != NULL, message);
      free(message);
    }
  }
  ~DLL() {
    if (_handle)
      dlclose(_handle);
    if (_name)
      free(_name);
  }
  bool isValid() { return (_handle != NULL) && (_name != NULL); }
  dll_func lookup(char* funcname) {
    dll_func function = dll_func(dlsym(_handle, funcname));
    checkHandle((void*)function, "could not find function: %s");
    return function;
  }
  friend class os;
};

// 1 reference - prims/dll.cpp
dll_func os::dll_lookup(char* name, DLL* library) {
  return library->lookup(name);
}

// 1 reference - prims/dll.cpp
DLL* os::dll_load(char* name) {
  DLL* library = new DLL(name);
  if (library->isValid())
    return library;
  delete library;
  return NULL;
}

// 1 reference - prims/dll.cpp
bool os::dll_unload(DLL* library) {
  delete library;
  return true;
}
char* os::dll_extension() {
  return ".dylib";
}
int nCmdShow = 0;

// 1 reference - prims/system_prims.cpp
void* os::get_hInstance() {
  return (void*)NULL;
}
// 1 reference - prims/system_prims.cpp
void* os::get_prevInstance() {
  return (void*)NULL;
}
// 1 reference - prims/system_prims.cpp
int os::get_nCmdShow() {
  return 0;
}

extern int bootstrapping;

// 1 reference - prims/debug_prims.cpp
void os::timerStart() {}

// 1 reference - prims/debug_prims.cpp
void os::timerStop() {}

// 1 reference - prims/debug_prims.cpp
void os::timerPrintBuffer() {}

char* os::reserve_memory(int size) {
  return (char*)mmap(0, size, PROT_NONE, MAP_PRIVATE | MAP_ANON, 0, 0);
}

bool os::commit_memory(char* addr, int size) {
  return !mprotect(addr, size, PROT_READ | PROT_WRITE);
}

bool os::uncommit_memory(char* addr, int size) {
  return !mprotect(addr, size, PROT_NONE);
}

bool os::release_memory(char* addr, int size) {
  return !munmap(addr, size);
}

char* os::exec_memory(int size) {
#if defined(__arm64__)
  // On Apple Silicon, W+X pages require the MAP_JIT flag and the region is
  // subject to the W^X enforcement of pthread_jit_write_protect_np: writes
  // fault while protection is on, execution faults while it is off. Map the
  // region and leave it writable so code generation can proceed;
  // os::jit_write_protect(true) must be called before executing generated
  // code (see Process::basic_transfer).
  void* p = mmap(0, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, 0, 0);
  if (p == MAP_FAILED)
    return (char*)-1;
  jit_write_protect(false);
  return (char*)p;
#else
  return (char*)mmap(0, size, PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, 0, 0);
#endif
}

// Toggle the write-protect state of all MAP_JIT regions (no-op on other
// platforms / architectures). The VM keeps generated code write-enabled while
// it is emitting code and re-enables the exec state before running it.
// MAP_JIT write-protect state (Apple Silicon). Kept here so the toggle state
// can be queried; pthread_jit_write_protect_np returns void on some SDKs.
static bool jit_write_protected_state = false;

void os::jit_write_protect(bool protect) {
  jit_write_protected_state = protect;
#if defined(__arm64__)
  pthread_jit_write_protect_np(protect ? 1 : 0);
#endif
}

bool os::jit_write_protect_enabled() {
  return jit_write_protected_state;
}

// No references
bool os::guard_memory(char* addr, int size) {
  return false;
}

void* os::malloc(int size) {
  return ::malloc(size);
}

void* os::calloc(int size, char filler) {
  return ::calloc(size, filler);
}

void os::free(void* p) {
  ::free(p);
}

// 1 reference - process.cpp
void os::transfer(Thread* from_thread, Event* from_event, Thread* to_thread, Event* to_event) {
  from_event->reset();
  to_event->signal();
  from_event->waitFor();
}

// 1 reference - process.cpp
void os::transfer_and_continue(Thread* from_thread, Event* from_event, Thread* to_thread, Event* to_event) {
  from_event->reset();
  to_event->signal();
}

// 1 reference - process.cpp
void os::suspend_thread(Thread* thread) {
  os_dump_context();
  pthread_kill(thread->_threadId, SIGUSR1);
}

void suspendHandler(int signum) {
  Thread* current = Thread::find(pthread_self());
  assert(current, "Suspended thread not found!");
  current->suspend();
}
// 1 reference - process.cpp
void os::resume_thread(Thread* thread) {
  thread->resume();
}

// No references
void os::sleep(int ms) {}

// 1 reference - process.cpp
void os::fetch_top_frame(Thread* thread, int** sp, int** fp, char** pc) {}

// 1 reference - callBack.cpp
int os::current_thread_id() {
  Thread* currentThread = Thread::find(pthread_self());
  if (currentThread == NULL)
    return -1;
  return currentThread->_thread_index;
}

// 1 reference - process.cpp
void os::wait_for_event(Event* event) {
  event->waitFor();
}

// 1 reference - process.cpp
void os::reset_event(Event* event) {
  event->reset();
}

// 1 reference - process.cpp
void os::signal_event(Event* event) {
  event->signal();
}

// 1 reference - process.cpp
bool os::wait_for_event_or_timer(Event* event, int timeout_in_ms) {
  return false;
}

extern "C" bool WizardMode;

void process_settings_file(char* file_name, bool quiet);

static int number_of_ctrl_c = 0;

// 2 references - memory/universe, runtime/virtualspace
int os::_vm_page_size = getpagesize();

// 1 reference - timer.cpp
long_int os::elapsed_counter() {
  struct timespec current_time;
  //	clock_gettime(CLOCK_REALTIME, &current_time);
  //	int64_t current64 = ((int64_t)current_time.tv_sec) * 1000000000 + current_time.tv_nsec;
  //	uint high = current64 >> 32;
  //	uint low  = current64 & 0xffffffff;
  //	long_int current(low, high);
  //	return current;
  long_int current(0, 0);
  return current;
}

// 1 reference - timer.cpp
long_int os::elapsed_frequency() {
  return long_int(1000000000, 0);
}

static struct timeval initial_time;

// 1 reference - prims/system_prims.cpp
double os::elapsedTime() {
  struct timeval current_time;
  gettimeofday(&current_time, NULL);
  long int secs = current_time.tv_sec - initial_time.tv_sec;
  long int usecs = current_time.tv_usec - initial_time.tv_usec;
  if (usecs < 0) {
    secs--;
    usecs += 1000000;
  }
  return secs + (usecs / 1000000.0);
}

// No references
double os::currentTime() {
  return 0;
}

static void initialize_performance_counter() {
  gettimeofday(&initial_time, NULL);
}

// No references
void os::initialize_system_info() {
  Thread::init();
  main_thread = new Thread(pthread_self(), calcStackLimit());
  initialize_performance_counter();
}

// 1 reference - memory/error.cpp
int os::message_box(char* title, char* message) {
  return 0;
}

char* os::platform_class_name() {
  return "MacOSXPlatform";
}

int os::error_code() {
  return errno;
}

void os::add_exception_handler(void new_handler(void* fp, void* sp, void* pc)) {}

extern "C" bool EnableTasks;

pthread_mutex_t ThreadSection;

bool ThreadCritical::_initialized = false;
void ThreadCritical::intialize() {
  pthread_mutex_init(&ThreadSection, NULL);
  _initialized = true;
}
void ThreadCritical::release() {
  pthread_mutex_destroy(&ThreadSection);
}

ThreadCritical::ThreadCritical() {
  pthread_mutex_lock(&ThreadSection);
}

ThreadCritical::~ThreadCritical() {
  pthread_mutex_unlock(&ThreadSection);
}

void real_time_tick(int delay_time);

void* watcherMain(void* ignored) {
  const struct timespec delay = {0, 1 * 1000 * 1000};
  const int delay_interval = 1; // Delay 1 ms
  while (1) {
    int status = nanosleep(&delay, NULL);
    if (!status)
      return 0;
    real_time_tick(delay_interval);
  }
  return 0;
}

void segv_repeated(int signum, siginfo_t* info, void* context) {
  printf("SEGV during signal handling. Aborting.");
  exit(-2);
}

void install_dummy_handler() {
  struct sigaction sa;

  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART | SA_SIGINFO;
  sa.sa_sigaction = segv_repeated;
  if (sigaction(SIGSEGV, &sa, NULL) == -1)
    /* Handle error */;
}

void trace_stack(int thread_id);

static void handler(int signum, siginfo_t* info, void* context) {
  printf("\nsignal: %d  fault_addr: %p\n", signum, info->si_addr);
  os_dump_context2((ucontext_t*)context);
#if defined(__APPLE__) && !defined(DELTA_ASSEMBLER_BACKEND_AARCH64)
  {
    unsigned char* rip_ptr = (unsigned char*)((ucontext_t*)context)->uc_mcontext->__ss.__rip;
    printf("  bytes at rip: ");
    for (int i = 0; i < 15; i++)
      printf("%02x ", rip_ptr[i]);
    printf("\n");
  }
#endif
  fflush(stdout);
  _exit(1);
}

void handleTerminate(int signum) {
  pthread_exit(NULL);
}

#if defined(__x86_64__)
extern "C" intptr_t diag_truncated_oop_value;
extern "C" intptr_t diag_truncated_oop_pc;
extern "C" intptr_t diag_truncated_oop_check_id;
extern "C" intptr_t diag_last_eax_writer_pc;
extern "C" intptr_t diag_last_eax_writer_id;
extern "C" intptr_t diag_last_eax_writer_value;
extern "C" intptr_t diag_pre_check_eax;
extern "C" intptr_t diag_pre_check_id;

static void trap_handler(int signum, siginfo_t* info, void* context) {
  ucontext_t* uc = (ucontext_t*)context;
  mcontext_t mc = uc->uc_mcontext;
  if (diag_truncated_oop_check_id != 0) {
    printf("\n** SIGTRAP: Truncated pointer detected!\n");
    printf("   truncated_oop_value = 0x%llx\n", (unsigned long long)diag_truncated_oop_value);
    printf("   bytecode_pc         = 0x%llx\n", (unsigned long long)diag_truncated_oop_pc);
    printf("   check_id            = %lld\n", (long long)diag_truncated_oop_check_id);
    printf("   last_eax_writer_pc  = 0x%llx\n", (unsigned long long)diag_last_eax_writer_pc);
    printf("   last_eax_writer_id  = %lld\n", (unsigned long long)diag_last_eax_writer_id);
    printf("   last_eax_writer_val = 0x%llx\n", (unsigned long long)diag_last_eax_writer_value);
    printf("   pre_check_eax       = 0x%llx (at check_id=%lld)\n", (unsigned long long)diag_pre_check_eax,
           (long long)diag_pre_check_id);
    printf("   rax=0x%llx rbx=0x%llx rcx=0x%llx rdx=0x%llx\n", (unsigned long long)mc->__ss.__rax,
           (unsigned long long)mc->__ss.__rbx, (unsigned long long)mc->__ss.__rcx, (unsigned long long)mc->__ss.__rdx);
    printf("   rsi=0x%llx rdi=0x%llx rbp=0x%llx rsp=0x%llx\n", (unsigned long long)mc->__ss.__rsi,
           (unsigned long long)mc->__ss.__rdi, (unsigned long long)mc->__ss.__rbp, (unsigned long long)mc->__ss.__rsp);
    printf("   rip=0x%llx\n", (unsigned long long)mc->__ss.__rip);
    // rbp is the current frame pointer - dump frame words
    intptr_t rbp = (intptr_t)mc->__ss.__rbp;
    printf("   frame at rbp=0x%llx:\n", (unsigned long long)rbp);
    for (int i = -6; i <= 6; i++) {
      intptr_t* addr = (intptr_t*)(rbp + i * sizeof(intptr_t));
      printf("     [%+d] = 0x%llx  (addr %p)\n", i, (unsigned long long)*addr, (void*)addr);
    }
    // Dump stack at rsp
    intptr_t rsp = (intptr_t)mc->__ss.__rsp;
    printf("   stack at rsp=0x%llx:\n", (unsigned long long)rsp);
    for (int i = 0; i < 8; i++) {
      intptr_t* addr = (intptr_t*)(rsp + i * sizeof(intptr_t));
      printf("     [rsp%+d] = 0x%llx\n", i * 8, (unsigned long long)*addr);
    }
    // Dump bytecodes: esi (from truncation check) = send opcode address (before advance_aligned)
    // We need bytecodes BEFORE esi (push bytecodes) AND at/after esi (send opcode + IC)
    intptr_t esi = (intptr_t)mc->__ss.__rsi;
    printf("   bytecodes before esi=0x%llx (push bytecodes that loaded values):\n", (unsigned long long)esi);
    unsigned char* bc_before = (unsigned char*)(esi - 48);
    for (int i = 0; i < 48; i++) {
      if (i % 16 == 0)
        printf("     [%+3d]: ", i - 48);
      printf("%02x ", bc_before[i]);
      if (i % 16 == 15)
        printf("\n");
    }
    // Dump bytecodes AT esi (send opcode) and after (IC data follows)
    // BOO format: [opcode 1B] [padding 0-7B] [IC_method 8B] [IC_klass 8B]
    printf("   bytecodes AT/after esi (send opcode + IC):\n");
    unsigned char* bc_at = (unsigned char*)esi;
    for (int i = 0; i < 32; i++) {
      if (i % 16 == 0)
        printf("     [%+3d]: ", i);
      printf("%02x ", bc_at[i]);
      if (i % 16 == 15)
        printf("\n");
    }
    // Interpret: opcode byte, nargs byte (if BBOO), IC first_word, IC second_word
    unsigned char opcode = bc_at[0];
    printf("   send opcode = 0x%02x", opcode);
    // Check if it's a recognized send code
    if (opcode >= 0x80 && opcode <= 0x8f) {
      const char* name = "unknown_send";
      switch (opcode) {
        case 0x80:
          name = "interpreted_send_0";
          break;
        case 0x81:
          name = "interpreted_send_1";
          break;
        case 0x82:
          name = "interpreted_send_2";
          break;
        case 0x83:
          name = "interpreted_send_n";
          break;
        case 0x84:
          name = "compiled_send_0";
          break;
        case 0x85:
          name = "compiled_send_1";
          break;
        case 0x86:
          name = "compiled_send_2";
          break;
        case 0x87:
          name = "compiled_send_n";
          break;
        case 0x88:
          name = "polymorphic_send_0";
          break;
        case 0x89:
          name = "polymorphic_send_1";
          break;
        case 0x8a:
          name = "polymorphic_send_2";
          break;
        case 0x8b:
          name = "polymorphic_send_n";
          break;
        case 0x8c:
          name = "megamorphic_send_0";
          break;
        case 0x8d:
          name = "megamorphic_send_1";
          break;
        case 0x8e:
          name = "megamorphic_send_2";
          break;
        case 0x8f:
          name = "megamorphic_send_n";
          break;
      }
      printf(" (%s)", name);
    } else if (opcode >= 0x90 && opcode <= 0x9f) {
      const char* name = "unknown_pred";
      switch (opcode) {
        case 0x90:
          name = "smi_add";
          break;
        case 0x91:
          name = "smi_sub";
          break;
        case 0x92:
          name = "smi_mul";
          break;
        case 0x93:
          name = "smi_div";
          break;
        case 0x94:
          name = "smi_mod";
          break;
        case 0x95:
          name = "smi_equal";
          break;
        case 0x96:
          name = "smi_not_equal";
          break;
        case 0x97:
          name = "smi_less";
          break;
        case 0x98:
          name = "smi_less_equal";
          break;
        case 0x99:
          name = "smi_greater";
          break;
        case 0x9a:
          name = "smi_greater_equal";
          break;
        case 0x9b:
          name = "smi_bitAnd";
          break;
        case 0x9c:
          name = "smi_bitOr";
          break;
        case 0x9d:
          name = "smi_bitXor";
          break;
        case 0x9e:
          name = "smi_bitShift";
          break;
      }
      printf(" (%s)", name);
    }
    printf("\n");
    // Dump IC: for BOO format, IC first_word at esi+8, second_word at esi+16
    // For BBOO format (nargs byte), IC is at same offsets
    intptr_t cur_ic_method = (intptr_t)*(intptr_t*)(esi + 8);
    intptr_t cur_ic_klass = (intptr_t)*(intptr_t*)(esi + 16);
    printf("   current IC: first_word(method/selector)=0x%llx  second_word(klass/0)=0x%llx\n",
           (unsigned long long)cur_ic_method, (unsigned long long)cur_ic_klass);
    fflush(stdout);
    _exit(1);
  } else {
    // Non-truncation SIGTRAP (e.g., should_not_reach_here, StopInterpreterAt)
    printf("** SIGTRAP (non-truncation) at rip=0x%llx\n", (unsigned long long)mc->__ss.__rip);
    fflush(stdout);
    _exit(1);
  }
}
#endif

void install_signal_handlers() {
  struct sigaction sa;

  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART; /* Restart functions if
	 interrupted by handler */
  sa.sa_handler = suspendHandler;
  if (sigaction(SIGUSR1, &sa, NULL) == -1)
    /* Handle error */;

  sa.sa_handler = handleTerminate;
  if (sigaction(SIGUSR2, &sa, NULL) == -1)
    /* Handle error */;

  sa.sa_flags |= SA_SIGINFO;
  sa.sa_sigaction = handler;
  if (sigaction(SIGSEGV, &sa, NULL) == -1)
    /* Handle error */;

#if defined(__x86_64__)
  // Install SIGTRAP handler for truncated pointer detection
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sa.sa_sigaction = trap_handler;
  if (sigaction(SIGTRAP, &sa, NULL) == -1)
    /* Handle error */;
#endif
}

void os_init() {
  ThreadCritical::intialize();

  install_signal_handlers();
  os::initialize_system_info();

  pthread_setconcurrency(1);

  threadCreated = new Event(false);

  if (EnableTasks) {
    pthread_t watcherThread;
    int status = pthread_create(&watcherThread, NULL, &watcherMain, NULL);
    if (status != 0) {
      fatal("Unable to create thread");
    }
  }
}

void os_exit() {
  ThreadCritical::release();
}
#endif /* __GNUC__ */
#endif
