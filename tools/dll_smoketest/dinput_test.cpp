// Smoke test for the DirectInput hooks: creates a keyboard device through the
// proxy, asks for foreground/exclusive mode (the log must show it rewritten to
// background/non-exclusive) and reads its state. Run with the game closed.
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <cstdio>
int main() {
  HMODULE m = LoadLibraryW(L"dinput8.dll");
  auto f = (HRESULT(WINAPI*)(HINSTANCE,DWORD,REFIID,LPVOID*,LPUNKNOWN))GetProcAddress(m, "DirectInput8Create");
  IDirectInput8W* di = nullptr;
  HRESULT hr = f(GetModuleHandleW(nullptr), 0x0800, IID_IDirectInput8W, (void**)&di, nullptr);
  printf("create di hr=%08lX\n", hr);
  IDirectInputDevice8W* kb = nullptr;
  hr = di->CreateDevice(GUID_SysKeyboard, &kb, nullptr); printf("CreateDevice hr=%08lX\n", hr);
  hr = kb->SetDataFormat(&c_dfDIKeyboard); printf("SetDataFormat hr=%08lX\n", hr);
  HWND w = GetConsoleWindow();
  hr = kb->SetCooperativeLevel(w, DISCL_FOREGROUND | DISCL_EXCLUSIVE); printf("SetCooperativeLevel hr=%08lX\n", hr);
  hr = kb->Acquire(); printf("Acquire hr=%08lX\n", hr);
  BYTE keys[256]; hr = kb->GetDeviceState(256, keys); printf("GetDeviceState hr=%08lX\n", hr);
  DIDEVICEOBJECTDATA d[8]; DWORD n = 8; hr = kb->GetDeviceData(sizeof(d[0]), d, &n, 0); printf("GetDeviceData hr=%08lX n=%lu\n", hr, n);
  kb->Unacquire(); kb->Release(); di->Release();
  return 0;
}
