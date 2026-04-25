/*
 * imgui_impl_yetty.h — Dear ImGui renderer backend that emits OSC to yetty.
 *
 * Usage (client side):
 *     ImGui::CreateContext();
 *     ImGui_ImplYetty_Init();                 // uploads font atlas
 *     // ... each frame ...
 *     ImGui_ImplYetty_NewFrame();
 *     ImGui::NewFrame();
 *     // ... your UI ...
 *     ImGui::Render();
 *     ImGui_ImplYetty_RenderDrawData(ImGui::GetDrawData());
 *     ImGui_ImplYetty_Shutdown();
 *
 * Output fd defaults to STDOUT_FILENO. Call ImGui_ImplYetty_SetOutputFd()
 * before Init to redirect.
 *
 * This file is the minimum C++ surface — the body lives in imgui_impl_yetty.cpp
 * which is a very thin wrapper over C helpers in ymgui_encode.c / ymgui_pack.c.
 *
 * This backend does NOT handle input. Pair with a platform backend that
 * decodes yetty's OSC mouse events (DEC 1500/1501) into ImGuiIO.
 */

#ifndef YETTY_YMGUI_IMGUI_IMPL_YETTY_H
#define YETTY_YMGUI_IMGUI_IMPL_YETTY_H

#include <stdint.h>
#include "imgui.h"

#ifndef IMGUI_IMPL_API
#define IMGUI_IMPL_API
#endif

IMGUI_IMPL_API bool ImGui_ImplYetty_Init(void);
IMGUI_IMPL_API void ImGui_ImplYetty_Shutdown(void);
IMGUI_IMPL_API void ImGui_ImplYetty_NewFrame(void);
IMGUI_IMPL_API void ImGui_ImplYetty_RenderDrawData(ImDrawData* draw_data);

/* Optional — call before Init. Default is STDOUT_FILENO. */
IMGUI_IMPL_API void ImGui_ImplYetty_SetOutputFd(int fd);

/* Optional — re-upload the font atlas (e.g. after AddFontFromFile at runtime). */
IMGUI_IMPL_API bool ImGui_ImplYetty_UploadFontAtlas(void);

/* Optional — emit --clear to wipe the receiver canvas. */
IMGUI_IMPL_API void ImGui_ImplYetty_Clear(void);

/*=============================================================================
 * Platform side — input/raw-mode wiring (Unix only for v1).
 *
 * PlatformInit:
 *   - puts stdin in raw (cbreak) mode
 *   - subscribes to yetty's pixel-precise mouse events: \e[?1500h \e[?1501h
 *   - stashes the previous termios for shutdown
 *
 * PollInput: drain stdin, parse OSC 777777 / 777778 / 777780 from yetty,
 * feed io.MousePos / MouseDown / MouseWheel / DisplaySize. Call once per
 * frame *before* ImGui::NewFrame().
 *
 * PlatformShutdown: \e[?1500l \e[?1501l, restore termios.
 *
 * Input fd defaults to STDIN_FILENO; override before PlatformInit.
 *===========================================================================*/
IMGUI_IMPL_API bool ImGui_ImplYetty_PlatformInit(void);
IMGUI_IMPL_API void ImGui_ImplYetty_PlatformShutdown(void);
IMGUI_IMPL_API void ImGui_ImplYetty_PollInput(void);
IMGUI_IMPL_API void ImGui_ImplYetty_SetInputFd(int fd);

/* Block on stdin until yetty pushes data OR `timeout_ms` elapses.
 * Drives an event-driven loop: 0 CPU at idle, wakes immediately on any
 * mouse / resize event from yetty. Returns true if input is available
 * (caller should follow with PollInput). timeout_ms < 0 → wait
 * indefinitely. */
IMGUI_IMPL_API bool ImGui_ImplYetty_WaitInput(int timeout_ms);

#endif /* YETTY_YMGUI_IMGUI_IMPL_YETTY_H */
