/*
 * demo-ymgui-01-demo-window
 *
 * Runs inside a yetty session (`./yetty -e ./demo-ymgui-01-demo-window`).
 * Drives Dear ImGui through the shared yetty_yclient event loop:
 *
 *   stdin → uv_poll → yface stream decode → typed callbacks → ImGuiIO
 *                                                       → frame_pending
 *   uv_check fires once per iteration → frame_cb → ImGui::NewFrame /
 *                                                  Render / RenderDrawData
 *
 * No frame timer, no fps cap. The loop runs entirely on input events:
 * idle = 0 CPU. Pass --frames to cap how many frames we render before
 * exiting (default: keep going).
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "imgui.h"
#include "imgui_impl_yetty.h"
#include <yetty/yclient-lib/event-loop.h>

struct demo_state {
    struct yetty_yclient_event_loop *loop;
    int   frames_rendered;
    int   frames_max;       /* <=0 = unbounded */
    uint64_t last_ns;

    /* Telemetry. */
    uint64_t ui_total_ns;
    uint64_t render_total_ns;
    int      sample_n;
};

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void on_frame(void *user)
{
    struct demo_state *S = (struct demo_state *)user;
    ImGuiIO& io = ImGui::GetIO();

    /* DeltaTime from wall-clock. ImGui needs > 0. */
    uint64_t now = now_ns();
    if (S->last_ns == 0) S->last_ns = now;
    uint64_t dt_ns = now - S->last_ns;
    S->last_ns = now;
    if (dt_ns == 0) dt_ns = 1;
    io.DeltaTime = (float)((double)dt_ns / 1e9);

    uint64_t t1 = now_ns();
    ImGui_ImplYetty_NewFrame();
    ImGui::NewFrame();

    /* Override the demo window's hardcoded first-use position so it
     * lands inside the viewport on small displays. */
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

    S->ui_total_ns     += (t2 - t1);
    S->render_total_ns += (t3 - t2);
    S->sample_n++;
    if (S->sample_n >= 30) {
        fprintf(stderr,
                "[demo] avg/iter: ui=%.2fms  render=%.2fms\n",
                (double)S->ui_total_ns     / S->sample_n / 1e6,
                (double)S->render_total_ns / S->sample_n / 1e6);
        S->ui_total_ns = S->render_total_ns = 0;
        S->sample_n = 0;
    }

    S->frames_rendered++;
    if (S->frames_max > 0 && S->frames_rendered >= S->frames_max)
        yetty_yclient_event_loop_stop(S->loop);
}

int main(int argc, char **argv)
{
    int frames_max = 0;     /* 0 = unbounded */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            frames_max = atoi(argv[++i]);
    }

    /* Keep stderr off the PTY — yetty wires the child's stderr to the
     * PTY, and trace lines would overwrite the rendered demo. Redirect
     * to a file the user can `tail -f`. */
    (void)freopen("/tmp/ymgui-demo.log", "w", stderr);
    setvbuf(stderr, NULL, _IOLBF, 0);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = NULL;
    /* Placeholder — the platform backend overwrites this as soon as
     * yetty's first OSC resize arrives. */
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    io.DeltaTime   = 1.0f / 60.0f;

    if (!ImGui_ImplYetty_Init()) {
        fprintf(stderr, "ymgui: init failed\n");
        return 1;
    }
    if (!ImGui_ImplYetty_PlatformInit()) {
        fprintf(stderr, "ymgui: platform init failed\n");
        ImGui_ImplYetty_Shutdown();
        return 1;
    }

    struct demo_state state = {};
    state.frames_max = frames_max;

    struct yetty_yclient_event_loop_config cfg = {};
    cfg.in_fd = -1;          /* default: STDIN_FILENO */
    cfg.user  = &state;
    state.loop = yetty_yclient_event_loop_create(&cfg);
    if (!state.loop) {
        fprintf(stderr, "ymgui: event loop create failed\n");
        ImGui_ImplYetty_PlatformShutdown();
        ImGui_ImplYetty_Shutdown();
        return 1;
    }

    /* Wire mouse/resize from yface → ImGuiIO. The bridge re-points the
     * loop's `user` to imgui's internal state, so we set our frame_cb
     * AFTER attach (frame_cb's user is the loop's `user`). */
    ImGui_ImplYetty_AttachEventLoop(state.loop);
    yetty_yclient_event_loop_set_user(state.loop, &state);
    yetty_yclient_event_loop_set_frame_cb(state.loop, on_frame);

    /* Block until yetty pushes its first OSC (resize on subscribe) or
     * the user moves the mouse / clicks. Idle is 0 CPU. */
    int rc = yetty_yclient_event_loop_run(state.loop);
    fprintf(stderr, "[demo] loop exited rc=%d frames=%d\n", rc, state.frames_rendered);

    yetty_yclient_event_loop_destroy(state.loop);
    ImGui_ImplYetty_Clear();
    ImGui_ImplYetty_PlatformShutdown();
    ImGui_ImplYetty_Shutdown();
    ImGui::DestroyContext();
    return 0;
}
