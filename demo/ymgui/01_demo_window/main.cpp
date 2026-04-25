/*
 * demo-ymgui-01-demo-window
 *
 * Runs inside a yetty session (`./yetty -e ./demo-ymgui-01-demo-window`).
 * Emits one ImGui frame per tick (default: one per 33 ms) as OSC vendor
 * 666680, which the ymgui terminal-layer picks up and draws anchored at
 * the cursor.
 *
 * The demo does no input handling — ImGuiIO.MousePos is pinned off-screen
 * so the demo window renders static. Input plumbing lives in a separate
 * followup.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "imgui.h"
#include "imgui_impl_yetty.h"

static void msleep(unsigned ms) {
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

int main(int argc, char **argv) {
    int frames = 300;       /* ~10 s at 30 fps */
    int fps    = 30;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            fps = atoi(argv[++i]);
        }
    }
    if (fps <= 0) fps = 30;
    unsigned dt_ms = 1000u / (unsigned)fps;

    /* Keep stderr off the PTY (yetty wires the child's stderr to the PTY;
     * letting logs through would overwrite the rendered demo). Send to
     * a file so we can inspect frontend traces. */
    (void)freopen("/tmp/ymgui-demo.log", "w", stderr);
    setvbuf(stderr, NULL, _IOLBF, 0);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = NULL;                  /* don't write imgui.ini */
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    io.DeltaTime   = (float)dt_ms / 1000.0f;
    io.MousePos    = ImVec2(-1.0f, -1.0f);  /* no hover */
    /* ImGui asserts on missing platform/backend names in some paths. */
    io.BackendPlatformName = "imgui_yetty_test_platform";

    if (!ImGui_ImplYetty_Init()) {
        fprintf(stderr, "ymgui: init failed\n");
        return 1;
    }

    for (int n = 0; n < frames; n++) {
        io.DeltaTime = (float)dt_ms / 1000.0f;
        ImGui_ImplYetty_NewFrame();
        ImGui::NewFrame();

        /* Override the demo window's hardcoded first-use position
         * (650, 20) — that puts it almost off-screen at small displays. */
        ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(
            ImVec2(io.DisplaySize.x - 40.0f, io.DisplaySize.y - 40.0f),
            ImGuiCond_Always);

        bool show = true;
        ImGui::ShowDemoWindow(&show);

        ImGui::Render();
        ImGui_ImplYetty_RenderDrawData(ImGui::GetDrawData());
        msleep(dt_ms);
    }

    ImGui_ImplYetty_Clear();
    ImGui_ImplYetty_Shutdown();
    ImGui::DestroyContext();
    return 0;
}
