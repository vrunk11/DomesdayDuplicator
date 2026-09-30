/************************************************************************

    board_setup_page.h

    The Board setup tab: declaring what the capture board is
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#pragma once

#include <QString>
#include <QWidget>
#include <array>
#include <cstddef>
#include <cstdint>

#include "board_setup.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace ddd::gui {

class CaptureController;

// What a board setup's source means, as a sentence for the page.
QString DescribeBoardSetupSource(capture::BoardSetupSource source);

// When an offset was measured, or that it was entered by hand.
QString DescribeMeasuredAt(uint32_t measured);

// The board setup in one line, for the capture panel: the converter and the
// rate it allows, the wiring, the offset in force at `rate_mhz` and the range
// the capture runs at, and the name — or that the defaults are in force. Empty
// when no device is attached.
QString DescribeBoardSummary(const capture::BoardSetupReading& reading,
                             uint8_t rate_mhz, bool range_2vpp);

// The Board setup tab of the Settings dialog.
//
// Unlike every other tab there, nothing on it waits for OK. It edits a
// declaration the *device* keeps (board_setup.h), so Write to board sends it
// there and then, and Measure runs the stream to measure the offset; Cancel on
// the dialog cannot take either back, and the page says so. What it never does
// is write on its own: a measurement fills the fields, and only the button
// puts them on the board.
class BoardSetupPage : public QWidget {
  Q_OBJECT

 public:
  explicit BoardSetupPage(CaptureController* controller,
                          QWidget* parent = nullptr);

  // What the fields say, as a declaration.
  capture::BoardSetup Edited() const;

  static constexpr const char* kNameEditName = "board_setup_name";
  static constexpr const char* kAdcComboName = "board_setup_adc";
  static constexpr const char* kRselComboName = "board_setup_rsel";
  static constexpr const char* kMeasureButtonName = "board_setup_measure";
  static constexpr const char* kWriteButtonName = "board_setup_write";
  static constexpr const char* kSourceLabelName = "board_setup_source";
  static constexpr const char* kResultLabelName = "board_setup_result";
  static constexpr const char* kMeasuredLabelName = "board_setup_measured";

  // The offset field for a range at an ADC rate: "board_setup_offset_2vpp_75".
  static QString OffsetSpinName(bool range_2vpp, uint8_t rate_mhz);

 private:
  using SpinColumn = std::array<QSpinBox*, capture::kDcOffsetRateCount>;

  void LoadFromController();
  void UpdateEnabledState();
  void OnAdcChosen(int index);
  void OnMeasureClicked();
  void OnWriteClicked();
  void OnMeasured(bool range_2vpp, int rate_mhz, int offset);
  void OnMeasurementFinished(bool succeeded, const QString& message);
  void OnOffsetEdited();

  CaptureController* controller_ = nullptr;

  QLabel* source_ = nullptr;
  QLineEdit* name_ = nullptr;
  QComboBox* adc_ = nullptr;
  QComboBox* rsel_ = nullptr;

  // One field per ADC rate in each range, rows in kDcOffsetRatesMhz order.
  SpinColumn offsets_1vpp_{};
  SpinColumn offsets_2vpp_{};

  QLabel* measured_label_ = nullptr;
  QPushButton* measure_ = nullptr;
  QPushButton* write_ = nullptr;
  QLabel* busy_note_ = nullptr;
  QLabel* result_ = nullptr;

  // When the offsets shown were measured, carried with the fields until they
  // are written. Offsets typed by hand have none.
  uint32_t measured_ = 0;

  // Runs reported so far by the measurement in progress, for its progress.
  size_t measured_runs_ = 0;

  // The converter the combo showed before the latest change, so a change the
  // user does not confirm can be put back.
  int adc_index_ = 0;

  // Set while the page itself is changing its fields, so that doing so is not
  // taken for the user editing them.
  bool loading_ = false;
};

}  // namespace ddd::gui
