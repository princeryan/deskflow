/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/KeyState.h"

#include <cstdint>
#include <xkbcommon/xkbcommon.h>

struct xkb_context;
struct xkb_keymap;
struct xkb_state;

namespace deskflow {

class UInputScreen;

/// Key state for the uinput (headless evdev) backend.
///
/// The keymap logic is intentionally identical to EiKeyState: it builds a
/// deskflow KeyMap from a libxkbcommon keymap and stores the evdev keycode
/// (X keycode - 8) as KeyItem::m_button, so the value handed to
/// UInputScreen::fakeKey() is already a raw evdev keycode.
class UInputKeyState : public KeyState
{
public:
  UInputKeyState(UInputScreen *screen, IEventQueue *events);
  ~UInputKeyState() override;

  void initDefaultKeymap();

  // IKeyState overrides
  bool fakeCtrlAltDel() override;
  KeyModifierMask pollActiveModifiers() const override;
  std::int32_t pollActiveGroup() const override;
  void pollPressedKeys(KeyButtonSet &pressedKeys) const override;
  void updateXkbState(std::uint32_t keyval, bool isPressed);
  void clearStaleModifiers() override;

  //! Adopt the lock state the session is actually in.
  /*!
  We inject through a kernel device and cannot read the compositor's keyboard
  state, so our xkb shadow is a guess that drifts the moment anything else
  toggles a lock -- or the moment we are started while one is already on. The
  session broadcasts the truth as LED state to every keyboard device, so
  UInputScreen feeds it back here. \p mask carries KeyModifierCapsLock,
  KeyModifierNumLock and KeyModifierScrollLock. Returns true if this changed
  what we believed.
  */
  bool setLockLeds(KeyModifierMask mask);

protected:
  // KeyState overrides
  void getKeyMap(KeyMap &keyMap) override;
  void fakeKey(const Keystroke &keystroke) override;

private:
  std::uint32_t convertModMask(xkb_mod_mask_t xkbModMaskIn) const;
  void assignGeneratedModifiers(std::uint32_t keycode, KeyMap::KeyItem &item);
  //! Force the xkb shadow's locked modifiers to match m_lockLeds.
  void applyLockLeds();

  UInputScreen *m_screen = nullptr;

  xkb_context *m_xkb = nullptr;
  xkb_keymap *m_xkbKeymap = nullptr;
  xkb_state *m_xkbState = nullptr;

  // Lock state as last reported by the session (see setLockLeds).
  KeyModifierMask m_lockLeds = 0;
};

} // namespace deskflow
