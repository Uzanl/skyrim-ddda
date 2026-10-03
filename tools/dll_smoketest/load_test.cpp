// Smoke test: load the bridge DLL in a non-DDDA process. The DLL must forward
// DirectInput8Create, refuse to install its game hooks ("unexpected vtable
// contents" in ddda_bridge.log) and not crash. Note: it opens the same shared
// mapping as a running DDDA, so run it with the game closed.
#include <windows.h>
#include <cstdio>
static const GUID IID_IDirectInput8W = {0xBF798031,0x483A,0x4DA2,{0xAA,0x99,0x5D,0x64,0xED,0x36,0x97,0x00}};
int main() {
  HMODULE m = LoadLibraryW(L"dinput8.dll");
  wchar_t p[MAX_PATH]; GetModuleFileNameW(m, p, MAX_PATH); printf("loaded %ls\n", p);
  auto f = (HRESULT(WINAPI*)(HINSTANCE,DWORD,REFIID,LPVOID*,LPUNKNOWN))GetProcAddress(m, "DirectInput8Create");
  void* di = nullptr;
  HRESULT hr = f(GetModuleHandleW(nullptr), 0x0800, IID_IDirectInput8W, &di, nullptr);
  printf("DirectInput8Create hr=0x%08lX obj=%p\n", hr, di);
  if (di) ((IUnknown*)di)->Release();
  Sleep(4000);
  return 0;
}
