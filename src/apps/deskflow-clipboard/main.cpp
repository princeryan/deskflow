/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 *
 * deskflow-clipboard: native session clipboard helper.
 *
 * Runs inside the user's graphical session and bridges the clipboard between
 * the headless uinput client (over a Unix socket) and the local clipboard.
 *
 * It uses xclip on Xwayland rather than the Wayland clipboard, because on GNOME:
 *   - Wayland gives no clipboard-change events to unfocused apps (no
 *     data-control protocol), and QClipboard only sees changes while focused.
 *   - reading/writing the Wayland selection from a background app momentarily
 *     grabs focus and makes the screen flicker.
 * X11 selections need no focus, mutter's XWayland bridge keeps them in sync with
 * the Wayland clipboard both ways, and xclip does not flicker.
 *
 * Protocol (request/response, driven by the client on screen transitions):
 *   client -> 'R'                        : read the clipboard now
 *   helper -> [4-byte BE len][frame]     : the current clipboard
 *   client -> 'W' [4-byte BE len][frame] : write this to the clipboard now
 * A frame is [1 byte format count] then count x
 *   [1 byte format: 0=Text(UTF-8) 1=HTML(UTF-8) 2=Bitmap(deskflow DIB)]
 *   [4 byte big-endian length][data]  -- matches UInputScreen on the client.
 */

#include <QBuffer>
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSocketNotifier>
#include <QStringList>
#include <QUrl>

#include <arpa/inet.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

std::string socketPath()
{
  const char *rt = std::getenv("XDG_RUNTIME_DIR");
  std::string dir = (rt != nullptr && *rt != '\0') ? rt : "/tmp";
  return dir + "/deskflow-clipboard.sock";
}

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

bool readAll(int fd, char *buf, size_t len)
{
  while (len > 0) {
    ssize_t n = ::read(fd, buf, len);
    if (n <= 0)
      return false;
    buf += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

// xclip needs an X display + auth; supply them even if the service env is bare.
QProcessEnvironment xclipEnv()
{
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  if (env.value(QStringLiteral("DISPLAY")).isEmpty())
    env.insert(QStringLiteral("DISPLAY"), QStringLiteral(":0"));
  if (env.value(QStringLiteral("XAUTHORITY")).isEmpty()) {
    const QString rt = env.value(QStringLiteral("XDG_RUNTIME_DIR"), QStringLiteral("/run/user/1000"));
    QDir d(rt);
    const QStringList files =
        d.entryList(QStringList{QStringLiteral(".mutter-Xwaylandauth.*")}, QDir::Files | QDir::Hidden, QDir::Time);
    if (!files.isEmpty())
      env.insert(QStringLiteral("XAUTHORITY"), d.filePath(files.first()));
  }
  return env;
}

QByteArray xclipRead(const QString &target)
{
  QProcess p;
  p.setProcessEnvironment(xclipEnv());
  p.start(
      QStringLiteral("xclip"),
      {QStringLiteral("-selection"), QStringLiteral("clipboard"), QStringLiteral("-o"), QStringLiteral("-t"), target}
  );
  if (!p.waitForStarted(800))
    return {};
  p.waitForFinished(1200);
  return p.readAllStandardOutput();
}

// Write data to the clipboard. Empty target -> xclip offers all text aliases.
void xclipWrite(const QByteArray &data, const QString &target)
{
  QProcess p;
  p.setProcessEnvironment(xclipEnv());
  QStringList args{QStringLiteral("-selection"), QStringLiteral("clipboard"), QStringLiteral("-i")};
  if (!target.isEmpty())
    args << QStringLiteral("-t") << target;
  p.start(QStringLiteral("xclip"), args);
  if (!p.waitForStarted(800))
    return;
  p.write(data);
  p.closeWriteChannel();
  // xclip forks a daemon to serve the selection and the parent exits; just wait
  // for that parent so we don't leave a zombie.
  p.waitForFinished(1200);
}

void frameAdd(QByteArray &body, std::uint8_t &count, std::uint8_t id, const QByteArray &data)
{
  body.append(static_cast<char>(id));
  std::uint32_t n = htonl(static_cast<std::uint32_t>(data.size()));
  body.append(reinterpret_cast<const char *>(&n), 4);
  body.append(data);
  ++count;
}

// ---- File transfer (Format::File) --------------------------------------------
// Payload: [4B BE count] then per file [4B BE nameLen][name][8B BE size][bytes].

void beAppend32(QByteArray &out, std::uint32_t v)
{
  std::uint32_t n = htonl(v);
  out.append(reinterpret_cast<const char *>(&n), 4);
}
void beAppend64(QByteArray &out, std::uint64_t v)
{
  char b[8];
  for (int i = 0; i < 8; ++i)
    b[i] = static_cast<char>((v >> (56 - 8 * i)) & 0xff);
  out.append(b, 8);
}
std::uint32_t beRead32(const char *p)
{
  std::uint32_t n = 0;
  std::memcpy(&n, p, 4);
  return ntohl(n);
}
std::uint64_t beRead64(const char *p)
{
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i)
    v = (v << 8) | static_cast<std::uint8_t>(p[i]);
  return v;
}

// Read the files the clipboard points at into a File payload (empty if none).
QByteArray readClipboardFiles()
{
  const QByteArray gcf = xclipRead(QStringLiteral("x-special/gnome-copied-files"));
  QStringList paths;
  if (!gcf.isEmpty()) {
    // First line is the operation ("copy"/"cut"); the rest are file:// URIs.
    const QList<QByteArray> lines = gcf.split('\n');
    for (int i = 1; i < lines.size(); ++i) {
      const QByteArray line = lines[i].trimmed();
      if (line.isEmpty())
        continue;
      const QString path = QUrl(QString::fromUtf8(line)).toLocalFile();
      if (!path.isEmpty())
        paths << path;
    }
  }
  if (paths.isEmpty())
    return {};

  QByteArray body;
  std::uint32_t count = 0;
  for (const QString &p : paths) {
    QFileInfo fi(p);
    if (!fi.isFile())
      continue; // directories not supported yet
    QFile f(p);
    if (!f.open(QIODevice::ReadOnly))
      continue;
    const QByteArray data = f.readAll();
    const QByteArray name = fi.fileName().toUtf8();
    beAppend32(body, static_cast<std::uint32_t>(name.size()));
    body.append(name);
    beAppend64(body, static_cast<std::uint64_t>(data.size()));
    body.append(data);
    ++count;
  }
  if (count == 0)
    return {};
  QByteArray out;
  beAppend32(out, count);
  out.append(body);
  return out;
}

// Write a received File payload to a temp dir and point the clipboard at it, so
// a file manager pastes real files.
void writeClipboardFiles(const QByteArray &payload)
{
  if (payload.size() < 4)
    return;
  int pos = 0;
  const std::uint32_t count = beRead32(payload.constData() + pos);
  pos += 4;

  const QString rt =
      QProcessEnvironment::systemEnvironment().value(QStringLiteral("XDG_RUNTIME_DIR"), QStringLiteral("/tmp"));
  QDir base(rt + QStringLiteral("/deskflow-files"));
  if (base.exists())
    base.removeRecursively(); // don't accumulate old received files
  QDir().mkpath(base.absolutePath());

  QStringList uris;
  for (std::uint32_t i = 0; i < count; ++i) {
    if (pos + 4 > payload.size())
      break;
    const std::uint32_t nameLen = beRead32(payload.constData() + pos);
    pos += 4;
    if (pos + static_cast<int>(nameLen) + 8 > payload.size())
      break;
    const QString name = QString::fromUtf8(payload.mid(pos, static_cast<int>(nameLen)));
    pos += static_cast<int>(nameLen);
    const std::uint64_t size = beRead64(payload.constData() + pos);
    pos += 8;
    if (static_cast<std::uint64_t>(pos) + size > static_cast<std::uint64_t>(payload.size()))
      break;
    const QByteArray data = payload.mid(pos, static_cast<int>(size));
    pos += static_cast<int>(size);

    const QString safe = QFileInfo(name).fileName(); // basename only (no traversal)
    if (safe.isEmpty())
      continue;
    const QString outPath = base.absoluteFilePath(safe);
    QFile out(outPath);
    if (out.open(QIODevice::WriteOnly)) {
      out.write(data);
      out.close();
      uris << QUrl::fromLocalFile(outPath).toString();
    }
  }
  if (uris.isEmpty())
    return;
  const QByteArray gcf = QByteArray("copy\n") + uris.join('\n').toUtf8();
  xclipWrite(gcf, QStringLiteral("x-special/gnome-copied-files"));
}

// Read the clipboard (all supported formats) into a frame.
QByteArray buildReadFrame()
{
  const QByteArray targetsRaw = xclipRead(QStringLiteral("TARGETS"));
  const QList<QByteArray> targets = targetsRaw.split('\n');
  auto has = [&](const char *t) { return targets.contains(QByteArray(t)); };

  QByteArray body;
  std::uint8_t count = 0;

  // Files (a file-manager copy). If present it's the whole intent, so send only
  // the File format and skip text/image.
  if (has("x-special/gnome-copied-files")) {
    const QByteArray files = readClipboardFiles();
    if (!files.isEmpty()) {
      frameAdd(body, count, 3, files);
      QByteArray out;
      out.append(static_cast<char>(count));
      out.append(body);
      return out;
    }
  }

  // Text: prefer UTF-8 aliases.
  QByteArray text;
  if (has("UTF8_STRING"))
    text = xclipRead(QStringLiteral("UTF8_STRING"));
  else if (has("text/plain;charset=utf-8"))
    text = xclipRead(QStringLiteral("text/plain;charset=utf-8"));
  else if (has("STRING"))
    text = xclipRead(QStringLiteral("STRING"));
  else if (has("text/plain"))
    text = xclipRead(QStringLiteral("text/plain"));
  if (!text.isEmpty())
    frameAdd(body, count, 0, text);

  if (has("text/html")) {
    const QByteArray html = xclipRead(QStringLiteral("text/html"));
    if (!html.isEmpty())
      frameAdd(body, count, 1, html);
  }

  if (has("image/png")) {
    const QByteArray png = xclipRead(QStringLiteral("image/png"));
    QImage img;
    if (img.loadFromData(png, "PNG")) {
      QByteArray bmp;
      QBuffer buf(&bmp);
      buf.open(QIODevice::WriteOnly);
      if (img.save(&buf, "BMP") && bmp.size() > 14)
        frameAdd(body, count, 2, bmp.mid(14)); // strip 14-byte BITMAPFILEHEADER -> DIB
    }
  }

  QByteArray out;
  out.append(static_cast<char>(count));
  out.append(body);
  return out;
}

// Write a frame to the clipboard (server -> local). xclip owns one selection, so
// we set the single most useful format: image, else text, else html.
void applyWriteFrame(const QByteArray &frame)
{
  int pos = 0;
  if (frame.size() < 1)
    return;
  const int count = static_cast<std::uint8_t>(frame[pos++]);
  QByteArray text;
  QByteArray html;
  QByteArray dib;
  QByteArray files;
  for (int i = 0; i < count && pos + 5 <= frame.size(); ++i) {
    const int id = static_cast<std::uint8_t>(frame[pos++]);
    std::uint32_t n = 0;
    std::memcpy(&n, frame.constData() + pos, 4);
    n = ntohl(n);
    pos += 4;
    if (pos + static_cast<int>(n) > frame.size())
      break;
    const QByteArray data = frame.mid(pos, static_cast<int>(n));
    pos += static_cast<int>(n);
    if (id == 0)
      text = data;
    else if (id == 1)
      html = data;
    else if (id == 2)
      dib = data;
    else if (id == 3)
      files = data;
  }

  // Files take priority: reconstruct them and point the clipboard at real files.
  if (!files.isEmpty()) {
    writeClipboardFiles(files);
    return;
  }

  if (!dib.isEmpty()) {
    // deskflow DIB -> BMP -> QImage -> PNG for xclip.
    const std::uint32_t fileSize = 14 + static_cast<std::uint32_t>(dib.size());
    const std::uint32_t dataOffset = 14 + 40;
    QByteArray bmp;
    bmp.append("BM");
    auto le32 = [&](std::uint32_t v) {
      char b[4] = {static_cast<char>(v & 0xff), static_cast<char>((v >> 8) & 0xff), static_cast<char>((v >> 16) & 0xff),
                   static_cast<char>((v >> 24) & 0xff)};
      bmp.append(b, 4);
    };
    le32(fileSize);
    le32(0);
    le32(dataOffset);
    bmp.append(dib);
    QImage img;
    if (img.loadFromData(bmp, "BMP")) {
      QByteArray png;
      QBuffer buf(&png);
      buf.open(QIODevice::WriteOnly);
      img.save(&buf, "PNG");
      xclipWrite(png, QStringLiteral("image/png"));
      return;
    }
  }
  if (!text.isEmpty())
    xclipWrite(text, QString()); // no -t: offer STRING/UTF8_STRING/TEXT/text/plain
  else if (!html.isEmpty())
    xclipWrite(html, QStringLiteral("text/html"));
}

struct Bridge
{
  int clientFd = -1;
  QSocketNotifier *clientNotifier = nullptr;

  void dropClient()
  {
    if (clientNotifier != nullptr) {
      clientNotifier->setEnabled(false);
      clientNotifier->deleteLater();
      clientNotifier = nullptr;
    }
    if (clientFd >= 0) {
      ::close(clientFd);
      clientFd = -1;
    }
  }

  void handleRequest()
  {
    char cmd = 0;
    if (!readAll(clientFd, &cmd, 1)) {
      dropClient();
      return;
    }
    if (cmd == 'R') {
      const QByteArray frame = buildReadFrame();
      std::uint32_t len = htonl(static_cast<std::uint32_t>(frame.size()));
      if (!writeAll(clientFd, reinterpret_cast<const char *>(&len), 4) ||
          !writeAll(clientFd, frame.constData(), frame.size()))
        dropClient();
    } else if (cmd == 'W') {
      std::uint32_t nlen = 0;
      if (!readAll(clientFd, reinterpret_cast<char *>(&nlen), 4)) {
        dropClient();
        return;
      }
      std::uint32_t len = ntohl(nlen);
      QByteArray data(static_cast<int>(len), '\0');
      if (len > 0 && !readAll(clientFd, data.data(), len)) {
        dropClient();
        return;
      }
      applyWriteFrame(data);
    } else {
      dropClient();
    }
  }
};

} // namespace

int main(int argc, char **argv)
{
  // No GUI surface (offscreen) -> no Wayland window, no flicker; QImage still works.
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QGuiApplication app(argc, argv);
  QGuiApplication::setApplicationName(QStringLiteral("deskflow-clipboard"));

  const std::string path = socketPath();
  ::unlink(path.c_str());

  int listenFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (listenFd < 0) {
    qCritical("deskflow-clipboard: cannot create socket: %s", std::strerror(errno));
    return 1;
  }
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  if (::bind(listenFd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || ::listen(listenFd, 1) != 0) {
    qCritical("deskflow-clipboard: cannot listen on %s: %s", path.c_str(), std::strerror(errno));
    return 1;
  }

  auto *bridge = new Bridge;

  auto *acceptNotifier = new QSocketNotifier(listenFd, QSocketNotifier::Read, &app);
  QObject::connect(acceptNotifier, &QSocketNotifier::activated, &app, [bridge, listenFd] {
    int fd = ::accept(listenFd, nullptr, nullptr);
    if (fd < 0)
      return;
    bridge->dropClient();
    bridge->clientFd = fd;
    bridge->clientNotifier = new QSocketNotifier(fd, QSocketNotifier::Read, QCoreApplication::instance());
    QObject::connect(bridge->clientNotifier, &QSocketNotifier::activated, bridge->clientNotifier, [bridge] {
      bridge->handleRequest();
    });
  });

  return QGuiApplication::exec();
}
