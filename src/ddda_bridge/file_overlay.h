// The tile overlay (docs/terrain-proxy.md, "Overlay"): in a bridge session, DDDA's opens
// of a file under nativePC\ get the overlay's copy (<root>\nativePC\..., written by
// tools/terrain/stream.py) when it has one. DDDA's own files are never written, so DDDA
// started without a session is the plain game.
//
// Wraps DDDA.exe's CreateFileW and CreateFileA imports. Only opens for reading are
// redirected; anything else goes to the game's file unchanged.
#pragma once

namespace file_overlay {

using LogFn = void (*)(const char* fmt, ...);
using PatchFn = void* (*)(const char* dll, const char* func, void* hook);

// root: the folder that holds the overlay's nativePC. Call from DllMain, before the game
// opens its files.
void Install(LogFn log, PatchFn patchImport, const wchar_t* root);

}  // namespace file_overlay
