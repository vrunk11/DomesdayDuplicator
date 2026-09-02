/************************************************************************

    flow_layout.cpp

    A row of controls that wraps instead of widening the window
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#include "flow_layout.h"

#include <QLayoutItem>
#include <QMargins>
#include <QWidget>
#include <algorithm>

namespace ddd::gui {

FlowLayout::FlowLayout(QWidget* parent, int horizontal_spacing,
                       int vertical_spacing)
    : QLayout(parent),
      horizontal_spacing_(horizontal_spacing),
      vertical_spacing_(vertical_spacing) {}

FlowLayout::~FlowLayout() {
  // A layout owns its items, and QLayout's own destructor does not delete
  // them: takeAt() is how it hands ownership back, so the loop is the
  // destructor.
  while (QLayoutItem* const item = takeAt(0)) {
    delete item;
  }
}

void FlowLayout::AddTrailing(QWidget* widget) {
  addWidget(widget);
  trailing_ = items_.isEmpty() ? nullptr : items_.constLast();
}

void FlowLayout::addItem(QLayoutItem* item) { items_.append(item); }

int FlowLayout::count() const { return static_cast<int>(items_.size()); }

QLayoutItem* FlowLayout::itemAt(int index) const {
  return index >= 0 && index < items_.size() ? items_.at(index) : nullptr;
}

QLayoutItem* FlowLayout::takeAt(int index) {
  if (index < 0 || index >= items_.size()) {
    return nullptr;
  }

  QLayoutItem* const item = items_.takeAt(index);
  if (item == trailing_) {
    trailing_ = nullptr;
  }
  return item;
}

Qt::Orientations FlowLayout::expandingDirections() const {
  // Neither. Height is a consequence of the width the layout is given, not
  // something it asks for, and asking for width is the whole of what this
  // class exists not to do.
  return {};
}

bool FlowLayout::hasHeightForWidth() const { return true; }

int FlowLayout::heightForWidth(int width) const {
  return Arrange(QRect(0, 0, width, 0), true);
}

QSize FlowLayout::minimumSize() const {
  // The widest single item, not the sum of them: one item per row is the
  // narrowest arrangement, so it is also the narrowest this can be asked to
  // be.
  //
  // Measured at each item's *preferred* size, because that is the size
  // Arrange() gives it. Reporting a minimum an item would then overflow would
  // be worse than reporting an honest one.
  QSize size;
  for (QLayoutItem* const item : items_) {
    if (item->isEmpty() || item == trailing_) {
      continue;
    }
    size = size.expandedTo(item->sizeHint().expandedTo(item->minimumSize()));
  }

  // The trailing item's height counts even though its width does not: it is on
  // one of these rows and the row has to hold it.
  if (trailing_ != nullptr && !trailing_->isEmpty()) {
    size.setHeight(std::max(size.height(), trailing_->minimumSize().height()));
  }

  const QMargins margins = contentsMargins();
  return size + QSize(margins.left() + margins.right(),
                      margins.top() + margins.bottom());
}

QSize FlowLayout::sizeHint() const {
  // One row of everything: the width at which nothing has to wrap. This is a
  // preference and not a demand — minimumSize() is what a window is actually
  // held to — so a layout that has the room lays out as a single row and one
  // that has not gives up width before any of its neighbours have to.
  const int spacing = HorizontalSpacing();

  int width = 0;
  int height = 0;
  bool first = true;
  for (QLayoutItem* const item : items_) {
    if (item->isEmpty() || item == trailing_) {
      continue;
    }

    const QSize hint = item->sizeHint().expandedTo(item->minimumSize());
    width += hint.width() + (first ? 0 : spacing);
    height = std::max(height, hint.height());
    first = false;
  }

  if (trailing_ != nullptr && !trailing_->isEmpty()) {
    height = std::max(height, trailing_->sizeHint().height());
  }

  const QMargins margins = contentsMargins();
  return QSize(width, height) + QSize(margins.left() + margins.right(),
                                      margins.top() + margins.bottom());
}

void FlowLayout::setGeometry(const QRect& rect) {
  QLayout::setGeometry(rect);
  Arrange(rect, false);
}

int FlowLayout::HorizontalSpacing() const {
  return horizontal_spacing_ >= 0
             ? horizontal_spacing_
             : StyleSpacing(QStyle::PM_LayoutHorizontalSpacing);
}

int FlowLayout::VerticalSpacing() const {
  return vertical_spacing_ >= 0
             ? vertical_spacing_
             : StyleSpacing(QStyle::PM_LayoutVerticalSpacing);
}

int FlowLayout::StyleSpacing(QStyle::PixelMetric metric) const {
  QObject* const owner = parent();
  if (owner == nullptr) {
    return 0;
  }
  if (owner->isWidgetType()) {
    QWidget* const widget = static_cast<QWidget*>(owner);
    return widget->style()->pixelMetric(metric, nullptr, widget);
  }
  return static_cast<QLayout*>(owner)->spacing();
}

int FlowLayout::Arrange(const QRect& rect, bool measure_only) const {
  const QMargins margins = contentsMargins();
  const QRect content = rect.adjusted(margins.left(), margins.top(),
                                      -margins.right(), -margins.bottom());

  const int h_spacing = HorizontalSpacing();
  const int v_spacing = VerticalSpacing();

  int x = content.x();
  int y = content.y();
  int row_height = 0;

  for (QLayoutItem* const item : items_) {
    // A hidden widget is not a gap in the row: the spectrum panel shows and
    // hides whole controls as the view changes, and a row that reserved their
    // places would wrap for controls nobody can see.
    if (item->isEmpty() || item == trailing_) {
      continue;
    }

    const QSize hint = item->sizeHint().expandedTo(item->minimumSize());

    // Wrap only when something is already on this row. An item wider than the
    // whole rect goes on a row of its own and overflows it, which is the
    // honest outcome: the layout said it needed that much in minimumSize().
    if (row_height > 0 && x + hint.width() - 1 > content.right()) {
      x = content.x();
      y += row_height + v_spacing;
      row_height = 0;
    }

    if (!measure_only) {
      item->setGeometry(QRect(QPoint(x, y), hint));
    }

    x += hint.width() + h_spacing;
    row_height = std::max(row_height, hint.height());
  }

  // Whatever is left of the row the last item landed on. Never wrapped, and
  // allowed to come out empty: an elided readout showing nothing is the right
  // answer at a width that has nothing to spare.
  if (trailing_ != nullptr && !trailing_->isEmpty()) {
    const int height = std::max(row_height, trailing_->sizeHint().height());
    if (!measure_only) {
      trailing_->setGeometry(
          QRect(x, y, std::max(0, content.right() - x + 1), height));
    }
    row_height = height;
  }

  return y + row_height - rect.y() + margins.bottom();
}

}  // namespace ddd::gui
