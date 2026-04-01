#pragma once

#include <cstdint>

namespace yetty {

// TextCell structure for terminal grid (12 bytes)
// Wire format used by: ymux, ygrid card, gpu-screen
// The glyph field contains UTF codepoints on the wire,
// gpu-screen converts to font-specific indices internally.
struct TextCell {
  uint32_t glyph; // UTF codepoint (wire) or glyph index (internal)
  uint8_t fgR, fgG, fgB, alpha; // Foreground RGBA
  uint8_t bgR, bgG, bgB, style; // Background RGB + style byte
};
static_assert(sizeof(TextCell) == 12, "GridCell must be 12 bytes");

} // namespace yetty
