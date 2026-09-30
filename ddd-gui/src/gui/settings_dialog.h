/************************************************************************

    settings_dialog.h

    The application's settings, grouped by what they are about
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#pragma once

#include <QDialog>
#include <vector>

#include "capture_settings.h"
#include "player_settings.h"
#include "serial_port_scanner.h"
#include "usb_device_info.h"

class QCheckBox;
class QComboBox;
class QLineEdit;
class QListWidget;
class QTabWidget;
class QWidget;

namespace ddd::gui {

class CaptureController;

// Everything that is set once for a machine and then left alone.
//
// A dialog rather than a panel, because none of it is worth screen space during
// a capture: the panels are for what is happening, and this is for how.
//
// Tabbed, and the tabs are the point rather than decoration. These settings are
// about two different pieces of equipment — how this machine moves data off the
// Duplicator, and what is on the other end of a serial cable — and a single
// form would put "which serial port" directly beneath "USB transfer size" under
// one **OK**. Somebody looking for one of them would have to read all of them.
class SettingsDialog : public QDialog {
  Q_OBJECT

 public:
  // Which tab to open on. Kept because a caller may know which half somebody
  // is after, so that reaching it is not a hunt through a dialog
  // named after something else.
  enum class Tab {
    kCapture,
    kPlayer,
    kBoardSetup,
  };

  SettingsDialog(const CaptureSettings& capture,
                 const std::vector<ddd::capture::DeviceInfo>& devices,
                 const PlayerSettings& player,
                 const std::vector<SerialPortCandidate>& ports,
                 Tab initial_tab = Tab::kCapture, QWidget* parent = nullptr);

  // Add the Board setup tab, which acts on the device through `controller`
  // rather than returning anything on OK — see BoardSetupPage. Separate from
  // the constructor because it is the one tab that needs the live controller
  // rather than a copy of the settings, and a dialog built without one simply
  // does not have it. Opens on it if the dialog was asked to.
  void AddBoardSetupTab(CaptureController* controller);

  // What the dialog was left showing. Only meaningful after Accepted.
  CaptureSettings Settings() const;
  PlayerSettings Player() const;

  static constexpr const char* kTabsName = "settings_tabs";

  static constexpr const char* kQueueSizeComboName = "settings_queue_size";
  static constexpr const char* kTransferModeComboName =
      "settings_transfer_mode";
  static constexpr const char* kDeviceComboName = "settings_device";
  static constexpr const char* kDirectoryEditName = "settings_directory";
  static constexpr const char* kBrowseButtonName = "settings_browse";
  static constexpr const char* kFrontEndGainComboName =
      "settings_front_end_gain";
  static constexpr const char* kEcoModeCheckName = "settings_eco_mode";

  static constexpr const char* kPlayerEnabledCheckName =
      "settings_player_enabled";
  static constexpr const char* kPlayerModelComboName = "settings_player_model";
  static constexpr const char* kPlayerPortComboName = "settings_player_port";
  static constexpr const char* kPlayerBaudComboName = "settings_player_baud";
  static constexpr const char* kPlayerExcludedListName =
      "settings_player_excluded";
  static constexpr const char* kPlayerStopCaptureCheckName =
      "settings_player_stop_capture";

 private:
  QWidget* BuildCapturePage(
      const std::vector<ddd::capture::DeviceInfo>& devices);
  QWidget* BuildPlayerPage(const std::vector<SerialPortCandidate>& ports);

  CaptureSettings capture_;
  PlayerSettings player_;

  QTabWidget* tabs_ = nullptr;

  // The tab asked for at construction, kept for AddBoardSetupTab().
  Tab initial_tab_ = Tab::kCapture;

  QComboBox* queue_size_ = nullptr;
  QComboBox* transfer_mode_ = nullptr;
  QComboBox* device_ = nullptr;
  QLineEdit* directory_ = nullptr;
  QComboBox* front_end_gain_ = nullptr;
  QCheckBox* eco_mode_ = nullptr;

  QCheckBox* player_enabled_ = nullptr;
  QComboBox* player_model_ = nullptr;
  QComboBox* player_port_ = nullptr;
  QComboBox* player_baud_ = nullptr;
  QListWidget* player_excluded_ = nullptr;
  QCheckBox* stop_capture_ = nullptr;
};

}  // namespace ddd::gui
