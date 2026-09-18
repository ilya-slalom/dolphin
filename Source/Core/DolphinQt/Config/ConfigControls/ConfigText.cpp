// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Config/ConfigControls/ConfigText.h"

#include <QLineEdit>

ConfigText::ConfigText(const Config::Info<std::string>& setting) : ConfigText(setting, nullptr)
{
}

ConfigText::ConfigText(const Config::Info<std::string>& setting, Config::Layer* layer)
    : ConfigControl(setting.GetLocation(), layer), m_setting(setting)
{
  setText(QString::fromStdString(ReadValue(setting)));

  connect(this, &QLineEdit::editingFinished, this, &ConfigText::Update);
}

void ConfigText::SetTextAndUpdate(const QString& text)
{
  if (text == this->text())
    return;

  setText(text);
  // Deliberately not Update(): that is the editingFinished path, and it refuses to write a
  // read-only control. This is the opposite case -- an explicit programmatic set, which is the only
  // way a read-only field is *meant* to be written -- so it saves unconditionally.
  SaveValue(m_setting, text.toStdString());
}

void ConfigText::Update()
{
  // editingFinished fires on focus-out and on window close, and setReadOnly() leaves the field
  // focusable (Qt keeps StrongFocus), so a read-only field that was clicked once writes its
  // displayed text back as soon as the window closes. That text was never typed: it is whatever
  // some other code put there to display. A read-only control is an output, so it must never be the
  // source of a write -- on a layer-bound control (Game Properties passes one) the write would
  // create a per-game override for a value the user never edited, bolded and needing a right-click
  // to clear. SetTextAndUpdate above is the write path for those fields.
  if (isReadOnly())
    return;

  SaveValue(m_setting, text().toStdString());
}

void ConfigText::OnConfigChanged()
{
  setText(QString::fromStdString(ReadValue(m_setting)));
}
