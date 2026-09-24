/************************************************************************

    cursor_readout.cpp

    The line of figures under a plot, saying what the pointer is over
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#include "cursor_readout.h"

#include <QEvent>
#include <QFontMetrics>
#include <QResizeEvent>
#include <QSizePolicy>

namespace ddd::gui {

CursorReadout::CursorReadout(QWidget* parent) : QLabel(parent) {
  // The horizontal half is the whole point of the class — see the header. The
  // vertical half is a plain label's, because the height of the row does
  // depend on the text and ought to.
  setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

  // Right-aligned, and given the slack at the end of the row rather than
  // sharing it with a spacer: a spacer holding half of it would shorten
  // readings the row had room to show in full.
  setAlignment(Qt::AlignRight | Qt::AlignVCenter);

  // A reading is a figure somebody may want to write down.
  setTextInteractionFlags(Qt::TextSelectableByMouse);
}

void CursorReadout::SetReadout(const QString& text) {
  readout_ = text;
  ShowElided();
}

void CursorReadout::resizeEvent(QResizeEvent* event) {
  QLabel::resizeEvent(event);
  ShowElided();
}

void CursorReadout::changeEvent(QEvent* event) {
  QLabel::changeEvent(event);

  // The same string is a different width in a different font, so a reading on
  // screen has to be measured again when one arrives.
  if (event->type() == QEvent::FontChange) {
    ShowElided();
  }
}

void CursorReadout::ShowElided() {
  const QString shown = fontMetrics().elidedText(readout_, Qt::ElideRight,
                                                 contentsRect().width());

  // Only when it would differ. This runs from resizeEvent, and setting a
  // label's text updates its geometry — which cannot resize this widget, whose
  // width the layout decides without reference to its text, but there is no
  // reason to ask.
  if (shown != text()) {
    setText(shown);
  }

  // What did not fit is still reachable. Cleared when everything fits, so a
  // tooltip never just repeats what is already on screen.
  setToolTip(shown == readout_ ? QString() : readout_);
}

}  // namespace ddd::gui
