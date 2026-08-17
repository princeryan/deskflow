/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/PlatformScreen.h"

#include <cstdint>
#include <string>

namespace deskflow {

class UInputKeyState;
class UInputClipboardBridge;

//! Headless client injection backend using the Linux /dev/uinput interface.
/*!
Unlike EiScreen (libei/portal) and XWindowsScreen (XTEST), this backend does
not talk to any display server. It creates kernel virtual input devices, so
injected events are seen as real hardware and reach the greeter and lock
screen. It is a secondary (client) backend only.
*/
class UInputScreen : public PlatformScreen
{
public:
  UInputScreen(bool isPrimary, IEventQueue *events);
  ~UInputScreen() override;

  // IScreen overrides
  void *getEventTarget() const override;
  bool getClipboard(ClipboardID id, IClipboard *) const override;
  void getShape(std::int32_t &x, std::int32_t &y, std::int32_t &width, std::int32_t &height) const override;
  void getCursorPos(std::int32_t &x, std::int32_t &y) const override;

  // IPrimaryScreen overrides (unused for a client; stubbed)
  void reconfigure(std::uint32_t activeSides) override;
  std::uint32_t activeSides() override;
  void warpCursor(std::int32_t x, std::int32_t y) override;
  std::uint32_t registerHotKey(KeyID key, KeyModifierMask mask) override;
  void unregisterHotKey(std::uint32_t id) override;
  void fakeInputBegin() override;
  void fakeInputEnd() override;
  std::int32_t getJumpZoneSize() const override;
  bool isAnyMouseButtonDown(std::uint32_t &buttonID) const override;
  void getCursorCenter(std::int32_t &x, std::int32_t &y) const override;

  // ISecondaryScreen overrides
  void fakeMouseButton(ButtonID id, bool press) override;
  void fakeMouseMove(std::int32_t x, std::int32_t y) override;
  void fakeMouseRelativeMove(std::int32_t dx, std::int32_t dy) const override;
  void fakeMouseWheel(ScrollDelta delta) const override;

  //! Inject a raw evdev key event (called by UInputKeyState).
  void fakeKey(std::uint32_t keycode, bool isDown) const;

  // IPlatformScreen overrides
  void enable() override;
  void disable() override;
  void enter() override;
  bool canLeave() override;
  void leave() override;
  bool setClipboard(ClipboardID, const IClipboard *) override;
  void checkClipboards() override;
  void openScreensaver(bool notify) override;
  void closeScreensaver() override;
  void screensaver(bool activate) override;
  void resetOptions() override;
  void setOptions(const OptionsList &options) override;
  void setSequenceNumber(std::uint32_t) override;
  bool isPrimary() const override;

protected:
  // IPlatformScreen overrides
  void handleSystemEvent(const Event &event) override;
  void updateButtons() override;
  IKeyState *getKeyState() const override;
  std::string getSecureInputApp() const override;

private:
  void createDevices();
  void destroyDevices();
  void sendClipboardEvent(EventTypes type, ClipboardID id) const;
  //! Emit one evdev event followed by a SYN_REPORT is done separately.
  void emit(int fd, std::uint16_t type, std::uint16_t code, std::int32_t value) const;
  void syn(int fd) const;
  //! Place the pointer. Set \p force when the position must reach the
  //! compositor even if it matches what we last injected (see the definition).
  void moveAbsolute(std::int32_t x, std::int32_t y, bool force = false) const;

  bool m_isPrimary = false;
  IEventQueue *m_events = nullptr;
  UInputKeyState *m_keyState = nullptr;
  UInputClipboardBridge *m_clipboardBridge = nullptr;
  std::string m_lastLocalClip; // last frame we grabbed for the server (dedup)
  std::uint32_t m_sequenceNumber = 0;

  int m_keyboardFd = -1;
  int m_pointerFd = -1;

  // Virtual screen geometry (also the absolute-axis range).
  std::int32_t m_x = 0;
  std::int32_t m_y = 0;
  std::int32_t m_w = 1920;
  std::int32_t m_h = 1080;

  bool m_isOnScreen = false;

  // Position sent before enter() / latched current absolute position.
  mutable std::int32_t m_cursorX = 0;
  mutable std::int32_t m_cursorY = 0;
};

} // namespace deskflow
