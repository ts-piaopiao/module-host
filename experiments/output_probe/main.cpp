#include "core_contract.h"
#include "output_manager.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

static void WaitMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static void SendKeyScript(OutputManager* out, int32_t vk, bool down) {
    core_decision d = {};
    d.actions[0].kind = CORE_ACTION_KEY;
    d.actions[0].a = vk;
    d.actions[0].b = down ? 1 : 0;
    d.actions[0].c = 0;
    d.out_count = 1;
    out->SendScript(&d);
}

static void SendKeyAsync(OutputManager* out, int32_t vk, bool down) {
    core_action a = {};
    a.kind = CORE_ACTION_KEY;
    a.a = vk;
    a.b = down ? 1 : 0;
    a.c = 0;
    out->SendAsync(&a);
}

int main() {
    OutputManager out;
    if (!out.Start("input_port = none\n")) {
        std::fprintf(stderr, "[probe] FAIL: Start failed\n");
        return 1;
    }
    WaitMs(100);

    std::fprintf(stderr, "[probe] --- case 1: SendScript press LEFT (0x25) ---\n");
    SendKeyScript(&out, 0x25, true);
    WaitMs(100);

    std::fprintf(stderr, "[probe] --- case 2: SetScriptPaused(true) should release LEFT ---\n");
    out.SetScriptPaused(true);
    WaitMs(100);

    std::fprintf(stderr, "[probe] --- case 3: SendScript press RIGHT (0x27) while paused -> dropped ---\n");
    SendKeyScript(&out, 0x27, true);
    WaitMs(100);

    std::fprintf(stderr, "[probe] --- case 4: SendAsync press A (0x41) while paused -> should pass ---\n");
    SendKeyAsync(&out, 0x41, true);
    WaitMs(100);

    std::fprintf(stderr, "[probe] --- case 5: SetScriptPaused(false) ---\n");
    out.SetScriptPaused(false);
    WaitMs(100);

    std::fprintf(stderr, "[probe] --- case 6: SendScript press LEFT again (0x25) ---\n");
    SendKeyScript(&out, 0x25, true);
    WaitMs(100);

    out.Stop();

    std::fprintf(stderr, "[probe] DONE exit=0\n");
    return 0;
}
