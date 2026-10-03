// "Party only" rendering for the Skyrim view: while linked, DDDA skips every mesh
// draw that does not belong to the party pawns (or the models they carry), so its
// frame shows the pawns alone. The G-buffer (cleared to white) then marks which
// pixels hold a pawn, and is exported as the transparency mask.
//
// The party's meshes are recognised by their vertex buffers, learned by diffing:
// every few seconds the pawns are hidden for a few frames (parts masks), and the
// vertex buffers drawn before and after but not while hidden are theirs. The set
// accumulates: a buffer that was not drawn at all (pawns off screen) keeps its
// status. Hidden frames are not published.
#pragma once

#include <d3d9.h>

#include <cstdint>

namespace isolate {

using LogFn = void (*)(const char* fmt, ...);
void Init(LogFn log);

// Bridge thread.
void SetLinked(bool linked);
// ddda_experiment.txt "nolearn": no learning cycles once the party is known and the
// first 10 s after linking are over (an A/B test for the periodic hitch).
void SetLearnPaused(bool paused);

// Game threads (move hooks): a pawn, or a model carried by `owner`.
void NotePawn(uintptr_t pawn);
bool IsPawn(uintptr_t obj);
void NoteCarried(uintptr_t model);

// Render thread.
bool SkipDraw(IDirect3DVertexDeclaration9* decl, IDirect3DVertexBuffer9* vb, bool indexed);
void OnClear(IDirect3DDevice9* dev, DWORD flags, D3DCOLOR color);
void OnPresent();
bool Filtering();               // party-only rendering is on and this frame shows it
bool PublishThisFrame();        // false while the pawns are hidden for learning
IDirect3DSurface9* GBuffer();   // mask source when Filtering(), else nullptr

}  // namespace isolate
