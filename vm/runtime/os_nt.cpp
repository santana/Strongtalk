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

#ifdef WIN32
#define STACK_SIZE ThreadStackSize* K

#include "memory/allocation.hpp"
#include "runtime/os.hpp"
#include "runtime/debug.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/ostream.hpp"

#include <cstdlib>
#include <windows.h>
#include <signal.h>
typedef struct _thread_start {
  int (*main)(void*);
  void* parameter;
  void* stackLimit;
} thread_start;

int WINAPI startThread(void* params);

class Thread : public CHeapObj {
  static GrowableArray<Thread*>* threads;
  static Event* thread_created;
  HANDLE thread_handle;
  int thread_id;
  void* stack_limit;

  static void initialize() {
    threads = new (true) GrowableArray<Thread*>(10, true);
    thread_created = os::create_event(false);
  }
  static void release() {}
  static bool equals(void* token, Thread* element) { return token == (void*)element; }
  Thread(HANDLE handle, int id, void* stackLimit) : thread_handle(handle), thread_id(id), stack_limit(stackLimit) {
    int index = threads->find(NULL, equals);
    if (index < 0)
      threads->append(this);
    else
      threads->at_put(index, this);
  }
  virtual ~Thread() {
    int index = threads->find(this);
    threads->at_put(index, NULL);
  }

  static Thread* createThread(int main(void* parameter), void* parameter, int* id_addr) {
    ThreadCritical tc;
    thread_start params;
    params.main = main;
    params.parameter = parameter;

    os::reset_event(thread_created);
    HANDLE result =
      CreateThread(NULL, STACK_SIZE, (LPTHREAD_START_ROUTINE)startThread, &params, 0, (unsigned long*)id_addr);
    if (result == NULL)
      return NULL;

    os::wait_for_event(thread_created);

    return new Thread(result, *id_addr, params.stackLimit);
  }

  static Thread* findThread(int thread_id) {
    for (int index = 0; index < threads->length(); index++) {
      Thread* thread = threads->at(index);
      if (thread == NULL)
        continue;
      if (thread->thread_id == thread_id)
        return thread;
    }
    return NULL;
  }

  friend class os;
  friend int WINAPI startThread(void*);
  friend void os_init();
  friend void os_exit();
};

int WINAPI startThread(void* params) {
  char* spptr;
  // MSVC and MinGW dropped 32-bit inline asm; read the native stack pointer.
#if defined(_WIN64) || defined(__x86_64__)
  __asm__("movq %%rsp, %0;" : "=r"(spptr));
#else
  __asm__("movl %%esp, %0;" : "=r"(spptr));
#endif
  int stackHeadroom = 2 * os::vm_page_size();
  ((thread_start*)params)->stackLimit = spptr - STACK_SIZE + stackHeadroom;

  int (*main)(void*) = ((thread_start*)params)->main;
  void* parameter = ((thread_start*)params)->parameter;

  os::signal_event(Thread::thread_created);
  return main(parameter);
}

Event* Thread::thread_created = NULL;
GrowableArray<Thread*>* Thread::threads = NULL;

static HANDLE main_process;
//static HANDLE main_thread;
static int main_thread_id;
static HANDLE watcher_thread;
static Thread* main_thread;

static FILETIME process_creation_time;
static FILETIME process_exit_time;
static FILETIME process_user_time;
static FILETIME process_kernel_time;

extern void intercept_for_single_step();

static inline double fileTimeAsDouble(FILETIME* time) {
  const double high = (double)((unsigned int)~0);
  const double split = 10000000.0;
  double result = (time->dwLowDateTime / split) + time->dwHighDateTime * (high / split);
  return result;
}

int os::getenv(char* name, char* buffer, int len) {
  int result = GetEnvironmentVariable(name, buffer, len);
  return result != 0;
}

bool os::move_file(char* from, char* to) {
  return MoveFileEx(from, to, MOVEFILE_REPLACE_EXISTING) ? true : false;
}

bool os::check_directory(char* dir_name) {
  bool result = CreateDirectory(dir_name, NULL) ? true : false;
  if (!result) {
    int error = GetLastError();
    if (error == ERROR_ALREADY_EXISTS)
      return true;
    return false;
  }
  return true;
}

void os::breakpoint() {
  DebugBreak();
}

Thread* os::starting_thread(int* id_addr) {
  *id_addr = main_thread_id;
  return main_thread;
}

Thread* os::create_thread(int main(void* parameter), void* parameter, int* id_addr) {
  return Thread::createThread(main, parameter, id_addr);
}

void* os::stack_limit(Thread* thread) {
  return thread->stack_limit;
}
void os::terminate_thread(Thread* thread) {
  HANDLE handle = thread->thread_handle;
  delete thread;

  TerminateThread(handle, 0);
  CloseHandle(handle);
}

void os::delete_event(Event* event) {
  CloseHandle((HANDLE)event);
}

Event* os::create_event(bool initial_state) {
  HANDLE result = CreateEvent(NULL, TRUE, initial_state, NULL);
  if (result == NULL)
    fatal("CreateEvent failed");
  return (Event*)result;
}

int os::updateTimes() {
  return GetProcessTimes(main_process, &process_creation_time, &process_exit_time, &process_kernel_time,
                         &process_user_time);
}

double os::userTime() {
  return fileTimeAsDouble(&process_user_time);
}

double os::systemTime() {
  return fileTimeAsDouble(&process_kernel_time);
}

double os::user_time_for(Thread* thread) {
  FILETIME creation_time;
  FILETIME exit_time;
  FILETIME user_time;
  FILETIME kernel_time;
  if (GetThreadTimes(thread->thread_handle, &creation_time, &exit_time, &kernel_time, &user_time)) {
    return fileTimeAsDouble(&user_time);
  }
  return 0.0;
}

double os::system_time_for(Thread* thread) {
  FILETIME creation_time;
  FILETIME exit_time;
  FILETIME user_time;
  FILETIME kernel_time;
  if (GetThreadTimes(thread->thread_handle, &creation_time, &exit_time, &kernel_time, &user_time)) {
    return fileTimeAsDouble(&kernel_time);
  }
  return 0.0;
}

static int has_performance_count = 0;
static long_int initial_performance_count(0, 0);
static long_int performance_frequency(0, 0);

long_int os::elapsed_counter() {
  LARGE_INTEGER count;
  QueryPerformanceCounter(&count);
  long_int current(count.LowPart, count.HighPart);
  return current;
}

long_int os::elapsed_frequency() {
  return performance_frequency;
}

static void initialze_performance_counter() {
  LARGE_INTEGER count;
  if (QueryPerformanceFrequency(&count)) {
    has_performance_count = 1;
    performance_frequency = long_int(count.LowPart, count.HighPart);
    QueryPerformanceCounter(&count);
    initial_performance_count = long_int(count.LowPart, count.HighPart);
  } else {
    has_performance_count = 0;
  }
}

double os::elapsedTime() {
  if (!has_performance_count)
    return 0.0;
  LARGE_INTEGER current_count;
  QueryPerformanceCounter(&current_count);

  long_int current(current_count.LowPart, current_count.HighPart);
  double count = (current - initial_performance_count).as_double();
  double freq = performance_frequency.as_double();

  return count / freq;
}

double os::currentTime() {
  SYSTEMTIME s;
  FILETIME f;
  GetSystemTime(&s);
  SystemTimeToFileTime(&s, &f);
  return fileTimeAsDouble(&f);
}

void os::fatalExit(int num) {
  ExitProcess(num);
}

dll_func os::dll_lookup(char* name, DLL* library) {
  dll_func result = (dll_func)GetProcAddress((HINSTANCE)library, name);
  return result;
}

DLL* os::dll_load(char* name) {
  HINSTANCE lib = LoadLibrary(name);
  return (DLL*)lib;
}

bool os::dll_unload(DLL* library) {
  return FreeLibrary((HINSTANCE)library) ? true : false;
}

char* os::dll_extension() {
  return ".dll";
}
char* exception_name(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
      return "Access violation";
    case EXCEPTION_DATATYPE_MISALIGNMENT:
      return "Data misaligned";
    case EXCEPTION_BREAKPOINT:
      return "Breakpoint";
    case EXCEPTION_SINGLE_STEP:
      return "Single step";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
      return "Array bounds exceeded";
    case EXCEPTION_FLT_DENORMAL_OPERAND:
      return "Float denormal operand";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
      return "Float divide by zero";
    case EXCEPTION_FLT_INEXACT_RESULT:
      return "Float inexact result";
    case EXCEPTION_FLT_INVALID_OPERATION:
      return "Float invalid operation";
    case EXCEPTION_FLT_OVERFLOW:
      return "Float overflow";
    case EXCEPTION_FLT_STACK_CHECK:
      return "Float stack check";
    case EXCEPTION_FLT_UNDERFLOW:
      return "Float underflow";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
      return "Integer divide by zero";
    case EXCEPTION_INT_OVERFLOW:
      return "Integer overflow";
    case EXCEPTION_PRIV_INSTRUCTION:
      return "Privileged instruction";
    case EXCEPTION_IN_PAGE_ERROR:
      return "In page error";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
      return "Illegal Instruction";
    case EXCEPTION_STACK_OVERFLOW:
      return "Stack overflow";
    case EXCEPTION_GUARD_PAGE:
      return "Guard page";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
      return "Noncontinuable exception";
    case EXCEPTION_INVALID_DISPOSITION:
      return "Invalid disposition";
    default:
      return "Unknown exception";
  }
}

void trace_stack_at_exception(int* sp, int* fp, char* pc);
void suspend_process_at_stack_overflow(int* sp, void** fp, char* pc);

// Describe a fault address: module base + offset when it falls inside a
// loaded image (the VM exe or the strongtalk silhouette DLL), otherwise say
// it lives outside the modules -- i.e. in JIT-generated code or heap.
static void report_exception_address(void* addr) {
  HMODULE module = NULL;
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     (LPCSTR)addr, &module);
  if (module == NULL) {
    MEMORY_BASIC_INFORMATION mbi;
    const char* region = "unknown";
    SIZE_T size = 0;
    if (VirtualQuery(addr, &mbi, sizeof(mbi))) {
      region = (mbi.State == MEM_COMMIT) ? "committed" : "unmapped";
      size = mbi.RegionSize;
    }
    lprintf("  ExceptionAddress %p: outside loaded modules "
            "(%s allocation, size %lu) -- likely JIT-generated code or heap\n",
            addr, region, (unsigned long)size);
    return;
  }
  char name[BUFSIZ];
  if (GetModuleFileNameA(module, name, sizeof(name)) == 0) {
    snprintf(name, sizeof(name), "module @%p", (void*)module);
  }
  lprintf("  ExceptionAddress %p: in %s at offset 0x%lx\n", addr, name, (long)((char*)addr - (char*)module));
}

// Dump up to n instruction bytes at a fault site. Guarded by a VirtualQuery
// so we never re-fault while trying to report the original fault.
static void dump_bytes_at(void* addr, int n) {
  MEMORY_BASIC_INFORMATION mbi;
  if (addr == NULL || !VirtualQuery(addr, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
      (mbi.Protect & PAGE_GUARD) != 0 ||
      (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                      PAGE_EXECUTE_WRITECOPY)) == 0) {
    lprintf("  <unreadable>\n");
    return;
  }
  const unsigned char* p = (const unsigned char*)addr;
  lprintf("    ");
  for (int i = 0; i < n; i++)
    lprintf("%02x ", p[i]);
  lprintf("\n");
}

// Dump a return-address chain if the faulting thread's stack is readable.
static void dump_return_chain(void* rsp, int words) {
  MEMORY_BASIC_INFORMATION mbi;
  if (rsp == NULL || !VirtualQuery(rsp, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
      (mbi.Protect & PAGE_GUARD) != 0) {
    lprintf("  <stack unreadable>\n");
    return;
  }
  for (int i = 0; i < words; i++) {
    void** slot = (void**)rsp + i;
    // If the walk leaves the committed region, stop.
    if (slot >= (void**)((char*)mbi.BaseAddress + mbi.RegionSize)) {
      break;
    }
    lprintf("  [rsp+%d] %p\n", i * 8, slot[0]);
  }
}

LONG WINAPI topLevelExceptionFilter(struct _EXCEPTION_POINTERS* exceptionInfo) {
  // Guard against re-entry: tracing/walking a corrupt frame from inside the
  // handler can fault again, which would loop forever (seen as a hang).
  static bool in_exception_handler = false;
  if (in_exception_handler) {
    lprintf("Exception while handling exception; aborting.\n");
    return EXCEPTION_CONTINUE_SEARCH;
  }
  in_exception_handler = true;

  struct _EXCEPTION_RECORD* rec = exceptionInfo->ExceptionRecord;
  DWORD code = rec->ExceptionCode;

  if (code == EXCEPTION_BREAKPOINT) {
    // This exception is called when an assertion fails (__asm { int 3} is executed).
    // It is therefore imperative we continue the search hereby enabling
    // spawning of a Just-in-time debugger.
    return EXCEPTION_CONTINUE_SEARCH;
  }

  lprintf("Exception caught \"%s\".\n", exception_name(code));
  report_exception_address(rec->ExceptionAddress);

#if defined(_WIN64)
  CONTEXT* c = exceptionInfo->ContextRecord;
  lprintf("  Rip=%p Rsp=%p Rbp=%p\n", (void*)c->Rip, (void*)c->Rsp, (void*)c->Rbp);
  lprintf("  Rax=%p Rbx=%p Rcx=%p Rdx=%p Rsi=%p Rdi=%p\n", (void*)c->Rax, (void*)c->Rbx, (void*)c->Rcx, (void*)c->Rdx,
          (void*)c->Rsi, (void*)c->Rdi);
  lprintf("  R8=%p R9=%p R10=%p R11=%p R12=%p R13=%p R14=%p R15=%p\n", (void*)c->R8, (void*)c->R9, (void*)c->R10,
          (void*)c->R11, (void*)c->R12, (void*)c->R13, (void*)c->R14, (void*)c->R15);
  lprintf("  Instructions at Rip:\n");
  dump_bytes_at((void*)c->Rip, 32);
  lprintf("  Return chain:\n");
  dump_return_chain((void*)c->Rsp, 5);
#else
  lprintf("  Eip=%p Esp=%p Ebp=%p\n", (void*)exceptionInfo->ContextRecord->Eip,
          (void*)exceptionInfo->ContextRecord->Esp, (void*)exceptionInfo->ContextRecord->Ebp);
#endif

  if (code == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
    // ExceptionInformation[0]: 0 = read, 1 = write, 8 = execute.
    // ExceptionInformation[1]:  the address that was accessed.
    lprintf("  Access violation: %s at %p\n",
            rec->ExceptionInformation[0] == 0   ? "reading"
            : rec->ExceptionInformation[0] == 1 ? "writing"
                                                : "executing",
            (void*)rec->ExceptionInformation[1]);
  }

  if (code == EXCEPTION_STACK_OVERFLOW) {
    lprintf("  Oops, we encounted a stack overflow.\n");
    lprintf("  You should check your program for infinite recursion!\n");
#if defined(_WIN64)
    suspend_process_at_stack_overflow((int*)exceptionInfo->ContextRecord->Rsp,
                                      (void**)exceptionInfo->ContextRecord->Rbp,
                                      (char*)exceptionInfo->ContextRecord->Rip);
#else
    suspend_process_at_stack_overflow((int*)exceptionInfo->ContextRecord->Esp,
                                      (void**)exceptionInfo->ContextRecord->Ebp,
                                      (char*)exceptionInfo->ContextRecord->Eip);
#endif
    lprintf("  Coutinue execution ??????????????\n");
  } else {
    // Do not report vm state when getting stack overflow
    report_vm_state();
  }

  // The vframe trace needs a sane Delta stack; on the faulting thread the
  // last_Delta_* liveness may be garbage, and walking it can hang. Only do
  // it when explicitly requested (os::message_box is stubbed to always
  // answer "yes" on this build).
  if (getenv("STRONGTALK_EXCEPTION_TRACE")) {
#if defined(_WIN64)
    trace_stack_at_exception((int*)exceptionInfo->ContextRecord->Rsp, (int*)exceptionInfo->ContextRecord->Rbp,
                             (char*)exceptionInfo->ContextRecord->Rip);
#else
    trace_stack_at_exception((int*)exceptionInfo->ContextRecord->Esp, (int*)exceptionInfo->ContextRecord->Ebp,
                             (char*)exceptionInfo->ContextRecord->Eip);
#endif
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

HINSTANCE hInstance = NULL;
HINSTANCE hPrevInstance = NULL;
int nCmdShow = 0;

extern int vm_main(int argc, char* argv[]);

void os::set_args(int argc, char* argv[]) {}

extern int __argc;
extern char** __argv;

int os::argc() {
  return __argc;
}

char** os::argv() {
  return __argv;
}

#ifdef _WINDOWS

int CALLBACK WinMain(HINSTANCE hInst, HINSTANCE hPrevInst, LPSTR cmdLine, int cmdShow) {
  // Save all parameters
  hInstance = hInst;
  hPrevInstance = hPrevInst;
  nCmdShow = cmdShow;
  return my_main(__argc, __argv);
}
#else
//int main(int argc, char**argv) {
//    return vm_main(argc, argv);
//}
#endif

void* os::get_hInstance() {
  return (void*)hInstance;
}
void* os::get_prevInstance() {
  return (void*)hPrevInstance;
}
int os::get_nCmdShow() {
  return nCmdShow;
}

extern int bootstrapping;

void os::timerStart() {}

void os::timerStop() {}

void os::timerPrintBuffer() {}

// Windows has no MAP_JIT/W^X enforcement; these toggles are no-ops that keep
// the "enabled" state for the debug output, mirroring the Linux backend.
static bool jit_write_protected_state = false;
void os::jit_write_protect(bool protect) {
  jit_write_protected_state = protect;
}
bool os::jit_write_protect_enabled() {
  return jit_write_protected_state;
}

// Virtual Memory

char* os::reserve_memory(int size) {
  return (char*)VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_READWRITE);
}

bool os::commit_memory(char* addr, int size) {
  bool result = VirtualAlloc(addr, size, MEM_COMMIT, PAGE_READWRITE) != NULL;
  if (!result) {
    int error = GetLastError();
    lprintf("commit_memory error %d 0x%lx\n", error, error);
  }
  return result;
}

bool os::uncommit_memory(char* addr, int size) {
  return VirtualFree(addr, size, MEM_DECOMMIT) ? true : false;
}

bool os::release_memory(char* addr, int size) {
  return VirtualFree(addr, 0, MEM_RELEASE) ? true : false;
}

bool os::guard_memory(char* addr, int size) {
  DWORD old_status;
  return VirtualProtect(addr, size, PAGE_READWRITE | PAGE_GUARD, &old_status) ? true : false;
}

char* os::exec_memory(int size) {
  return (char*)VirtualAlloc(NULL, size, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
}

void* os::malloc(int size) {
  if (!ThreadCritical::initialized()) {
    return ::malloc(size);
  } else {
    ThreadCritical tc;
    return ::malloc(size);
  }
}

void* os::calloc(int size, char filler) {
  if (!ThreadCritical::initialized()) {
    return ::calloc(size, filler);
  } else {
    ThreadCritical tc;
    return ::calloc(size, filler);
  }
}

void os::free(void* p) {
  if (!ThreadCritical::initialized()) {
    ::free(p);
  } else {
    ThreadCritical tc;
    ::free(p);
  }
}

void os::transfer(Thread* from_thread, Event* from_event, Thread* to_thread, Event* to_event) {
  ResetEvent((HANDLE)from_event);
  SetEvent((HANDLE)to_event);
  WaitForSingleObject((HANDLE)from_event, INFINITE);
}

void os::transfer_and_continue(Thread* from_thread, Event* from_event, Thread* to_thread, Event* to_event) {
  ResetEvent((HANDLE)from_event);
  SetEvent((HANDLE)to_event);
}

void os::suspend_thread(Thread* thread) {
  SuspendThread(thread->thread_handle);
}

void os::resume_thread(Thread* thread) {
  ResumeThread(thread->thread_handle);
}

void os::sleep(int ms) {
  Sleep(ms);
}

void os::fetch_top_frame(Thread* thread, int** sp, int** fp, char** pc) {
  CONTEXT context;
  context.ContextFlags = CONTEXT_CONTROL;
  if (GetThreadContext(thread->thread_handle, &context)) {
#if defined(_WIN64)
    *sp = (int*)context.Rsp;
    *fp = (int*)context.Rbp;
    *pc = (char*)context.Rip;
#else
    *sp = (int*)context.Esp;
    *fp = (int*)context.Ebp;
    *pc = (char*)context.Eip;
#endif
  } else {
    *sp = NULL;
    *fp = NULL;
    *pc = NULL;
  }
}

int os::current_thread_id() {
  return GetCurrentThreadId();
}

void os::wait_for_event(Event* event) {
  WaitForSingleObject((HANDLE)event, INFINITE);
}

void os::reset_event(Event* event) {
  ResetEvent((HANDLE)event);
}

void os::signal_event(Event* event) {
  SetEvent((HANDLE)event);
}

bool os::wait_for_event_or_timer(Event* event, int timeout_in_ms) {
  return WAIT_TIMEOUT == WaitForSingleObject((HANDLE)event, timeout_in_ms);
}

extern "C" bool WizardMode;

void process_settings_file(char* file_name, bool quiet);

static int number_of_ctrl_c = 0;

BOOL WINAPI HandlerRoutine(DWORD dwCtrlType) {
  if (CTRL_BREAK_EVENT == dwCtrlType) {
    lprintf("\n{receiving break}\n");
    intercept_for_single_step();
  } else {
    // if (number_of_ctrl_c < 10) {
    lprintf("\n{reading .breakrc}");
    process_settings_file(".breakrc", false);
    /* } else {
      lprintf("\n{aborting}\n");
   //   _asm { int 3 }
   breakpoint();
    }
    number_of_ctrl_c++;*/
  }
  return TRUE;
}

void real_time_tick(int delay_time);

// The sole purpose of the watcher thread is simulating
// timer interrupts.

DWORD WINAPI WatcherMain(LPVOID lpvParam) {
  const int delay_interval = 1; // Delay 1 ms
  while (1) {
    Sleep(delay_interval);
    real_time_tick(delay_interval);
  }
  return 0;
}

int os::_vm_page_size = 0;

void os::initialize_system_info() {
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  _vm_page_size = si.dwPageSize;
  initialze_performance_counter();
}

int os::message_box(char* title, char* message) {
  /*  int result = MessageBox(NULL, message, title,
                          MB_YESNO | MB_ICONERROR | MB_SYSTEMMODAL | MB_DEFAULT_DESKTOP_ONLY);
			  */
  int result = IDYES; // ugly hack to reduce DLL depends
  return result == IDYES;
}

char* os::platform_class_name() {
  return "Win32Platform";
}

extern "C" bool EnableTasks;

LARGE_INTEGER counter;

CRITICAL_SECTION ThreadSection;

bool ThreadCritical::_initialized = false;
void ThreadCritical::intialize() {
  InitializeCriticalSection(&ThreadSection);
  _initialized = true;
}
void ThreadCritical::release() {
  DeleteCriticalSection(&ThreadSection);
}

ThreadCritical::ThreadCritical() {
  EnterCriticalSection(&ThreadSection);
}

ThreadCritical::~ThreadCritical() {
  LeaveCriticalSection(&ThreadSection);
}

void (*handler)(void* fp, void* sp, void* pc) = NULL;
bool handling_exception;

LONG WINAPI testVectoredHandler(struct _EXCEPTION_POINTERS* exceptionInfo) {
  //lprintf("Caught exception.\n");
  if (false && handler && !handling_exception) {
    handling_exception = true;
#if defined(_WIN64)
    handler((void*)exceptionInfo->ContextRecord->Rbp, (void*)exceptionInfo->ContextRecord->Rsp,
            (void*)exceptionInfo->ContextRecord->Rip);
#else
    handler((void*)exceptionInfo->ContextRecord->Ebp, (void*)exceptionInfo->ContextRecord->Esp,
            (void*)exceptionInfo->ContextRecord->Eip);
#endif
    handling_exception = false;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

void os::add_exception_handler(void new_handler(void* fp, void* sp, void* pc)) {
  handler = new_handler;
  AddVectoredExceptionHandler(0, testVectoredHandler);
}

int os::error_code() {
  return GetLastError();
}

void os_init() {
  ThreadCritical::intialize();
  Thread::initialize();

  if (hInstance == NULL) {
    hInstance = GetModuleHandle(NULL);
    nCmdShow = SW_SHOWNORMAL;
  }

  os::initialize_system_info();

  //%todo: remove this processor affinity stuff
  DWORD_PTR systemMask;
  DWORD_PTR processMask;
  GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask);

  DWORD_PTR processorId = 1;
  while (!(processMask & processorId) && processorId < processMask)
    processorId >>= 1;
  _mystd->print_cr("processor: %lu", (unsigned long)processorId);
  if (!SetProcessAffinityMask(GetCurrentProcess(), processorId))
    _mystd->print_cr("error code: %d", GetLastError());
  // end processor affinity - for removal

  SetConsoleCtrlHandler(&HandlerRoutine, TRUE);

  HANDLE threadHandle;
  // Initialize main_process and main_thread
  main_process = GetCurrentProcess(); // Remember main_process is a pseudo handle
  if (!DuplicateHandle(main_process, GetCurrentThread(), main_process, &threadHandle, THREAD_ALL_ACCESS, FALSE, 0)) {
    fatal("DuplicateHandle failed\n");
  }
  main_thread_id = (int)GetCurrentThreadId();

  main_thread = new Thread(threadHandle, main_thread_id, NULL);

  // Setup Windows Exceptions

  SetUnhandledExceptionFilter(topLevelExceptionFilter);

  // Create the watcher thread

  if (EnableTasks) {
    DWORD watcher_id;
    watcher_thread = CreateThread(NULL, 0, &WatcherMain, 0, 0, &watcher_id);
    SetThreadPriority(watcher_thread, THREAD_PRIORITY_HIGHEST);
  }
}

void os_exit() {
  Thread::release();
  ThreadCritical::release();
}

#endif /* _WIN32 */
