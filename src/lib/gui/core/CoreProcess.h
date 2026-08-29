/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2024 - 2025 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "common/Enums.h"
#include "common/Settings.h"
#include "gui/FileTail.h"
#include "gui/config/ServerConfig.h"

#include <QMutex>
#include <QObject>
#include <QProcess>
#include <QTimer>

namespace deskflow::gui {

namespace ipc {
class CoreIpcClient;
class DaemonIpcClient;
} // namespace ipc

class CoreProcess : public QObject
{
  using ConnectionState = deskflow::core::ConnectionState;
  using ProcessMode = Settings::ProcessMode;
  using ProcessState = deskflow::core::ProcessState;
  Q_OBJECT

public:
  enum class Error
  {
    AddressMissing,
    StartFailed
  };

  explicit CoreProcess(const ServerConfig &serverConfig);

  void start(std::optional<ProcessMode> processMode = std::nullopt);
  void stop(std::optional<ProcessMode> processMode = std::nullopt);
  void restart();
  void cleanup();
  void applyLogLevel();
  void clearSettings();
  void retryDaemon();

  // getters
  Settings::CoreMode mode() const
  {
    return m_mode;
  }
  QString secureSocketVersion() const
  {
    return m_secureSocketVersion;
  }
  bool isStarted() const
  {
    return m_processState == ProcessState::Started;
  }
  /**
   * @brief True while a core is meant to be running: starting, started, or
   * waiting on a retry after an exit.
   *
   * isStarted() answers the strict "is it up this instant" question. Callers
   * deciding whether the user may stop the core, or whether it should come back
   * on the next launch, need the wider one: without it a core that is starting
   * or churning through retries is indistinguishable from one the user
   * deliberately stopped.
   */
  bool isActive() const
  {
    using enum ProcessState;
    return m_processState == Starting || m_processState == Started || m_processState == RetryPending;
  }
  ProcessState processState() const
  {
    return m_processState;
  }
  ConnectionState connectionState() const
  {
    return m_connectionState;
  }

  // setters
  void setAddress(const QString &address)
  {
    m_address = correctedAddress(address);
  }
  void setMode(Settings::CoreMode mode)
  {
    m_mode = mode;
  }

Q_SIGNALS:
  void error(deskflow::gui::CoreProcess::Error error);
  void logLine(const QString &line);
  void connectionStateChanged(deskflow::core::ConnectionState state);
  void processStateChanged(deskflow::core::ProcessState state);
  void secureSocket(bool enabled);
  void daemonIpcClientConnectionFailed();
  void connectedClientsChanged(const QStringList &clients);
  void securityLevelChanged(QString securityLevel);
  void unrecognisedClient(const QString &clientName);
  void connectionRefused(deskflow::core::ConnectionRefusal reason);
  void retryIn(int seconds);
  void peerFingerprint(const QString &fingerprint);
  void missingKeyboardLayouts(const QString &layouts);

private Q_SLOTS:
  void onProcessFinished(int exitCode, QProcess::ExitStatus);
  void onProcessReadyReadStandardOutput();
  void onProcessReadyReadStandardError();
  void onCoreIpcMessageReceived(const QString &command, const QString &args);
  void daemonIpcClientConnected();

private:
  void startForegroundProcess(const QStringList &args);
  void startProcessFromDaemon();
  void stopForegroundProcess();
  void stopProcessFromDaemon();
  // Drive the headless login-screen client service instead of launching our own
  // core, so the GUI is a front-end for the running service.
  void startExternalService();
  void stopExternalService();
  // Attach to a service that is already running: tail its journal and reflect
  // its state, without restarting it and dropping the user's connection.
  void adoptExternalService();
  void startJournalTail();
  bool externalServiceIsActive() const;
  static QString resolveServiceUnit();
  static bool unitIsLoaded(const QString &unit);
  QPair<bool, QString> persistServerConfig() const;
  void setConnectionState(ConnectionState state);
  void setProcessState(ProcessState state);
  bool checkSecureSocket(const QString &line);
  void handleLogLines(const QString &text);
  QString correctedAddress(const QString &address) const;
  void setupDaemonLogTail(const QString &logPath);
  void checkExistingProcess();
  static QString makeQuotedArgs(const QString &app, const QStringList &args);
  static QString processModeToString(const Settings::ProcessMode mode);
  static QString processStateToString(const CoreProcess::ProcessState state);
  static QString wrapIpv6(const QString &address);

  const ServerConfig &m_serverConfig;
  QString m_address;
  ProcessState m_processState = ProcessState::Stopped;
  ConnectionState m_connectionState = ConnectionState::Disconnected;
  Settings::CoreMode m_mode = Settings::CoreMode::None;
  QMutex m_processMutex;
  QString m_secureSocketVersion;
  std::optional<ProcessMode> m_lastProcessMode = std::nullopt;
  QTimer m_retryTimer;
  deskflow::gui::ipc::CoreIpcClient *m_coreIpcClient = nullptr;
  deskflow::gui::ipc::DaemonIpcClient *m_daemonIpcClient = nullptr;
  FileTail *m_daemonFileTail = nullptr;
  QProcess *m_process = nullptr;
  QString m_appPath;

  // External-service mode (Linux headless login-screen client).
  bool m_externalService = false;
  QString m_serviceUnit;
  QProcess *m_journalTail = nullptr;
};

} // namespace deskflow::gui
