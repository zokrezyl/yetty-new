/*
 * imgui_impl_yetty.h — Dear ImGui renderer + platform backend for yetty.
 *
 * Renderer (no input):
 *
 *     ImGui::CreateContext();
 *     ImGui_ImplYetty_Init();              // uploads font atlas
 *     // each frame:
 *     ImGui_ImplYetty_NewFrame();
 *     ImGui::NewFrame();
 *     // … your UI …
 *     ImGui::Render();
 *     ImGui_ImplYetty_RenderDrawData(ImGui::GetDrawData());
 *     ImGui_ImplYetty_Shutdown();
 *
 * Output goes to STDOUT_FILENO by default (redirect with SetOutputFd
 * before Init).
 *
 * Platform / input — three options, pick one:
 *
 *   (1) Manage your own loop: call PlatformInit, then PollInput / WaitInput
 *       once per frame. Same shape as the original sync API.
 *
 *   (2) Push events explicitly: implement your own loop and call
 *       ImGui_ImplYetty_OnMousePos / OnMouseButton / OnMouseWheel /
 *       OnResize from wherever the events come from. The renderer is
 *       independent of input plumbing.
 *
 *   (3) Async with libuv: create a `yetty_yclient_event_loop`
 *       (<yetty/yclient-lib/event-loop.h>), then call
 *       ImGui_ImplYetty_AttachEventLoop(loop) — wires the loop's
 *       per-event callbacks into ImGuiIO so the app's only job is to
 *       set a frame_cb that does NewFrame → Render → RenderDrawData.
 *       App code can also register its own fds / timers / posted tasks
 *       on the same loop for network etc. The loop is shared with
 *       ygui / yrich / ycat — same primitive across all client tools.
 */

#ifndef YETTY_YMGUI_IMGUI_IMPL_YETTY_H
#define YETTY_YMGUI_IMGUI_IMPL_YETTY_H

#include <stdint.h>
#include "imgui.h"

#ifndef IMGUI_IMPL_API
#define IMGUI_IMPL_API
#endif

struct yetty_yclient_event_loop;

IMGUI_IMPL_API bool ImGui_ImplYetty_Init(void);
IMGUI_IMPL_API void ImGui_ImplYetty_Shutdown(void);
IMGUI_IMPL_API void ImGui_ImplYetty_NewFrame(void);
IMGUI_IMPL_API void ImGui_ImplYetty_RenderDrawData(ImDrawData* draw_data);

/* Optional — call before Init. Default is STDOUT_FILENO. */
IMGUI_IMPL_API void ImGui_ImplYetty_SetOutputFd(int fd);

/* Optional — re-upload the font atlas (e.g. after AddFontFromFile at runtime). */
IMGUI_IMPL_API bool ImGui_ImplYetty_UploadFontAtlas(void);

/* Optional — emit clear to wipe the receiver canvas. */
IMGUI_IMPL_API void ImGui_ImplYetty_Clear(void);

/*=============================================================================
 * Platform side — raw mode + DEC ?1500/?1501 subscription
 *
 * PlatformInit:
 *   - puts stdin in raw (cbreak) mode
 *   - subscribes to yetty's pixel-precise mouse events: \e[?1500h \e[?1501h
 *   - stashes the previous termios for shutdown
 *
 * PlatformShutdown: \e[?1500l \e[?1501l, restore termios.
 *
 * Input fd defaults to STDIN_FILENO; override with SetInputFd before Init.
 *===========================================================================*/
IMGUI_IMPL_API bool ImGui_ImplYetty_PlatformInit(void);
IMGUI_IMPL_API void ImGui_ImplYetty_PlatformShutdown(void);
IMGUI_IMPL_API void ImGui_ImplYetty_SetInputFd(int fd);

/*=============================================================================
 * Sync input (option 1)
 *
 * PollInput drains stdin through yface and routes decoded OSC events
 * into ImGuiIO. Call once per frame, before ImGui::NewFrame().
 *
 * WaitInput blocks until either input arrives on the input fd or
 * timeout_ms elapses; pair with PollInput in a sleep-then-drain loop
 * for a 0-CPU idle.
 *===========================================================================*/
IMGUI_IMPL_API void ImGui_ImplYetty_PollInput(void);
IMGUI_IMPL_API bool ImGui_ImplYetty_WaitInput(int timeout_ms);

/*=============================================================================
 * Push input (option 2) — call from anywhere to feed ImGuiIO
 *
 * GLFW-style: each function pushes one event. Same routing the sync
 * PollInput uses internally — safe to mix with manual polling if you want.
 *===========================================================================*/
IMGUI_IMPL_API void ImGui_ImplYetty_OnMousePos   (double x, double y,
                                                  uint32_t buttons_held);
IMGUI_IMPL_API void ImGui_ImplYetty_OnMouseButton(int button, int pressed,
                                                  double x, double y);
IMGUI_IMPL_API void ImGui_ImplYetty_OnMouseWheel (double dy,
                                                  double x, double y);
IMGUI_IMPL_API void ImGui_ImplYetty_OnResize     (double width, double height);

/*=============================================================================
 * Async with libuv (option 3) — preferred
 *
 * Wires the loop's input callbacks into the ImGui_ImplYetty_On* hooks above
 * and sets the loop's `user` pointer to point at this backend's state. The
 * app's frame_cb is the only thing the app still owns — it does
 * NewFrame → ImGui::NewFrame → render UI → ImGui::Render → RenderDrawData.
 *===========================================================================*/
IMGUI_IMPL_API void ImGui_ImplYetty_AttachEventLoop(struct yetty_yclient_event_loop *loop);

#endif /* YETTY_YMGUI_IMGUI_IMPL_YETTY_H */
