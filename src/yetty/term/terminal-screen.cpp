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
  std::vector<TextCell> cells;
};

// Pen state for current text attributes
struct Pen {
  VTermColor fg;
  VTermColor bg;
  bool bold = false;
  bool italic = false;
  uint8_t underline = 0;
  bool strike = false;
  bool reverse = false;
  bool blink = false;
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

  const TextCell *getCellData() const override;
  TextCell getCell(int row, int col) const override;

  int getCursorRow() const override { return _cursorRow; }
  int getCursorCol() const override { return _cursorCol; }
  bool isCursorVisible() const override { return _cursorVisible; }
  int getCursorShape() const override { return _cursorShape; }
  bool isCursorBlink() const override { return _cursorBlink; }

  Font* getFont() const override { return _font; }

  void scrollUp(int lines) override;
  void scrollDown(int lines) override;
  void scrollToBottom() override;
  bool isScrolledBack() const override { return _scrollOffset > 0; }

  // Rendering
  void render(WGPURenderPassEncoder pass) override;

  // EventListener — called by EventLoop when PTY has data
  Result<bool> onEvent(const core::Event &event) override;

  // VTerm state callbacks (static, called by libvterm)
  static int onPutglyph(VTermGlyphInfo *info, VTermPos pos, void *user);
  static int onMoveCursor(VTermPos pos, VTermPos oldpos, int visible,
                          void *user);
  static int onScrollRect(VTermRect rect, int downward, int rightward,
                          void *user);
  static int onMoveRect(VTermRect dest, VTermRect src, void *user);
  static int onErase(VTermRect rect, int selective, void *user);
  static int onInitPen(void *user);
  static int onSetPenAttr(VTermAttr attr, VTermValue *val, void *user);
  static int onSetTermProp(VTermProp prop, VTermValue *val, void *user);
  static int onBell(void *user);
  static int onResize(int rows, int cols, VTermStateFields *fields, void *user);
  static int onSetLineInfo(int row, const VTermLineInfo *newinfo,
                           const VTermLineInfo *oldinfo, void *user);

private:
  void createVTerm(uint32_t cols, uint32_t rows);
  void attach(VTerm *vt);

  void setCell(int row, int col, uint32_t glyph, uint8_t fgR, uint8_t fgG,
               uint8_t fgB, uint8_t bgR, uint8_t bgG, uint8_t bgB,
               uint8_t style);

  void colorToRGB(const VTermColor &color, uint8_t &r, uint8_t &g, uint8_t &b);

  size_t cellIndex(int row, int col) const {
    return static_cast<size_t>(row * _cols + col);
  }

  void clearBuffer(std::vector<TextCell> &buffer);
  void switchToScreen(bool alt);
  void resizeInternal(int rows, int cols, VTermStateFields *fields);

  void composeViewBuffer();
  void decompressLine(const ScrollbackLine &line, int viewRow);
  void pushLineToScrollback(int row);

  // VTerm
  VTerm *_vterm = nullptr;
  VTermState *_state = nullptr;

  // Flat contiguous buffers — GPU-ready via getCellData()
  std::vector<TextCell> _primaryBuffer;
  std::vector<TextCell> _altBuffer;
  std::vector<TextCell> _viewBuffer;
  std::vector<TextCell> _scratchBuffer;
  std::vector<TextCell> *_visibleBuffer = nullptr;

  // Scrollback
  std::deque<ScrollbackLine> _scrollback;
  size_t _maxScrollback = 10000;
  int _scrollOffset = 0;
  bool _viewBufferDirty = false;

  // Screen state
  int _rows = 0;
  int _cols = 0;
  int _cursorRow = 0;
  int _cursorCol = 0;
  bool _cursorVisible = true;
  bool _cursorBlink = true;
  int _cursorShape = 1; // VTERM_PROP_CURSORSHAPE_BLOCK
  bool _isAltScreen = false;
  bool _hasDamage = false;

  // Pen state
  Pen _pen;
  VTermColor _defaultFg;
  VTermColor _defaultBg;

  // Context - stores ONLY our level, access parent via _terminalScreenContext.terminalContext
  TerminalScreenContext _terminalScreenContext;

  // Cell size in pixels
  float _cellWidth = 10.0f;
  float _cellHeight = 20.0f;

  // Font (owned by TerminalScreenImpl)
  Font* _font = nullptr;

  // Render layer (owned by TerminalScreenImpl)
  RenderableLayer* _renderableLayer = nullptr;

  // Render methods (implemented in render-terminal-screen.incl)
  Result<void> initRender();
  void cleanupRender();
  void renderFrame();
};

// Render implementation
#include "render-terminal-screen.incl"

//=============================================================================
// VTerm state callbacks struct
//=============================================================================

static VTermStateCallbacks stateCallbacks = {
    .putglyph = TerminalScreenImpl::onPutglyph,
    .movecursor = TerminalScreenImpl::onMoveCursor,
    .scrollrect = TerminalScreenImpl::onScrollRect,
    .moverect = TerminalScreenImpl::onMoveRect,
    .erase = TerminalScreenImpl::onErase,
    .initpen = TerminalScreenImpl::onInitPen,
    .setpenattr = TerminalScreenImpl::onSetPenAttr,
    .settermprop = TerminalScreenImpl::onSetTermProp,
    .bell = TerminalScreenImpl::onBell,
    .resize = TerminalScreenImpl::onResize,
    .setlineinfo = TerminalScreenImpl::onSetLineInfo,
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
  vterm_color_rgb(&_defaultFg, 204, 204, 204);
  vterm_color_rgb(&_defaultBg, 15, 15, 35);
  _pen.fg = _defaultFg;
  _pen.bg = _defaultBg;
  _isAltScreen = false;

  createVTerm(cols, rows);
  if (!_vterm) {
    return Err<void>("TerminalScreen: failed to create vterm");
  }

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

void TerminalScreenImpl::createVTerm(uint32_t cols, uint32_t rows) {
  if (_vterm)
    return;

  _rows = static_cast<int>(rows);
  _cols = static_cast<int>(cols);

  // Allocate buffers
  size_t numCells = static_cast<size_t>(_rows * _cols);
  _primaryBuffer.resize(numCells);
  clearBuffer(_primaryBuffer);
  _viewBuffer.resize(numCells);
  _scratchBuffer.resize(_cols);
  _visibleBuffer = &_primaryBuffer;

  _vterm = vterm_new(_rows, _cols);
  if (!_vterm)
    return;

  vterm_set_utf8(_vterm, 1);
  attach(_vterm);
}

void TerminalScreenImpl::attach(VTerm *vt) {
  _vterm = vt;
  _state = vterm_obtain_state(vt);

  vterm_state_set_callbacks(_state, &stateCallbacks, this);

  vterm_state_get_default_colors(_state, &_defaultFg, &_defaultBg);

  // Convert indexed default colors to RGB
  if (VTERM_COLOR_IS_INDEXED(&_defaultFg)) {
    vterm_state_convert_color_to_rgb(_state, &_defaultFg);
  }
  if (VTERM_COLOR_IS_INDEXED(&_defaultBg)) {
    vterm_state_convert_color_to_rgb(_state, &_defaultBg);
  }

  _pen.fg = _defaultFg;
  _pen.bg = _defaultBg;

  vterm_state_reset(_state, 1);
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
  } else {
    resizeInternal(static_cast<int>(rows), static_cast<int>(cols), nullptr);
  }
}

void TerminalScreenImpl::resizeInternal(int rows, int cols,
                                        VTermStateFields *fields) {
  if (rows == _rows && cols == _cols && !_primaryBuffer.empty())
    return;

  bool altActive = _isAltScreen;

  // TODO: reflow logic from old resizeBuffer — for now simple resize
  size_t newSize = static_cast<size_t>(rows * cols);
  _primaryBuffer.resize(newSize);
  clearBuffer(_primaryBuffer);

  if (!_altBuffer.empty()) {
    _altBuffer.resize(newSize);
    clearBuffer(_altBuffer);
  }

  // Handle lineinfo resize for vterm
  if (fields) {
    for (int i = 0; i < 2; i++) {
      if (fields->lineinfos[i]) {
        free(fields->lineinfos[i]);
        fields->lineinfos[i] = static_cast<VTermLineInfo *>(
            calloc(static_cast<size_t>(rows), sizeof(VTermLineInfo)));
      }
    }
  }

  _rows = rows;
  _cols = cols;

  _viewBuffer.resize(newSize);
  _scratchBuffer.resize(cols);
  _visibleBuffer = altActive ? &_altBuffer : &_primaryBuffer;

  if (!altActive) {
    _scrollOffset = 0;
  }

  _hasDamage = true;
  _viewBufferDirty = true;
}

//=============================================================================
// Cell access — zero-copy GPU upload path
//=============================================================================

const TextCell *TerminalScreenImpl::getCellData() const {
  if (_scrollOffset > 0) {
    const_cast<TerminalScreenImpl *>(this)->composeViewBuffer();
    return _viewBuffer.data();
  }
  if (_visibleBuffer) {
    return _visibleBuffer->data();
  }
  return nullptr;
}

TextCell TerminalScreenImpl::getCell(int row, int col) const {
  if (row < 0 || row >= _rows || col < 0 || col >= _cols) {
    return TextCell{};
  }
  const TextCell *data = getCellData();
  return data ? data[cellIndex(row, col)] : TextCell{};
}

//=============================================================================
// Scrollback navigation
//=============================================================================

void TerminalScreenImpl::scrollUp(int lines) {
  int maxOffset = static_cast<int>(_scrollback.size());
  int newOffset = std::min(_scrollOffset + lines, maxOffset);
  if (newOffset != _scrollOffset) {
    _scrollOffset = newOffset;
    _viewBufferDirty = true;
    _hasDamage = true;
  }
}

void TerminalScreenImpl::scrollDown(int lines) {
  int newOffset = std::max(_scrollOffset - lines, 0);
  if (newOffset != _scrollOffset) {
    _scrollOffset = newOffset;
    _viewBufferDirty = true;
    _hasDamage = true;
  }
}

void TerminalScreenImpl::scrollToBottom() {
  if (_scrollOffset != 0) {
    _scrollOffset = 0;
    _viewBufferDirty = true;
    _hasDamage = true;
  }
}

//=============================================================================
// View buffer composition (when scrolled back)
//=============================================================================

void TerminalScreenImpl::composeViewBuffer() {
  if (!_viewBufferDirty)
    return;

  int sbSize = static_cast<int>(_scrollback.size());
  int sbLinesToShow = std::min(_scrollOffset, _rows);

  // Fill top rows from scrollback
  for (int viewRow = 0; viewRow < sbLinesToShow; viewRow++) {
    int sbIndex = sbSize - _scrollOffset + viewRow;
    if (sbIndex >= 0 && sbIndex < sbSize) {
      decompressLine(_scrollback[sbIndex], viewRow);
    }
  }

  // Fill bottom rows from visible buffer
  int visibleStart = sbLinesToShow;
  int visibleLines = _rows - sbLinesToShow;
  if (visibleLines > 0 && _visibleBuffer) {
    size_t dstOffset = static_cast<size_t>(visibleStart * _cols);
    std::memcpy(&_viewBuffer[dstOffset], _visibleBuffer->data(),
                static_cast<size_t>(visibleLines * _cols) * sizeof(TextCell));
  }

  _viewBufferDirty = false;
}

void TerminalScreenImpl::decompressLine(const ScrollbackLine &line,
                                        int viewRow) {
  size_t dstOffset = static_cast<size_t>(viewRow * _cols);
  int lineCols = std::min(static_cast<int>(line.cells.size()), _cols);

  if (lineCols > 0) {
    std::memcpy(&_viewBuffer[dstOffset], line.cells.data(),
                static_cast<size_t>(lineCols) * sizeof(TextCell));
  }

  // Fill remainder with default cells
  TextCell defaultCell{};
  defaultCell.glyph = ' ';
  defaultCell.fgR = _defaultFg.rgb.red;
  defaultCell.fgG = _defaultFg.rgb.green;
  defaultCell.fgB = _defaultFg.rgb.blue;
  defaultCell.bgR = _defaultBg.rgb.red;
  defaultCell.bgG = _defaultBg.rgb.green;
  defaultCell.bgB = _defaultBg.rgb.blue;
  defaultCell.alpha = 255;
  defaultCell.style = 0;

  for (int col = lineCols; col < _cols; col++) {
    _viewBuffer[dstOffset + col] = defaultCell;
  }
}

void TerminalScreenImpl::pushLineToScrollback(int row) {
  if (!_visibleBuffer)
    return;

  ScrollbackLine line;
  line.cells.resize(_cols);
  size_t srcOffset = static_cast<size_t>(row * _cols);
  std::memcpy(line.cells.data(), &(*_visibleBuffer)[srcOffset],
              _cols * sizeof(TextCell));

  _scrollback.push_back(std::move(line));

  if (_scrollOffset > 0) {
    _scrollOffset++;
  }

  while (_scrollback.size() > _maxScrollback) {
    _scrollback.pop_front();
    if (_scrollOffset > static_cast<int>(_scrollback.size())) {
      _scrollOffset = static_cast<int>(_scrollback.size());
    }
  }
}

//=============================================================================
// Buffer helpers
//=============================================================================

void TerminalScreenImpl::clearBuffer(std::vector<TextCell> &buffer) {
  uint8_t fgR, fgG, fgB, bgR, bgG, bgB;
  colorToRGB(_defaultFg, fgR, fgG, fgB);
  colorToRGB(_defaultBg, bgR, bgG, bgB);

  TextCell defaultCell{};
  defaultCell.glyph = ' ';
  defaultCell.fgR = fgR;
  defaultCell.fgG = fgG;
  defaultCell.fgB = fgB;
  defaultCell.bgR = bgR;
  defaultCell.bgG = bgG;
  defaultCell.bgB = bgB;
  defaultCell.alpha = 255;
  defaultCell.style = 0;

  std::fill(buffer.begin(), buffer.end(), defaultCell);
}

void TerminalScreenImpl::switchToScreen(bool alt) {
  if (_isAltScreen == alt)
    return;

  _isAltScreen = alt;

  if (alt) {
    if (_altBuffer.empty()) {
      _altBuffer.resize(static_cast<size_t>(_rows * _cols));
    }
    _visibleBuffer = &_altBuffer;
    clearBuffer(_altBuffer);
    _scrollOffset = 0;
  } else {
    _visibleBuffer = &_primaryBuffer;
  }

  _hasDamage = true;
  _viewBufferDirty = true;
}

//=============================================================================
// Cell manipulation
//=============================================================================

void TerminalScreenImpl::setCell(int row, int col, uint32_t glyph, uint8_t fgR,
                                 uint8_t fgG, uint8_t fgB, uint8_t bgR,
                                 uint8_t bgG, uint8_t bgB, uint8_t style) {
  if (row < 0 || row >= _rows || col < 0 || col >= _cols)
    return;
  if (!_visibleBuffer)
    return;

  size_t idx = cellIndex(row, col);
  if (idx >= _visibleBuffer->size())
    return;

  TextCell &cell = (*_visibleBuffer)[idx];
  if (_font && glyph != GLYPH_WIDE_CONT) {
    cell.glyph = _font->getGlyphIndex(glyph, (style & 0x01) != 0, (style & 0x02) != 0);
  } else {
    cell.glyph = glyph;
  }
  cell.fgR = fgR;
  cell.fgG = fgG;
  cell.fgB = fgB;
  cell.bgR = bgR;
  cell.bgG = bgG;
  cell.bgB = bgB;
  cell.alpha = 255;
  cell.style = style;
}

void TerminalScreenImpl::colorToRGB(const VTermColor &color, uint8_t &r,
                                    uint8_t &g, uint8_t &b) {
  if (VTERM_COLOR_IS_DEFAULT_FG(&color)) {
    r = _defaultFg.rgb.red;
    g = _defaultFg.rgb.green;
    b = _defaultFg.rgb.blue;
  } else if (VTERM_COLOR_IS_DEFAULT_BG(&color)) {
    r = _defaultBg.rgb.red;
    g = _defaultBg.rgb.green;
    b = _defaultBg.rgb.blue;
  } else if (VTERM_COLOR_IS_INDEXED(&color)) {
    VTermColor rgb = color;
    if (_state) {
      vterm_state_convert_color_to_rgb(_state, &rgb);
    }
    r = rgb.rgb.red;
    g = rgb.rgb.green;
    b = rgb.rgb.blue;
  } else {
    r = color.rgb.red;
    g = color.rgb.green;
    b = color.rgb.blue;
  }
}

//=============================================================================
// VTerm state callbacks
//=============================================================================

int TerminalScreenImpl::onPutglyph(VTermGlyphInfo *info, VTermPos pos,
                                   void *user) {
  auto *self = static_cast<TerminalScreenImpl *>(user);

  uint32_t cp = info->chars[0];
  if (cp == 0)
    cp = ' ';

  uint8_t fgR, fgG, fgB, bgR, bgG, bgB;
  self->colorToRGB(self->_pen.fg, fgR, fgG, fgB);
  self->colorToRGB(self->_pen.bg, bgR, bgG, bgB);

  if (self->_pen.reverse) {
    std::swap(fgR, bgR);
    std::swap(fgG, bgG);
    std::swap(fgB, bgB);
  }

  // Pack style: bits 0-4 = text attrs, bits 5-7 = font type
  // Font type: 0=MSDF, 1=Bitmap, 2=Shader, 3=Card, 4=Vector, 5=Coverage, 6=Raster
  uint8_t style = 0;
  if (self->_pen.bold)
    style |= 0x01;
  if (self->_pen.italic)
    style |= 0x02;
  style |= (self->_pen.underline & 0x03) << 2;
  if (self->_pen.strike)
    style |= 0x10;
  // Set font render method in style bits 5-7
  if (self->_font) {
    style |= (static_cast<uint8_t>(self->_font->renderMethod()) & 0x07) << 5;
  }

  self->setCell(pos.row, pos.col, cp, fgR, fgG, fgB, bgR, bgG, bgB, style);

  for (int i = 1; i < info->width; i++) {
    self->setCell(pos.row, pos.col + i, GLYPH_WIDE_CONT, fgR, fgG, fgB, bgR,
                  bgG, bgB, style);
  }

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

int TerminalScreenImpl::onScrollRect(VTermRect rect, int downward, int,
                                     void *user) {
  auto *self = static_cast<TerminalScreenImpl *>(user);

  bool isFullWidth = (rect.start_col == 0 && rect.end_col == self->_cols);

  if (downward > 0 && rect.start_row == 0 && isFullWidth) {
    for (int i = 0; i < downward && i < rect.end_row; i++) {
      self->pushLineToScrollback(i);
    }
  } else if (downward < 0 && rect.start_row == 0 && isFullWidth) {
    int upAmount = -downward;
    for (int i = 0; i < upAmount && i < rect.end_row; i++) {
      self->pushLineToScrollback(i);
    }
  }

  // Return 0 — let vterm handle via moverect/erase
  return 0;
}

int TerminalScreenImpl::onMoveRect(VTermRect dest, VTermRect src, void *user) {
  auto *self = static_cast<TerminalScreenImpl *>(user);

  int height = src.end_row - src.start_row;
  int width = src.end_col - src.start_col;
  int cols = self->_cols;
  int rows = self->_rows;

  if (height <= 0 || width <= 0)
    return 1;
  if (src.start_row < 0 || src.end_row > rows)
    return 1;
  if (dest.start_row < 0 || dest.start_row + height > rows)
    return 1;
  if (src.start_col < 0 || src.end_col > cols)
    return 1;
  if (dest.start_col < 0 || dest.start_col + width > cols)
    return 1;
  if (!self->_visibleBuffer)
    return 1;

  // Ultra-fast path: full-width move — single memmove
  if (src.start_col == 0 && dest.start_col == 0 && width == cols) {
    size_t srcIdx = self->cellIndex(src.start_row, 0);
    size_t dstIdx = self->cellIndex(dest.start_row, 0);
    size_t totalCells = static_cast<size_t>(height) * cols;
    std::memmove(&(*self->_visibleBuffer)[dstIdx],
                 &(*self->_visibleBuffer)[srcIdx],
                 totalCells * sizeof(TextCell));
  }
  // Fast path: full-width, row-by-row memmove
  else if (src.start_col == dest.start_col && width == cols) {
    if (dest.start_row < src.start_row) {
      for (int row = 0; row < height; row++) {
        size_t si = self->cellIndex(src.start_row + row, 0);
        size_t di = self->cellIndex(dest.start_row + row, 0);
        std::memmove(&(*self->_visibleBuffer)[di], &(*self->_visibleBuffer)[si],
                     width * sizeof(TextCell));
      }
    } else {
      for (int row = height - 1; row >= 0; row--) {
        size_t si = self->cellIndex(src.start_row + row, 0);
        size_t di = self->cellIndex(dest.start_row + row, 0);
        std::memmove(&(*self->_visibleBuffer)[di], &(*self->_visibleBuffer)[si],
                     width * sizeof(TextCell));
      }
    }
  }
  // General case: scratch buffer
  else {
    if (static_cast<int>(self->_scratchBuffer.size()) < width) {
      self->_scratchBuffer.resize(width);
    }
    bool copyForward =
        (dest.start_row < src.start_row) ||
        (dest.start_row == src.start_row && dest.start_col <= src.start_col);

    auto copyRow = [&](int row) {
      size_t si =
          self->cellIndex((copyForward ? src.start_row + row
                                       : src.start_row + height - 1 - row),
                          src.start_col);
      size_t di =
          self->cellIndex((copyForward ? dest.start_row + row
                                       : dest.start_row + height - 1 - row),
                          dest.start_col);
      std::memcpy(self->_scratchBuffer.data(), &(*self->_visibleBuffer)[si],
                  width * sizeof(TextCell));
      std::memcpy(&(*self->_visibleBuffer)[di], self->_scratchBuffer.data(),
                  width * sizeof(TextCell));
    };

    for (int row = 0; row < height; row++) {
      copyRow(row);
    }
  }

  self->_hasDamage = true;
  self->_viewBufferDirty = true;
  return 1;
}

int TerminalScreenImpl::onErase(VTermRect rect, int, void *user) {
  auto *self = static_cast<TerminalScreenImpl *>(user);

  if (!self->_visibleBuffer)
    return 1;

  int startRow = std::max(0, rect.start_row);
  int endRow = std::min(self->_rows, rect.end_row);
  int startCol = std::max(0, rect.start_col);
  int endCol = std::min(self->_cols, rect.end_col);
  int width = endCol - startCol;
  int cols = self->_cols;

  if (width <= 0 || startRow >= endRow)
    return 1;

  uint8_t fgR, fgG, fgB, bgR, bgG, bgB;
  self->colorToRGB(self->_pen.fg, fgR, fgG, fgB);
  self->colorToRGB(self->_pen.bg, bgR, bgG, bgB);
  if (self->_pen.reverse) {
    std::swap(fgR, bgR);
    std::swap(fgG, bgG);
    std::swap(fgB, bgB);
  }

  TextCell defaultCell{};
  defaultCell.glyph = ' ';
  defaultCell.fgR = fgR;
  defaultCell.fgG = fgG;
  defaultCell.fgB = fgB;
  defaultCell.bgR = bgR;
  defaultCell.bgG = bgG;
  defaultCell.bgB = bgB;
  defaultCell.alpha = 255;
  defaultCell.style = 0;

  if (startCol == 0 && endCol == cols) {
    for (int row = startRow; row < endRow; row++) {
      size_t off = static_cast<size_t>(row * cols);
      std::fill(self->_visibleBuffer->begin() + static_cast<ptrdiff_t>(off),
                self->_visibleBuffer->begin() +
                    static_cast<ptrdiff_t>(off + cols),
                defaultCell);
    }
  } else {
    for (int row = startRow; row < endRow; row++) {
      size_t off = static_cast<size_t>(row * cols);
      for (int col = startCol; col < endCol; col++) {
        (*self->_visibleBuffer)[off + col] = defaultCell;
      }
    }
  }

  self->_hasDamage = true;
  if (self->_scrollOffset > 0) {
    self->_viewBufferDirty = true;
  }

  return 1;
}

int TerminalScreenImpl::onInitPen(void *user) {
  auto *self = static_cast<TerminalScreenImpl *>(user);
  self->_pen.fg = self->_defaultFg;
  self->_pen.bg = self->_defaultBg;
  self->_pen.bold = false;
  self->_pen.italic = false;
  self->_pen.underline = 0;
  self->_pen.strike = false;
  self->_pen.reverse = false;
  self->_pen.blink = false;
  return 1;
}

int TerminalScreenImpl::onSetPenAttr(VTermAttr attr, VTermValue *val,
                                     void *user) {
  auto *self = static_cast<TerminalScreenImpl *>(user);

  switch (attr) {
  case VTERM_ATTR_BOLD:
    self->_pen.bold = val->boolean != 0;
    break;
  case VTERM_ATTR_ITALIC:
    self->_pen.italic = val->boolean != 0;
    break;
  case VTERM_ATTR_UNDERLINE:
    self->_pen.underline = static_cast<uint8_t>(val->number & 0x03);
    break;
  case VTERM_ATTR_STRIKE:
    self->_pen.strike = val->boolean != 0;
    break;
  case VTERM_ATTR_REVERSE:
    self->_pen.reverse = val->boolean != 0;
    break;
  case VTERM_ATTR_BLINK:
    self->_pen.blink = val->boolean != 0;
    break;
  case VTERM_ATTR_FOREGROUND:
    self->_pen.fg = val->color;
    break;
  case VTERM_ATTR_BACKGROUND:
    self->_pen.bg = val->color;
    break;
  default:
    break;
  }

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
  } else if (prop == VTERM_PROP_ALTSCREEN) {
    self->switchToScreen(val->boolean != 0);
  }

  return 1;
}

int TerminalScreenImpl::onBell(void *) { return 1; }

int TerminalScreenImpl::onResize(int rows, int cols, VTermStateFields *fields,
                                 void *user) {
  auto *self = static_cast<TerminalScreenImpl *>(user);
  self->resizeInternal(rows, cols, fields);
  return 1;
}

int TerminalScreenImpl::onSetLineInfo(int, const VTermLineInfo *,
                                      const VTermLineInfo *, void *) {
  return 1;
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
    renderFrame();
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

  // Resize - resize terminal grid and PTY
  if (event.type == core::Event::Type::Resize) {
    uint32_t newCols = static_cast<uint32_t>(event.resize.width / _cellWidth);
    uint32_t newRows = static_cast<uint32_t>(event.resize.height / _cellHeight);
    if (newCols > 0 && newRows > 0 && (newCols != static_cast<uint32_t>(_cols) || newRows != static_cast<uint32_t>(_rows))) {
      resize(newCols, newRows);
      auto* pty = _terminalScreenContext.terminalContext.pty;
      if (pty) {
        pty->resize(newCols, newRows);
      }
      ydebug("TerminalScreen: resized to {}x{}", newCols, newRows);
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
