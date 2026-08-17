/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <mutex>
#include <string>

namespace deskflow {

//! Bridges the headless client to the native session clipboard helper.
/*!
The uinput client runs headless (no display), so it cannot reach the clipboard
directly. The `deskflow-clipboard` helper runs inside the user's graphical
session and reads/writes the clipboard via `xclip` on Xwayland -- which, unlike
the Wayland clipboard, works from an unfocused background process and does not
flicker.

This class is the client end of a tiny, synchronous, request/response Unix
socket protocol to that helper:

  read()  -> sends 'R', the helper reads the clipboard now and returns a frame.
  write() -> sends 'W' + frame, the helper writes it to the clipboard now.

There is no background thread, no polling and no change-notification: deskflow
already syncs the clipboard on screen transitions, so we read on leave and write
on receive, on demand. A frame is [1 byte format count] then count x
[1 byte format][4 byte big-endian length][data].
*/
class UInputClipboardBridge
{
public:
  UInputClipboardBridge() = default;
  ~UInputClipboardBridge();

  void start()
  {
  }
  void stop();

  //! Read the session clipboard now; returns a frame (empty on failure).
  std::string read();

  //! Write a frame to the session clipboard now (server -> local).
  void write(const std::string &frame);

private:
  bool ensureConnected();
  void dropConnection();
  static std::string socketPath();

  std::mutex m_mutex;
  int m_fd = -1;
};

} // namespace deskflow
