// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Config/Graphics/ShaderParametersDialog.h"

#include <algorithm>
#include <cmath>

#include <QApplication>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QVBoxLayout>

#include "VideoCommon/PostProcessing/LibrashaderPostProcessing.h"

namespace Parameters = VideoCommon::LibrashaderParameters;

ShaderParametersDialog::ShaderParametersDialog(QWidget* parent, const QString& preset,
                                               bool opened_from_game_properties)
    : QDialog(parent)
{
  const std::string preset_id = preset.toStdString();

  // PCSX2 titles the window with Path::GetFileTitle(preset), the leaf name without its extension.
  // Dolphin's stored preset id carries no extension, so the leaf segment already *is* the file
  // title -- and it is the same string the picker's tree shows as a leaf label.
  const size_t leaf = preset_id.rfind('/');
  const std::string title = leaf == std::string::npos ? preset_id : preset_id.substr(leaf + 1);
  setWindowTitle(tr("Shader Parameters - %1").arg(QString::fromStdString(title)));
  resize(720, 560);

  CreateWidgets(opened_from_game_properties);

  const std::string path = VideoCommon::ResolvePresetPath(preset_id);
  m_key = Parameters::KeyForPreset(path, preset_id);
  if (!LoadParameters(path))
    return;

  BuildRows();
  ApplyOverrides(Parameters::Load(m_key));
}

void ShaderParametersDialog::CreateWidgets(bool opened_from_game_properties)
{
  // Hand-built rather than ported from PCSX2's ShaderParametersDialog.ui because DolphinQt has no
  // uic step: there is no .ui file anywhere under Source/Core/DolphinQt and no AUTOUIC in its
  // CMake. The values below are that file's.
  m_status = new QLabel(this);
  m_status->setWordWrap(true);

  auto* const contents = new QWidget(this);
  m_grid = new QGridLayout(contents);
  m_grid->setContentsMargins(8, 8, 8, 8);
  m_grid->setHorizontalSpacing(10);

  m_scroll = new QScrollArea(this);
  m_scroll->setWidgetResizable(true);
  m_scroll->setFrameShape(QFrame::StyledPanel);
  m_scroll->setWidget(contents);

  m_reset_all = new QPushButton(tr("Reset All"), this);
  auto* const close = new QPushButton(tr("Close"), this);
  close->setDefault(true);

  auto* const footer = new QHBoxLayout();
  footer->addWidget(m_reset_all);
  footer->addStretch();
  footer->addWidget(close);

  auto* const layout = new QVBoxLayout(this);
  if (opened_from_game_properties)
  {
    // Above the parameters, not on a tooltip: the values below are shared, and the one page that
    // makes that surprising is the one this was opened from. A tooltip would only reach the user
    // after they had already changed something for what they thought was one game.
    auto* const scope = new QLabel(
        tr("Note: parameter values are global. They apply to every game that uses this preset."),
        this);
    scope->setWordWrap(true);
    layout->addWidget(scope);
  }
  layout->addWidget(m_status);
  layout->addWidget(m_scroll);
  layout->addLayout(footer);

  connect(m_reset_all, &QPushButton::clicked, this, &ShaderParametersDialog::OnResetAllClicked);
  connect(close, &QPushButton::clicked, this, &QDialog::accept);
}

bool ShaderParametersDialog::LoadParameters(const std::string& absolute_preset_path)
{
  std::vector<Parameters::ParameterInfo> params;
  std::string error;
  if (absolute_preset_path.empty() || !Parameters::Enumerate(absolute_preset_path, &params, &error))
  {
    m_status->setText(tr("Could not load preset: %1")
                          .arg(absolute_preset_path.empty() ? tr("invalid preset path") :
                                                              QString::fromStdString(error)));
    m_reset_all->setEnabled(false);
    return false;
  }

  if (params.empty())
  {
    m_status->setText(tr("This preset has no adjustable parameters."));
    m_reset_all->setEnabled(false);
    return false;
  }

  m_status->hide();
  m_rows.reserve(params.size());
  for (Parameters::ParameterInfo& info : params)
  {
    Row row;
    row.value = info.initial;
    row.info = std::move(info);
    m_rows.push_back(std::move(row));
  }
  return true;
}

void ShaderParametersDialog::BuildRows()
{
  for (size_t i = 0; i < m_rows.size(); i++)
  {
    Row& row = m_rows[i];
    const Parameters::ParameterInfo& info = row.info;
    const int grid_row = static_cast<int>(i);

    // The label is the human-readable description, falling back to the id when a shader declares
    // none; the tooltip is always the id, so it stays discoverable.
    auto* const label =
        new QLabel(QString::fromStdString(info.description.empty() ? info.name : info.description));
    // Pack data, so pin the format: left at Qt::AutoText a description shaped like an HTML tag
    // would render with the "tag" swallowed and the row become unidentifiable. No preset in the
    // libretro pack declares one today, and the reference leaves this at the default.
    label->setTextFormat(Qt::PlainText);
    label->setToolTip(QString::fromStdString(info.name));
    m_grid->addWidget(label, grid_row, 0);

    // A slider needs at least one whole step: has_range rules out a non-positive step and an
    // inverted range, whole_steps rules out a range narrower than a single step (those have no
    // usable positions, so they get the spin box alone).
    const bool has_range = (info.step > 0.0f && info.maximum > info.minimum);
    const double whole_steps =
        has_range ? std::round((info.maximum - info.minimum) / info.step) : 0.0;
    const bool degenerate = (whole_steps < 1.0);
    if (!degenerate)
    {
      // One position per whole step, so a position is exactly minimum + p * step. Ranges with more
      // steps than the slider can carry are capped, which stretches the increment.
      const int steps =
          static_cast<int>(std::min<double>(whole_steps, static_cast<double>(MAX_SLIDER_STEPS)));
      row.slider_increment =
          (whole_steps <= static_cast<double>(MAX_SLIDER_STEPS)) ?
              info.step :
              (info.maximum - info.minimum) / static_cast<float>(MAX_SLIDER_STEPS);
      row.slider = new QSlider(Qt::Horizontal);
      row.slider->setRange(0, steps);
      row.slider->setMinimumWidth(180);
      row.slider->setFocusPolicy(Qt::StrongFocus);
      row.slider->installEventFilter(this);
      m_grid->addWidget(row.slider, grid_row, 1);
      connect(row.slider, &QSlider::valueChanged, this, [this, i](int pos) {
        Row& r = m_rows[i];
        // The last position is the maximum exactly, even when the range is not a whole number of
        // steps.
        float v = (pos >= r.slider->maximum()) ?
                      r.info.maximum :
                      r.info.minimum + static_cast<float>(pos) * r.slider_increment;
        // The position nearest the default *is* the default: minimum + p * step accumulates float
        // noise on wide ranges, which would leave Reset enabled at the default position and persist
        // that noise.
        if (std::abs(v - r.info.initial) < r.slider_increment * 0.5f)
          v = r.info.initial;
        OnValueEdited(r, v);
      });
    }

    row.spin = new QDoubleSpinBox();
    // Decimals first: QDoubleSpinBox rounds the range and the step to the current precision, so
    // setting them the other way round quantises both.
    row.spin->setDecimals(Parameters::DecimalsForStep(info.step));
    if (has_range)
    {
      // Includes the sub-step ranges that lost their slider: the preset's range still applies.
      row.spin->setRange(info.minimum, info.maximum);
      row.spin->setSingleStep(info.step);
    }
    else
    {
      row.spin->setRange(-1.0e9, 1.0e9);
      row.spin->setSingleStep(info.step > 0.0f ? info.step : 1.0);
    }
    row.spin->setMinimumWidth(90);
    // Typing a multi-digit value would otherwise fire an edit per keystroke.
    row.spin->setKeyboardTracking(false);
    row.spin->setFocusPolicy(Qt::StrongFocus);
    row.spin->installEventFilter(this);
    m_grid->addWidget(row.spin, grid_row, 2);
    connect(row.spin, &QDoubleSpinBox::valueChanged, this,
            [this, i](double v) { OnValueEdited(m_rows[i], static_cast<float>(v)); });

    row.reset = new QPushButton(tr("Reset"));
    m_grid->addWidget(row.reset, grid_row, 3);
    connect(row.reset, &QPushButton::clicked, this,
            [this, i] { OnValueEdited(m_rows[i], m_rows[i].info.initial); });

    RefreshRowWidgets(row);
  }
  m_grid->setColumnStretch(1, 1);
  m_grid->setRowStretch(static_cast<int>(m_rows.size()), 1);
}

bool ShaderParametersDialog::eventFilter(QObject* watched, QEvent* event)
{
  // A wheel event over an unfocused slider or spin box scrolls the list instead of editing the
  // parameter. On a preset with a hundred rows this is the difference between scrolling past them
  // and silently changing every value on the way.
  QWidget* const widget = qobject_cast<QWidget*>(watched);
  if (event->type() == QEvent::Wheel && widget != nullptr && !widget->hasFocus())
  {
    QApplication::sendEvent(m_scroll->viewport(), event);
    return true;
  }
  return QDialog::eventFilter(watched, event);
}

void ShaderParametersDialog::ApplyOverrides(const Parameters::Overrides& overrides)
{
  for (Row& row : m_rows)
  {
    float value = row.info.initial;
    const auto it = std::ranges::find_if(
        overrides, [&row](const auto& pair) { return pair.first == row.info.name; });
    if (it != overrides.end())
    {
      value = it->second;
      // A persisted value outside the preset's declared range is clamped into it, and the clamped
      // value is what the next edit pushes and persists.
      if (row.info.maximum > row.info.minimum)
        value = std::clamp(value, row.info.minimum, row.info.maximum);
    }
    SetRowValue(row, value);
  }
}

void ShaderParametersDialog::SetRowValue(Row& row, float value)
{
  row.value = value;
  RefreshRowWidgets(row);
}

void ShaderParametersDialog::RefreshRowWidgets(Row& row)
{
  const bool was_updating = m_updating;
  m_updating = true;
  if (row.slider != nullptr)
  {
    const int position =
        static_cast<int>(std::lround((row.value - row.info.minimum) / row.slider_increment));
    row.slider->setValue(std::clamp(position, 0, row.slider->maximum()));
  }
  row.spin->setValue(row.value);
  row.reset->setEnabled(!IsDefault(row));
  m_updating = was_updating;
}

bool ShaderParametersDialog::IsDefault(const Row& row) const
{
  return Parameters::IsDefaultValue(row.value, row.info.initial);
}

Parameters::Overrides ShaderParametersDialog::CollectOverrides() const
{
  // Only the non-default values are persisted, so resetting a parameter removes it instead of
  // writing the default back -- which is how a preset's own defaults can change on a pack update
  // without users being pinned to the old ones.
  Parameters::Overrides overrides;
  for (const Row& row : m_rows)
  {
    if (!IsDefault(row))
      overrides.emplace_back(row.info.name, row.value);
  }
  return overrides;
}

void ShaderParametersDialog::OnValueEdited(Row& row, float value)
{
  if (m_updating)
    return;
  if (std::abs(value - row.value) < 1e-7f)
    return;

  SetRowValue(row, value);
  SaveOverrides();
}

void ShaderParametersDialog::OnResetAllClicked()
{
  for (Row& row : m_rows)
    SetRowValue(row, row.info.initial);
  SaveOverrides();
}

void ShaderParametersDialog::SaveOverrides()
{
  // PCSX2 debounces this write behind a timer and pushes to the live chain separately, because its
  // write goes through Host::CommitBaseSettingChanges(), which writes the INI. Neither is ported,
  // deliberately: Config::Layer::Set is in-memory only -- it assigns into the layer's map and sets
  // a dirty flag -- and the INI is written by Config::Save() when the settings window closes. So
  // there is no disk write here to debounce, and nothing a slider drag could thrash.
  //
  // The live chain is reached by the generation counter Save bumps, which it polls per frame. It
  // then pushes *every* parameter rather than only the ones stored here, because a chain remembers
  // the last value it was given and a reset therefore has to send the default explicitly; that push
  // lives in LibrashaderPostProcessing::ApplyStoredOverrides().
  //
  // The write is to the base layer whichever page opened this dialog, so parameter values are
  // global. That is a known limitation rather than an oversight -- see the note CreateWidgets adds
  // when the caller is a game's own graphics page.
  Parameters::Save(m_key, CollectOverrides());
}
