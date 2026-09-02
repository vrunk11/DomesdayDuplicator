/************************************************************************

    test_cursor_readout.cpp

    T1 tests for the pointer readout under a plot
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#include <gtest/gtest.h>

#include <QApplication>
#include <QHBoxLayout>
#include <QPushButton>
#include <QString>
#include <QWidget>

#include "cursor_readout.h"

namespace ddd::gui {
namespace {

QString LongReading() {
  return QStringLiteral("12.345 µs · code 812 · +412 mV");
}

// A control row of the shape the monitor panels have: fixed controls on the
// left and the readout filling what is left at the right.
struct Row {
  QWidget widget;
  CursorReadout* readout = nullptr;

  Row() {
    auto* layout = new QHBoxLayout(&widget);
    layout->addWidget(new QPushButton(QStringLiteral("Reset peaks"), &widget));
    readout = new CursorReadout(&widget);
    layout->addWidget(readout, 1);
  }

  // Lay the row out at a given width, as the panel's layout would.
  void Settle(int width) {
    widget.resize(width, 40);
    widget.show();
    QApplication::processEvents();
  }
};

TEST(CursorReadoutTest, ShowsTheWholeReadingWhenThereIsRoom) {
  Row row;
  row.Settle(600);
  row.readout->SetReadout(LongReading());

  EXPECT_EQ(row.readout->text(), LongReading());
  EXPECT_EQ(row.readout->readout(), LongReading());

  // Nothing was left out, so there is nothing for a tooltip to add.
  EXPECT_TRUE(row.readout->toolTip().isEmpty());
}

TEST(CursorReadoutTest, ShortensAReadingThatDoesNotFitAndKeepsTheWhole) {
  Row row;
  row.Settle(600);
  row.readout->SetReadout(LongReading());
  ASSERT_EQ(row.readout->text(), LongReading());

  row.Settle(200);

  EXPECT_NE(row.readout->text(), LongReading())
      << "the reading was drawn at its full width in a row too narrow for it";
  EXPECT_LT(row.readout->text().size(), LongReading().size());

  // Shortened on screen only. What the reading actually was is still held, so
  // widening the window puts it back, and the tooltip has it meanwhile.
  EXPECT_EQ(row.readout->readout(), LongReading());
  EXPECT_EQ(row.readout->toolTip(), LongReading());

  row.Settle(600);
  EXPECT_EQ(row.readout->text(), LongReading());
}

TEST(CursorReadoutTest, AsksTheLayoutForNoWidthOfItsOwn) {
  // This is issue #180. The readout changes with every mouse move, and the row
  // it sits in is what decides the minimum width of the panel and so of the
  // dock. If the reading were allowed to set that minimum, then in a window too
  // small to satisfy every dock's minimum at once the dock layout would share
  // the space out afresh on every move of the pointer — which a user sees as
  // the panels jumping about while they are trying to read a trace.
  Row row;
  row.Settle(600);

  const int idle = row.widget.minimumSizeHint().width();

  row.readout->SetReadout(LongReading());
  row.widget.layout()->activate();
  EXPECT_EQ(row.widget.minimumSizeHint().width(), idle);

  row.readout->SetReadout(
      QStringLiteral("a reading very much longer than any of the controls "
                     "beside it could ever be"));
  row.widget.layout()->activate();
  EXPECT_EQ(row.widget.minimumSizeHint().width(), idle);

  // The preferred width is the other half of it: a dock area that fits shares
  // itself out by size hint rather than by minimum.
  const int hint = row.widget.sizeHint().width();
  row.readout->SetReadout(QStringLiteral("1 µs"));
  row.widget.layout()->activate();
  EXPECT_EQ(row.widget.sizeHint().width(), hint);
}

}  // namespace
}  // namespace ddd::gui
