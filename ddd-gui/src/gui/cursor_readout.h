/************************************************************************

    cursor_readout.h

    The line of figures under a plot, saying what the pointer is over
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#pragma once

#include <QLabel>
#include <QString>

class QEvent;
class QResizeEvent;

namespace ddd::gui {

// The readout at the end of a plot's control row: the sample, the frequency,
// the level — whatever the pointer is over at this instant.
//
// It is a class rather than a QLabel because of where it sits. The control row
// is the widest thing in a monitor panel, so the row's minimum width is the
// panel's, which is the dock's, which is part of the window's; and an ordinary
// label asks the layout for as much width as the text it holds. The text here
// changes with every mouse move, so all of those minimums moved with it. In a
// window sized down to about that minimum — half a screen, which is what the
// Windows snap keys give you — that means the dock layout shares out the space
// afresh on every reading, and the panels jump about while the pointer is over
// the trace. They settle only once a separator has been dragged by hand, which
// is what pins the sizes instead. That is issue #180.
//
// So this contributes nothing to the layout at all: an ignored horizontal size
// policy makes both its minimum and its preferred width zero, it takes
// whatever room is left at the end of the row, and it shortens the text to fit
// rather than asking for more room. Nothing it says can move anything else.
//
// Thread-safety: NOT thread-safe. Interface thread only.
class CursorReadout : public QLabel {
  Q_OBJECT

 public:
  explicit CursorReadout(QWidget* parent = nullptr);

  // Show a reading. The whole of it is kept whatever the width available, so
  // that widening the window puts back what a narrow one had to leave out.
  void SetReadout(const QString& text);

  // What SetReadout was last given, before any shortening. text() is what is on
  // screen; this is what it means.
  const QString& readout() const { return readout_; }

 protected:
  void resizeEvent(QResizeEvent* event) override;
  void changeEvent(QEvent* event) override;

 private:
  // Put as much of the reading on screen as the current width holds.
  void ShowElided();

  QString readout_;
};

}  // namespace ddd::gui
