/************************************************************************

    test_flow_layout.cpp

    T1 tests for the wrapping control row
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#include <gtest/gtest.h>

#include <QApplication>
#include <QLayout>
#include <QSizePolicy>
#include <QWidget>

#include "flow_layout.h"

namespace ddd::gui {
namespace {

constexpr int kBlockWidth = 100;
constexpr int kBlockHeight = 20;

// A host whose layout has no margins and no spacing, so that every figure a
// test checks is the layout's arithmetic and not the platform style's.
struct Host {
  QWidget widget;
  FlowLayout* flow = nullptr;

  Host() {
    flow = new FlowLayout(&widget, 0, 0);
    flow->setContentsMargins(0, 0, 0, 0);
  }

  // A fixed-size item, so its sizeHint is a number the test chose.
  QWidget* Add(int width = kBlockWidth) {
    auto* const block = new QWidget(&widget);
    block->setFixedSize(width, kBlockHeight);
    flow->addWidget(block);
    return block;
  }

  // Something shaped like a cursor readout: it asks for nothing and takes what
  // is left.
  QWidget* AddTrailing() {
    auto* const block = new QWidget(&widget);
    block->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    block->setFixedHeight(kBlockHeight);
    flow->AddTrailing(block);
    return block;
  }

  void Settle(int width) {
    widget.resize(width, 200);
    widget.show();
    QApplication::processEvents();
    flow->activate();
  }
};

TEST(FlowLayoutTest, StartsANewRowWhenTheNextItemWillNotFit) {
  Host host;
  QWidget* const first = host.Add();
  QWidget* const second = host.Add();
  QWidget* const third = host.Add();

  // Room for two of the three.
  host.Settle(2 * kBlockWidth + kBlockWidth / 2);

  EXPECT_EQ(second->y(), first->y());
  EXPECT_GT(third->y(), first->y());

  // And the row it started begins at the left again, rather than continuing
  // off the edge.
  EXPECT_EQ(third->x(), first->x());
}

TEST(FlowLayoutTest, TheMinimumWidthIsTheWidestItemAndNotTheSum) {
  Host host;
  host.Add();
  host.Add();
  host.Add(kBlockWidth * 2);

  // The whole point of the class. A QHBoxLayout here would report 400.
  EXPECT_EQ(host.flow->minimumSize().width(), kBlockWidth * 2);

  // The preference is still one row, so a panel that has the room lays out as
  // one and a panel that has not gives up width before its neighbours do.
  EXPECT_EQ(host.flow->sizeHint().width(), kBlockWidth * 4);
}

TEST(FlowLayoutTest,
     HeightIsWhatTheGivenWidthCostsRatherThanSomethingAskedFor) {
  Host host;
  host.Add();
  host.Add();

  EXPECT_TRUE(host.flow->hasHeightForWidth());
  EXPECT_EQ(host.flow->heightForWidth(2 * kBlockWidth), kBlockHeight);
  EXPECT_EQ(host.flow->heightForWidth(kBlockWidth), 2 * kBlockHeight);

  // Nothing is asked for in either direction: width is what the panel gives it
  // and height is what that width costs.
  EXPECT_EQ(host.flow->expandingDirections(), Qt::Orientations{});
}

TEST(FlowLayoutTest, AHiddenItemTakesNoPlaceInTheRow) {
  Host host;
  QWidget* const first = host.Add();
  QWidget* const hidden = host.Add();
  QWidget* const last = host.Add();

  hidden->hide();
  host.Settle(2 * kBlockWidth + kBlockWidth / 2);

  // Two items to place, so the third does not wrap — and it takes the hidden
  // one's place rather than leaving a gap where it used to be. The spectrum
  // panel hides whole controls when its view changes, and a row that reserved
  // their places would wrap for controls nobody can see.
  EXPECT_EQ(last->y(), first->y());
  EXPECT_EQ(last->x(), first->x() + kBlockWidth);
}

TEST(FlowLayoutTest, TheTrailingItemTakesWhatIsLeftOfTheLastRow) {
  Host host;
  host.Add();
  host.Add();
  QWidget* const trailing = host.AddTrailing();

  constexpr int kWidth = 3 * kBlockWidth;
  host.Settle(kWidth);

  EXPECT_EQ(trailing->x(), 2 * kBlockWidth);
  EXPECT_EQ(trailing->width(), kWidth - 2 * kBlockWidth);
}

TEST(FlowLayoutTest, TheTrailingItemNeitherWidensTheLayoutNorWrapsIt) {
  Host host;
  QWidget* const first = host.Add();
  QWidget* const second = host.Add();
  QWidget* const trailing = host.AddTrailing();

  // It is not in either figure — that is what lets a readout hold whatever it
  // likes without the window being re-laid out around it. Issue #180.
  EXPECT_EQ(host.flow->minimumSize().width(), kBlockWidth);
  EXPECT_EQ(host.flow->sizeHint().width(), 2 * kBlockWidth);

  // Exactly the two blocks, so there is nothing left over. The trailing item
  // stays on their row and comes out empty rather than starting one of its
  // own.
  host.Settle(2 * kBlockWidth);

  EXPECT_EQ(second->y(), first->y());
  EXPECT_EQ(trailing->y(), first->y());
  EXPECT_EQ(trailing->width(), 0);
}

}  // namespace
}  // namespace ddd::gui
