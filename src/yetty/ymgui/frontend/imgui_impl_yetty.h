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

#endif /* YETTY_YMGUI_IMGUI_IMPL_YETTY_H */
