#include <yetty/term/terminal-screen.hpp>
#include <yetty/term/terminal-screen-context.hpp>
#include <yetty/term/renderable-layer.hpp>
#include <yetty/term/text-grid-layer.hpp>
#include <yetty/core/event.hpp>
#include <yetty/core/event-loop.hpp>
#include <yetty/config.hpp>
#include <yetty/font/font.hpp>
#include <yetty/font/raster-font.hpp>
#include <yetty/platform/pty.hpp>
#include <ytrace/ytrace.hpp>

#include <algorithm>
#include <cstring>
#include <deque>
#include <memory>
#include <vector>
#include <vterm.h>

namespace yetty {

// Wide char continuation marker
constexpr uint32_t GLYPH_WIDE_CONT = 0xFFFE;

// Scrollback line storage
struct ScrollbackLine {
  std::vector<VTermScreenCell> cells;
};

//=============================================================================
// GPU rendering state
//=============================================================================

struct TerminalScreenRenderState {
  // Borrowed from ShaderManager (do not release)
  WGPURenderPipeline pipeline = nullptr;
  WGPUBindGroupLayout bindGroupLayout = nullptr;
  WGPUBuffer vertexBuffer = nullptr;

  // Owned by us
  WGPUBindGroup bindGroup = nullptr;
  WGPUBuffer cellBuffer = nullptr;
  WGPUBuffer uniformBuffer = nullptr;
};

// Grid uniforms matching shader (272 bytes, 16-byte aligned)
struct GridUniforms {
  float projection[16];      // 64 bytes - mat4x4
  float screenSize[2];       // 8 bytes
  float cellSize[2];         // 8 bytes
  float gridSize[2];         // 8 bytes (cols, rows)
  float pixelRange;          // 4 bytes
  float scale;               // 4 bytes
  float cursorPos[2];        // 8 bytes (col, row)
  float cursorVisible;       // 4 bytes
  float cursorShape;         // 4 bytes (1=block, 2=underline, 3=bar)
  float viewportOrigin[2];   // 8 bytes
  float cursorBlink;         // 4 bytes
  float hasSelection;        // 4 bytes
  float selStart[2];         // 8 bytes
  float selEnd[2];           // 8 bytes
  uint32_t preEffectIndex;   // 4 bytes (unused)
  uint32_t postEffectIndex;  // 4 bytes (unused)
  float preEffectP0;         // 4 bytes
  float preEffectP1;         // 4 bytes
  float preEffectP2;         // 4 bytes
  float preEffectP3;         // 4 bytes
  float preEffectP4;         // 4 bytes
  float preEffectP5;         // 4 bytes
  float postEffectP0;        // 4 bytes
  float postEffectP1;        // 4 bytes
  float postEffectP2;        // 4 bytes
  float postEffectP3;        // 4 bytes
  float postEffectP4;        // 4 bytes
  float postEffectP5;        // 4 bytes
  uint32_t defaultFg;        // 4 bytes
  uint32_t defaultBg;        // 4 bytes
  uint32_t spaceGlyph;       // 4 bytes (unused)
  uint32_t effectIndex;      // 4 bytes (unused)
  float effectP0;            // 4 bytes
  float effectP1;            // 4 bytes
  float effectP2;            // 4 bytes
  float effectP3;            // 4 bytes
  float effectP4;            // 4 bytes
  float effectP5;            // 4 bytes
  float visualZoomScale;     // 4 bytes (unused)
  float visualZoomOffsetX;   // 4 bytes
  float visualZoomOffsetY;   // 4 bytes
  uint32_t ypaintScrollingSlot; // 4 bytes (unused)
  uint32_t ypaintOverlaySlot;   // 4 bytes (unused)
  float _pad0;               // 4 bytes
  float _pad1;               // 4 bytes
  float _pad2;               // 4 bytes
};
static_assert(sizeof(GridUniforms) == 272, "GridUniforms must be 272 bytes");

//=============================================================================
// TerminalScreenImpl
//=============================================================================

class TerminalScreenImpl : public TerminalScreen {
public:
  explicit TerminalScreenImpl(const TerminalContext &terminalContext) {
    // Build our own context from parent - COPY includes entire hierarchy
    _terminalScreenContext.terminalContext = terminalContext;
  }
  ~TerminalScreenImpl() override;

  const char *typeName() const override { return "TerminalScreen"; }

  Result<void> init(uint32_t cols, uint32_t rows);

  // TerminalScreen interface
  void write(const char *data, size_t len) override;
  void resize(uint32_t cols, uint32_t rows) override;
  uint32_t getCols() const override { return static_cast<uint32_t>(_cols); }
  uint32_t getRows() const override { return static_cast<uint32_t>(_rows); }
  float getCellWidth() const override { return _cellWidth; }
  float getCellHeight() const override { return _cellHeight; }
  bool hasDamage() const override { return _hasDamage; }
  void clearDamage() override { _hasDamage = false; }

  const VTermScreenCell *getCellData() const override;
  VTermScreenCell getCell(int row, int col) const override;
  VTermScreen *getScreen() const override { return _screen; }

  int getCursorRow() const override { return _cursorRow; }
  int getCursorCol() const override { return _cursorCol; }
  bool isCursorVisible() const override { return _cursorVisible; }
  int getCursorShape() const override { return _cursorShape; }
  bool isCursorBlink() const override { return _cursorBlink; }

  Font* getFont() const override { return _font; }

  void scrollUp(int lines) override;
  void scrollDown(int lines) override;
  void scrollToBottom() override;
  bool isScrolledBack() const override { return false; /* TODO */ }

  // Rendering
  Result<void> render(WGPURenderPassEncoder pass) override;

  // EventListener — called by EventLoop when PTY has data
  Result<bool> onEvent(const core::Event &event) override;

  // VTerm screen callbacks (static, called by libvterm screen layer)
  static int onDamage(VTermRect rect, void *user);
  static int onMoveCursor(VTermPos pos, VTermPos oldpos, int visible, void *user);
  static int onSetTermProp(VTermProp prop, VTermValue *val, void *user);
  static int onBell(void *user);
  static int onResize(int rows, int cols, void *user);
  static int onSbPushline(int cols, const VTermScreenCell *cells, void *user);
  static int onSbPopline(int cols, VTermScreenCell *cells, void *user);

  // Glyph resolver callback
  static VTermResolvedGlyph glyphResolver(const uint32_t *chars, int count,
                                          int bold, int italic, void *user);

private:
  size_t cellIndex(int row, int col) const {
    return static_cast<size_t>(row * _cols + col);
  }

  // VTerm - screen layer manages buffers directly
  VTerm *_vterm = nullptr;
  VTermScreen *_screen = nullptr;

  // Scrollback storage (viewing TODO)
  std::deque<ScrollbackLine> _scrollback;
  size_t _maxScrollback = 10000;

  // Screen state
  int _rows = 0;
  int _cols = 0;
  int _cursorRow = 0;
  int _cursorCol = 0;
  bool _cursorVisible = true;
  bool _cursorBlink = true;
  int _cursorShape = 1; // VTERM_PROP_CURSORSHAPE_BLOCK
  bool _hasDamage = false;
  bool _reflow = true;

  // Default colors for scrollback
  VTermColor _defaultFg;
  VTermColor _defaultBg;

  // Context - stores ONLY our level, access parent via _terminalScreenContext.terminalContext
  TerminalScreenContext _terminalScreenContext;

  // Cell size in pixels
  float _cellWidth = 10.0f;
  float _cellHeight = 20.0f;

  // Current surface size (updated on resize)
  uint32_t _surfaceWidth = 0;
  uint32_t _surfaceHeight = 0;

  // Font (owned by TerminalScreenImpl)
  Font* _font = nullptr;

  // Render layer (owned by TerminalScreenImpl)
  RenderableLayer* _renderableLayer = nullptr;

  // Render methods (implemented in render-terminal-screen.incl)
  Result<void> initRender();
  void cleanupRender();
  Result<void> renderFrame();
};

// Render implementation
#include "render-terminal-screen.incl"

//=============================================================================
// VTerm state callbacks struct
//=============================================================================

/* yetty: screen callbacks - putglyph/erase/etc now handled by screen layer */
static VTermScreenCallbacks screenCallbacks = {
    .damage = TerminalScreenImpl::onDamage,
    .moverect = nullptr,
    .movecursor = TerminalScreenImpl::onMoveCursor,
    .settermprop = TerminalScreenImpl::onSetTermProp,
    .bell = TerminalScreenImpl::onBell,
    .resize = TerminalScreenImpl::onResize,
    .sb_pushline = TerminalScreenImpl::onSbPushline,
    .sb_popline = TerminalScreenImpl::onSbPopline,
    .sb_clear = nullptr,
};

//=============================================================================
// Construction / destruction
//=============================================================================

TerminalScreenImpl::~TerminalScreenImpl() {
  cleanupRender();
  if (_font) {
    delete _font;
    _font = nullptr;
  }
  if (_vterm) {
    vterm_free(_vterm);
  } else {
    yerror("vterm is null");
  }
}

Result<void> TerminalScreenImpl::init(uint32_t cols, uint32_t rows) {
  _rows = static_cast<int>(rows);
  _cols = static_cast<int>(cols);

  vterm_color_rgb(&_defaultFg, 204, 204, 204);
  vterm_color_rgb(&_defaultBg, 15, 15, 35);

  _vterm = vterm_new(_rows, _cols);
  if (!_vterm) {
    return Err<void>("TerminalScreen: failed to create vterm");
  }

  vterm_set_utf8(_vterm, 1);

  // Screen layer with glyph resolver - screen manages buffers
  _screen = vterm_obtain_screen(_vterm, glyphResolver, this);
  vterm_screen_set_callbacks(_screen, &screenCallbacks, this);
  vterm_screen_enable_altscreen(_screen, 1);
  vterm_screen_enable_reflow(_screen, _reflow);


  vterm_screen_reset(_screen, 1);

  // Set up vterm output callback to write directly to PTY
  vterm_output_set_callback(
      _vterm,
      [](const char *data, size_t len, void *user) {
        auto *self = static_cast<TerminalScreenImpl *>(user);
        if (self->_terminalScreenContext.terminalContext.pty) {
          self->_terminalScreenContext.terminalContext.pty->write(data, len);
        }
      },
      this);

  // Set up PTY poll - receive shell output
  auto* eventLoop = _terminalScreenContext.terminalContext.eventLoop;
  auto* pty = _terminalScreenContext.terminalContext.pty;
  if (eventLoop && pty) {
    auto ptyPollResult = eventLoop->createPtyPoll(pty->pollSource());
    if (!ptyPollResult) {
      return Err<void>("Failed to create PTY poll", ptyPollResult);
    }
    auto ptyPollId = *ptyPollResult;

    if (auto res = eventLoop->registerPollListener(ptyPollId, this); !res) {
      return Err<void>("Failed to register PTY poll listener", res);
    }

    if (auto res = eventLoop->startPoll(ptyPollId); !res) {
      return Err<void>("Failed to start PTY poll", res);
    }
    ydebug("TerminalScreen: PTY poll started");
  }

  // Create font
  auto* config = _terminalScreenContext.terminalContext.yettyContext.appContext.config;
  std::string fontsDir = config->get<std::string>("paths/fonts", "");
  std::string fontName = config->get<std::string>("font/family", "default");
  if (fontName == "default") {
    fontName = "DejaVuSansMNerdFontMono";
  }
  auto fontResult = RasterFont::createImpl(fontsDir, fontName, _cellWidth, _cellHeight, false);
  if (!fontResult) {
    return Err<void>("Failed to create font", fontResult);
  }
  _font = *fontResult;

  auto loadResult = static_cast<RasterFont*>(_font)->loadBasicLatin();
  if (!loadResult) {
    return Err<void>("Failed to load basic latin glyphs", loadResult);
  }

  auto renderResult = initRender();
  if (!renderResult) {
    return renderResult;
  }

  return Ok();
}


//=============================================================================
// Write / resize
//=============================================================================

void TerminalScreenImpl::write(const char *data, size_t len) {
  if (_vterm && len > 0) {
    vterm_input_write(_vterm, data, len);
  }
}

void TerminalScreenImpl::resize(uint32_t cols, uint32_t rows) {
  if (_vterm) {
    vterm_set_size(_vterm, static_cast<int>(rows), static_cast<int>(cols));
  }
}


//=============================================================================
// Cell access — zero-copy GPU upload path
//=============================================================================

const VTermScreenCell *TerminalScreenImpl::getCellData() const {
  return vterm_screen_get_buffer(_screen);
}

VTermScreenCell TerminalScreenImpl::getCell(int row, int col) const {
  if (row < 0 || row >= _rows || col < 0 || col >= _cols) {
    return VTermScreenCell{};
  }
  const VTermScreenCell *data = getCellData();
  return data ? data[cellIndex(row, col)] : VTermScreenCell{};
}

//=============================================================================
// Scrollback navigation (TODO: implement viewing later)
//=============================================================================

void TerminalScreenImpl::scrollUp(int) {}
void TerminalScreenImpl::scrollDown(int) {}
void TerminalScreenImpl::scrollToBottom() {}

//=============================================================================
// VTerm screen callbacks - handled by libvterm screen layer
//=============================================================================

int TerminalScreenImpl::onDamage(VTermRect, void *user) {
  auto *self = static_cast<TerminalScreenImpl *>(user);
  self->_hasDamage = true;
  return 1;
}

int TerminalScreenImpl::onMoveCursor(VTermPos pos, VTermPos, int visible,
                                     void *user) {
  auto *self = static_cast<TerminalScreenImpl *>(user);
  self->_cursorRow = pos.row;
  self->_cursorCol = pos.col;
  self->_cursorVisible = visible != 0;
  self->_hasDamage = true;
  return 1;
}

int TerminalScreenImpl::onSetTermProp(VTermProp prop, VTermValue *val,
                                      void *user) {
  auto *self = static_cast<TerminalScreenImpl *>(user);

  if (prop == VTERM_PROP_CURSORVISIBLE) {
    self->_cursorVisible = val->boolean != 0;
  } else if (prop == VTERM_PROP_CURSORBLINK) {
    self->_cursorBlink = val->boolean != 0;
  } else if (prop == VTERM_PROP_CURSORSHAPE) {
    self->_cursorShape = val->number;
  }
  /* ALTSCREEN handled by screen layer */

  return 1;
}

int TerminalScreenImpl::onBell(void *) { return 1; }

int TerminalScreenImpl::onResize(int rows, int cols, void *user) {
  auto *self = static_cast<TerminalScreenImpl *>(user);
  self->_rows = rows;
  self->_cols = cols;
  self->_hasDamage = true;
  return 1;
}

int TerminalScreenImpl::onSbPushline(int cols, const VTermScreenCell *cells,
                                     void *user) {
  auto *self = static_cast<TerminalScreenImpl *>(user);

  ScrollbackLine line;
  line.cells.resize(cols);
  std::memcpy(line.cells.data(), cells, cols * sizeof(VTermScreenCell));

  self->_scrollback.push_back(std::move(line));

  while (self->_scrollback.size() > self->_maxScrollback) {
    self->_scrollback.pop_front();
  }

  return 1;
}

int TerminalScreenImpl::onSbPopline(int cols, VTermScreenCell *cells,
                                    void *user) {
  auto *self = static_cast<TerminalScreenImpl *>(user);

  if (self->_scrollback.empty())
    return 0;

  ScrollbackLine &line = self->_scrollback.back();
  int copyCount = std::min(cols, static_cast<int>(line.cells.size()));
  std::memcpy(cells, line.cells.data(), copyCount * sizeof(VTermScreenCell));

  // Fill remainder with empty cells if scrollback line shorter than cols
  if (copyCount < cols) {
    VTermScreenCell empty{};
    empty.glyph_index = 0;
    empty.fg = self->_defaultFg;
    empty.bg = self->_defaultBg;
    for (int i = copyCount; i < cols; i++) {
      cells[i] = empty;
    }
  }

  self->_scrollback.pop_back();
  return 1;
}

VTermResolvedGlyph TerminalScreenImpl::glyphResolver(const uint32_t *chars,
                                                     int count, int bold,
                                                     int italic, void *user) {
  auto *self = static_cast<TerminalScreenImpl *>(user);

  VTermResolvedGlyph result;

  if (!self->_font) {
    result.glyph_index = 0xFFFFFFFF;
    result.font_type = 0;
    return result;
  }

  uint32_t codepoint = (count > 0 && chars[0]) ? chars[0] : ' ';
  result.glyph_index = self->_font->getGlyphIndex(codepoint, bold, italic);
  result.font_type = static_cast<uint8_t>(self->_font->renderMethod());

  return result;
}

//=============================================================================
// Factory
//=============================================================================

// Convert GLFW modifier flags to VTerm modifier flags
static VTermModifier glfwModsToVterm(int mods) {
  VTermModifier vtMod = VTERM_MOD_NONE;
  if (mods & 0x0001) vtMod = static_cast<VTermModifier>(vtMod | VTERM_MOD_SHIFT);
  if (mods & 0x0002) vtMod = static_cast<VTermModifier>(vtMod | VTERM_MOD_CTRL);
  if (mods & 0x0004) vtMod = static_cast<VTermModifier>(vtMod | VTERM_MOD_ALT);
  return vtMod;
}

// Convert GLFW key code to VTerm key (for special keys)
// Returns VTERM_KEY_NONE if it's a printable character (handled by Char)
static VTermKey glfwKeyToVterm(int key) {
  switch (key) {
    case 257: return VTERM_KEY_ENTER;      // GLFW_KEY_ENTER
    case 258: return VTERM_KEY_TAB;        // GLFW_KEY_TAB
    case 259: return VTERM_KEY_BACKSPACE;  // GLFW_KEY_BACKSPACE
    case 260: return VTERM_KEY_INS;        // GLFW_KEY_INSERT
    case 261: return VTERM_KEY_DEL;        // GLFW_KEY_DELETE
    case 262: return VTERM_KEY_RIGHT;      // GLFW_KEY_RIGHT
    case 263: return VTERM_KEY_LEFT;       // GLFW_KEY_LEFT
    case 264: return VTERM_KEY_DOWN;       // GLFW_KEY_DOWN
    case 265: return VTERM_KEY_UP;         // GLFW_KEY_UP
    case 266: return VTERM_KEY_PAGEUP;     // GLFW_KEY_PAGE_UP
    case 267: return VTERM_KEY_PAGEDOWN;   // GLFW_KEY_PAGE_DOWN
    case 268: return VTERM_KEY_HOME;       // GLFW_KEY_HOME
    case 269: return VTERM_KEY_END;        // GLFW_KEY_END
    case 256: return VTERM_KEY_ESCAPE;     // GLFW_KEY_ESCAPE
    case 290: return static_cast<VTermKey>(VTERM_KEY_FUNCTION_0 + 1);  // GLFW_KEY_F1
    case 291: return static_cast<VTermKey>(VTERM_KEY_FUNCTION_0 + 2);
    case 292: return static_cast<VTermKey>(VTERM_KEY_FUNCTION_0 + 3);
    case 293: return static_cast<VTermKey>(VTERM_KEY_FUNCTION_0 + 4);
    case 294: return static_cast<VTermKey>(VTERM_KEY_FUNCTION_0 + 5);
    case 295: return static_cast<VTermKey>(VTERM_KEY_FUNCTION_0 + 6);
    case 296: return static_cast<VTermKey>(VTERM_KEY_FUNCTION_0 + 7);
    case 297: return static_cast<VTermKey>(VTERM_KEY_FUNCTION_0 + 8);
    case 298: return static_cast<VTermKey>(VTERM_KEY_FUNCTION_0 + 9);
    case 299: return static_cast<VTermKey>(VTERM_KEY_FUNCTION_0 + 10);
    case 300: return static_cast<VTermKey>(VTERM_KEY_FUNCTION_0 + 11);
    case 301: return static_cast<VTermKey>(VTERM_KEY_FUNCTION_0 + 12);  // GLFW_KEY_F12
    default: return VTERM_KEY_NONE;
  }
}

Result<bool> TerminalScreenImpl::onEvent(const core::Event &event) {
  // Character input (printable characters)
  if (event.type == core::Event::Type::Char) {
    if (_vterm) {
      VTermModifier mod = glfwModsToVterm(event.chr.mods);
      vterm_keyboard_unichar(_vterm, event.chr.codepoint, mod);
      ydebug("TerminalScreen: Char codepoint={} mods={}", event.chr.codepoint, event.chr.mods);
    }
    return Ok(true);
  }

  // Key down - handle special keys (arrows, function keys, etc.)
  if (event.type == core::Event::Type::KeyDown) {
    if (_vterm) {
      VTermKey vtKey = glfwKeyToVterm(event.key.key);
      if (vtKey != VTERM_KEY_NONE) {
        VTermModifier mod = glfwModsToVterm(event.key.mods);
        vterm_keyboard_key(_vterm, vtKey, mod);
        ydebug("TerminalScreen: KeyDown key={} vtKey={} mods={}", event.key.key, static_cast<int>(vtKey), event.key.mods);
        return Ok(true);
      }
    }
    return Ok(false);  // Not a special key, let CharInput handle it
  }

  // Render event - do actual GPU frame rendering
  if (event.type == core::Event::Type::Render) {
    ydebug("TerminalScreen::onEvent Render - calling renderFrame");
    if (auto res = renderFrame(); !res) {
      return Err<bool>("renderFrame failed", res);
    }
    return Ok(true);
  }

  // PTY readable - read shell output and feed to vterm
  if (event.type == core::Event::Type::PollReadable) {
    auto* pty = _terminalScreenContext.terminalContext.pty;
    if (pty && _vterm) {
      char buf[4096];
      size_t n;
      while ((n = pty->read(buf, sizeof(buf))) > 0) {
        vterm_input_write(_vterm, buf, n);
      }
      // Request render after processing PTY output
      if (_hasDamage) {
        _terminalScreenContext.terminalContext.eventLoop->requestRender();
      }
    }
    return Ok(true);
  }

  // Resize - reconfigure surface and resize terminal grid/PTY
  if (event.type == core::Event::Type::Resize) {
    if (event.resize.width <= 0 || event.resize.height <= 0) {
      yerror("TerminalScreen: resize with zero dimensions {}x{}", event.resize.width, event.resize.height);
      return Err<bool>("Resize with zero dimensions");
    }
    if (!_terminalScreenContext.terminalContext.yettyContext.yettyGpuContext.appGpuContext.surface) {
      yerror("TerminalScreen: no surface for resize");
      return Err<bool>("No surface for resize");
    }
    if (!_terminalScreenContext.terminalContext.yettyContext.yettyGpuContext.device) {
      yerror("TerminalScreen: no device for resize");
      return Err<bool>("No device for resize");
    }

    // Reconfigure surface
    WGPUSurfaceConfiguration surfaceConfig = {};
    surfaceConfig.device = _terminalScreenContext.terminalContext.yettyContext.yettyGpuContext.device;
    surfaceConfig.format = _terminalScreenContext.terminalContext.yettyContext.yettyGpuContext.surfaceFormat;
    surfaceConfig.usage = WGPUTextureUsage_RenderAttachment;
    surfaceConfig.width = static_cast<uint32_t>(event.resize.width);
    surfaceConfig.height = static_cast<uint32_t>(event.resize.height);
    surfaceConfig.presentMode = WGPUPresentMode_Fifo;
    wgpuSurfaceConfigure(_terminalScreenContext.terminalContext.yettyContext.yettyGpuContext.appGpuContext.surface, &surfaceConfig);
    _surfaceWidth = surfaceConfig.width;
    _surfaceHeight = surfaceConfig.height;
    ydebug("TerminalScreen: surface {}x{}", _surfaceWidth, _surfaceHeight);

    // Resize grid and PTY
    uint32_t cols = static_cast<uint32_t>(event.resize.width / _cellWidth);
    uint32_t rows = static_cast<uint32_t>(event.resize.height / _cellHeight);
    if (cols == 0 || rows == 0) {
      yerror("TerminalScreen: resize resulted in zero grid {}x{}", cols, rows);
      return Err<bool>("Resize resulted in zero grid dimensions");
    }
    if (cols != static_cast<uint32_t>(_cols) || rows != static_cast<uint32_t>(_rows)) {
      resize(cols, rows);
      if (_terminalScreenContext.terminalContext.pty) {
        _terminalScreenContext.terminalContext.pty->resize(cols, rows);
      } else {
        yerror("TerminalScreen: no PTY for resize");
      }
      ydebug("TerminalScreen: grid {}x{}", cols, rows);
    }
    _terminalScreenContext.terminalContext.eventLoop->requestRender();
    return Ok(true);
  }

  // Events we don't handle but shouldn't error on
  if (event.type == core::Event::Type::KeyUp ||
      event.type == core::Event::Type::MouseDown ||
      event.type == core::Event::Type::MouseUp ||
      event.type == core::Event::Type::MouseMove ||
      event.type == core::Event::Type::MouseDrag ||
      event.type == core::Event::Type::Scroll ||
      event.type == core::Event::Type::SetFocus) {
    return Ok(false);
  }

  yerror("TerminalScreen: unhandled event type {}", static_cast<int>(event.type));
  return Err<bool>("TerminalScreen: unhandled event type");
}

Result<TerminalScreen *> TerminalScreen::createImpl(uint32_t cols,
                                                    uint32_t rows,
                                                    const TerminalContext &terminalContext) {
  auto *screen = new TerminalScreenImpl(terminalContext);
  if (auto res = screen->init(cols, rows); !res) {
    delete screen;
    return Err<TerminalScreen *>("TerminalScreen init failed", res);
  }
  return Ok(static_cast<TerminalScreen *>(screen));
}

} // namespace yetty
