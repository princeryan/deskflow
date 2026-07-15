/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/UInputClipboardBridge.h"

#include "base/Log.h"

#include <arpa/inet.h> // htonl/ntohl
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace deskflow {

namespace {

bool writeAll(int fd, const char *buf, size_t len)
{
  while (len > 0) {
    ssize_t n = ::write(fd, buf, len);
    if (n <= 0)
      return false;
    buf += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

// Read exactly len bytes, giving up after timeoutMs of no progress.
bool readAll(int fd, char *buf, size_t len, int timeoutMs)
{
  while (len > 0) {
    pollfd pfd{fd, POLLIN, 0};
    int pr = ::poll(&pfd, 1, timeoutMs);
    if (pr <= 0)
      return false; // timeout or error
    if ((pfd.revents & (POLLHUP | POLLERR)) != 0)
      return false;
    ssize_t n = ::read(fd, buf, len);
    if (n <= 0)
      return false;
    buf += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

} // namespace

UInputClipboardBridge::~UInputClipboardBridge()
{
  stop();
}

void UInputClipboardBridge::stop()
{
  std::scoped_lock<std::mutex> lock(m_mutex);
  dropConnection();
}

std::string UInputClipboardBridge::socketPath()
{
  // The client runs from a system unit, which starts with no XDG_RUNTIME_DIR; the
  // helper runs in the user session, which always has one. Both must resolve to the
  // same per-user runtime dir or they bind and dial different sockets and the
  // clipboard silently does nothing.
  const char *rt = std::getenv("XDG_RUNTIME_DIR");
  std::string dir = (rt != nullptr && *rt != '\0') ? rt : "/run/user/" + std::to_string(::getuid());
  return dir + "/deskflow-clipboard.sock";
}

void UInputClipboardBridge::dropConnection()
{
  if (m_fd >= 0) {
    ::close(m_fd);
    m_fd = -1;
  }
}

bool UInputClipboardBridge::ensureConnected()
{
  if (m_fd >= 0)
    return true;

  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return false;

  const std::string path = socketPath();
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

  if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    // Helper not running yet (e.g. before login).
    ::close(fd);
    return false;
  }
  m_fd = fd;
  return true;
}

std::string UInputClipboardBridge::read()
{
  std::scoped_lock<std::mutex> lock(m_mutex);
  if (!ensureConnected())
    return {};

  const char cmd = 'R';
  if (!writeAll(m_fd, &cmd, 1)) {
    dropConnection();
    return {};
  }

  std::uint32_t nlen = 0;
  if (!readAll(m_fd, reinterpret_cast<char *>(&nlen), 4, 8000)) {
    dropConnection();
    return {};
  }
  std::uint32_t len = ntohl(nlen);
  std::string frame(len, '\0');
  if (len > 0 && !readAll(m_fd, frame.data(), len, 8000)) {
    dropConnection();
    return {};
  }
  return frame;
}

void UInputClipboardBridge::write(const std::string &frame)
{
  std::scoped_lock<std::mutex> lock(m_mutex);
  if (!ensureConnected())
    return;

  const char cmd = 'W';
  std::uint32_t nlen = htonl(static_cast<std::uint32_t>(frame.size()));
  if (!writeAll(m_fd, &cmd, 1) || !writeAll(m_fd, reinterpret_cast<const char *>(&nlen), 4) ||
      !writeAll(m_fd, frame.data(), frame.size())) {
    dropConnection();
  }
}

} // namespace deskflow
