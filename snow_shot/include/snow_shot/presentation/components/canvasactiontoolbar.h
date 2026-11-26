// SPDX-License-Identifier: GPL-3.0-only
#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_CANVASACTIONTOOLBAR_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_CANVASACTIONTOOLBAR_H

#include "theme/theme_manager.h"
#include "widgets/button.h"
#include "widgets/divider.h"

#include <QHBoxLayout>
#include <QPainter>

namespace snow_shot::presentation::components {

class CanvasActionToolbar final : public QWidget {
  public:
    explicit CanvasActionToolbar(QWidget* parent = nullptr, QSize buttonSize = {26, 26},
                                 QSize iconSize = {14, 14})
        : QWidget(parent), m_buttonSize(buttonSize), m_iconSize(iconSize) {
        setAttribute(Qt::WA_NoMousePropagation);
        m_layout = new QHBoxLayout(this);
        m_layout->setContentsMargins(3, 2, 3, 2);
        m_layout->setSpacing(0);
        connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged,
                this, [this] { update(); });
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
        divider->setOrientation(AdDivider::Orientation::Vertical);
        divider->setFixedSize(7, std::max(14, m_iconSize.height()));
        m_layout->addWidget(divider, 0, Qt::AlignVCenter);
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
    QHBoxLayout* m_layout;
    QSize m_buttonSize;
    QSize m_iconSize;
};

} // namespace snow_shot::presentation::components

#endif // SNOW_SHOT_PRESENTATION_COMPONENTS_CANVASACTIONTOOLBAR_H
