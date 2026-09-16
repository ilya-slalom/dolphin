// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QDialog>
#include <QString>
#include <string>
#include <vector>

class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;

// Picks post-processing presets by category and either replaces the chain or appends to it,
// mirroring the Android picker (SettingsFragmentPresenter.showPostShaderList).
class PostProcessingChainDialog final : public QDialog
{
  Q_OBJECT
public:
  explicit PostProcessingChainDialog(QWidget* parent, QString chain_spec);

  // The chain as edited. Only meaningful after the dialog was accepted.
  QString ChainSpec() const { return m_chain_spec; }

private:
  void CreateWidgets();
  void ConnectWidgets();
  void RefreshCategories();
  void RefreshPresets();
  void UpdateChainLabel();
  void UpdateButtons();
  std::string SelectedPreset() const;

  QComboBox* m_category_combo;
  QListWidget* m_preset_list;
  QLabel* m_chain_label;
  QPushButton* m_select_button;
  QPushButton* m_add_button;

  std::vector<std::string> m_presets;  // every preset, unfiltered
  QString m_chain_spec;
};
