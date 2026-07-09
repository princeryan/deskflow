/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "platform/MSWindowsClipboard.h"

//! Convert to/from CF_HDROP for cross-machine file transfer.
/*!
Bridges the deskflow File clipboard format (files serialized with their bytes)
and the Windows CF_HDROP format used by Explorer. Copying files puts CF_HDROP on
the clipboard; toIClipboard() reads the referenced files' bytes into the File
payload so they travel to the other machine. fromIClipboard() writes the received
files to a temp folder and builds a CF_HDROP pointing at them, so they paste as
real files.

Payload (must match the deskflow-clipboard helper on Linux):
  [4B BE count] then per file [4B BE nameLen][name UTF-8][8B BE size][bytes]
*/
class MSWindowsClipboardFileConverter : public IMSWindowsClipboardConverter
{
public:
  MSWindowsClipboardFileConverter() = default;
  ~MSWindowsClipboardFileConverter() override = default;

  // IMSWindowsClipboardConverter overrides
  IClipboard::Format getFormat() const override;
  UINT getWin32Format() const override;
  HANDLE fromIClipboard(const std::string &) const override;
  std::string toIClipboard(HANDLE) const override;
};
