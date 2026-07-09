/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/MSWindowsClipboardFileConverter.h"

#include "base/Log.h"

#include <algorithm>
#include <cstdint>
#include <shlobj.h>
#include <string>
#include <vector>
#include <windows.h>

namespace {

// Big-endian (de)serialization -- must match the Linux deskflow-clipboard helper.
void beAppend32(std::string &out, std::uint32_t v)
{
  const char b[4] = {
      static_cast<char>((v >> 24) & 0xff), static_cast<char>((v >> 16) & 0xff), static_cast<char>((v >> 8) & 0xff),
      static_cast<char>(v & 0xff)
  };
  out.append(b, 4);
}
void beAppend64(std::string &out, std::uint64_t v)
{
  char b[8];
  for (int i = 0; i < 8; ++i)
    b[i] = static_cast<char>((v >> (56 - 8 * i)) & 0xff);
  out.append(b, 8);
}
std::uint32_t beRead32(const std::string &d, std::size_t pos)
{
  return (static_cast<std::uint32_t>(static_cast<std::uint8_t>(d[pos])) << 24) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(d[pos + 1])) << 16) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(d[pos + 2])) << 8) |
         static_cast<std::uint32_t>(static_cast<std::uint8_t>(d[pos + 3]));
}
std::uint64_t beRead64(const std::string &d, std::size_t pos)
{
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i)
    v = (v << 8) | static_cast<std::uint8_t>(d[pos + i]);
  return v;
}

std::wstring utf8ToWide(const std::string &s)
{
  if (s.empty())
    return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}
std::string wideToUtf8(const std::wstring &w)
{
  if (w.empty())
    return std::string();
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}
std::wstring baseName(const std::wstring &p)
{
  const size_t slash = p.find_last_of(L"\\/");
  return (slash == std::wstring::npos) ? p : p.substr(slash + 1);
}

std::wstring receiveDir()
{
  wchar_t buf[MAX_PATH];
  const DWORD n = GetTempPathW(MAX_PATH, buf);
  std::wstring dir(buf, n);
  dir += L"deskflow-files";
  return dir; // no trailing slash
}

bool writeWholeFile(const std::wstring &path, const char *data, std::uint64_t size)
{
  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE)
    return false;
  std::uint64_t off = 0;
  bool ok = true;
  while (off < size) {
    const DWORD chunk = static_cast<DWORD>(std::min<std::uint64_t>(size - off, 1u << 20));
    DWORD wrote = 0;
    if (!WriteFile(h, data + off, chunk, &wrote, nullptr) || wrote != chunk) {
      ok = false;
      break;
    }
    off += wrote;
  }
  CloseHandle(h);
  return ok;
}

bool readWholeFile(const std::wstring &path, std::string &out)
{
  HANDLE h =
      CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE)
    return false;
  LARGE_INTEGER sz;
  if (!GetFileSizeEx(h, &sz)) {
    CloseHandle(h);
    return false;
  }
  out.resize(static_cast<size_t>(sz.QuadPart));
  std::uint64_t off = 0;
  bool ok = true;
  while (off < static_cast<std::uint64_t>(sz.QuadPart)) {
    const DWORD chunk = static_cast<DWORD>(std::min<std::uint64_t>(static_cast<std::uint64_t>(sz.QuadPart) - off, 1u << 20));
    DWORD got = 0;
    if (!ReadFile(h, out.data() + off, chunk, &got, nullptr) || got == 0) {
      ok = false;
      break;
    }
    off += got;
  }
  CloseHandle(h);
  return ok;
}

} // namespace

IClipboard::Format MSWindowsClipboardFileConverter::getFormat() const
{
  return IClipboard::Format::File;
}

UINT MSWindowsClipboardFileConverter::getWin32Format() const
{
  return CF_HDROP;
}

// deskflow File payload -> CF_HDROP: write files to a temp dir, point CF_HDROP at them.
HANDLE MSWindowsClipboardFileConverter::fromIClipboard(const std::string &data) const
{
  if (data.size() < 4)
    return nullptr;
  std::size_t pos = 0;
  const std::uint32_t count = beRead32(data, pos);
  pos += 4;
  LOG_DEBUG("file converter: writing %u received file(s), %zu payload bytes", count, data.size());

  const std::wstring dir = receiveDir();
  CreateDirectoryW(dir.c_str(), nullptr); // ok if it already exists
  const std::wstring prefix = dir + L"\\";

  std::vector<std::wstring> paths;
  for (std::uint32_t i = 0; i < count; ++i) {
    if (pos + 4 > data.size())
      break;
    const std::uint32_t nameLen = beRead32(data, pos);
    pos += 4;
    if (pos + nameLen + 8 > data.size())
      break;
    const std::string nameUtf8 = data.substr(pos, nameLen);
    pos += nameLen;
    const std::uint64_t size = beRead64(data, pos);
    pos += 8;
    if (pos + size > data.size())
      break;

    std::wstring name = baseName(utf8ToWide(nameUtf8));
    if (name.empty()) {
      pos += static_cast<std::size_t>(size);
      continue;
    }
    const std::wstring full = prefix + name;
    if (writeWholeFile(full, data.data() + pos, size))
      paths.push_back(full);
    pos += static_cast<std::size_t>(size);
  }
  if (paths.empty()) {
    LOG_WARN("file converter: could not write any received files to %ls", dir.c_str());
    return nullptr;
  }

  // Build CF_HDROP: DROPFILES header + double-null-terminated wide path list.
  std::size_t listChars = 1; // trailing extra null
  for (const auto &p : paths)
    listChars += p.size() + 1;
  const std::size_t bytes = sizeof(DROPFILES) + listChars * sizeof(wchar_t);

  HGLOBAL gData = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, bytes);
  if (gData == nullptr)
    return nullptr;
  char *base = static_cast<char *>(GlobalLock(gData));
  if (base == nullptr) {
    GlobalFree(gData);
    return nullptr;
  }
  auto *df = reinterpret_cast<DROPFILES *>(base);
  df->pFiles = sizeof(DROPFILES);
  df->pt.x = 0;
  df->pt.y = 0;
  df->fNC = FALSE;
  df->fWide = TRUE;
  auto *list = reinterpret_cast<wchar_t *>(base + sizeof(DROPFILES));
  for (const auto &p : paths) {
    std::memcpy(list, p.c_str(), (p.size() + 1) * sizeof(wchar_t));
    list += p.size() + 1;
  }
  *list = L'\0';
  GlobalUnlock(gData);
  return gData;
}

// CF_HDROP -> deskflow File payload: read the referenced files' bytes.
std::string MSWindowsClipboardFileConverter::toIClipboard(HANDLE data) const
{
  auto drop = static_cast<HDROP>(data);
  const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
  LOG_DEBUG("file converter: reading %u file(s) from CF_HDROP", n);
  if (n == 0)
    return std::string();

  std::string body;
  std::uint32_t count = 0;
  for (UINT i = 0; i < n; ++i) {
    const UINT len = DragQueryFileW(drop, i, nullptr, 0); // length excluding null
    std::wstring path(len + 1, L'\0');                    // room for the null
    if (DragQueryFileW(drop, i, path.data(), len + 1) == 0)
      continue;
    path.resize(len);

    std::string bytes;
    if (!readWholeFile(path, bytes)) // skip directories / unreadable entries
      continue;
    const std::string nameUtf8 = wideToUtf8(baseName(path));
    beAppend32(body, static_cast<std::uint32_t>(nameUtf8.size()));
    body.append(nameUtf8);
    beAppend64(body, static_cast<std::uint64_t>(bytes.size()));
    body.append(bytes);
    ++count;
  }
  if (count == 0)
    return std::string();

  std::string out;
  beAppend32(out, count);
  out += body;
  return out;
}
