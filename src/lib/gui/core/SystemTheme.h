/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QColor>
#include <QObject>

class QApplication;

namespace deskflow::gui {

//! Applies a clean, system-following theme (light/dark + accent) to the app.
/*!
Follows the desktop's colour scheme (via Qt's style hints) and accent colour
(via the org.freedesktop.appearance portal), and re-applies live when either
changes. The look is a spacious, rounded, "cards" style tuned for modern Ubuntu
with macOS-like clarity, generated as a Qt style sheet over the Fusion style.
*/
class SystemTheme : public QObject
{
  Q_OBJECT

public:
  explicit SystemTheme(QApplication *app);

  //! Detect the current scheme/accent and apply the style sheet now.
  void apply();

private Q_SLOTS:
  void reapply(); // re-detect + re-apply on a system appearance change

private:
  bool isDark() const;
  QColor accentColor() const;
  static QColor readPortalAccent();
  QString buildStyleSheet() const;

  QApplication *m_app;
};

} // namespace deskflow::gui
