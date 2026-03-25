#pragma once

#include <yetty/core/event-listener.hpp>
#include <yetty/core/factory-object.hpp>
#include <yetty/term/text-cell.hpp>
#include <cstdint>
#include <functional>

namespace yetty {

class Pty;

class TerminalScreen : public core::FactoryObject<TerminalScreen>,
                       public core::EventListener {
public:
  static Result<TerminalScreen *> createImpl(uint32_t cols, uint32_t rows,
                                              Pty *pty);

  virtual ~TerminalScreen() = default;

  // Write data from PTY to vterm
  virtual void write(const char *data, size_t len) = 0;

  // Resize terminal
  virtual void resize(uint32_t cols, uint32_t rows) = 0;
  virtual uint32_t getCols() const = 0;
  virtual uint32_t getRows() const = 0;

  // Damage tracking
  virtual bool hasDamage() const = 0;
  virtual void clearDamage() = 0;

  // Callback for vterm output (to write to PTY)
  using OutputCallback = std::function<void(const char *, size_t)>;
  virtual void setOutputCallback(OutputCallback cb) = 0;


  // Cell data access — flat contiguous buffer, zero-copy GPU upload
  virtual const TextCell *getCellData() const = 0;
  virtual TextCell getCell(int row, int col) const = 0;

  // Cursor state
  virtual int getCursorRow() const = 0;
  virtual int getCursorCol() const = 0;
  virtual bool isCursorVisible() const = 0;
  virtual int getCursorShape() const = 0;
  virtual bool isCursorBlink() const = 0;

  // Scrollback navigation
  virtual void scrollUp(int lines) = 0;
  virtual void scrollDown(int lines) = 0;
  virtual void scrollToBottom() = 0;
  virtual bool isScrolledBack() const = 0;

protected:
  TerminalScreen() = default;
};

} // namespace yetty
