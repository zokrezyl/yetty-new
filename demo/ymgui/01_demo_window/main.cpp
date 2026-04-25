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
#include <stdint.h>
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
    int frames = 18000;     /* ~10 min at 30 fps — interactive runs */
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
    /* DisplaySize is overridden by the platform backend as soon as yetty
     * sends OSC 777780. The placeholder is just so the very first frame
     * (before any input arrives) doesn't blow up. */
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    io.DeltaTime   = (float)dt_ms / 1000.0f;

    if (!ImGui_ImplYetty_Init()) {
        fprintf(stderr, "ymgui: init failed\n");
        return 1;
    }
    if (!ImGui_ImplYetty_PlatformInit()) {
        fprintf(stderr, "ymgui: platform init failed\n");
        ImGui_ImplYetty_Shutdown();
        return 1;
    }

    /* Pure event-driven loop. Block on stdin (WaitInput) until yetty
     * pushes anything (mouse move, click, resize). When woken, drain
     * the events into ImGui, run one frame, emit unconditionally. No
     * msleep, no fps cap, no diff/hash — if we got woken, ImGui's
     * state changed (or could have), and an emit is the right answer.
     * Idle = poll() blocked on stdin = literally 0 CPU. */
    auto now_ns = []() {
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    };
    uint64_t poll_total = 0, ui_total = 0, render_total = 0;
    int sample_n = 0;

    for (int n = 0; n < frames; n++) {
        /* Block until yetty sends something. -1 = wait forever. */
        ImGui_ImplYetty_WaitInput(-1);

        io.DeltaTime = (float)dt_ms / 1000.0f;

        uint64_t t0 = now_ns();
        ImGui_ImplYetty_PollInput();
        uint64_t t1 = now_ns();
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

        uint64_t t2 = now_ns();
        ImGui::Render();
        ImGui_ImplYetty_RenderDrawData(ImGui::GetDrawData());
        uint64_t t3 = now_ns();

        poll_total   += (t1 - t0);
        ui_total     += (t2 - t1);
        render_total += (t3 - t2);
        sample_n++;
        if (sample_n >= 30) {
            fprintf(stderr,
                    "[demo] avg/iter: poll=%.2fms  ui=%.2fms  render=%.2fms\n",
                    (double)poll_total   / sample_n / 1e6,
                    (double)ui_total     / sample_n / 1e6,
                    (double)render_total / sample_n / 1e6);
            poll_total = ui_total = render_total = 0;
            sample_n = 0;
        }
    }

    ImGui_ImplYetty_Clear();
    ImGui_ImplYetty_PlatformShutdown();
    ImGui_ImplYetty_Shutdown();
    ImGui::DestroyContext();
    return 0;
}
