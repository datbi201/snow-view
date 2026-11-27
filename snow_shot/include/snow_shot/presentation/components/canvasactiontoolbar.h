// SPDX-License-Identifier: GPL-3.0-only
#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_CANVASACTIONTOOLBAR_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_CANVASACTIONTOOLBAR_H

#include "theme/theme_manager.h"
#include "widgets/button.h"
#include "widgets/divider.h"

#include <QBoxLayout>
#include <QList>
#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <functional>

namespace snow_shot::presentation::components {

class CanvasActionToolbar final : public QWidget {
  public:
    explicit CanvasActionToolbar(QWidget* parent = nullptr, QSize buttonSize = {26, 26},
                                 QSize iconSize = {14, 14})
        : QWidget(parent), m_buttonSize(buttonSize), m_iconSize(iconSize) {
        setAttribute(Qt::WA_NoMousePropagation);
        m_layout = new QBoxLayout(QBoxLayout::LeftToRight, this);
        m_layout->setContentsMargins(3, 2, 3, 2);
        m_layout->setSpacing(0);
        connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged,
                this, [this] {
                    update();
                    if (m_dragHandle)
                        m_dragHandle->update();
                });
    }

    std::function<void(const QPoint& globalPos)> dragStarted;
    std::function<void(const QPoint& globalPos)> dragMoved;
    std::function<void(const QPoint& globalPos)> dragFinished;

    void setDragHandleEnabled(bool enabled) {
        if (enabled && !m_dragHandle) {
            m_dragHandle = new DragHandle(this);
            updateDragHandleGeometry();
            m_layout->insertWidget(0, m_dragHandle, 0, Qt::AlignCenter);
            m_layout->activate();
            adjustSize();
        } else if (!enabled && m_dragHandle) {
            m_layout->removeWidget(m_dragHandle);
            m_dragHandle->deleteLater();
            m_dragHandle = nullptr;
            m_layout->activate();
            adjustSize();
        }
    }

    QWidget* dragHandle() const {
        return m_dragHandle;
    }

    void setOrientation(Qt::Orientation orientation) {
        if (m_orientation == orientation)
            return;
        m_orientation = orientation;
        const bool vertical = orientation == Qt::Vertical;
        m_layout->setDirection(vertical ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
        if (m_buttonSize.width() >= 40)
            m_layout->setContentsMargins(vertical ? 3 : 4, vertical ? 4 : 3, vertical ? 3 : 4,
                                         vertical ? 4 : 3);
        else
            m_layout->setContentsMargins(vertical ? 2 : 3, vertical ? 3 : 2, vertical ? 2 : 3,
                                         vertical ? 3 : 2);
        updateDragHandleGeometry();
        for (auto* divider : m_dividers) {
            if (!divider)
                continue;
            divider->setOrientation(vertical ? adqt::widgets::AdDivider::Orientation::Horizontal
                                             : adqt::widgets::AdDivider::Orientation::Vertical);
            divider->setFixedSize(vertical ? QSize(std::max(14, m_iconSize.width()), 7)
                                           : QSize(7, std::max(14, m_iconSize.height())));
        }
        m_layout->activate();
        adjustSize();
    }

    Qt::Orientation orientation() const {
        return m_orientation;
    }

    adqt::widgets::AdButton* addAction(const adqt::icons::IconRef& icon, const char* name) {
        using adqt::widgets::AdButton;
        auto* button = new AdButton(this);
        button->setObjectName(QString::fromLatin1(name));
        button->setIconRef(icon);
        button->setSizeClass(AdButton::SizeClass::Small);
        button->setButtonStyle(AdButton::ButtonStyle::Text);
        button->setFixedSize(m_buttonSize);
        button->setReferenceIconSize(m_iconSize);
        button->setFocusPolicy(Qt::StrongFocus);
        m_layout->addWidget(button);
        return button;
    }

    void addSeparator() {
        using adqt::widgets::AdDivider;
        auto* divider = new AdDivider(this);
        const bool vertical = m_orientation == Qt::Vertical;
        divider->setOrientation(vertical ? AdDivider::Orientation::Horizontal
                                         : AdDivider::Orientation::Vertical);
        divider->setFixedSize(vertical ? QSize(std::max(14, m_iconSize.width()), 7)
                                       : QSize(7, std::max(14, m_iconSize.height())));
        m_dividers.append(divider);
        m_layout->addWidget(divider, 0, Qt::AlignCenter);
    }

    QSize actionButtonSize() const {
        return m_buttonSize;
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        const auto theme = adqt::theme::ThemeManager::instance().resolveTheme(this);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(theme.colorBorderSecondary);
        painter.setBrush(theme.colorBgContainer);
        painter.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), theme.borderRadiusSM,
                                theme.borderRadiusSM);
    }

  private:
    class DragHandle final : public QWidget {
      public:
        explicit DragHandle(CanvasActionToolbar* owner) : QWidget(owner), m_owner(owner) {
            setObjectName(QStringLiteral("canvasActionToolbarDragHandle"));
            setCursor(Qt::SizeAllCursor);
            setMouseTracking(true);
            setAttribute(Qt::WA_NoMousePropagation);
        }

      protected:
        void mousePressEvent(QMouseEvent* event) override {
            if (event->button() == Qt::LeftButton) {
                m_dragging = true;
                grabMouse();
                if (m_owner && m_owner->dragStarted)
                    m_owner->dragStarted(event->globalPosition().toPoint());
                event->accept();
                return;
            }
            QWidget::mousePressEvent(event);
        }
        void mouseMoveEvent(QMouseEvent* event) override {
            if (m_dragging) {
                if (m_owner && m_owner->dragMoved)
                    m_owner->dragMoved(event->globalPosition().toPoint());
                event->accept();
                return;
            }
            QWidget::mouseMoveEvent(event);
        }
        void mouseReleaseEvent(QMouseEvent* event) override {
            if (m_dragging && event->button() == Qt::LeftButton) {
                m_dragging = false;
                if (mouseGrabber() == this)
                    releaseMouse();
                if (m_owner && m_owner->dragFinished)
                    m_owner->dragFinished(event->globalPosition().toPoint());
                event->accept();
                return;
            }
            QWidget::mouseReleaseEvent(event);
        }
        void paintEvent(QPaintEvent*) override {
            const auto theme = adqt::theme::ThemeManager::instance().resolveTheme(this);
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen);
            painter.setBrush(theme.colorTextQuaternary);
            const bool vertical = m_owner && m_owner->orientation() == Qt::Vertical;
            const QPointF c = QRectF(rect()).center();
            const qreal spacing = 4.5;
            for (int a = -1; a <= 1; ++a) {
                for (int b = -1; b <= 1; b += 2) {
                    const QPointF pt = vertical ? QPointF(c.x() + a * spacing, c.y() + b * 2.2)
                                                : QPointF(c.x() + b * 2.2, c.y() + a * spacing);
                    painter.drawEllipse(pt, 1.35, 1.35);
                }
            }
        }

      private:
        CanvasActionToolbar* m_owner = nullptr;
        bool m_dragging = false;
    };

    void updateDragHandleGeometry() {
        if (!m_dragHandle)
            return;
        if (m_orientation == Qt::Vertical)
            m_dragHandle->setFixedSize(m_buttonSize.width(), 16);
        else
            m_dragHandle->setFixedSize(16, m_buttonSize.height());
    }

    QBoxLayout* m_layout = nullptr;
    QSize m_buttonSize;
    QSize m_iconSize;
    Qt::Orientation m_orientation = Qt::Horizontal;
    DragHandle* m_dragHandle = nullptr;
    QList<adqt::widgets::AdDivider*> m_dividers;
};

} // namespace snow_shot::presentation::components

#endif // SNOW_SHOT_PRESENTATION_COMPONENTS_CANVASACTIONTOOLBAR_H
