// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT

#pragma once

namespace deskflow::gui {

/**
 * @brief Manages an OS-level "start on login" entry for the Deskflow GUI.
 *
 * The entry launches the GUI at login, which (with "start core with GUI")
 * brings up input sharing automatically on first login. Implemented per OS:
 *  - Linux/BSD: ~/.config/autostart/org.deskflow.deskflow.desktop
 *  - Windows:   HKCU\Software\Microsoft\Windows\CurrentVersion\Run
 *  - macOS:     ~/Library/LaunchAgents/org.deskflow.deskflow.plist
 */
class AutoStart
{
public:
  /// @return true if autostart is implemented for the current platform.
  static bool isSupported();

  /// @return true if the autostart entry currently exists.
  static bool isEnabled();

  /**
   * @brief Create or remove the autostart entry.
   * @param enabled create the entry when true, remove it when false.
   * @return true on success (including when already in the desired state).
   */
  static bool setEnabled(bool enabled);
};

} // namespace deskflow::gui
