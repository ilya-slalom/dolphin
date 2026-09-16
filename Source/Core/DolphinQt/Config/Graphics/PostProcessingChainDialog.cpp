// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Config/Graphics/PostProcessingChainDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include "VideoCommon/PostProcessing/MultipassPostProcessing.h"
#include "VideoCommon/PostProcessing/ShaderChainSpec.h"

PostProcessingChainDialog::PostProcessingChainDialog(QWidget* parent, QString chain_spec)
    : QDialog(parent), m_chain_spec(std::move(chain_spec))
{
  setWindowTitle(tr("Post-Processing Chain"));
  m_presets = VideoCommon::MultipassPostProcessing::GetPresetList();
  CreateWidgets();
  ConnectWidgets();
  RefreshCategories();
  RefreshPresets();
  UpdateChainLabel();
  UpdateButtons();
}

void PostProcessingChainDialog::CreateWidgets()
{
  m_category_combo = new QComboBox(this);
  m_preset_list = new QListWidget(this);
  m_chain_label = new QLabel(this);
  m_chain_label->setWordWrap(true);
  m_select_button = new QPushButton(tr("Select"), this);
  m_add_button = new QPushButton(tr("Add to Chain"), this);

  auto* const buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
  buttons->addButton(m_select_button, QDialogButtonBox::AcceptRole);
  buttons->addButton(m_add_button, QDialogButtonBox::ActionRole);

  auto* const layout = new QVBoxLayout(this);
  layout->addWidget(new QLabel(tr("Category:"), this));
  layout->addWidget(m_category_combo);
  layout->addWidget(m_preset_list);
  layout->addWidget(m_chain_label);
  layout->addWidget(buttons);
  setLayout(layout);

  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void PostProcessingChainDialog::ConnectWidgets()
{
  connect(m_category_combo, &QComboBox::currentIndexChanged, this, [this] {
    RefreshPresets();
    UpdateButtons();
  });
  connect(m_preset_list, &QListWidget::currentRowChanged, this,
          &PostProcessingChainDialog::UpdateButtons);
  connect(m_preset_list, &QListWidget::itemDoubleClicked, this, [this] { m_select_button->click(); });
  connect(m_select_button, &QPushButton::clicked, this, [this] {
    // "Select" replaces the whole chain, including with (off).
    m_chain_spec = QString::fromStdString(SelectedPreset());
    accept();
  });
  connect(m_add_button, &QPushButton::clicked, this, [this] {
    m_chain_spec = QString::fromStdString(VideoCommon::AppendToChainSpec(
        m_chain_spec.toStdString(), SelectedPreset()));
    accept();
  });
}

void PostProcessingChainDialog::RefreshCategories()
{
  const QSignalBlocker blocker(m_category_combo);
  m_category_combo->clear();
  m_category_combo->addItem(tr("All"), QString{});
  for (const std::string& category : VideoCommon::ChainCategories(m_presets))
  {
    const QString text = QString::fromStdString(category);
    m_category_combo->addItem(text, text);
  }
}

void PostProcessingChainDialog::RefreshPresets()
{
  const std::string category = m_category_combo->currentData().toString().toStdString();
  const QSignalBlocker blocker(m_preset_list);
  m_preset_list->clear();

  auto* const off = new QListWidgetItem(tr("(off)"), m_preset_list);
  off->setData(Qt::UserRole, QString{});

  // Highlight the chain's last entry, matching Android.
  const auto chain = VideoCommon::SplitChainSpec(m_chain_spec.toStdString());
  const std::string current = chain.empty() ? std::string() : chain.back();
  int current_row = 0;
  for (const std::string& preset : VideoCommon::PresetsInCategory(m_presets, category))
  {
    auto* const item = new QListWidgetItem(QString::fromStdString(preset), m_preset_list);
    item->setData(Qt::UserRole, QString::fromStdString(preset));
    if (preset == current)
      current_row = m_preset_list->row(item);
  }
  m_preset_list->setCurrentRow(current_row);
}

void PostProcessingChainDialog::UpdateChainLabel()
{
  const std::string description = VideoCommon::DescribeChainSpec(m_chain_spec.toStdString());
  m_chain_label->setText(tr("Current chain: %1")
                             .arg(description.empty() ? tr("(off)") :
                                                        QString::fromStdString(description)));
}

void PostProcessingChainDialog::UpdateButtons()
{
  // Appending "(off)" is meaningless, and there is nothing to append to an empty chain.
  m_add_button->setEnabled(!SelectedPreset().empty() && !m_chain_spec.isEmpty());
}

std::string PostProcessingChainDialog::SelectedPreset() const
{
  const QListWidgetItem* const item = m_preset_list->currentItem();
  return item ? item->data(Qt::UserRole).toString().toStdString() : std::string();
}
