//#ifdef WIN32
#include <windows.h>
extern "C" BOOL WINAPI DllMain(HINSTANCE const instance,
                               DWORD const reason,
                               LPVOID const reserved)
{
  return TRUE;
}

__declspec(dllexport) int myexport(void)
{
  return 1;
}

//#endif
