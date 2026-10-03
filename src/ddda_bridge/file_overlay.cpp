#include "file_overlay.h"

#include <windows.h>

#include <cstdio>
#include <cwchar>

namespace file_overlay {
namespace {

using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileA_t = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

LogFn g_log = nullptr;
wchar_t g_root[MAX_PATH];
CreateFileW_t g_origW = nullptr;
CreateFileA_t g_origA = nullptr;
volatile LONG g_arcSeen[2] = {};  // .arc opens seen through W and A (first few are logged)
constexpr LONG kArcLogged = 3;

bool IsSlash(wchar_t c) { return c == L'\\' || c == L'/'; }

// "nativePC\..." inside path (any case, either slash), or nullptr.
const wchar_t* GameRelative(const wchar_t* path) {
    for (const wchar_t* p = path; *p; ++p)
        if ((p == path || IsSlash(p[-1])) && _wcsnicmp(p, L"nativePC", 8) == 0 && IsSlash(p[8])) return p;
    return nullptr;
}

void NoteArc(const wchar_t* path, int api) {
    size_t n = wcslen(path);
    if (n < 4 || _wcsicmp(path + n - 4, L".arc") != 0) return;
    LONG k = InterlockedIncrement(&g_arcSeen[api]);
    if (k <= kArcLogged) g_log("overlay: CreateFile%c opens archives (%ld): %ls", api ? 'A' : 'W', k, path);
}

// The overlay's copy of a game file opened for reading, into out; false if none.
bool Redirect(const wchar_t* path, DWORD access, wchar_t (&out)[MAX_PATH]) {
    if (!path || (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA))) return false;
    const wchar_t* rel = GameRelative(path);
    if (!rel) return false;
    if (swprintf_s(out, L"%s\\%s", g_root, rel) < 0) return false;
    for (wchar_t* p = out; *p; ++p)
        if (*p == L'/') *p = L'\\';
    DWORD a = GetFileAttributesW(out);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

HANDLE OpenRedirected(const wchar_t* alt, const wchar_t* path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                      DWORD disp, DWORD flags, HANDLE tmpl) {
    HANDLE h = CreateFileW(alt, access, share, sa, disp, flags, tmpl);
    if (h == INVALID_HANDLE_VALUE) {
        g_log("overlay: could not open %ls (%lu); the game's file is used", alt, GetLastError());
        return h;
    }
    g_log("overlay: %ls", GameRelative(path));
    return h;
}

HANDLE WINAPI HookCreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp,
                              DWORD flags, HANDLE tmpl) {
    if (path) {
        NoteArc(path, 0);
        wchar_t alt[MAX_PATH];
        if (Redirect(path, access, alt)) {
            HANDLE h = OpenRedirected(alt, path, access, share, sa, disp, flags, tmpl);
            if (h != INVALID_HANDLE_VALUE) return h;
        }
    }
    return g_origW(path, access, share, sa, disp, flags, tmpl);
}

HANDLE WINAPI HookCreateFileA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp,
                              DWORD flags, HANDLE tmpl) {
    wchar_t wide[MAX_PATH];
    if (path && MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH) > 0) {
        NoteArc(wide, 1);
        wchar_t alt[MAX_PATH];
        if (Redirect(wide, access, alt)) {
            HANDLE h = OpenRedirected(alt, wide, access, share, sa, disp, flags, tmpl);
            if (h != INVALID_HANDLE_VALUE) return h;
        }
    }
    return g_origA(path, access, share, sa, disp, flags, tmpl);
}

}  // namespace

void Install(LogFn log, PatchFn patchImport, const wchar_t* root) {
    g_log = log;
    wcscpy_s(g_root, root);
    for (size_t n = wcslen(g_root); n && IsSlash(g_root[n - 1]); --n) g_root[n - 1] = 0;
    g_origW = reinterpret_cast<CreateFileW_t>(
        patchImport("KERNEL32.dll", "CreateFileW", reinterpret_cast<void*>(&HookCreateFileW)));
    g_origA = reinterpret_cast<CreateFileA_t>(
        patchImport("KERNEL32.dll", "CreateFileA", reinterpret_cast<void*>(&HookCreateFileA)));
    log("overlay: %ls (CreateFileW import %s, CreateFileA import %s)", g_root, g_origW ? "hooked" : "not found",
        g_origA ? "hooked" : "not found");
}

}  // namespace file_overlay
