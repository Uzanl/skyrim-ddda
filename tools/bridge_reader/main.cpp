// Test reader for the DDDA bridge: opens the shared mapping and prints the
// party's state. Build x64 to prove the layout works across bitness.
//   bridge_reader [seconds]
#include <windows.h>

#include <cstdio>
#include <cstdlib>

#include "../../src/common/bridge_shared.h"

static bool Snapshot(const bridge::State* s, bridge::State* out) {
    for (int tries = 0; tries < 1000; ++tries) {
        uint32_t a = s->seq;
        if (a & 1) continue;
        MemoryBarrier();
        *out = *const_cast<const bridge::State*>(s);
        MemoryBarrier();
        if (s->seq == a) return true;
    }
    return false;
}

int main(int argc, char** argv) {
    double seconds = argc > 1 ? atof(argv[1]) : 5.0;

    HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, bridge::kMappingName);
    if (!h) {
        printf("mapping %ls not found (is DDDA running with the bridge?)\n", bridge::kMappingName);
        return 1;
    }
    auto* s = static_cast<const bridge::State*>(MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(bridge::State)));
    if (!s) {
        printf("MapViewOfFile failed: %lu\n", GetLastError());
        return 1;
    }
    if (s->magic != bridge::kMagic || s->version != bridge::kVersion) {
        printf("bad header: magic=0x%08X version=%u\n", s->magic, s->version);
        return 1;
    }

    static const char* kNames[] = {"Arisen", "Main  ", "Hired1", "Hired2"};
    ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(seconds * 1000);
    do {
        bridge::State st;
        if (!Snapshot(s, &st)) {
            printf("snapshot failed (writer stuck mid-update?)\n");
        } else {
            printf("upd=%llu hooks=%s cam%s=(%.0f, %.0f, %.0f)\n", st.updates,
                   (st.flags & bridge::kHooksActive) ? "yes" : "NO", (st.flags & bridge::kCamValid) ? "" : "[x]",
                   st.cam[0], st.cam[1], st.cam[2]);
            for (uint32_t r = 0; r < bridge::kRoleCount; ++r) {
                const bridge::Actor& a = st.actors[r];
                if (a.msSinceSeen == 0xFFFFFFFF) {
                    printf("  %s  never seen\n", kNames[r]);
                    continue;
                }
                printf("  %s  %s seen %5ums ago  pos=(%.0f, %.0f, %.0f)  hp%s=%.0f/%.0f\n", kNames[r],
                       (a.flags & bridge::kActorPresent) ? "PRESENT" : "absent ", a.msSinceSeen, a.pos[0], a.pos[1],
                       a.pos[2], (a.flags & bridge::kActorHpValid) ? "" : "[x]", a.hp, a.hpMax);
            }
        }
        Sleep(500);
    } while (GetTickCount64() < end);
    return 0;
}
