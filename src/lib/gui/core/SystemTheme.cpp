/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "gui/core/SystemTheme.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QPalette>
#include <QStandardPaths>
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
  QString accentSoft;  // accent at low alpha (selection wash)
  QString control;     // raised button fill
  QString controlHover;
  QString controlBorder;
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

// Write a checkmark SVG (in the given colour) to the cache and return its path,
// so checked checkboxes show a real tick rather than a solid fill. The colour
// is in the filename so a light/dark switch doesn't hit Qt's image cache.
QString checkIconPath(const QColor &color)
{
  const QString hexName = color.name(QColor::HexRgb).mid(1);
  const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
  QDir().mkpath(dir);
  const QString path = dir + QStringLiteral("/deskflow-check-%1.svg").arg(hexName);
  if (!QFile::exists(path)) {
    const QString svg =
        QStringLiteral("<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16'>"
                       "<path d='M3.5 8.5 l3 3 l6 -7.2' fill='none' stroke='%1' stroke-width='2.2' "
                       "stroke-linecap='round' stroke-linejoin='round'/></svg>")
            .arg(color.name(QColor::HexRgb));
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
      f.write(svg.toUtf8());
      f.close();
    }
  }
  return path;
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
    // Yaru dark
    p.bg = "#1e1e1e";
    p.surface = "#303030";
    p.surfaceAlt = "#282828";
    p.border = "#3d3d3d";
    p.borderStrong = "#4d4d4d";
    p.text = "#ffffff";
    p.textDim = "#b0b0b0";
    p.hover = "rgba(255,255,255,0.07)";
    p.control = "#3c3c3c";
    p.controlHover = "#474747";
    p.controlBorder = "#4a4a4a";
  } else {
    // Yaru light
    p.bg = "#f6f6f6";
    p.surface = "#ffffff";
    p.surfaceAlt = "#fafafa";
    p.border = "#e6e6e6";
    p.borderStrong = "#d0d0d0";
    p.text = "#2c2c2c";
    p.textDim = "#5c5c5c";
    p.hover = "rgba(0,0,0,0.045)";
    p.control = "#ececec";
    p.controlHover = "#e0e0e0";
    p.controlBorder = "#d4d4d4";
  }
  p.accent = hex(accent);
  p.accentText = isLight(accent) ? QStringLiteral("#1d1d1f") : QStringLiteral("#ffffff");
  p.accentHover = hex(dark ? accent.lighter(115) : accent.darker(108));
  p.accentSoft = QStringLiteral("rgba(%1,%2,%3,%4)")
                     .arg(accent.red())
                     .arg(accent.green())
                     .arg(accent.blue())
                     .arg(dark ? QStringLiteral("0.22") : QStringLiteral("0.14"));

  static const char *tpl = R"QSS(
* {
  font-family: "Ubuntu", "Inter", "SF Pro Text", "Segoe UI", system-ui, sans-serif;
  font-size: 14px;
  color: %TEXT%;
  outline: 0;
}
QMainWindow, QDialog { background: %BG%; }
QWidget#shell, QStackedWidget, QWidget#page { background: %BG%; }
/* card-internal containers show the card surface behind them */
QWidget#widget, QWidget#widgetModeSelection, QWidget#widgetModeOptions,
QWidget#serverOptions, QWidget#clientOptions, QWidget#horizontalWidget { background: transparent; }

/* ---- Sidebar ---- */
QWidget#sidebar { background: %BG%; border-right: 1px solid %BORDER%; }
QLabel#brand { color: %TEXT%; font-size: 19px; font-weight: 700; padding: 2px 10px 4px 10px; }
QWidget#nav { background: transparent; }
QPushButton#navItem {
  background: transparent; color: %TEXTDIM%; border: 0; border-radius: 9px;
  padding: 9px 12px; text-align: left; min-height: 20px; font-weight: 500;
}
QPushButton#navItem:hover { background: %HOVER%; color: %TEXT%; }
QPushButton#navItem:checked { background: %ACCENTSOFT%; color: %TEXT%; font-weight: 700; }
QToolButton#menuButton {
  background: transparent; color: %TEXTDIM%; border: 0; border-radius: 9px;
  padding: 8px 12px; text-align: left;
}
QToolButton#menuButton:hover { background: %HOVER%; color: %TEXT%; }
QToolButton#menuButton::menu-indicator { image: none; width: 0; }

/* ---- Content ---- */
QLabel#pageTitle { color: %TEXT%; font-size: 26px; font-weight: 700; }
QLabel#cardTitle { color: %TEXT%; font-size: 15px; font-weight: 700; }
QFrame#card { background: %SURFACE%; border: 1px solid %BORDER%; border-radius: 12px; }

/* group boxes are now plain containers inside cards */
QGroupBox { background: transparent; border: 0; margin: 0; padding: 0; }
QGroupBox::title { subcontrol-origin: margin; color: %TEXTDIM%; }

QMenuBar { background: %BG%; padding: 4px 6px; }
QMenuBar::item { background: transparent; padding: 5px 12px; border-radius: 7px; }
QMenuBar::item:selected { background: %HOVER%; }
QMenu {
  background: %SURFACE%; border: 1px solid %BORDER%; border-radius: 10px; padding: 6px;
}
QMenu::item { padding: 7px 26px 7px 22px; border-radius: 6px; }
QMenu::item:selected { background: %ACCENT%; color: %ACCENTTEXT%; }
QMenu::separator { height: 1px; background: %BORDER%; margin: 5px 8px; }

QLabel { background: transparent; color: %TEXT%; }
QLabel[dim="true"] { color: %TEXTDIM%; }

QPushButton {
  background: %CONTROL%; color: %TEXT%;
  border: none; border-radius: 16px;
  padding: 0 18px; min-height: 34px; font-weight: 600;
}
QPushButton:hover { background: %CONTROLHOVER%; }
QPushButton:pressed { background: %CONTROLBORDER%; }
QPushButton:disabled { background: %SURFACEALT%; color: %TEXTDIM%; }
QPushButton:default, QPushButton[accent="true"] {
  background: %ACCENT%; color: %ACCENTTEXT%; border: none; font-weight: 600;
}
QPushButton:default:hover, QPushButton[accent="true"]:hover { background: %ACCENTHOVER%; border-color: %ACCENTHOVER%; }

QLineEdit, QComboBox, QSpinBox, QAbstractSpinBox {
  background: %SURFACE%; color: %TEXT%;
  border: 1px solid %BORDERSTRONG%; border-radius: 8px;
  padding: 0 12px; min-height: 32px;
  selection-background-color: %ACCENT%; selection-color: %ACCENTTEXT%;
}
QPlainTextEdit, QTextEdit {
  background: %SURFACEALT%; color: %TEXT%;
  border: 1px solid %BORDER%; border-radius: 8px;
  padding: 8px 10px; selection-background-color: %ACCENT%; selection-color: %ACCENTTEXT%;
  font-family: "Ubuntu Mono", "SF Mono", "DejaVu Sans Mono", monospace;
}
QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus, QComboBox:focus, QAbstractSpinBox:focus {
  border: 1px solid %ACCENT%;
}
QComboBox::drop-down { border: 0; width: 22px; }
QComboBox QAbstractItemView {
  background: %SURFACE%; border: 1px solid %BORDER%; border-radius: 8px;
  selection-background-color: %ACCENT%; selection-color: %ACCENTTEXT%; padding: 4px;
}

QRadioButton, QCheckBox { background: transparent; spacing: 10px; padding: 6px 0; }
QRadioButton::indicator, QCheckBox::indicator { width: 18px; height: 18px; }
QRadioButton::indicator {
  border: 2px solid %BORDERSTRONG%; border-radius: 9px; background: %SURFACE%;
}
QRadioButton::indicator:hover { border-color: %ACCENT%; }
QRadioButton::indicator:checked {
  border: 2px solid %ACCENT%;
  border-radius: 9px;
  background: qradialgradient(cx:0.5, cy:0.5, radius:0.5, fx:0.5, fy:0.5,
              stop:0 %ACCENT%, stop:0.42 %ACCENT%, stop:0.5 %SURFACE%, stop:1 %SURFACE%);
}
QCheckBox::indicator {
  border: 2px solid %BORDERSTRONG%; border-radius: 6px; background: %SURFACE%;
}
QCheckBox::indicator:checked {
  background: %ACCENT%; border-color: %ACCENT%; image: url("%CHECKICON%");
}
QCheckBox::indicator:disabled { border-color: %BORDER%; }

QScrollArea#pageScroll { background: %BG%; border: 0; }

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
  qss.replace("%CHECKICON%", checkIconPath(QColor(p.accentText)));
  qss.replace("%CONTROLBORDER%", p.controlBorder);
  qss.replace("%CONTROLHOVER%", p.controlHover);
  qss.replace("%CONTROL%", p.control);
  qss.replace("%ACCENTSOFT%", p.accentSoft);
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
