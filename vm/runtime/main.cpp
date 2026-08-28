#include "runtime/os.hpp"
#include "runtime/shell.hpp"

#ifdef _WINDOWS
int CALLBACK WinMain(HINSTANCE hInst, HINSTANCE hPrevInst, LPSTR cmdLine, int cmdShow) {
  // Save all parameters
  hInstance = hInst;
  hPrevInstance = hPrevInst;
  nCmdShow = cmdShow;
  os::set_args(__argc, __argv);
  return vm_main(__argc, __argv);
}
#elif defined(__linux__) || defined(__APPLE__) || defined(WIN32) || defined(__OpenBSD__)
int main(int argc, char* argv[]) {
  os::set_args(argc, argv);
  return vm_main(argc, argv);
}
#endif
