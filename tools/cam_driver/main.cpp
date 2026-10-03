// Test writer for the camera command: makes DDDA's camera orbit the Arisen, to
// prove an outside process can drive what DDDA renders. Releases the camera on
// exit (and DDDA drops it anyway once updates stop).
//   cam_driver [seconds] [radius] [fovY]
#include <windows.h>

#include <cmath>
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

static void Write(bridge::CameraCmd* c, uint32_t flags, const float* pos, const float* target, float fovY) {
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&c->seq));  // odd: writing
    c->flags = flags;
    c->fovY = fovY;
    for (int i = 0; i < 3; ++i) {
        c->pos[i] = pos[i];
        c->target[i] = target[i];
        c->up[i] = i == 1 ? 1.0f : 0.0f;  // DD is Y up
    }
    c->body[0] = c->body[1] = c->body[2] = 0.0f;
    c->updates++;
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&c->seq));  // even: done
}

int main(int argc, char** argv) {
    double seconds = argc > 1 ? atof(argv[1]) : 15.0;
    float radius = argc > 2 ? static_cast<float>(atof(argv[2])) : 500.0f;
    float fovY = argc > 3 ? static_cast<float>(atof(argv[3])) : 0.0f;

    HANDLE hs = OpenFileMappingW(FILE_MAP_READ, FALSE, bridge::kMappingName);
    auto* s = hs ? static_cast<const bridge::State*>(MapViewOfFile(hs, FILE_MAP_READ, 0, 0, sizeof(bridge::State)))
                 : nullptr;
    if (!s || s->magic != bridge::kMagic || s->version != bridge::kVersion) {
        printf("bridge state %ls not available (is DDDA running with the bridge?)\n", bridge::kMappingName);
        return 1;
    }
    HANDLE hc = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(bridge::CameraCmd),
                                   bridge::kCamMappingName);
    auto* c = hc ? static_cast<bridge::CameraCmd*>(
                       MapViewOfFile(hc, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(bridge::CameraCmd)))
                 : nullptr;
    if (!c) {
        printf("camera mapping failed: %lu\n", GetLastError());
        return 1;
    }
    c->magic = bridge::kCamMagic;
    c->version = bridge::kCamVersion;
    c->writerPid = GetCurrentProcessId();

    printf("orbiting the Arisen for %.0f s, radius %.0f, fov %s\n", seconds, radius, fovY > 0 ? argv[3] : "DD's");
    ULONGLONG start = GetTickCount64(), lastPrint = 0;
    float pos[3] = {}, target[3] = {};
    for (;;) {
        ULONGLONG now = GetTickCount64();
        if (now - start >= static_cast<ULONGLONG>(seconds * 1000)) break;
        bridge::State st;
        if (Snapshot(s, &st) && (st.actors[bridge::kArisen].flags & bridge::kActorPresent)) {
            const float* a = st.actors[bridge::kArisen].pos;
            float angle = static_cast<float>((now - start) / 1000.0 * 2.0 * 3.14159265 / 8.0);  // 8 s per turn
            target[0] = a[0];
            target[1] = a[1] + 150.0f;  // about chest height
            target[2] = a[2];
            pos[0] = a[0] + radius * std::cos(angle);
            pos[1] = a[1] + 250.0f;
            pos[2] = a[2] + radius * std::sin(angle);
            Write(c, bridge::kCamOverride, pos, target, fovY);
            if (now - lastPrint >= 1000) {
                lastPrint = now;
                printf("cam (%.0f, %.0f, %.0f) -> Arisen (%.0f, %.0f, %.0f)\n", pos[0], pos[1], pos[2], a[0], a[1],
                       a[2]);
            }
        } else if (now - lastPrint >= 1000) {
            lastPrint = now;
            printf("Arisen not present; not overriding\n");
        }
        Sleep(16);
    }
    Write(c, 0, pos, target, fovY);
    printf("camera released\n");
    return 0;
}
