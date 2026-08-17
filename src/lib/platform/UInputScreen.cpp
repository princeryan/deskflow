/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/UInputScreen.h"

#include "base/Event.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "deskflow/ClipboardTypes.h"
#include "deskflow/IClipboard.h"
#include "deskflow/MouseTypes.h"
#include "platform/UInputClipboardBridge.h"
#include "platform/UInputKeyState.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdexcept>
#include <unistd.h>

namespace deskflow {

UInputScreen::UInputScreen(bool isPrimary, IEventQueue *events)
    : PlatformScreen{events},
      m_isPrimary{isPrimary},
      m_events{events},
      m_isOnScreen{isPrimary}
{
  // Optional geometry override; the absolute axis range must match getShape().
  if (const char *res = std::getenv("DESKFLOW_UINPUT_RESOLUTION"); res != nullptr) {
    int w = 0;
    int h = 0;
    if (std::sscanf(res, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
      m_w = w;
      m_h = h;
    } else {
      LOG_WARN("ignoring invalid DESKFLOW_UINPUT_RESOLUTION '%s' (expected WxH)", res);
    }
  }
  m_cursorX = m_w / 2;
  m_cursorY = m_h / 2;

  createDevices();

  // Clipboard is handled by the deskflow-clipboard session helper, which owns
  // the clipboard natively via QClipboard (event-driven — no polling, no CLI,
  // no flicker). We bridge to it over a Unix socket; it connects once the user
  // session helper is running.
  m_clipboardBridge = new UInputClipboardBridge();

  m_keyState = new UInputKeyState(this, events);
  m_keyState->updateKeyMap();

  m_events->addHandler(EventTypes::System, m_events->getSystemTarget(), [this](const auto &e) {
    handleSystemEvent(e);
  });

  // Keep our idea of the session's lock state current. The kernel hands us LED
  // updates on the keyboard fd, but nothing wakes the event loop for them, so
  // we look on a timer as well as on enter().
  m_events->addHandler(EventTypes::Timer, this, [this](const auto &) { pollLockLeds(); });
  m_ledTimer = m_events->newTimer(0.25, this);

  LOG_INFO("uinput screen ready (virtual size %dx%d)", m_w, m_h);
}

UInputScreen::~UInputScreen()
{
  m_events->removeHandler(EventTypes::System, m_events->getSystemTarget());
  if (m_ledTimer != nullptr) {
    m_events->deleteTimer(m_ledTimer);
    m_ledTimer = nullptr;
  }
  m_events->removeHandler(EventTypes::Timer, this);
  destroyDevices();
  delete m_keyState;
  delete m_clipboardBridge;
}

//
// device setup
//

void UInputScreen::emit(int fd, std::uint16_t type, std::uint16_t code, std::int32_t value) const
{
  if (fd < 0)
    return;
  struct input_event ev = {};
  ev.type = type;
  ev.code = code;
  ev.value = value;
  [[maybe_unused]] auto n = write(fd, &ev, sizeof(ev));
}

void UInputScreen::syn(int fd) const
{
  emit(fd, EV_SYN, SYN_REPORT, 0);
}

void UInputScreen::createDevices()
{
  // Keyboard device: all standard evdev key codes. Opened read-write because we
  // also want what the kernel sends back to us (see pollLockLeds).
  m_keyboardFd = open("/dev/uinput", O_RDWR | O_NONBLOCK);
  if (m_keyboardFd < 0)
    throw std::runtime_error(std::string("uinput: cannot open /dev/uinput for keyboard: ") + strerror(errno));

  ioctl(m_keyboardFd, UI_SET_EVBIT, EV_KEY);
  ioctl(m_keyboardFd, UI_SET_EVBIT, EV_SYN);
  for (int k = 0; k < 256; k++)
    ioctl(m_keyboardFd, UI_SET_KEYBIT, k);

  // Claiming the lock LEDs is what makes the session tell us its lock state:
  // X11 and every Wayland compositor push LED updates to all keyboards that
  // have them, and for a uinput device those land back on this fd. Without it
  // our idea of Caps Lock is a guess that drifts the first time anything else
  // toggles it, and a Caps Lock stuck on has no way to ever be noticed.
  ioctl(m_keyboardFd, UI_SET_EVBIT, EV_LED);
  ioctl(m_keyboardFd, UI_SET_LEDBIT, LED_CAPSL);
  ioctl(m_keyboardFd, UI_SET_LEDBIT, LED_NUML);
  ioctl(m_keyboardFd, UI_SET_LEDBIT, LED_SCROLLL);

  struct uinput_setup kb = {};
  kb.id.bustype = BUS_VIRTUAL;
  kb.id.vendor = 0x1d6b; // "Linux Foundation"
  kb.id.product = 0x0001;
  std::strncpy(kb.name, "Deskflow Virtual Keyboard", sizeof(kb.name) - 1);
  if (ioctl(m_keyboardFd, UI_DEV_SETUP, &kb) < 0 || ioctl(m_keyboardFd, UI_DEV_CREATE) < 0) {
    throw std::runtime_error(std::string("uinput: cannot create keyboard device: ") + strerror(errno));
  }

  // Pointer device: buttons + absolute position + wheel. Relative moves are
  // converted to absolute (see fakeMouseRelativeMove) so we never mix REL_X/Y
  // with ABS_X/Y on one device, which confuses libinput's device classing.
  m_pointerFd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
  if (m_pointerFd < 0)
    throw std::runtime_error(std::string("uinput: cannot open /dev/uinput for pointer: ") + strerror(errno));

  ioctl(m_pointerFd, UI_SET_EVBIT, EV_KEY);
  ioctl(m_pointerFd, UI_SET_EVBIT, EV_ABS);
  ioctl(m_pointerFd, UI_SET_EVBIT, EV_REL);
  ioctl(m_pointerFd, UI_SET_EVBIT, EV_SYN);
  ioctl(m_pointerFd, UI_SET_KEYBIT, BTN_LEFT);
  ioctl(m_pointerFd, UI_SET_KEYBIT, BTN_RIGHT);
  ioctl(m_pointerFd, UI_SET_KEYBIT, BTN_MIDDLE);
  ioctl(m_pointerFd, UI_SET_KEYBIT, BTN_SIDE);
  ioctl(m_pointerFd, UI_SET_KEYBIT, BTN_EXTRA);
  ioctl(m_pointerFd, UI_SET_RELBIT, REL_WHEEL);
  ioctl(m_pointerFd, UI_SET_RELBIT, REL_HWHEEL);
  ioctl(m_pointerFd, UI_SET_ABSBIT, ABS_X);
  ioctl(m_pointerFd, UI_SET_ABSBIT, ABS_Y);

  struct uinput_abs_setup absX = {};
  absX.code = ABS_X;
  absX.absinfo.minimum = 0;
  absX.absinfo.maximum = m_w - 1;
  ioctl(m_pointerFd, UI_ABS_SETUP, &absX);

  struct uinput_abs_setup absY = {};
  absY.code = ABS_Y;
  absY.absinfo.minimum = 0;
  absY.absinfo.maximum = m_h - 1;
  ioctl(m_pointerFd, UI_ABS_SETUP, &absY);

  struct uinput_setup ptr = {};
  ptr.id.bustype = BUS_VIRTUAL;
  ptr.id.vendor = 0x1d6b;
  ptr.id.product = 0x0002;
  std::strncpy(ptr.name, "Deskflow Virtual Pointer", sizeof(ptr.name) - 1);
  if (ioctl(m_pointerFd, UI_DEV_SETUP, &ptr) < 0 || ioctl(m_pointerFd, UI_DEV_CREATE) < 0) {
    throw std::runtime_error(std::string("uinput: cannot create pointer device: ") + strerror(errno));
  }
}

void UInputScreen::destroyDevices()
{
  for (int *fd : {&m_keyboardFd, &m_pointerFd}) {
    if (*fd >= 0) {
      ioctl(*fd, UI_DEV_DESTROY);
      close(*fd);
      *fd = -1;
    }
  }
}

//
// injection
//

void UInputScreen::pollLockLeds()
{
  if (m_keyboardFd < 0 || m_keyState == nullptr)
    return;

  // Drain everything the kernel has for us; only the last value of each LED
  // matters. Reads are non-blocking, so an empty queue just gives EAGAIN.
  bool sawLed = false;
  struct input_event ev = {};
  while (read(m_keyboardFd, &ev, sizeof(ev)) == static_cast<ssize_t>(sizeof(ev))) {
    if (ev.type != EV_LED)
      continue;

    KeyModifierMask bit = 0;
    switch (ev.code) {
    case LED_CAPSL:
      bit = KeyModifierCapsLock;
      break;
    case LED_NUML:
      bit = KeyModifierNumLock;
      break;
    case LED_SCROLLL:
      bit = KeyModifierScrollLock;
      break;
    default:
      continue;
    }

    sawLed = true;
    if (ev.value != 0)
      m_lockLeds |= bit;
    else
      m_lockLeds &= ~bit;
  }

  if (sawLed) {
    m_keyState->setLockLeds(m_lockLeds);
  }
}

void UInputScreen::fakeKey(std::uint32_t keycode, bool isDown) const
{
  // Keep the xkb shadow state in sync for modifier polling (xkb uses X
  // keycodes, i.e. evdev + 8), then inject the raw evdev keycode.
  m_keyState->updateXkbState(keycode + 8, isDown);
  emit(m_keyboardFd, EV_KEY, static_cast<std::uint16_t>(keycode), isDown ? 1 : 0);
  syn(m_keyboardFd);
}

void UInputScreen::moveAbsolute(std::int32_t x, std::int32_t y, bool force) const
{
  m_cursorX = std::clamp(x, 0, m_w - 1);
  m_cursorY = std::clamp(y, 0, m_h - 1);

  if (force) {
    // The kernel drops an EV_ABS that carries the value the device already
    // holds, and then drops the SYN_REPORT closing the now-empty report along
    // with it, so re-sending the position we last sent injects precisely
    // nothing. Our shadow only tracks what *we* injected, so when another
    // uinput client -- a remote-desktop tool sharing this seat, say -- has
    // moved the real pointer meanwhile, the shadow still matches the device
    // and the placement is silently swallowed. The pointer then never arrives
    // where the server put us, and because relative moves are resolved against
    // the shadow, every later move stays anchored to a position the cursor
    // does not occupy. Step a pixel off target first so the report carrying
    // the real position is always a genuine change on both axes.
    const std::int32_t stepX = m_cursorX > 0 ? m_cursorX - 1 : std::min(m_cursorX + 1, m_w - 1);
    const std::int32_t stepY = m_cursorY > 0 ? m_cursorY - 1 : std::min(m_cursorY + 1, m_h - 1);
    emit(m_pointerFd, EV_ABS, ABS_X, stepX);
    emit(m_pointerFd, EV_ABS, ABS_Y, stepY);
    syn(m_pointerFd);
  }

  emit(m_pointerFd, EV_ABS, ABS_X, m_cursorX);
  emit(m_pointerFd, EV_ABS, ABS_Y, m_cursorY);
  syn(m_pointerFd);
}

void UInputScreen::fakeMouseButton(ButtonID button, bool press)
{
  std::uint16_t code;
  switch (button) {
  case kButtonLeft:
    code = BTN_LEFT;
    break;
  case kButtonMiddle:
    code = BTN_MIDDLE;
    break;
  case kButtonRight:
    code = BTN_RIGHT;
    break;
  default:
    code = static_cast<std::uint16_t>(BTN_LEFT + (button - 1));
    break;
  }
  emit(m_pointerFd, EV_KEY, code, press ? 1 : 0);
  syn(m_pointerFd);
}

void UInputScreen::fakeMouseMove(std::int32_t x, std::int32_t y)
{
  // A motion event arrives before enter() with the target position; latch it.
  if (!m_isOnScreen) {
    m_cursorX = std::clamp(x, 0, m_w - 1);
    m_cursorY = std::clamp(y, 0, m_h - 1);
    return;
  }
  moveAbsolute(x, y);
}

void UInputScreen::fakeMouseRelativeMove(std::int32_t dx, std::int32_t dy) const
{
  moveAbsolute(m_cursorX + dx, m_cursorY + dy);
}

void UInputScreen::fakeMouseWheel(ScrollDelta delta) const
{
  delta = applyScrollModifier(delta);

  auto notches = [](std::int32_t d) -> std::int32_t {
    if (d == 0)
      return 0;
    std::int32_t n = d / 120;
    return n != 0 ? n : (d > 0 ? 1 : -1);
  };

  if (const auto v = notches(delta.y); v != 0)
    emit(m_pointerFd, EV_REL, REL_WHEEL, v);
  if (const auto h = notches(delta.x); h != 0)
    emit(m_pointerFd, EV_REL, REL_HWHEEL, h);
  syn(m_pointerFd);
}

//
// screen geometry / lifecycle
//

void *UInputScreen::getEventTarget() const
{
  return const_cast<void *>(static_cast<const void *>(this));
}

void UInputScreen::getShape(std::int32_t &x, std::int32_t &y, std::int32_t &w, std::int32_t &h) const
{
  x = m_x;
  y = m_y;
  w = m_w;
  h = m_h;
}

void UInputScreen::getCursorPos(std::int32_t &x, std::int32_t &y) const
{
  x = m_cursorX;
  y = m_cursorY;
}

void UInputScreen::getCursorCenter(std::int32_t &x, std::int32_t &y) const
{
  x = m_x + m_w / 2;
  y = m_y + m_h / 2;
}

void UInputScreen::enable()
{
  if (m_clipboardBridge != nullptr)
    m_clipboardBridge->start();
}

void UInputScreen::disable()
{
  if (m_clipboardBridge != nullptr)
    m_clipboardBridge->stop();
}

void UInputScreen::enter()
{
  m_isOnScreen = true;
  // Anything could have happened to Caps Lock while we were away; find out
  // before the first key of this visit is mapped.
  pollLockLeds();
  // Flush the position latched before we were on-screen. This must be forced:
  // entering at the same edge position as last time leaves the latched value
  // equal to our shadow, and anything else sharing the seat may have moved the
  // real cursor while we were away, so an unforced report would be dropped and
  // the pointer would never arrive.
  moveAbsolute(m_cursorX, m_cursorY, /*force=*/true);
}

bool UInputScreen::canLeave()
{
  return true;
}

void UInputScreen::leave()
{
  m_isOnScreen = false;
}

bool UInputScreen::isPrimary() const
{
  return m_isPrimary;
}

//
// unused primary / clipboard / screensaver surface (client, headless)
//

void UInputScreen::reconfigure(std::uint32_t)
{
}
std::uint32_t UInputScreen::activeSides()
{
  return 0;
}
void UInputScreen::warpCursor(std::int32_t x, std::int32_t y)
{
  m_cursorX = std::clamp(x, 0, m_w - 1);
  m_cursorY = std::clamp(y, 0, m_h - 1);
}
std::uint32_t UInputScreen::registerHotKey(KeyID, KeyModifierMask)
{
  return 0;
}
void UInputScreen::unregisterHotKey(std::uint32_t)
{
}
void UInputScreen::fakeInputBegin()
{
}
void UInputScreen::fakeInputEnd()
{
}
std::int32_t UInputScreen::getJumpZoneSize() const
{
  return 1;
}
bool UInputScreen::isAnyMouseButtonDown(std::uint32_t &) const
{
  return false;
}
namespace {
// Frame = [1 byte count] then count x [1 byte fmt][4 byte BE len][data].
// fmt: 0=Text, 1=HTML, 2=Bitmap, 3=File. Matches the deskflow-clipboard helper.
void frameAppend(std::string &out, std::uint8_t id, const std::string &data)
{
  out.push_back(static_cast<char>(id));
  std::uint32_t n = htonl(static_cast<std::uint32_t>(data.size()));
  out.append(reinterpret_cast<const char *>(&n), 4);
  out.append(data);
}

IClipboard::Format frameFormat(std::uint8_t id, bool &ok)
{
  ok = true;
  switch (id) {
  case 0:
    return IClipboard::Format::Text;
  case 1:
    return IClipboard::Format::HTML;
  case 2:
    return IClipboard::Format::Bitmap;
  case 3:
    return IClipboard::Format::File;
  default:
    ok = false;
    return IClipboard::Format::Text;
  }
}
} // namespace

bool UInputScreen::getClipboard(ClipboardID id, IClipboard *clipboard) const
{
  // Only the standard clipboard is bridged (not the primary selection).
  if (id != kClipboardClipboard || clipboard == nullptr || m_clipboardBridge == nullptr)
    return false;

  const std::string frame = m_clipboardBridge->read();
  if (clipboard->open(0)) {
    clipboard->empty();
    std::size_t pos = 0;
    if (pos < frame.size()) {
      const auto count = static_cast<std::uint8_t>(frame[pos++]);
      for (int i = 0; i < count && pos + 5 <= frame.size(); ++i) {
        const auto fmtId = static_cast<std::uint8_t>(frame[pos++]);
        std::uint32_t n = 0;
        std::memcpy(&n, frame.data() + pos, 4);
        n = ntohl(n);
        pos += 4;
        if (pos + n > frame.size())
          break;
        std::string data = frame.substr(pos, n);
        pos += n;
        bool ok = false;
        const IClipboard::Format fmt = frameFormat(fmtId, ok);
        if (ok)
          clipboard->add(fmt, data);
      }
    }
    clipboard->close();
  }
  return true;
}
bool UInputScreen::setClipboard(ClipboardID id, const IClipboard *clipboard)
{
  if (id != kClipboardClipboard || clipboard == nullptr || m_clipboardBridge == nullptr)
    return false;

  std::string body;
  std::uint8_t count = 0;
  if (clipboard->open(0)) {
    using enum IClipboard::Format;
    for (const auto &[id2, fmt] :
         {std::pair{std::uint8_t{0}, Text}, std::pair{std::uint8_t{1}, HTML}, std::pair{std::uint8_t{2}, Bitmap},
          std::pair{std::uint8_t{3}, File}}) {
      if (clipboard->has(fmt)) {
        frameAppend(body, id2, clipboard->get(fmt));
        ++count;
      }
    }
    clipboard->close();
  }
  std::string frame;
  frame.push_back(static_cast<char>(count));
  frame += body;
  m_clipboardBridge->write(frame);
  return true;
}
void UInputScreen::checkClipboards()
{
  // Called on screen transitions (after leave()). Read the session clipboard
  // now via the helper (xclip/Xwayland -- unfocused-safe, no flicker) and, if it
  // changed since we last sent it, grab it for the server. No polling: this is
  // the only moment deskflow needs the local clipboard.
  if (m_clipboardBridge == nullptr)
    return;

  const std::string frame = m_clipboardBridge->read();
  // An empty/format-count-zero frame means the clipboard is empty; ignore it so
  // we never clobber the server's clipboard with nothing.
  if (frame.empty() || static_cast<std::uint8_t>(frame[0]) == 0)
    return;
  if (frame == m_lastLocalClip)
    return;

  m_lastLocalClip = frame;
  // Must be ClipboardGrabbed (what Client::handleClipboardGrabbed consumes),
  // not ClipboardChanged (which only the server handles).
  LOG_DEBUG("uinput: local clipboard changed, grabbing for server");
  sendClipboardEvent(EventTypes::ClipboardGrabbed, kClipboardClipboard);
}
void UInputScreen::sendClipboardEvent(EventTypes type, ClipboardID id) const
{
  auto *info = static_cast<IScreen::ClipboardInfo *>(malloc(sizeof(IScreen::ClipboardInfo)));
  if (info == nullptr) {
    LOG_ERR("malloc failed for ClipboardInfo");
    return;
  }
  info->m_id = id;
  info->m_sequenceNumber = m_sequenceNumber;
  m_events->addEvent(Event(type, getEventTarget(), info));
}
void UInputScreen::openScreensaver(bool)
{
}
void UInputScreen::closeScreensaver()
{
}
void UInputScreen::screensaver(bool)
{
}
void UInputScreen::resetOptions()
{
}
void UInputScreen::setOptions(const OptionsList &)
{
}
void UInputScreen::setSequenceNumber(std::uint32_t seqNum)
{
  m_sequenceNumber = seqNum;
}
void UInputScreen::handleSystemEvent(const Event &)
{
}
void UInputScreen::updateButtons()
{
}
IKeyState *UInputScreen::getKeyState() const
{
  return m_keyState;
}
std::string UInputScreen::getSecureInputApp() const
{
  return "";
}

} // namespace deskflow
