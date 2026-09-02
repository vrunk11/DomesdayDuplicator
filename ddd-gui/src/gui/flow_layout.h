/************************************************************************

    flow_layout.h

    A row of controls that wraps instead of widening the window
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#pragma once

#include <QLayout>
#include <QList>
#include <QRect>
#include <QSize>
#include <QStyle>

class QLayoutItem;
class QWidget;

namespace ddd::gui {

// Items laid left to right, starting a new row when the next one will not fit.
//
// The point of it is the minimum width. A QHBoxLayout's minimum is the sum of
// everything in it, so a row of ten controls is a floor under the panel, the
// dock and the window in turn — and it is a floor nobody chose: it is whatever
// the widest item of every combo box happens to measure in the user's font.
// The spectrum panel's row came to 959 pixels that way, which was more than
// two thirds of the default window, and the panels that had to give the space
// up were the ones with modest minimums of their own. That is issue #181.
//
// Here the minimum is the widest single item instead, and a narrow panel costs
// height rather than refusing to be narrow. Nothing in it is a pixel figure, so
// it holds in a larger system font, at a different display scale, and in a
// translation whose words are longer — none of which a hand-tuned two-row
// arrangement would survive.
//
// Thread-safety: NOT thread-safe. Interface thread only.
class FlowLayout : public QLayout {
 public:
  // Spacing of -1 means the style's own, per direction, which is what makes a
  // row here sit the same way every other row on the platform does.
  //
  // It is a constructor argument and not setSpacing() because QLayout has no
  // way to tell a custom layout that setSpacing() was never called: spacing()
  // answers with the style's horizontal figure either way, so honouring it
  // would silently use the horizontal spacing between rows as well. Anything
  // passed to setSpacing() on one of these is ignored.
  explicit FlowLayout(QWidget* parent = nullptr, int horizontal_spacing = -1,
                      int vertical_spacing = -1);
  ~FlowLayout() override;

  FlowLayout(const FlowLayout&) = delete;
  FlowLayout& operator=(const FlowLayout&) = delete;
  FlowLayout(FlowLayout&&) = delete;
  FlowLayout& operator=(FlowLayout&&) = delete;

  // The item that takes whatever width is left over at the end of the last
  // row.
  //
  // It is asked for nothing and it never causes a wrap, so it cannot widen the
  // layout however much it holds — which is what a plot's cursor readout has
  // to be: its text changes with every mouse move, and a row whose width
  // followed it would re-lay the whole window out while the pointer was over
  // the trace. See CursorReadout, and issue #180.
  //
  // One per layout; a second call replaces the first.
  void AddTrailing(QWidget* widget);

  void addItem(QLayoutItem* item) override;
  int count() const override;
  QLayoutItem* itemAt(int index) const override;
  QLayoutItem* takeAt(int index) override;

  Qt::Orientations expandingDirections() const override;
  bool hasHeightForWidth() const override;
  int heightForWidth(int width) const override;
  QSize minimumSize() const override;
  QSize sizeHint() const override;
  void setGeometry(const QRect& rect) override;

  // What the constructor was given, or the style's figure for that direction.
  int HorizontalSpacing() const;
  int VerticalSpacing() const;

 private:
  // The style's spacing for one direction, asked of the widget this layout
  // belongs to so that a styled window and a plain one each get their own.
  int StyleSpacing(QStyle::PixelMetric metric) const;

  // Place the items in rect, or — when measuring — work out how tall doing so
  // would be without moving anything. Returns the height used.
  int Arrange(const QRect& rect, bool measure_only) const;

  QList<QLayoutItem*> items_;

  // Held as the item rather than as an index, so that a takeAt() of something
  // else cannot silently promote a different item to trailing.
  QLayoutItem* trailing_ = nullptr;

  // Negative for "ask the style".
  int horizontal_spacing_ = -1;
  int vertical_spacing_ = -1;
};

}  // namespace ddd::gui
