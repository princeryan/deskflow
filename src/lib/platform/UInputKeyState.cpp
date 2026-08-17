/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 *
 * The xkb keymap logic here mirrors EiKeyState: the deskflow KeyMap is built
 * from a libxkbcommon keymap and stores the evdev keycode (X keycode - 8) in
 * KeyItem::m_button, so UInputScreen::fakeKey() receives a raw evdev keycode.
 */

#include "platform/UInputKeyState.h"

#include "base/Log.h"
#include "common/Settings.h"
#include "deskflow/AppUtil.h"
#include "platform/UInputScreen.h"
#include "platform/XDGKeyUtil.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

namespace deskflow {

namespace {

std::string dequote(std::string s)
{
  auto notSpace = [](unsigned char c) { return std::isspace(c) == 0; };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
  s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
  if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front())
    s = s.substr(1, s.size() - 2);
  return s;
}

// Determine the keyboard layout to inject with. Priority: env override, then
// the system config /etc/default/keyboard (what the GDM greeter itself uses).
// This is the right source for a headless/login-screen backend, since there is
// no display server or logged-in session to read a per-user layout from.
void readKeyboardConfig(std::string &model, std::string &layout, std::string &variant, std::string &options)
{
  if (const char *l = std::getenv("DESKFLOW_UINPUT_LAYOUT"); l != nullptr)
    layout = l;
  if (const char *v = std::getenv("DESKFLOW_UINPUT_VARIANT"); v != nullptr)
    variant = v;

  std::ifstream f("/etc/default/keyboard");
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    const auto eq = line.find('=');
    if (eq == std::string::npos)
      continue;
    const std::string key = line.substr(0, eq);
    const std::string val = dequote(line.substr(eq + 1));
    if (key == "XKBMODEL" && model.empty())
      model = val;
    else if (key == "XKBLAYOUT" && layout.empty())
      layout = val;
    else if (key == "XKBVARIANT" && variant.empty())
      variant = val;
    else if (key == "XKBOPTIONS" && options.empty())
      options = val;
  }
}

} // namespace

UInputKeyState::UInputKeyState(UInputScreen *screen, IEventQueue *events)
    : KeyState(
          events, AppUtil::instance().getKeyboardLayoutList(), Settings::value(Settings::Client::LanguageSync).toBool()
      ),
      m_screen{screen}
{
  m_xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  initDefaultKeymap();
}

void UInputKeyState::initDefaultKeymap()
{
  if (m_xkbKeymap) {
    xkb_keymap_unref(m_xkbKeymap);
    m_xkbKeymap = nullptr;
  }

  std::string model;
  std::string layout;
  std::string variant;
  std::string options;
  readKeyboardConfig(model, layout, variant, options);

  if (!layout.empty()) {
    struct xkb_rule_names names = {};
    names.model = model.empty() ? nullptr : model.c_str();
    names.layout = layout.c_str();
    names.variant = variant.empty() ? nullptr : variant.c_str();
    names.options = options.empty() ? nullptr : options.c_str();
    m_xkbKeymap = xkb_keymap_new_from_names(m_xkb, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (m_xkbKeymap)
      LOG_INFO("uinput keymap: layout='%s' variant='%s' model='%s'", layout.c_str(), variant.c_str(), model.c_str());
    else
      LOG_WARN("uinput: could not build keymap for layout '%s'; using compiled default", layout.c_str());
  }

  // Fall back to the libxkbcommon compiled default (honours XKB_DEFAULT_* env).
  if (!m_xkbKeymap)
    m_xkbKeymap = xkb_keymap_new_from_names(m_xkb, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);

  if (m_xkbState) {
    xkb_state_unref(m_xkbState);
  }
  m_xkbState = xkb_state_new(m_xkbKeymap);
}

UInputKeyState::~UInputKeyState()
{
  xkb_context_unref(m_xkb);
  xkb_keymap_unref(m_xkbKeymap);
  xkb_state_unref(m_xkbState);
}

bool UInputKeyState::fakeCtrlAltDel()
{
  // pass keys through unchanged
  return false;
}

KeyModifierMask UInputKeyState::pollActiveModifiers() const
{
  const auto xkbMask = xkb_state_serialize_mods(m_xkbState, XKB_STATE_MODS_EFFECTIVE);
  return convertModMask(xkbMask);
}

std::int32_t UInputKeyState::pollActiveGroup() const
{
  return xkb_state_serialize_layout(m_xkbState, XKB_STATE_LAYOUT_EFFECTIVE);
}

void UInputKeyState::pollPressedKeys(KeyButtonSet &) const
{
  // Not tracked for the headless uinput backend.
  return;
}

std::uint32_t UInputKeyState::convertModMask(xkb_mod_mask_t xkbModMaskIn) const
{
  // This is our own modifier mask, not xkb's.
  std::uint32_t modMaskOut = 0;

  for (xkb_mod_index_t xkbModIdx = 0; xkbModIdx < xkb_keymap_num_mods(m_xkbKeymap); xkbModIdx++) {
    const char *name = xkb_keymap_mod_get_name(m_xkbKeymap, xkbModIdx);

#ifdef HAVE_XKB_KEYMAP_MOD_GET_MASK
    const auto xkbModMask = xkb_keymap_mod_get_mask(m_xkbKeymap, name);
#else
    const xkb_mod_mask_t xkbModMask = (1 << xkbModIdx);
#endif

    if (xkbModMask == 0 || (xkbModMaskIn & xkbModMask) != xkbModMask)
      continue;

#ifndef XKB_VMOD_NAME_ALT
    static const auto XKB_VMOD_NAME_ALT = "Alt";
    static const auto XKB_VMOD_NAME_LEVEL3 = "LevelThree";
    static const auto XKB_VMOD_NAME_LEVEL5 = "LevelFive";
    static const auto XKB_VMOD_NAME_META = "Meta";
    static const auto XKB_VMOD_NAME_NUM = "NumLock";
    static const auto XKB_VMOD_NAME_SCROLL = "ScrollLock";
    static const auto XKB_VMOD_NAME_SUPER = "Super";
    static const auto XKB_VMOD_NAME_HYPER = "Hyper";
    static const auto XKB_MOD_NAME_MOD2 = "Mod2";
    static const auto XKB_MOD_NAME_MOD3 = "Mod3";
    static const auto XKB_MOD_NAME_MOD5 = "Mod5";
#endif

    if (strcmp(XKB_MOD_NAME_SHIFT, name) == 0)
      modMaskOut |= (1 << kKeyModifierBitShift);
    else if (strcmp(XKB_MOD_NAME_CAPS, name) == 0)
      modMaskOut |= (1 << kKeyModifierBitCapsLock);
    else if (strcmp(XKB_MOD_NAME_CTRL, name) == 0)
      modMaskOut |= (1 << kKeyModifierBitControl);
    else if (strcmp(XKB_MOD_NAME_ALT, name) == 0 || strcmp(XKB_VMOD_NAME_ALT, name) == 0)
      modMaskOut |= (1 << kKeyModifierBitAlt);
    else if (strcmp(XKB_MOD_NAME_LOGO, name) == 0 || strcmp(XKB_VMOD_NAME_SUPER, name) == 0 ||
             strcmp(XKB_VMOD_NAME_HYPER, name) == 0)
      modMaskOut |= (1 << kKeyModifierBitSuper);
    else if (strcmp(XKB_MOD_NAME_MOD5, name) == 0 || strcmp(XKB_VMOD_NAME_LEVEL3, name) == 0)
      modMaskOut |= (1 << kKeyModifierBitAltGr);
    else if (strcmp(XKB_VMOD_NAME_LEVEL5, name) == 0)
      modMaskOut |= (1 << kKeyModifierBitLevel5Lock);
    else if (strcmp(XKB_VMOD_NAME_NUM, name) == 0)
      modMaskOut |= (1 << kKeyModifierBitNumLock);
    else if (strcmp(XKB_VMOD_NAME_SCROLL, name) == 0)
      modMaskOut |= (1 << kKeyModifierBitScrollLock);
    else if ((strcmp(XKB_VMOD_NAME_META, name) == 0) || (strcmp(XKB_MOD_NAME_MOD2, name) == 0) ||
             (strcmp(XKB_MOD_NAME_MOD3, name) == 0))
      LOG_VERBOSE("modifier mask %s ignored", name);
    else
      LOG_WARN("modifier mask %s not accounted for, this is a bug", name);
  }

  return modMaskOut;
}

void UInputKeyState::assignGeneratedModifiers(std::uint32_t keycode, deskflow::KeyMap::KeyItem &item)
{
  xkb_mod_mask_t xkbModMask = 0;
  auto state = xkb_state_new(m_xkbKeymap);

  if (enum xkb_state_component changed = xkb_state_update_key(state, keycode, XKB_KEY_DOWN); changed) {
    for (xkb_mod_index_t m = 0; m < xkb_keymap_num_mods(m_xkbKeymap); m++) {
      if (xkb_state_mod_index_is_active(state, m, XKB_STATE_MODS_LOCKED))
        item.m_lock = true;

      if (xkb_state_mod_index_is_active(state, m, XKB_STATE_MODS_EFFECTIVE)) {
        xkbModMask |= (1 << m);
      }
    }
  }
  xkb_state_update_key(state, keycode, XKB_KEY_UP);
  xkb_state_unref(state);

  item.m_generates = convertModMask(xkbModMask);
}

void UInputKeyState::getKeyMap(deskflow::KeyMap &keyMap)
{
  auto minKeycode = xkb_keymap_min_keycode(m_xkbKeymap);
  auto maxKeycode = xkb_keymap_max_keycode(m_xkbKeymap);

  // X keycodes are evdev keycodes + 8; store the evdev code in m_button.
  for (auto keycode = minKeycode; keycode <= maxKeycode; keycode++) {

    if (xkb_keymap_num_layouts_for_key(m_xkbKeymap, keycode) == 0)
      continue;

    for (auto group = 0U; group < xkb_keymap_num_layouts(m_xkbKeymap); group++) {
      for (auto level = 0U; level < xkb_keymap_num_levels_for_key(m_xkbKeymap, keycode, group); level++) {
        const xkb_keysym_t *syms;
        xkb_mod_mask_t masks[64];
        auto nmasks = xkb_keymap_key_get_mods_for_level(m_xkbKeymap, keycode, group, level, masks, 64);
        auto nsyms = xkb_keymap_key_get_syms_by_level(m_xkbKeymap, keycode, group, level, &syms);

        if (nsyms == 0)
          continue;

        if (nsyms > 1)
          LOG_WARN("multiple keysyms per keycode are not supported, keycode %d", keycode);

        xkb_keysym_t keysym = syms[0];

        char keysymName[128] = {0};
        xkb_keysym_get_name(keysym, keysymName, sizeof(keysymName));

        if (strncmp(keysymName, "XF86_Switch_VT_", 15) == 0) {
          LOG_VERBOSE("skipping VT switch keysym %s for keycode %d", keysymName, keycode);
          continue;
        }

        deskflow::KeyMap::KeyItem item{};
        KeySym sym = keysym;
        item.m_id = XDGKeyUtil::mapKeySymToKeyID(sym);
        item.m_button = static_cast<KeyButton>(keycode) - 8; // X keycode offset -> evdev
        item.m_group = group;

        uint32_t modSensitive = 0;
        uint32_t modRequired = 0xFFFFFFFF;
        int minBits = 32;
        for (std::size_t m = 0; m < nmasks; m++) {
          modSensitive |= masks[m];
          int bits = __builtin_popcount(masks[m]);
          if (bits < minBits) {
            minBits = bits;
            modRequired = masks[m];
          }
        }
        if (modRequired == 0xFFFFFFFF) {
          modRequired = 0;
        }
        item.m_sensitive = convertModMask(modSensitive);
        item.m_required = convertModMask(modRequired);

        assignGeneratedModifiers(keycode, item);

        // Add a CapsLock alternative ONLY for entries that are reached via Shift
        // (i.e. the uppercase level). Doing this for the base level would destroy
        // the zero-modifier lowercase entry and make deskflow toggle CapsLock to
        // type every letter (turning lowercase into uppercase).
        if ((item.m_required & KeyModifierShift) && (item.m_sensitive & KeyModifierCapsLock)) {
          item.m_required &= ~KeyModifierShift;
          item.m_required |= KeyModifierCapsLock;
          keyMap.addKeyEntry(item);
          item.m_required |= KeyModifierShift;
          item.m_required &= ~KeyModifierCapsLock;
        }

        keyMap.addKeyEntry(item);
      }
    }
  }

  keyMap.allowGroupSwitchDuringCompose();
}

void UInputKeyState::fakeKey(const Keystroke &keystroke)
{
  if (keystroke.m_type != Keystroke::KeyType::Button)
    return;

  LOG_VERBOSE(
      "fake key: %03x (%08x) %s", keystroke.m_data.m_button.m_button, keystroke.m_data.m_button.m_client,
      keystroke.m_data.m_button.m_press ? "down" : "up"
  );
  m_screen->fakeKey(keystroke.m_data.m_button.m_button, keystroke.m_data.m_button.m_press);
}

void UInputKeyState::updateXkbState(uint32_t keyval, bool isPressed)
{
  xkb_state_update_key(m_xkbState, keyval, isPressed ? XKB_KEY_DOWN : XKB_KEY_UP);
}

void UInputKeyState::clearStaleModifiers()
{
  if (m_xkbState) {
    xkb_state_unref(m_xkbState);
  }
  m_xkbState = xkb_state_new(m_xkbKeymap);
}

} // namespace deskflow
