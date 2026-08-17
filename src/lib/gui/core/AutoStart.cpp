// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT

#include "core/AutoStart.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

#if defined(Q_OS_WIN)
#include <QSettings>
#endif

namespace deskflow::gui {

namespace {

const auto kAppId = QStringLiteral("org.deskflow.deskflow");
const auto kAppName = QStringLiteral("Deskflow");

#if defined(Q_OS_WIN)
const auto kRunKey = QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run");
#endif

#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
QString entryPath()
{
  return QDir::homePath() + QStringLiteral("/.config/autostart/") + kAppId + QStringLiteral(".desktop");
}
#elif defined(Q_OS_MACOS)
QString entryPath()
{
  return QDir::homePath() + QStringLiteral("/Library/LaunchAgents/") + kAppId + QStringLiteral(".plist");
}
#endif

} // namespace

bool AutoStart::isSupported()
{
#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD) || defined(Q_OS_WIN) || defined(Q_OS_MACOS)
  return true;
#else
  return false;
#endif
}

bool AutoStart::isEnabled()
{
#if defined(Q_OS_WIN)
  QSettings run(kRunKey, QSettings::NativeFormat);
  return run.contains(kAppName);
#elif defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD) || defined(Q_OS_MACOS)
  return QFile::exists(entryPath());
#else
  return false;
#endif
}

bool AutoStart::setEnabled(bool enabled)
{
  const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());

#if defined(Q_OS_WIN)
  QSettings run(kRunKey, QSettings::NativeFormat);
  if (enabled)
    run.setValue(kAppName, QStringLiteral("\"%1\"").arg(exe));
  else
    run.remove(kAppName);
  run.sync();
  return run.status() == QSettings::NoError;

#elif defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD) || defined(Q_OS_MACOS)
  const QString path = entryPath();

  if (!enabled)
    return !QFile::exists(path) || QFile::remove(path);

  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
    return false;

  QTextStream out(&file);
#if defined(Q_OS_MACOS)
  out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
         "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
      << "<plist version=\"1.0\">\n<dict>\n"
      << "  <key>Label</key><string>" << kAppId << "</string>\n"
      << "  <key>ProgramArguments</key><array><string>" << exe << "</string></array>\n"
      << "  <key>RunAtLoad</key><true/>\n"
      << "</dict>\n</plist>\n";
#else
  out << "[Desktop Entry]\n"
      << "Type=Application\n"
      << "Version=1.0\n"
      << "Name=" << kAppName << "\n"
      << "Comment=Mouse and keyboard sharing utility\n"
      << "Exec=" << exe << "\n"
      << "Icon=" << kAppId << "\n"
      << "Terminal=false\n"
      << "Categories=Utility;\n"
      << "X-GNOME-Autostart-enabled=true\n";
#endif
  file.close();
  return file.error() == QFile::NoError;

#else
  Q_UNUSED(enabled)
  Q_UNUSED(exe)
  return false;
#endif
}

} // namespace deskflow::gui
