// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

#include <QDialog>

#include "VideoCommon/PostProcessing/LibrashaderParameters.h"

class QDoubleSpinBox;
class QGridLayout;
class QLabel;
class QPushButton;
class QScrollArea;
class QSlider;

// Editor for the selected preset's #pragma parameters, ported from PCSX2's ShaderParametersDialog.
// One row per parameter: its description, a slider, a spin box and a Reset. Every edit is stored in
// the global config layer, whose generation counter a running chain polls, so a preset can be tuned
// while a game is on screen without reloading anything.
class ShaderParametersDialog final : public QDialog
{
  Q_OBJECT
public:
  // `preset` is the relative preset id the picker stores in GFX_ENHANCE_POST_SHADER ('/'
  // separators, no ".slangp"), already resolved through VideoCommon::ResolveConfiguredPreset.
  ShaderParametersDialog(QWidget* parent, const QString& preset);

protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

private:
  struct Row
  {
    VideoCommon::LibrashaderParameters::ParameterInfo info;
    QSlider* slider = nullptr;  // Null when the range is degenerate.
    QDoubleSpinBox* spin = nullptr;
    QPushButton* reset = nullptr;
    float slider_increment = 0.0f;  // Value per slider position.
    float value = 0.0f;
  };

  static constexpr int MAX_SLIDER_STEPS = 10000;

  void CreateWidgets();
  bool LoadParameters(const std::string& absolute_preset_path);
  void BuildRows();
  void ApplyOverrides(const VideoCommon::LibrashaderParameters::Overrides& overrides);
  void SetRowValue(Row& row, float value);
  void RefreshRowWidgets(Row& row);
  bool IsDefault(const Row& row) const;
  VideoCommon::LibrashaderParameters::Overrides CollectOverrides() const;
  void OnValueEdited(Row& row, float value);
  void OnResetAllClicked();
  void SaveOverrides();

  // The [LibrashaderParameters] key this preset's overrides live under.
  std::string m_key;

  QLabel* m_status;
  QScrollArea* m_scroll;
  QGridLayout* m_grid;
  QPushButton* m_reset_all;
  std::vector<Row> m_rows;

  // Set while widgets are being refreshed programmatically. The slider and the spin box each drive
  // the other, so without it an edit to either oscillates between them.
  bool m_updating = false;
};
