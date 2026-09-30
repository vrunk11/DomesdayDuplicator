/************************************************************************

    board_setup_page.cpp

    The Board setup tab: declaring what the capture board is
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#include "board_setup_page.h"

#include <QComboBox>
#include <QDateTime>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>
#include <ctime>
#include <vector>

#include "capture_controller.h"
#include "capture_format.h"

namespace ddd::gui {
namespace {

QLabel* MakeNote(const QString& text, QWidget* parent) {
  auto* note = new QLabel(text, parent);
  note->setWordWrap(true);
  note->setForegroundRole(QPalette::PlaceholderText);
  return note;
}

uint32_t Now() { return static_cast<uint32_t>(std::time(nullptr)); }

}  // namespace

QString DescribeBoardSetupSource(capture::BoardSetupSource source) {
  switch (source) {
    case capture::BoardSetupSource::kDeclared:
      return BoardSetupPage::tr("Read from the device.");
    case capture::BoardSetupSource::kBlank:
      return BoardSetupPage::tr(
          "Nothing has been declared on this device yet, so the defaults "
          "below are in force: an ADS825 with RSEL tied high.");
    case capture::BoardSetupSource::kDamaged:
      return BoardSetupPage::tr(
          "The board setup on this device is damaged, so the defaults are in "
          "force until it is written again.");
    case capture::BoardSetupSource::kNewerLayout:
      return BoardSetupPage::tr(
          "The board setup on this device was written by a newer version of "
          "this application and cannot be read here, so the defaults are in "
          "force.");
    case capture::BoardSetupSource::kUnsupported:
      return BoardSetupPage::tr(
          "This device's firmware cannot store a board setup. What is written "
          "here applies until the application closes; update the firmware to "
          "keep it on the board.");
    case capture::BoardSetupSource::kUnavailable:
      return BoardSetupPage::tr("No Domesday Duplicator is attached.");
  }
  return QString();
}

QString DescribeMeasuredAt(uint32_t measured) {
  if (measured == 0) {
    return BoardSetupPage::tr("not measured");
  }
  return BoardSetupPage::tr("measured %1")
      .arg(QDateTime::fromSecsSinceEpoch(static_cast<qint64>(measured))
               .toString(QStringLiteral("yyyy-MM-dd HH:mm")));
}

QString DescribeBoardSummary(const capture::BoardSetupReading& reading,
                             uint8_t rate_mhz, bool range_2vpp) {
  if (reading.source == capture::BoardSetupSource::kUnavailable) {
    return QString();
  }

  const capture::BoardSetup& setup = reading.setup;
  const int offset = capture::DcOffsetFor(setup, range_2vpp, rate_mhz);

  // The one offset in force for the rate and range the capture runs at: the
  // table has sixteen, and fifteen of them do not apply to this capture.
  const QString offsets =
      BoardSetupPage::tr("offset %1 at %2 MSPS, %3")
          .arg(offset > 0 ? QStringLiteral("+%1").arg(offset)
                          : QString::number(offset))
          .arg(static_cast<int>(rate_mhz))
          .arg(QString::fromUtf8(capture::InputRangeName(range_2vpp)));

  QString wiring;
  switch (setup.rsel_wiring) {
    case capture::RselWiring::kAuto:
      wiring = BoardSetupPage::tr("RSEL auto");
      break;
    case capture::RselWiring::kLow:
      wiring = BoardSetupPage::tr("RSEL low, 1Vpp");
      break;
    case capture::RselWiring::kHigh:
      wiring = BoardSetupPage::tr("RSEL high, 2Vpp");
      break;
  }

  QString summary =
      BoardSetupPage::tr("%1 · %2 MHz max · %3 · %4")
          .arg(QString::fromUtf8(capture::AdcPartName(setup.adc)))
          .arg(static_cast<int>(capture::AdcPartMaxRateMhz(setup.adc)))
          .arg(wiring, offsets);

  const bool defaults = setup == capture::BoardSetup{};
  switch (reading.source) {
    case capture::BoardSetupSource::kDeclared:
      break;
    case capture::BoardSetupSource::kUnsupported:
      summary += defaults ? BoardSetupPage::tr(
                                " (defaults — this firmware cannot store a "
                                "board setup)")
                          : BoardSetupPage::tr(" (this session only)");
      break;
    case capture::BoardSetupSource::kBlank:
    case capture::BoardSetupSource::kDamaged:
    case capture::BoardSetupSource::kNewerLayout:
    case capture::BoardSetupSource::kUnavailable:
      summary += BoardSetupPage::tr(" (defaults — nothing declared)");
      break;
  }

  if (!setup.name.empty()) {
    summary =
        QString::fromStdString(setup.name) + QStringLiteral(" — ") + summary;
  }
  return summary;
}

BoardSetupPage::BoardSetupPage(CaptureController* controller, QWidget* parent)
    : QWidget(parent), controller_(controller) {
  auto* layout = new QVBoxLayout(this);

  layout->addWidget(MakeNote(
      tr("What this capture board is. It is declared once per board and kept "
         "on the device, so it follows the FX3 kit to any computer. It is not "
         "a capture setting: it bounds what the capture settings can offer."),
      this));

  source_ = new QLabel(this);
  source_->setObjectName(QLatin1String(kSourceLabelName));
  source_->setWordWrap(true);
  layout->addWidget(source_);

  auto* grid = new QGridLayout();

  name_ = new QLineEdit(this);
  name_->setObjectName(QLatin1String(kNameEditName));
  name_->setMaxLength(static_cast<int>(capture::kBoardNameMaximumBytes));
  name_->setPlaceholderText(tr("For example, the board's serial or a label"));
  name_->setToolTip(
      tr("A name for this capture board, recorded in every capture's "
         "metadata. The FX3 kit can be moved to another board, and seeing the "
         "old name here is the reminder to declare the new one."));
  grid->addWidget(new QLabel(tr("Board name"), this), 0, 0);
  grid->addWidget(name_, 0, 1, 1, 3);

  adc_ = new QComboBox(this);
  adc_->setObjectName(QLatin1String(kAdcComboName));
  adc_->addItem(tr("ADS825 — up to 40 MSPS"),
                static_cast<int>(capture::AdcPart::kAds825));
  adc_->addItem(tr("ADS828 — up to 75 MSPS"),
                static_cast<int>(capture::AdcPart::kAds828));
  adc_->setToolTip(
      tr("Read the part number printed on the converter chip. Nothing on the "
         "board reports it."));
  grid->addWidget(new QLabel(tr("ADC fitted"), this), 1, 0);
  grid->addWidget(adc_, 1, 1, 1, 3);

  rsel_ = new QComboBox(this);
  rsel_->setObjectName(QLatin1String(kRselComboName));
  rsel_->addItem(tr("Auto — routed to the FPGA, range chosen per capture"),
                 static_cast<int>(capture::RselWiring::kAuto));
  rsel_->addItem(tr("Low — tied low, always 1Vpp"),
                 static_cast<int>(capture::RselWiring::kLow));
  rsel_->addItem(tr("High — tied high, always 2Vpp"),
                 static_cast<int>(capture::RselWiring::kHigh));
  rsel_->setToolTip(
      tr("How the converter's RSEL pin is wired on this board. Both converters "
         "have it; whether the board routes it to the FPGA is a separate "
         "question."));
  grid->addWidget(new QLabel(tr("RSEL wiring"), this), 2, 0);
  grid->addWidget(rsel_, 2, 1, 1, 3);

  layout->addLayout(grid);

  // The offsets: a row per ADC rate and a column per input range, because the
  // offset moves with the clock as well as with the range.
  auto* table = new QGridLayout();
  table->addWidget(new QLabel(tr("DC offset (10-bit)"), this), 0, 0);
  table->addWidget(new QLabel(tr("1Vpp"), this), 0, 1, Qt::AlignHCenter);
  table->addWidget(new QLabel(tr("2Vpp"), this), 0, 2, Qt::AlignHCenter);

  const auto make_offset = [this](bool range_2vpp, uint8_t rate_mhz) {
    auto* spin = new QSpinBox(this);
    spin->setObjectName(OffsetSpinName(range_2vpp, rate_mhz));
    spin->setRange(capture::kDcOffsetMinimum, capture::kDcOffsetMaximum);
    spin->setToolTip(
        tr("How far this board's signal sits from the centre with nothing "
           "connected, at this ADC rate and range, in steps of the 10-bit "
           "converter: 0 to 1023, centred on 512, so +20 means the signal sits "
           "at 532. It is taken out of every sample written at this rate and "
           "range, and recorded in the file so it can be put back."));
    connect(spin, &QSpinBox::valueChanged, this, [this] { OnOffsetEdited(); });
    return spin;
  };

  for (size_t index = 0; index < capture::kDcOffsetRateCount; ++index) {
    const uint8_t rate = capture::kDcOffsetRatesMhz[index];
    const int row = static_cast<int>(index) + 1;
    table->addWidget(
        new QLabel(tr("%1 MSPS").arg(static_cast<int>(rate)), this), row, 0);
    offsets_1vpp_[index] = make_offset(false, rate);
    offsets_2vpp_[index] = make_offset(true, rate);
    table->addWidget(offsets_1vpp_[index], row, 1);
    table->addWidget(offsets_2vpp_[index], row, 2);
  }

  measure_ = new QPushButton(tr("Measure…"), this);
  measure_->setObjectName(QLatin1String(kMeasureButtonName));
  measure_->setToolTip(
      tr("Average one second of the stream with nothing connected to the "
         "input, at every ADC rate the declared converter allows and every "
         "range the wiring can select — up to sixteen runs, a little over "
         "twenty seconds."));
  measured_label_ = MakeNote(QString(), this);
  measured_label_->setObjectName(QLatin1String(kMeasuredLabelName));

  auto* side = new QVBoxLayout();
  side->addWidget(measure_);
  side->addWidget(measured_label_);
  side->addStretch(1);
  table->addLayout(side, 1, 3, static_cast<int>(capture::kDcOffsetRateCount),
                   1);
  table->setColumnStretch(4, 1);
  layout->addLayout(table);

  layout->addWidget(MakeNote(
      tr("Declaring a converter that is not fitted does not make the board "
         "faster. Past its rated speed an ADC keeps sending samples, but wrong "
         "ones, and nothing reports it: the integrity check and the received "
         "sample rate both stay correct."),
      this));

  layout->addWidget(MakeNote(
      tr("Measure and Write to board act on the device at once. The dialog's "
         "OK and Cancel do not affect them."),
      this));

  busy_note_ = MakeNote(
      tr("Stop monitoring to measure or to write to the board."), this);
  layout->addWidget(busy_note_);

  auto* buttons = new QHBoxLayout();
  write_ = new QPushButton(tr("Write to board"), this);
  write_->setObjectName(QLatin1String(kWriteButtonName));
  buttons->addWidget(write_);
  buttons->addStretch(1);
  layout->addLayout(buttons);

  result_ = new QLabel(this);
  result_->setObjectName(QLatin1String(kResultLabelName));
  result_->setWordWrap(true);
  layout->addWidget(result_);

  layout->addStretch(1);

  connect(adc_, &QComboBox::currentIndexChanged, this,
          &BoardSetupPage::OnAdcChosen);
  connect(rsel_, &QComboBox::currentIndexChanged, this,
          [this] { UpdateEnabledState(); });
  connect(measure_, &QPushButton::clicked, this,
          &BoardSetupPage::OnMeasureClicked);
  connect(write_, &QPushButton::clicked, this, &BoardSetupPage::OnWriteClicked);

  if (controller_ != nullptr) {
    connect(controller_, &CaptureController::BoardSetupChanged, this,
            &BoardSetupPage::LoadFromController);
    connect(controller_, &CaptureController::MonitoringChanged, this,
            [this] { UpdateEnabledState(); });
    connect(controller_, &CaptureController::DcOffsetMeasured, this,
            &BoardSetupPage::OnMeasured);
    connect(controller_, &CaptureController::DcOffsetMeasurementFinished, this,
            &BoardSetupPage::OnMeasurementFinished);
  }

  LoadFromController();
}

QString BoardSetupPage::OffsetSpinName(bool range_2vpp, uint8_t rate_mhz) {
  return QStringLiteral("board_setup_offset_%1_%2")
      .arg(range_2vpp ? QStringLiteral("2vpp") : QStringLiteral("1vpp"))
      .arg(static_cast<int>(rate_mhz));
}

capture::BoardSetup BoardSetupPage::Edited() const {
  capture::BoardSetup setup;
  setup.name = name_->text().toStdString();
  setup.adc = static_cast<capture::AdcPart>(adc_->currentData().toInt());
  setup.rsel_wiring =
      static_cast<capture::RselWiring>(rsel_->currentData().toInt());
  for (size_t index = 0; index < capture::kDcOffsetRateCount; ++index) {
    setup.dc_offset_1vpp[index] =
        static_cast<int16_t>(offsets_1vpp_[index]->value());
    setup.dc_offset_2vpp[index] =
        static_cast<int16_t>(offsets_2vpp_[index]->value());
  }
  setup.measured = measured_;
  return setup;
}

void BoardSetupPage::OnOffsetEdited() {
  // A figure typed by hand is not a measurement, and the table is one
  // declaration: once any of it is edited, none of it is "measured on" a date.
  if (!loading_) {
    measured_ = 0;
    measured_label_->setText(DescribeMeasuredAt(measured_));
  }
}

void BoardSetupPage::LoadFromController() {
  const capture::BoardSetupReading reading = controller_ != nullptr
                                                 ? controller_->board_setup()
                                                 : capture::BoardSetupReading{};
  const capture::BoardSetup& setup = reading.setup;

  loading_ = true;
  source_->setText(DescribeBoardSetupSource(reading.source));
  name_->setText(QString::fromStdString(setup.name));
  adc_->setCurrentIndex(adc_->findData(static_cast<int>(setup.adc)));
  adc_index_ = adc_->currentIndex();
  rsel_->setCurrentIndex(rsel_->findData(static_cast<int>(setup.rsel_wiring)));
  for (size_t index = 0; index < capture::kDcOffsetRateCount; ++index) {
    offsets_1vpp_[index]->setValue(setup.dc_offset_1vpp[index]);
    offsets_2vpp_[index]->setValue(setup.dc_offset_2vpp[index]);
  }
  measured_ = setup.measured;
  measured_label_->setText(DescribeMeasuredAt(measured_));
  loading_ = false;

  UpdateEnabledState();
}

void BoardSetupPage::UpdateEnabledState() {
  const auto wiring =
      static_cast<capture::RselWiring>(rsel_->currentData().toInt());

  // Only the offsets of the ranges the wiring can select, at the rates the
  // declared converter can run at: any other can never be in force, and a
  // figure for it would be one nobody could check.
  const uint8_t max_rate = capture::AdcPartMaxRateMhz(
      static_cast<capture::AdcPart>(adc_->currentData().toInt()));
  for (size_t index = 0; index < capture::kDcOffsetRateCount; ++index) {
    const bool rate_allowed = capture::kDcOffsetRatesMhz[index] <= max_rate;
    offsets_1vpp_[index]->setEnabled(rate_allowed &&
                                     wiring != capture::RselWiring::kHigh);
    offsets_2vpp_[index]->setEnabled(rate_allowed &&
                                     wiring != capture::RselWiring::kLow);
  }

  const bool attached =
      controller_ != nullptr && controller_->board_setup().source !=
                                    capture::BoardSetupSource::kUnavailable;
  const bool measuring =
      controller_ != nullptr && controller_->measuring_dc_offset();
  const bool monitoring = controller_ != nullptr && controller_->monitoring();

  measure_->setEnabled(attached && !measuring && !monitoring);
  write_->setEnabled(attached && !measuring && !monitoring);
  busy_note_->setVisible(monitoring && !measuring);
}

void BoardSetupPage::OnAdcChosen(int index) {
  if (loading_) {
    adc_index_ = index;
    return;
  }

  // Asked only on the way up. Declaring the slower converter can cost speed
  // and never correctness; declaring the faster one on a board that does not
  // have it produces captures that are wrong in a way nothing detects.
  const bool faster = adc_->itemData(index).toInt() ==
                          static_cast<int>(capture::AdcPart::kAds828) &&
                      adc_->itemData(adc_index_).toInt() ==
                          static_cast<int>(capture::AdcPart::kAds825);
  if (faster) {
    const QMessageBox::StandardButton answer = QMessageBox::question(
        this, tr("Declare an ADS828"),
        tr("Check that the converter chip on this board is marked ADS828 "
           "before declaring it.\n\nAn ADS825 run faster than 40 MSPS still "
           "sends samples, but wrong ones, and nothing will report it."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
      const QSignalBlocker blocker(adc_);
      adc_->setCurrentIndex(adc_index_);
      return;
    }
  }
  adc_index_ = index;

  // The rows the declared converter can run at.
  UpdateEnabledState();
}

void BoardSetupPage::OnMeasureClicked() {
  if (controller_ == nullptr) {
    return;
  }

  QMessageBox box(QMessageBox::Warning, tr("Measure DC offset"),
                  tr("Disconnect the BNC input connector before measuring."),
                  QMessageBox::Ok | QMessageBox::Cancel, this);
  box.setInformativeText(
      tr("The offset is averaged over one second with nothing connected; any "
         "signal on the input will make the measurement wrong."));
  box.setDefaultButton(QMessageBox::Cancel);
  if (box.exec() != QMessageBox::Ok) {
    return;
  }

  // The ranges the wiring on this page can select, not the wiring on the
  // device: somebody declaring a board for the first time measures before
  // writing, and the device still holds the defaults.
  std::vector<bool> ranges;
  switch (static_cast<capture::RselWiring>(rsel_->currentData().toInt())) {
    case capture::RselWiring::kAuto:
      ranges = {false, true};
      break;
    case capture::RselWiring::kLow:
      ranges = {false};
      break;
    case capture::RselWiring::kHigh:
      ranges = {true};
      break;
  }

  // And the rates the converter on this page is rated for, for the same reason.
  const uint8_t max_rate = capture::AdcPartMaxRateMhz(
      static_cast<capture::AdcPart>(adc_->currentData().toInt()));

  measured_runs_ = 0;
  result_->setText(tr("Measuring…"));
  controller_->MeasureDcOffset(ranges, max_rate);
  if (controller_->measuring_dc_offset()) {
    result_->setText(tr("Measuring… 0 of %1")
                         .arg(static_cast<qulonglong>(
                             controller_->dc_offset_measurement_runs())));
  }
  UpdateEnabledState();
}

void BoardSetupPage::OnMeasured(bool range_2vpp, int rate_mhz, int offset) {
  const int index = capture::DcOffsetRateIndex(static_cast<uint8_t>(rate_mhz));
  if (index < 0) {
    return;
  }

  loading_ = true;
  SpinColumn& column = range_2vpp ? offsets_2vpp_ : offsets_1vpp_;
  column[static_cast<size_t>(index)]->setValue(offset);
  measured_ = Now();
  measured_label_->setText(DescribeMeasuredAt(measured_));
  loading_ = false;

  ++measured_runs_;
  if (controller_ != nullptr) {
    result_->setText(tr("Measuring… %1 of %2")
                         .arg(static_cast<qulonglong>(measured_runs_))
                         .arg(static_cast<qulonglong>(
                             controller_->dc_offset_measurement_runs())));
  }
}

void BoardSetupPage::OnMeasurementFinished(bool succeeded,
                                           const QString& message) {
  result_->setText(succeeded ? tr("Measured. Write to board to keep it.")
                             : message);
  UpdateEnabledState();
}

void BoardSetupPage::OnWriteClicked() {
  if (controller_ == nullptr) {
    return;
  }

  QString message;
  const bool written = controller_->WriteBoardSetup(Edited(), message);
  result_->setText(message);
  if (written) {
    // What the device now holds, which is what was sent cut to fit the record.
    LoadFromController();
    result_->setText(message);
  }
}

}  // namespace ddd::gui
