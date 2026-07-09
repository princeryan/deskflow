/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "gui/core/SystemTheme.h"

#include <QApplication>
#include <QPalette>
#include <QStyleHints>

#ifdef Q_OS_LINUX
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusVariant>
#endif

namespace deskflow::gui {

namespace {

// A token->value map applied to the style-sheet template below.
struct Palette
{
  QString bg;          // window background
  QString surface;     // cards, inputs, buttons
  QString surfaceAlt;  // log / sunken areas
  QString border;      // hairline separators
  QString borderStrong;// input outlines
  QString text;        // primary text
  QString textDim;     // secondary text
  QString hover;       // subtle hover wash
  QString accent;      // system accent
  QString accentText;  // text on the accent
  QString accentHover; // accent, slightly shifted for hover
};

QString hex(const QColor &c)
{
  return c.name(QColor::HexRgb);
}

// Perceived luminance, to pick readable text on the accent.
bool isLight(const QColor &c)
{
  return (0.299 * c.redF() + 0.587 * c.greenF() + 0.114 * c.blueF()) > 0.6;
}

} // namespace

SystemTheme::SystemTheme(QApplication *app) : QObject(app), m_app(app)
{
  // Live-follow the desktop light/dark switch.
  connect(m_app->styleHints(), &QStyleHints::colorSchemeChanged, this, &SystemTheme::reapply);

#ifdef Q_OS_LINUX
  // Live-follow accent (and scheme) changes from the appearance portal.
  QDBusConnection::sessionBus().connect(
      QStringLiteral("org.freedesktop.portal.Desktop"), QStringLiteral("/org/freedesktop/portal/desktop"),
      QStringLiteral("org.freedesktop.portal.Settings"), QStringLiteral("SettingChanged"), this, SLOT(reapply())
  );
#endif
}

void SystemTheme::reapply()
{
  apply();
}

bool SystemTheme::isDark() const
{
  return m_app->styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}

QColor SystemTheme::readPortalAccent()
{
#ifdef Q_OS_LINUX
  QDBusInterface iface(
      QStringLiteral("org.freedesktop.portal.Desktop"), QStringLiteral("/org/freedesktop/portal/desktop"),
      QStringLiteral("org.freedesktop.portal.Settings"), QDBusConnection::sessionBus()
  );
  if (!iface.isValid())
    return QColor();

  QDBusReply<QDBusVariant> reply =
      iface.call(QStringLiteral("Read"), QStringLiteral("org.freedesktop.appearance"), QStringLiteral("accent-color"));
  if (!reply.isValid())
    return QColor();

  const QVariant inner = reply.value().variant();
  QDBusArgument arg = inner.value<QDBusArgument>();
  if (arg.currentType() != QDBusArgument::StructureType)
    return QColor();

  double r = -1, g = -1, b = -1;
  arg.beginStructure();
  arg >> r >> g >> b;
  arg.endStructure();
  if (r < 0 || g < 0 || b < 0) // (-1,-1,-1) means "no accent set"
    return QColor();

  return QColor::fromRgbF(qBound(0.0, r, 1.0), qBound(0.0, g, 1.0), qBound(0.0, b, 1.0));
#else
  return QColor();
#endif
}

QColor SystemTheme::accentColor() const
{
  // Prefer the portal (exact, cross-desktop). Fall back to Qt's palette accent,
  // then to GNOME blue.
  QColor c = readPortalAccent();
  if (!c.isValid())
    c = m_app->palette().color(QPalette::Accent);
  if (!c.isValid() || c == QColor(Qt::black))
    c = QColor(QStringLiteral("#3584E4"));
  return c;
}

QString SystemTheme::buildStyleSheet() const
{
  const QColor accent = accentColor();
  const bool dark = isDark();

  Palette p;
  if (dark) {
    p.bg = "#1e1e1f";
    p.surface = "#2b2b2d";
    p.surfaceAlt = "#242426";
    p.border = "#3a3a3d";
    p.borderStrong = "#4a4a4e";
    p.text = "#f2f2f3";
    p.textDim = "#9a9aa0";
    p.hover = "rgba(255,255,255,0.06)";
  } else {
    p.bg = "#f5f5f7";
    p.surface = "#ffffff";
    p.surfaceAlt = "#fbfbfd";
    p.border = "#e4e4e8";
    p.borderStrong = "#d2d2d8";
    p.text = "#1d1d1f";
    p.textDim = "#6e6e73";
    p.hover = "rgba(0,0,0,0.04)";
  }
  p.accent = hex(accent);
  p.accentText = isLight(accent) ? QStringLiteral("#1d1d1f") : QStringLiteral("#ffffff");
  p.accentHover = hex(dark ? accent.lighter(115) : accent.darker(108));

  static const char *tpl = R"QSS(
* {
  font-family: "Ubuntu", "Inter", "SF Pro Text", "Segoe UI", system-ui, sans-serif;
  font-size: 14px;
  color: %TEXT%;
  outline: 0;
}
QMainWindow, QDialog, QWidget#centralWidget, QWidget {
  background: %BG%;
}
QMenuBar { background: transparent; padding: 4px 6px; }
QMenuBar::item { background: transparent; padding: 5px 12px; border-radius: 7px; }
QMenuBar::item:selected { background: %HOVER%; }
QMenu {
  background: %SURFACE%; border: 1px solid %BORDER%; border-radius: 10px; padding: 6px;
}
QMenu::item { padding: 7px 26px 7px 22px; border-radius: 6px; }
QMenu::item:selected { background: %ACCENT%; color: %ACCENTTEXT%; }
QMenu::separator { height: 1px; background: %BORDER%; margin: 5px 8px; }

QGroupBox, QFrame#card {
  background: %SURFACE%;
  border: 1px solid %BORDER%;
  border-radius: 12px;
  margin-top: 10px;
  padding: 16px 14px 14px 14px;
}
QGroupBox::title {
  subcontrol-origin: margin; subcontrol-position: top left; left: 14px; padding: 0 4px;
  color: %TEXTDIM%; font-size: 12px; font-weight: 600; text-transform: uppercase;
}

QLabel { background: transparent; color: %TEXT%; }
QLabel[dim="true"] { color: %TEXTDIM%; }

QPushButton {
  background: %SURFACE%; color: %TEXT%;
  border: 1px solid %BORDERSTRONG%; border-radius: 8px;
  padding: 7px 16px; min-height: 18px;
}
QPushButton:hover { background: %HOVER%; }
QPushButton:pressed { background: %BORDER%; }
QPushButton:disabled { color: %TEXTDIM%; border-color: %BORDER%; }
QPushButton:default, QPushButton[accent="true"] {
  background: %ACCENT%; color: %ACCENTTEXT%; border: 1px solid %ACCENT%;
}
QPushButton:default:hover, QPushButton[accent="true"]:hover { background: %ACCENTHOVER%; border-color: %ACCENTHOVER%; }

QLineEdit, QPlainTextEdit, QTextEdit, QComboBox, QSpinBox, QAbstractSpinBox {
  background: %SURFACE%; color: %TEXT%;
  border: 1px solid %BORDERSTRONG%; border-radius: 8px;
  padding: 6px 10px; selection-background-color: %ACCENT%; selection-color: %ACCENTTEXT%;
}
QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus, QComboBox:focus, QAbstractSpinBox:focus {
  border: 1px solid %ACCENT%;
}
QComboBox::drop-down { border: 0; width: 22px; }
QComboBox QAbstractItemView {
  background: %SURFACE%; border: 1px solid %BORDER%; border-radius: 8px;
  selection-background-color: %ACCENT%; selection-color: %ACCENTTEXT%; padding: 4px;
}

QPlainTextEdit, QTextEdit { background: %SURFACEALT%; font-family: "Ubuntu Mono", "SF Mono", monospace; }

QRadioButton, QCheckBox { background: transparent; spacing: 8px; padding: 3px 0; }
QRadioButton::indicator, QCheckBox::indicator { width: 18px; height: 18px; }
QRadioButton::indicator {
  border: 2px solid %BORDERSTRONG%; border-radius: 10px; background: %SURFACE%;
}
QRadioButton::indicator:checked {
  border: 5px solid %ACCENT%; border-radius: 10px; background: %SURFACE%;
}
QCheckBox::indicator {
  border: 2px solid %BORDERSTRONG%; border-radius: 6px; background: %SURFACE%;
}
QCheckBox::indicator:checked { background: %ACCENT%; border-color: %ACCENT%; }

QToolButton { background: transparent; border: 0; border-radius: 8px; padding: 6px; }
QToolButton:hover { background: %HOVER%; }
QToolButton:pressed { background: %BORDER%; }

QTabWidget::pane { border: 1px solid %BORDER%; border-radius: 12px; top: -1px; }
QTabBar::tab {
  background: transparent; color: %TEXTDIM%; padding: 8px 16px; border: 0;
  border-radius: 8px; margin: 3px 2px;
}
QTabBar::tab:selected { background: %SURFACE%; color: %TEXT%; }
QTabBar::tab:hover:!selected { background: %HOVER%; }

QScrollBar:vertical { background: transparent; width: 12px; margin: 2px; }
QScrollBar::handle:vertical { background: %BORDERSTRONG%; border-radius: 5px; min-height: 30px; }
QScrollBar::handle:vertical:hover { background: %TEXTDIM%; }
QScrollBar:horizontal { background: transparent; height: 12px; margin: 2px; }
QScrollBar::handle:horizontal { background: %BORDERSTRONG%; border-radius: 5px; min-width: 30px; }
QScrollBar::handle:horizontal:hover { background: %TEXTDIM%; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

QStatusBar { background: transparent; color: %TEXTDIM%; }
QToolTip {
  background: %SURFACE%; color: %TEXT%; border: 1px solid %BORDER%; border-radius: 8px; padding: 6px 8px;
}
)QSS";

  QString qss = QString::fromUtf8(tpl);
  qss.replace("%BG%", p.bg);
  qss.replace("%SURFACEALT%", p.surfaceAlt);
  qss.replace("%SURFACE%", p.surface);
  qss.replace("%BORDERSTRONG%", p.borderStrong);
  qss.replace("%BORDER%", p.border);
  qss.replace("%TEXTDIM%", p.textDim);
  qss.replace("%TEXT%", p.text);
  qss.replace("%HOVER%", p.hover);
  qss.replace("%ACCENTHOVER%", p.accentHover);
  qss.replace("%ACCENTTEXT%", p.accentText);
  qss.replace("%ACCENT%", p.accent);
  return qss;
}

void SystemTheme::apply()
{
  m_app->setStyleSheet(buildStyleSheet());
}

} // namespace deskflow::gui
