// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/presentation/imageeditingcanvas.h"
#include "snow_shot/presentation/canvasstatusreadout.h"
#include "snow_shot/presentation/canvastexteditingpreferences.h"
#include "snow_shot/presentation/screenshotcanvasrenderer.h"
#include "snow_draw_engine_qt/snow_canvas_overlay_style.h"
#include <QPainter>
#include <QPainterPath>
#include <QCursor>
#include <QPolygonF>
#include <QCoreApplication>
#include <QFontMetricsF>
#include <QMouseEvent>
#include <QEnterEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QResizeEvent>
#include <algorithm>
#include <array>
#include <cmath>
#include <tuple>

namespace snow_shot::presentation {
namespace {
constexpr double kCropPi = 3.14159265358979323846;
constexpr qreal kMaxCropExtent = 16384.0;

int normalizeCropDegrees180(double degrees) {
    int wrapped = qRound(degrees) % 360;
    if (wrapped > 180)
        wrapped -= 360;
    else if (wrapped < -180)
        wrapped += 360;
    return wrapped;
}

QCursor cropRotateCursor() {
    static const QCursor cursor = [] {
        QPixmap pm(24, 24);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRectF arcRect(4.5, 4.5, 15.0, 15.0);
        const auto drawArrow = [&](const QPen& pen) {
            p.setPen(pen);
            p.drawArc(arcRect, 20 * 16, 130 * 16);
            p.drawLine(QPointF(17.5, 6.2), QPointF(18.6, 10.1));
            p.drawLine(QPointF(17.5, 6.2), QPointF(13.8, 7.6));
            p.drawLine(QPointF(6.5, 17.8), QPointF(5.4, 13.9));
            p.drawLine(QPointF(6.5, 17.8), QPointF(10.2, 16.4));
            p.drawArc(arcRect, 200 * 16, 130 * 16);
        };
        drawArrow(QPen(QColor(0, 0, 0, 210), 3.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        drawArrow(QPen(Qt::white, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.end();
        return QCursor(pm, 12, 12);
    }();
    return cursor;
}
} // namespace

ImageEditingCanvas::ImageEditingCanvas(ImageEditingDocument& document, QWidget* parent)
    : SnowCanvasWidget(document.runtime(), parent), m_document(document),
      m_zoomReadout(new CanvasStatusReadout(this)) {
    bindCanvasTextEditingPreferences(*this);
    setObjectName(QStringLiteral("imageEditingCanvas"));
    setCustomRenderer(this);
    setClearBackgroundEnabled(false);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setAutoFillBackground(false);
    setWheelZoomEnabled(false);
    setMouseTracking(true);
    setMode(Mode::Pan);
    m_zoomReadout->setObjectName(QStringLiteral("imageEditingZoom"));
    m_zoomReadout->layoutIn(rect());
    connect(&document, &ImageEditingDocument::baseImageChanged, this, [this] { syncImage(); });
    connect(&document, &ImageEditingDocument::previewImageChanged, this, [this] {
        if (m_document.runtime().hasDocumentContent())
            setBaseImageSources(baseImageSources());
        update();
    });
}
ImageEditingCanvas::~ImageEditingCanvas() {
    setCustomRenderer(nullptr);
}
void ImageEditingCanvas::setMode(Mode mode) {
    m_cropping = m_panning = false;
    m_cropDragHandle = CropHandle::None;
    m_cropHoverHandle = CropHandle::None;
    m_cropAngleDeg = 0;
    m_cropBaseSize = {};
    m_mode = mode;
    setInteractionEnabled(mode == Mode::Draw);
    if (mode == Mode::CropRect) {
        // Allow rotated source pixels and extended crop regions outside original bounds.
        setContentClip(std::nullopt);
        initializeCropRect(m_cropAspectRatio);
        setCursor(Qt::ArrowCursor);
    } else {
        setContentClip(m_document.bounds());
        m_cropRect = {};
        setCursor(mode == Mode::Pan ? Qt::OpenHandCursor : Qt::CrossCursor);
    }
    update();
}
void ImageEditingCanvas::setCropAspectRatio(std::optional<double> ratio) {
    m_cropAspectRatio = (ratio && *ratio > 0.0) ? ratio : std::nullopt;
    if (m_mode == Mode::CropRect) {
        initializeCropRect(m_cropAspectRatio);
        update();
    }
}
void ImageEditingCanvas::setCropAngleDegrees(int degrees) {
    const int clamped = std::clamp(degrees, -180, 180);
    if (m_mode != Mode::CropRect) {
        m_cropAngleDeg = clamped;
        return;
    }
    if (clamped == m_cropAngleDeg && m_cropRect.isValid())
        return;
    m_cropAngleDeg = clamped;
    notifyCropRectChanged();
    update();
}
void ImageEditingCanvas::setCropExtensionFillColor(QColor color) {
    if (m_cropExtensionFillColor == color)
        return;
    m_cropExtensionFillColor = color;
    if (m_mode == Mode::CropRect)
        update();
}
void ImageEditingCanvas::resetCropRect() {
    if (m_mode == Mode::CropRect) {
        m_cropAngleDeg = 0;
        initializeCropRect(m_cropAspectRatio);
        update();
    }
}
QRect ImageEditingCanvas::cropPixelRect() const {
    if (m_document.image().isNull() || !m_cropRect.isValid())
        return {};
    const int w = std::clamp(qRound(m_cropRect.width()), 1, static_cast<int>(kMaxCropExtent));
    const int h = std::clamp(qRound(m_cropRect.height()), 1, static_cast<int>(kMaxCropExtent));
    return QRect(qRound(m_cropRect.left()), qRound(m_cropRect.top()), w, h);
}
bool ImageEditingCanvas::commitCrop() {
    if (m_mode != Mode::CropRect)
        return false;
    const QRectF box = cropPixelBox();
    if (!box.isValid() || box.isEmpty())
        return false;
    if (cropRectCommitted)
        cropRectCommitted(box, m_cropAngleDeg);
    return true;
}
void ImageEditingCanvas::cancelCropRect() {
    if (m_mode != Mode::CropRect)
        return;
    m_cropDragHandle = CropHandle::None;
    if (cropRectCancelled)
        cropRectCancelled();
}
void ImageEditingCanvas::syncImage() {
    m_zoomReadout->dismiss();
    setContentClip(m_mode == Mode::CropRect ? std::nullopt
                                            : std::optional<QRectF>(m_document.bounds()));
    setDecorationRenderAreas({m_document.bounds(), m_document.bounds()});
    setBaseImageSources(baseImageSources());
    if (m_mode == Mode::CropRect)
        initializeCropRect(m_cropAspectRatio);
    if (!m_fitted)
        fit();
    else
        syncCamera();
    update();
}
void ImageEditingCanvas::fit(bool showZoomReadout) {
    const auto bounds = m_document.bounds();
    if (bounds.isEmpty() || width() <= 0 || height() <= 0)
        return;
    m_center = bounds.center();
    m_zoom = std::min(std::max(1, width() - 48) / bounds.width(),
                      std::max(1, height() - 48) / bounds.height());
    m_fitted = true;
    syncCamera(showZoomReadout);
}
void ImageEditingCanvas::actualSize() {
    if (m_document.bounds().isEmpty())
        return;
    m_zoom = actualSizeZoom();
    syncCamera(true);
}
double ImageEditingCanvas::actualSizeZoom() const {
    return m_document.image().width() / m_document.bounds().width() / devicePixelRatioF();
}
void ImageEditingCanvas::syncCamera(bool showZoomReadout) {
    setViewportCamera(m_center.x(), m_center.y(), m_zoom);
    if (!m_document.bounds().isEmpty())
        m_zoomReadout->setScale(m_zoom / actualSizeZoom(), showZoomReadout && m_showImage);
    if (viewportChanged)
        viewportChanged();
}
void ImageEditingCanvas::setRecognitionDisplay(bool active, bool showImage, bool showFiltered) {
    if (m_recognitionActive == active && m_showImage == showImage && m_showFiltered == showFiltered)
        return;
    m_recognitionActive = active;
    m_showImage = showImage;
    m_showFiltered = showFiltered;
    if (!showImage)
        m_zoomReadout->dismiss();
    setCanvasContentVisible(!active);
    update();
}
void ImageEditingCanvas::setRecognitionBackground(QImage image, const QRectF& canvasRect) {
    if (m_recognitionBackground.isNull() && image.isNull())
        return;
    const QRect previous = viewRectForCanvasRect(m_recognitionBackgroundRect, 1);
    m_recognitionBackground = std::move(image);
    m_recognitionBackgroundRect = m_recognitionBackground.isNull() ? QRectF() : canvasRect;
    update(QRegion(previous) | viewRectForCanvasRect(m_recognitionBackgroundRect, 1));
}
QPointF ImageEditingCanvas::pixelAt(QPointF view) const {
    return m_document.imagePixelAt(canvasToViewTransform().inverted().map(view));
}
QRectF ImageEditingCanvas::cropCanvasRect(const QRectF& pixelRect) const {
    const QSize imgSize = m_document.image().size();
    const QRectF bounds = m_document.bounds();
    if (imgSize.isEmpty() || bounds.isEmpty() || !pixelRect.isValid())
        return {};
    const qreal scaleX = bounds.width() / imgSize.width();
    const qreal scaleY = bounds.height() / imgSize.height();
    return QRectF(bounds.x() + pixelRect.x() * scaleX, bounds.y() + pixelRect.y() * scaleY,
                  pixelRect.width() * scaleX, pixelRect.height() * scaleY);
}
std::pair<QPointF, QPointF> ImageEditingCanvas::cropPixelAxes() const {
    return {QPointF(1.0, 0.0), QPointF(0.0, 1.0)};
}
std::array<QPointF, 4> ImageEditingCanvas::cropViewCorners() const {
    const QSize imgSize = m_document.image().size();
    const QRectF bounds = m_document.bounds();
    if (m_mode != Mode::CropRect || !m_cropRect.isValid() || imgSize.isEmpty() || bounds.isEmpty())
        return {};
    const qreal sx = bounds.width() / imgSize.width();
    const qreal sy = bounds.height() / imgSize.height();
    const QTransform xform = canvasToViewTransform();
    const auto toView = [&](const QPointF& px) {
        return xform.map(QPointF(bounds.x() + px.x() * sx, bounds.y() + px.y() * sy));
    };
    return {toView(m_cropRect.topLeft()), toView(m_cropRect.topRight()),
            toView(m_cropRect.bottomRight()), toView(m_cropRect.bottomLeft())};
}
QRectF ImageEditingCanvas::cropViewRect() const {
    if (m_mode != Mode::CropRect || !m_cropRect.isValid())
        return {};
    const auto corners = cropViewCorners();
    QPolygonF poly;
    poly << corners[0] << corners[1] << corners[2] << corners[3];
    return poly.boundingRect();
}
ImageEditingCanvas::CropHandle ImageEditingCanvas::cropHandleAt(const QPointF& viewPos) const {
    if (m_mode != Mode::CropRect || !m_cropRect.isValid())
        return CropHandle::None;
    const auto corners = cropViewCorners();
    const QPointF& tl = corners[0];
    const QPointF& tr = corners[1];
    const QPointF& bl = corners[3];
    const QPointF uVec = tr - tl;
    const QPointF vVec = bl - tl;
    const qreal viewW = std::hypot(uVec.x(), uVec.y());
    const qreal viewH = std::hypot(vVec.x(), vVec.y());
    if (viewW < 1e-6 || viewH < 1e-6)
        return CropHandle::None;
    const QPointF uV = uVec / viewW;
    const QPointF vV = vVec / viewH;
    const QPointF centerView = (corners[0] + corners[2]) * 0.5;
    const QPointF rel = viewPos - centerView;
    const qreal lx = QPointF::dotProduct(rel, uV);
    const qreal ly = QPointF::dotProduct(rel, vV);
    const qreal hw = viewW * 0.5;
    const qreal hh = viewH * 0.5;

    constexpr qreal kCornerRadius = 11.0;
    constexpr qreal kEdgeHit = 8.0;
    constexpr qreal kRotateOuterRadius = 42.0;
    struct CornerCandidate {
        CropHandle handle;
        QPointF pos;
    };
    const std::array<CornerCandidate, 4> cornerList{{
        {CropHandle::TopLeft, corners[0]},
        {CropHandle::TopRight, corners[1]},
        {CropHandle::BottomRight, corners[2]},
        {CropHandle::BottomLeft, corners[3]},
    }};
    CropHandle nearestCorner = CropHandle::None;
    qreal nearestDist = kCornerRadius;
    for (const auto& c : cornerList) {
        const qreal d = std::hypot(viewPos.x() - c.pos.x(), viewPos.y() - c.pos.y());
        if (d <= nearestDist) {
            nearestDist = d;
            nearestCorner = c.handle;
        }
    }
    if (nearestCorner != CropHandle::None)
        return nearestCorner;
    if (std::abs(lx) <= hw + kEdgeHit) {
        if (std::abs(ly + hh) <= kEdgeHit)
            return CropHandle::Top;
        if (std::abs(ly - hh) <= kEdgeHit)
            return CropHandle::Bottom;
    }
    if (std::abs(ly) <= hh + kEdgeHit) {
        if (std::abs(lx + hw) <= kEdgeHit)
            return CropHandle::Left;
        if (std::abs(lx - hw) <= kEdgeHit)
            return CropHandle::Right;
    }
    if (std::abs(lx) <= hw && std::abs(ly) <= hh)
        return CropHandle::Move;

    qreal minOutsideCornerDist = kRotateOuterRadius + 1.0;
    for (const auto& c : cornerList) {
        const qreal d = std::hypot(viewPos.x() - c.pos.x(), viewPos.y() - c.pos.y());
        minOutsideCornerDist = std::min(minOutsideCornerDist, d);
    }
    if (minOutsideCornerDist <= kRotateOuterRadius)
        return CropHandle::Rotate;
    return CropHandle::Create;
}
QCursor ImageEditingCanvas::cropCursorForHandle(CropHandle handle, bool dragging) const {
    switch (handle) {
    case CropHandle::TopLeft:
    case CropHandle::BottomRight:
        return Qt::SizeFDiagCursor;
    case CropHandle::TopRight:
    case CropHandle::BottomLeft:
        return Qt::SizeBDiagCursor;
    case CropHandle::Top:
    case CropHandle::Bottom:
        return Qt::SizeVerCursor;
    case CropHandle::Left:
    case CropHandle::Right:
        return Qt::SizeHorCursor;
    case CropHandle::Rotate:
        return cropRotateCursor();
    case CropHandle::Move:
        return dragging ? Qt::ClosedHandCursor : Qt::OpenHandCursor;
    case CropHandle::Create:
        return Qt::CrossCursor;
    case CropHandle::None:
        return Qt::ArrowCursor;
    }
    return Qt::ArrowCursor;
}
void ImageEditingCanvas::initializeCropRect(std::optional<double> ratio) {
    const QSize imgSize = m_document.image().size();
    if (imgSize.isEmpty()) {
        m_cropRect = {};
        m_cropBaseSize = {};
        notifyCropRectChanged();
        return;
    }
    const qreal w = imgSize.width();
    const qreal h = imgSize.height();
    if (!ratio || *ratio <= 0.0) {
        m_cropRect = QRectF(0.0, 0.0, w, h);
    } else {
        const qreal r = *ratio;
        qreal cw = w;
        qreal ch = cw / r;
        if (ch > h) {
            ch = h;
            cw = ch * r;
        }
        cw = std::clamp(cw, 1.0, kMaxCropExtent);
        ch = std::clamp(ch, 1.0, kMaxCropExtent);
        m_cropRect = QRectF((w - cw) * 0.5, (h - ch) * 0.5, cw, ch);
    }
    m_cropBaseSize = m_cropRect.size();
    notifyCropRectChanged();
}
void ImageEditingCanvas::notifyCropRectChanged() {
    if (cropRectChanged && m_mode == Mode::CropRect)
        cropRectChanged(cropPixelRect(), m_cropAngleDeg);
}
void ImageEditingCanvas::updateCropDrag(const QPointF& pixelPos, Qt::KeyboardModifiers modifiers) {
    const QSize imgSize = m_document.image().size();
    if (imgSize.isEmpty() || m_cropDragHandle == CropHandle::None)
        return;
    const qreal imgW = imgSize.width();
    const qreal imgH = imgSize.height();
    if (m_cropDragHandle == CropHandle::Rotate) {
        const QPointF center = m_cropDragStartRect.center();
        const double curAngle =
            std::atan2(pixelPos.y() - center.y(), pixelPos.x() - center.x()) * (180.0 / kCropPi);
        double dAngle = curAngle - m_cropDragStartPointerAngle;
        while (dAngle > 180.0)
            dAngle -= 360.0;
        while (dAngle < -180.0)
            dAngle += 360.0;
        const double raw = m_cropDragStartAngle + dAngle;
        const int snapped = modifiers.testFlag(Qt::ShiftModifier)
                                ? normalizeCropDegrees180(qRound(raw / 15.0) * 15.0)
                                : normalizeCropDegrees180(raw);
        setCropAngleDegrees(snapped);
        return;
    }

    const QPointF delta = pixelPos - m_cropDragStartPixel;
    const bool altCenter = modifiers.testFlag(Qt::AltModifier);
    std::optional<double> activeRatio = m_cropAspectRatio;
    if (!activeRatio && modifiers.testFlag(Qt::ShiftModifier) &&
        m_cropDragHandle != CropHandle::Move) {
        if (m_cropDragRatio && *m_cropDragRatio > 0.0)
            activeRatio = m_cropDragRatio;
        else if (m_cropDragStartRect.height() > 0.0)
            activeRatio = m_cropDragStartRect.width() / m_cropDragStartRect.height();
    }

    QRectF next = m_cropRect;
    const qreal minCoordX = -kMaxCropExtent;
    const qreal maxCoordX = imgW + kMaxCropExtent;
    const qreal minCoordY = -kMaxCropExtent;
    const qreal maxCoordY = imgH + kMaxCropExtent;
    if (m_cropDragHandle == CropHandle::Move) {
        const qreal w = m_cropDragStartRect.width();
        const qreal h = m_cropDragStartRect.height();
        const qreal minCx = minCoordX + w * 0.5;
        const qreal maxCx = maxCoordX - w * 0.5;
        const qreal minCy = minCoordY + h * 0.5;
        const qreal maxCy = maxCoordY - h * 0.5;
        const qreal cx =
            std::clamp(m_cropDragStartRect.center().x() + delta.x(), minCx, std::max(minCx, maxCx));
        const qreal cy =
            std::clamp(m_cropDragStartRect.center().y() + delta.y(), minCy, std::max(minCy, maxCy));
        next = QRectF(cx - w * 0.5, cy - h * 0.5, w, h);
    } else if (m_cropDragHandle == CropHandle::Create) {
        const QPointF anchor(std::clamp(m_cropDragStartPixel.x(), minCoordX, maxCoordX),
                             std::clamp(m_cropDragStartPixel.y(), minCoordY, maxCoordY));
        const QPointF cur(std::clamp(pixelPos.x(), minCoordX, maxCoordX),
                          std::clamp(pixelPos.y(), minCoordY, maxCoordY));
        if (std::hypot(cur.x() - anchor.x(), cur.y() - anchor.y()) < 2.0)
            return;
        if (altCenter) {
            const qreal maxHalfW =
                std::min({anchor.x() - minCoordX, maxCoordX - anchor.x(), kMaxCropExtent * 0.5});
            const qreal maxHalfH =
                std::min({anchor.y() - minCoordY, maxCoordY - anchor.y(), kMaxCropExtent * 0.5});
            qreal halfW = std::clamp(std::abs(cur.x() - anchor.x()), 0.5, maxHalfW);
            qreal halfH = std::clamp(std::abs(cur.y() - anchor.y()), 0.5, maxHalfH);
            if (activeRatio && *activeRatio > 0.0) {
                const qreal r = *activeRatio;
                const qreal limW = std::min(maxHalfW, maxHalfH * r);
                halfW = std::clamp(std::max(halfW, halfH * r), std::min(0.5, limW), limW);
                halfH = halfW / r;
            }
            next = QRectF(anchor.x() - halfW, anchor.y() - halfH, halfW * 2.0, halfH * 2.0);
        } else {
            const qreal sx = cur.x() >= anchor.x() ? 1.0 : -1.0;
            const qreal sy = cur.y() >= anchor.y() ? 1.0 : -1.0;
            const qreal maxW =
                std::min(sx > 0 ? maxCoordX - anchor.x() : anchor.x() - minCoordX, kMaxCropExtent);
            const qreal maxH =
                std::min(sy > 0 ? maxCoordY - anchor.y() : anchor.y() - minCoordY, kMaxCropExtent);
            qreal w = std::clamp(std::abs(cur.x() - anchor.x()), 1.0, std::max(1.0, maxW));
            qreal h = std::clamp(std::abs(cur.y() - anchor.y()), 1.0, std::max(1.0, maxH));
            if (activeRatio && *activeRatio > 0.0) {
                const qreal r = *activeRatio;
                const qreal limW = std::min(maxW, maxH * r);
                if (limW < 1.0)
                    return;
                w = std::clamp(std::max(w, h * r), std::min(1.0, limW), limW);
                h = w / r;
            }
            const qreal x0 = sx > 0 ? anchor.x() : anchor.x() - w;
            const qreal y0 = sy > 0 ? anchor.y() : anchor.y() - h;
            next = QRectF(x0, y0, w, h);
        }
    } else if (!activeRatio || *activeRatio <= 0.0) {
        const bool hasLeft = m_cropDragHandle == CropHandle::TopLeft ||
                             m_cropDragHandle == CropHandle::Left ||
                             m_cropDragHandle == CropHandle::BottomLeft;
        const bool hasRight = m_cropDragHandle == CropHandle::TopRight ||
                              m_cropDragHandle == CropHandle::Right ||
                              m_cropDragHandle == CropHandle::BottomRight;
        const bool hasTop = m_cropDragHandle == CropHandle::TopLeft ||
                            m_cropDragHandle == CropHandle::Top ||
                            m_cropDragHandle == CropHandle::TopRight;
        const bool hasBottom = m_cropDragHandle == CropHandle::BottomLeft ||
                               m_cropDragHandle == CropHandle::Bottom ||
                               m_cropDragHandle == CropHandle::BottomRight;
        if (altCenter) {
            const QPointF c = m_cropDragStartRect.center();
            qreal halfW = m_cropDragStartRect.width() * 0.5;
            qreal halfH = m_cropDragStartRect.height() * 0.5;
            if (hasLeft)
                halfW -= delta.x();
            else if (hasRight)
                halfW += delta.x();
            if (hasTop)
                halfH -= delta.y();
            else if (hasBottom)
                halfH += delta.y();
            const qreal maxHalfW =
                std::min({c.x() - minCoordX, maxCoordX - c.x(), kMaxCropExtent * 0.5});
            const qreal maxHalfH =
                std::min({c.y() - minCoordY, maxCoordY - c.y(), kMaxCropExtent * 0.5});
            halfW = std::clamp(halfW, 0.5, std::max(0.5, maxHalfW));
            halfH = std::clamp(halfH, 0.5, std::max(0.5, maxHalfH));
            next = QRectF(c.x() - halfW, c.y() - halfH, halfW * 2.0, halfH * 2.0);
        } else {
            qreal l = m_cropDragStartRect.left();
            qreal t = m_cropDragStartRect.top();
            qreal r = m_cropDragStartRect.right();
            qreal b = m_cropDragStartRect.bottom();
            if (hasLeft)
                l = std::clamp(l + delta.x(), std::max(minCoordX, r - kMaxCropExtent), r - 1.0);
            if (hasRight)
                r = std::clamp(r + delta.x(), l + 1.0, std::min(maxCoordX, l + kMaxCropExtent));
            if (hasTop)
                t = std::clamp(t + delta.y(), std::max(minCoordY, b - kMaxCropExtent), b - 1.0);
            if (hasBottom)
                b = std::clamp(b + delta.y(), t + 1.0, std::min(maxCoordY, t + kMaxCropExtent));
            next = QRectF(l, t, r - l, b - t);
        }
    } else {
        const qreal r = *activeRatio;
        const QPointF c = m_cropDragStartRect.center();
        if (m_cropDragHandle == CropHandle::TopLeft || m_cropDragHandle == CropHandle::TopRight ||
            m_cropDragHandle == CropHandle::BottomRight ||
            m_cropDragHandle == CropHandle::BottomLeft) {
            const bool fromLeft = m_cropDragHandle == CropHandle::TopLeft ||
                                  m_cropDragHandle == CropHandle::BottomLeft;
            const bool fromTop =
                m_cropDragHandle == CropHandle::TopLeft || m_cropDragHandle == CropHandle::TopRight;
            if (altCenter) {
                const qreal dx = fromLeft ? -delta.x() : delta.x();
                const qreal dy = fromTop ? -delta.y() : delta.y();
                const qreal maxHalfW =
                    std::min({c.x() - minCoordX, maxCoordX - c.x(), kMaxCropExtent * 0.5});
                const qreal maxHalfH =
                    std::min({c.y() - minCoordY, maxCoordY - c.y(), kMaxCropExtent * 0.5});
                const qreal limHalfW = std::min(maxHalfW, maxHalfH * r);
                const qreal minHalfW = std::min(limHalfW, std::max(0.5, 0.5 * r));
                const qreal candW = m_cropDragStartRect.width() * 0.5 + dx;
                const qreal candH = (m_cropDragStartRect.height() * 0.5 + dy) * r;
                const qreal halfW = std::clamp(std::max(candW, candH), minHalfW, limHalfW);
                const qreal halfH = halfW / r;
                next = QRectF(c.x() - halfW, c.y() - halfH, halfW * 2.0, halfH * 2.0);
            } else {
                const qreal ax =
                    fromLeft ? m_cropDragStartRect.right() : m_cropDragStartRect.left();
                const qreal ay = fromTop ? m_cropDragStartRect.bottom() : m_cropDragStartRect.top();
                const qreal maxW =
                    std::min(fromLeft ? ax - minCoordX : maxCoordX - ax, kMaxCropExtent);
                const qreal maxH =
                    std::min(fromTop ? ay - minCoordY : maxCoordY - ay, kMaxCropExtent);
                const qreal limW = std::min(maxW, maxH * r);
                const qreal minW = std::min(limW, std::max(1.0, r));
                const qreal candW =
                    m_cropDragStartRect.width() + (fromLeft ? -delta.x() : delta.x());
                const qreal candH =
                    (m_cropDragStartRect.height() + (fromTop ? -delta.y() : delta.y())) * r;
                const qreal w = std::clamp(std::max(candW, candH), minW, limW);
                const qreal h = w / r;
                next = QRectF(fromLeft ? ax - w : ax, fromTop ? ay - h : ay, w, h);
            }
        } else if (m_cropDragHandle == CropHandle::Left || m_cropDragHandle == CropHandle::Right) {
            const bool fromLeft = m_cropDragHandle == CropHandle::Left;
            const qreal maxHLimit = std::min(maxCoordY - minCoordY, kMaxCropExtent);
            if (altCenter) {
                const qreal maxW = std::min({2.0 * (c.x() - minCoordX), 2.0 * (maxCoordX - c.x()),
                                             kMaxCropExtent, maxHLimit * r});
                const qreal minW = std::min(maxW, std::max(1.0, r));
                const qreal w = std::clamp(m_cropDragStartRect.width() +
                                               2.0 * (fromLeft ? -delta.x() : delta.x()),
                                           minW, maxW);
                const qreal h = w / r;
                const qreal y = std::clamp(c.y() - h * 0.5, minCoordY, maxCoordY - h);
                next = QRectF(c.x() - w * 0.5, y, w, h);
            } else {
                const qreal ax =
                    fromLeft ? m_cropDragStartRect.right() : m_cropDragStartRect.left();
                const qreal maxW = std::min(
                    {fromLeft ? ax - minCoordX : maxCoordX - ax, kMaxCropExtent, maxHLimit * r});
                const qreal minW = std::min(maxW, std::max(1.0, r));
                const qreal w = std::clamp(
                    m_cropDragStartRect.width() + (fromLeft ? -delta.x() : delta.x()), minW, maxW);
                const qreal h = w / r;
                const qreal y = std::clamp(c.y() - h * 0.5, minCoordY, maxCoordY - h);
                next = QRectF(fromLeft ? ax - w : ax, y, w, h);
            }
        } else {
            const bool fromTop = m_cropDragHandle == CropHandle::Top;
            const qreal maxWLimit = std::min(maxCoordX - minCoordX, kMaxCropExtent);
            if (altCenter) {
                const qreal maxH = std::min({2.0 * (c.y() - minCoordY), 2.0 * (maxCoordY - c.y()),
                                             kMaxCropExtent, maxWLimit / r});
                const qreal minH = std::min(maxH, std::max(1.0, 1.0 / r));
                const qreal h = std::clamp(m_cropDragStartRect.height() +
                                               2.0 * (fromTop ? -delta.y() : delta.y()),
                                           minH, maxH);
                const qreal w = h * r;
                const qreal x = std::clamp(c.x() - w * 0.5, minCoordX, maxCoordX - w);
                next = QRectF(x, c.y() - h * 0.5, w, h);
            } else {
                const qreal ay = fromTop ? m_cropDragStartRect.bottom() : m_cropDragStartRect.top();
                const qreal maxH = std::min(
                    {fromTop ? ay - minCoordY : maxCoordY - ay, kMaxCropExtent, maxWLimit / r});
                const qreal minH = std::min(maxH, std::max(1.0, 1.0 / r));
                const qreal h = std::clamp(
                    m_cropDragStartRect.height() + (fromTop ? -delta.y() : delta.y()), minH, maxH);
                const qreal w = h * r;
                const qreal x = std::clamp(c.x() - w * 0.5, minCoordX, maxCoordX - w);
                next = QRectF(x, fromTop ? ay - h : ay, w, h);
            }
        }
    }
    if (next != m_cropRect) {
        m_cropRect = next;
        m_cropBaseSize = next.size();
        notifyCropRectChanged();
        update();
    }
}
void ImageEditingCanvas::mousePressEvent(QMouseEvent* event) {
    const auto previous = cropPreviewDamage();
    m_pointerView = event->position();
    if (event->button() == Qt::MiddleButton ||
        (m_mode == Mode::Pan && event->button() == Qt::LeftButton)) {
        m_panning = true;
        m_last = event->position();
        setCursor(Qt::ClosedHandCursor);
        update(previous);
        event->accept();
        return;
    }
    if (m_mode == Mode::CropRect && event->button() == Qt::LeftButton) {
        m_cropDragHandle = cropHandleAt(event->position());
        m_cropDragStartPixel = pixelAt(event->position());
        m_cropDragStartRect = m_cropRect;
        m_cropDragRatio = m_cropAspectRatio;
        m_cropDragStartAngle = m_cropAngleDeg;
        const QPointF c = m_cropRect.center();
        m_cropDragStartPointerAngle =
            std::atan2(m_cropDragStartPixel.y() - c.y(), m_cropDragStartPixel.x() - c.x()) *
            (180.0 / kCropPi);
        if (!m_cropDragRatio && m_cropDragStartRect.height() > 0.0)
            m_cropDragRatio = m_cropDragStartRect.width() / m_cropDragStartRect.height();
        setCursor(cropCursorForHandle(m_cropDragHandle, true));
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton &&
        (m_mode == Mode::CropRows || m_mode == Mode::CropColumns)) {
        if (!m_document.bounds().contains(
                canvasToViewTransform().inverted().map(event->position()))) {
            update(previous);
            event->accept();
            return;
        }
        m_cropping = true;
        m_firstPixel = m_lastPixel = pixelAt(event->position());
        event->accept();
        update(QRegion(previous) | cropPreviewDamage());
        return;
    }
    if (m_mode == Mode::Draw && event->button() == Qt::LeftButton)
        m_document.setCanvasGestureActive(true);
    SnowCanvasWidget::mousePressEvent(event);
}
void ImageEditingCanvas::mouseMoveEvent(QMouseEvent* event) {
    const auto previous = cropPreviewDamage();
    m_pointerView = event->position();
    if (m_panning) {
        m_center -= (event->position() - m_last) / m_zoom;
        m_last = event->position();
        syncCamera();
        event->accept();
        return;
    }
    if (m_mode == Mode::CropRect) {
        if (m_cropDragHandle != CropHandle::None) {
            updateCropDrag(pixelAt(event->position()), event->modifiers());
        } else {
            m_cropHoverHandle = cropHandleAt(event->position());
            setCursor(cropCursorForHandle(m_cropHoverHandle, false));
        }
        event->accept();
        return;
    }
    if (m_cropping) {
        m_lastPixel = pixelAt(event->position());
        update(QRegion(previous) | cropPreviewDamage());
        event->accept();
        return;
    }
    if (m_mode == Mode::CropRows || m_mode == Mode::CropColumns) {
        update(QRegion(previous) | cropPreviewDamage());
        event->accept();
        return;
    }
    SnowCanvasWidget::mouseMoveEvent(event);
}
void ImageEditingCanvas::mouseReleaseEvent(QMouseEvent* event) {
    const auto previous = cropPreviewDamage();
    m_pointerView = event->position();
    if (m_panning) {
        m_panning = false;
        setCursor(m_mode == Mode::CropRect
                      ? cropCursorForHandle(cropHandleAt(event->position()), false)
                      : (m_mode == Mode::Pan ? Qt::OpenHandCursor : Qt::CrossCursor));
        update(QRegion(previous) | cropPreviewDamage());
        event->accept();
        return;
    }
    if (m_mode == Mode::CropRect && m_cropDragHandle != CropHandle::None &&
        event->button() == Qt::LeftButton) {
        m_cropDragHandle = CropHandle::None;
        m_cropDragRatio.reset();
        m_cropHoverHandle = cropHandleAt(event->position());
        setCursor(cropCursorForHandle(m_cropHoverHandle, false));
        update();
        event->accept();
        return;
    }
    if (m_cropping && event->button() == Qt::LeftButton) {
        m_cropping = false;
        m_lastPixel = pixelAt(event->position());
        const bool rows = m_mode == Mode::CropRows;
        if (cropRequested)
            cropRequested(rows ? ImageEditingDocument::CropAxis::Rows
                               : ImageEditingDocument::CropAxis::Columns,
                          qRound(rows ? m_firstPixel.y() : m_firstPixel.x()),
                          qRound(rows ? m_lastPixel.y() : m_lastPixel.x()));
        update(QRegion(previous) | cropPreviewDamage());
        event->accept();
        return;
    }
    SnowCanvasWidget::mouseReleaseEvent(event);
    if (event->button() == Qt::LeftButton && !hasActiveTextEditing())
        m_document.setCanvasGestureActive(false);
}
void ImageEditingCanvas::mouseDoubleClickEvent(QMouseEvent* event) {
    if (m_mode == Mode::CropRect && event->button() == Qt::LeftButton) {
        const auto c = cropViewCorners();
        QPolygonF poly;
        poly << c[0] << c[1] << c[2] << c[3];
        if (poly.containsPoint(event->position(), Qt::OddEvenFill)) {
            commitCrop();
            event->accept();
            return;
        }
    }
    SnowCanvasWidget::mouseDoubleClickEvent(event);
}
void ImageEditingCanvas::enterEvent(QEnterEvent* event) {
    const auto previous = cropPreviewDamage();
    m_pointerView = event->position();
    SnowCanvasWidget::enterEvent(event);
    update(QRegion(previous) | cropPreviewDamage());
}
void ImageEditingCanvas::leaveEvent(QEvent* event) {
    const auto previous = cropPreviewDamage();
    m_pointerView.reset();
    SnowCanvasWidget::leaveEvent(event);
    update(QRegion(previous) | cropPreviewDamage());
}
void ImageEditingCanvas::wheelEvent(QWheelEvent* event) {
    if (hasActiveTextEditing()) {
        SnowCanvasWidget::wheelEvent(event);
        return;
    }
    if (!m_recognitionActive && event->modifiers() != Qt::NoModifier && styleWheel &&
        styleWheel(event))
        return;
    const auto anchor = canvasToViewTransform().inverted().map(event->position());
    const double steps = event->pixelDelta().isNull() ? event->angleDelta().y() / 120.0
                                                      : event->pixelDelta().y() / 50.0;
    m_zoom = std::clamp(m_zoom * std::pow(1.15, steps), 1e-9, 1e6);
    m_center = anchor - (event->position() - QPointF(width() / 2.0, height() / 2.0)) / m_zoom;
    syncCamera(true);
    event->accept();
}
void ImageEditingCanvas::keyPressEvent(QKeyEvent* event) {
    if (m_mode == Mode::CropRect) {
        if (event->key() == Qt::Key_Escape) {
            cancelCropRect();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            commitCrop();
            event->accept();
            return;
        }
    }
    if (event->key() == Qt::Key_Escape && m_cropping) {
        const auto previous = cropPreviewDamage();
        m_cropping = false;
        update(QRegion(previous) | cropPreviewDamage());
        event->accept();
        return;
    }
    if (!hasActiveTextEditing() &&
        (event->matches(QKeySequence::Undo) || event->matches(QKeySequence::Redo))) {
        const bool redo = event->matches(QKeySequence::Redo);
        if (historyRequested)
            historyRequested(redo);
        else if (redo)
            m_document.redo();
        else
            m_document.undo();
        event->accept();
        return;
    }
    SnowCanvasWidget::keyPressEvent(event);
}
void ImageEditingCanvas::resizeEvent(QResizeEvent* event) {
    SnowCanvasWidget::resizeEvent(event);
    m_zoomReadout->layoutIn(rect());
    if (!m_fitted)
        fit();
    else
        syncCamera();
}
std::uint64_t ImageEditingCanvas::contentRevision() const {
    return static_cast<std::uint64_t>(m_document.displayImage().cacheKey());
}
QList<SnowCanvasBaseImageSource> ImageEditingCanvas::baseImageSources() const {
    auto sources = m_document.baseImageSources();
    if (m_document.hasPreviewImage() && !sources.isEmpty())
        sources.last().image = m_document.displayImage();
    return sources;
}
std::optional<SnowCanvasFilterRenderReference> ImageEditingCanvas::filterRenderReference() const {
    if (m_document.bounds().isEmpty())
        return {};
    return SnowCanvasFilterRenderReference{
        m_document.bounds(), m_document.displayImage().width() / m_document.bounds().width(),
        SnowCanvasFilterRenderReference::Presentation::Composite};
}
void ImageEditingCanvas::renderBeforeCanvas(QPainter& painter,
                                            const SnowCanvasRenderContext& context) {
    const bool recognition =
        m_recognitionActive && context.purpose == SnowCanvasRenderContext::Purpose::Widget;
    if (recognition && !m_showImage)
        return;
    painter.save();
    renderOriginalBackground(painter, context);
    painter.restore();
    if (recognition && m_showFiltered) {
        painter.save();
        painter.setClipRect(context.canvasToViewTransform.mapRect(m_document.bounds()),
                            Qt::IntersectClip);
        paintScreenshotOcrFilteredImage(
            painter, m_recognitionBackground,
            context.canvasToViewTransform.mapRect(m_recognitionBackgroundRect),
            context.exposedRegion, true);
        painter.restore();
    }
}
void ImageEditingCanvas::renderOriginalBackground(QPainter& painter,
                                                  const SnowCanvasRenderContext& context) {
    // Filter replay and eraser restoration translate their painter into a bounded tile.
    // Compose the image projection with that view-to-tile transform to sample the same pixels.
    painter.setTransform(context.canvasToViewTransform, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const QImage& display = m_document.displayImage();
    const QRectF bounds = m_document.bounds();
    if (m_mode == Mode::CropRect && context.purpose == SnowCanvasRenderContext::Purpose::Widget) {
        if (!m_cropRect.isEmpty() && m_cropExtensionFillColor.alpha() > 0) {
            const QRectF fillArea = cropCanvasRect(m_cropRect);
            if (fillArea.isValid() && !fillArea.isEmpty())
                painter.fillRect(fillArea, m_cropExtensionFillColor);
        }
        if (m_cropAngleDeg != 0 && !bounds.isEmpty()) {
            const QPointF center = bounds.center();
            painter.translate(center);
            painter.rotate(static_cast<qreal>(m_cropAngleDeg));
            painter.translate(-center);
        }
        painter.drawImage(bounds, display);
        return;
    }
    painter.drawImage(bounds, display);
}
QRectF ImageEditingCanvas::cropStrip(int* pixelCount) const {
    if ((m_mode != Mode::CropRows && m_mode != Mode::CropColumns) || m_panning)
        return {};
    const auto bounds = m_document.bounds();
    const bool rows = m_mode == Mode::CropRows;
    const int extent = rows ? m_document.image().height() : m_document.image().width();
    if (extent == 0 || bounds.isEmpty())
        return {};
    QPointF firstPixel = m_firstPixel;
    QPointF lastPixel = m_lastPixel;
    if (!m_cropping) {
        if (!m_pointerView || !QRectF(rect()).contains(*m_pointerView) ||
            !bounds.contains(canvasToViewTransform().inverted().map(*m_pointerView)))
            return {};
        firstPixel = lastPixel = pixelAt(*m_pointerView);
    }
    const int first = std::clamp(qRound(rows ? firstPixel.y() : firstPixel.x()), 0, extent);
    const int last = std::clamp(qRound(rows ? lastPixel.y() : lastPixel.x()), 0, extent);
    const double scale = (rows ? bounds.height() : bounds.width()) / extent;
    if (pixelCount)
        *pixelCount = std::abs(first - last);
    return rows ? QRectF(bounds.x(), bounds.y() + std::min(first, last) * scale, bounds.width(),
                         std::abs(first - last) * scale)
                : QRectF(bounds.x() + std::min(first, last) * scale, bounds.y(),
                         std::abs(first - last) * scale, bounds.height());
}
QRect ImageEditingCanvas::cropPreviewDamage() const {
    const auto strip = cropStrip();
    // A zero-width/height strip still contains the hover or initial drag guide.
    if (strip.isNull())
        return {};
    return canvasToViewTransform()
        .mapRect(strip)
        .toAlignedRect()
        .adjusted(-2, -2, 2, 2)
        .intersected(rect());
}
void ImageEditingCanvas::renderAfterCanvas(QPainter& painter,
                                           const SnowCanvasRenderContext& context) {
    if (m_mode == Mode::CropRect && context.purpose == SnowCanvasRenderContext::Purpose::Widget &&
        !m_cropRect.isEmpty()) {
        const QSize imgSize = m_document.image().size();
        const QRectF bounds = m_document.bounds();
        if (!imgSize.isEmpty() && !bounds.isEmpty()) {
            const auto corners = cropViewCorners();
            const QPointF& tl = corners[0];
            const QPointF& tr = corners[1];
            const QPointF& br = corners[2];
            const QPointF& bl = corners[3];
            QPolygonF poly;
            poly << tl << tr << br << bl;
            const qreal viewW = std::hypot(tr.x() - tl.x(), tr.y() - tl.y());
            const qreal viewH = std::hypot(bl.x() - tl.x(), bl.y() - tl.y());
            if (viewW >= 1.0 && viewH >= 1.0) {
                const QPointF uV = (tr - tl) / viewW;
                const QPointF vV = (bl - tl) / viewH;
                painter.save();
                // Photoshop-style outer dark scrim outside active crop box.
                QPainterPath scrim;
                scrim.setFillRule(Qt::OddEvenFill);
                scrim.addRect(QRectF(rect()));
                scrim.addPolygon(poly);
                painter.fillPath(scrim, QColor(0, 0, 0, 145));

                // Crisp 1px axis-aligned frame + Rule of Thirds 3x3 grid inside crop box.
                painter.setRenderHint(QPainter::Antialiasing, false);
                if (viewW >= 24.0 && viewH >= 24.0) {
                    const QLineF gridLines[] = {
                        {tl + (tr - tl) / 3.0, bl + (br - bl) / 3.0},
                        {tl + (tr - tl) * (2.0 / 3.0), bl + (br - bl) * (2.0 / 3.0)},
                        {tl + (bl - tl) / 3.0, tr + (br - tr) / 3.0},
                        {tl + (bl - tl) * (2.0 / 3.0), tr + (br - tr) * (2.0 / 3.0)},
                    };
                    painter.setPen(QPen(QColor(0, 0, 0, 75), 1.0));
                    painter.drawLines(gridLines, 4);
                    painter.setPen(QPen(QColor(255, 255, 255, 135), 1.0));
                    painter.drawLines(gridLines, 4);
                }
                painter.setPen(QPen(QColor(255, 255, 255, 230), 1.0));
                painter.setBrush(Qt::NoBrush);
                painter.drawPolygon(poly);

                // Photoshop-style L-shaped corner handles & edge midpoint bar handles.
                const qreal arm = std::max(4.0, std::min({16.0, viewW * 0.28, viewH * 0.28}));
                const qreal midH = std::max(4.0, std::min(18.0, viewW * 0.25));
                const qreal midV = std::max(4.0, std::min(18.0, viewH * 0.25));
                const QPointF topMid = (tl + tr) * 0.5;
                const QPointF bottomMid = (bl + br) * 0.5;
                const QPointF leftMid = (tl + bl) * 0.5;
                const QPointF rightMid = (tr + br) * 0.5;
                const auto drawHandles = [&](const QPen& pen) {
                    painter.setPen(pen);
                    const QPointF hTL[3] = {tl + vV * arm, tl, tl + uV * arm};
                    const QPointF hTR[3] = {tr - uV * arm, tr, tr + vV * arm};
                    const QPointF hBR[3] = {br - vV * arm, br, br - uV * arm};
                    const QPointF hBL[3] = {bl + uV * arm, bl, bl - vV * arm};
                    painter.drawPolyline(hTL, 3);
                    painter.drawPolyline(hTR, 3);
                    painter.drawPolyline(hBR, 3);
                    painter.drawPolyline(hBL, 3);
                    if (viewW >= 28.0) {
                        painter.drawLine(topMid - uV * (midH * 0.5), topMid + uV * (midH * 0.5));
                        painter.drawLine(bottomMid - uV * (midH * 0.5),
                                         bottomMid + uV * (midH * 0.5));
                    }
                    if (viewH >= 28.0) {
                        painter.drawLine(leftMid - vV * (midV * 0.5), leftMid + vV * (midV * 0.5));
                        painter.drawLine(rightMid - vV * (midV * 0.5),
                                         rightMid + vV * (midV * 0.5));
                    }
                };
                painter.setRenderHint(QPainter::Antialiasing, true);
                drawHandles(
                    QPen(QColor(0, 0, 0, 170), 5.0, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
                drawHandles(QPen(QColor(255, 255, 255, 248), 3.0, Qt::SolidLine, Qt::SquareCap,
                                 Qt::MiterJoin));

                // Dimensions + exact 1-degree angle pill badge.
                const QRect px = cropPixelRect();
                const QString badgeText =
                    m_cropAngleDeg == 0
                        ? QStringLiteral("%1 × %2 px").arg(px.width()).arg(px.height())
                        : QStringLiteral("%1 × %2 px · %3%4°")
                              .arg(px.width())
                              .arg(px.height())
                              .arg(m_cropAngleDeg > 0 ? QStringLiteral("+") : QString())
                              .arg(m_cropAngleDeg);
                QFont badgeFont = painter.font();
                badgeFont.setPixelSize(11);
                badgeFont.setWeight(QFont::Medium);
                painter.setFont(badgeFont);
                const QSizeF textSz = QFontMetricsF(badgeFont).size(Qt::TextSingleLine, badgeText);
                const QSizeF pillSz(textSz.width() + 14.0, textSz.height() + 6.0);
                const QRectF bbox = poly.boundingRect();
                const bool fitsInside = bbox.width() >= pillSz.width() + 16.0 &&
                                        bbox.height() >= pillSz.height() + 16.0;
                qreal bx = fitsInside ? (bbox.right() - pillSz.width() - 8.0)
                                      : (bbox.center().x() - pillSz.width() * 0.5);
                qreal by = fitsInside ? (bbox.top() + 8.0) : (bbox.top() - pillSz.height() - 6.0);
                bx = std::clamp(bx, 8.0, std::max(8.0, width() - pillSz.width() - 8.0));
                by = std::clamp(by, 8.0, std::max(8.0, height() - pillSz.height() - 8.0));
                const QRectF pillRect(bx, by, pillSz.width(), pillSz.height());
                painter.setPen(QPen(QColor(255, 255, 255, 50), 1.0));
                painter.setBrush(QColor(20, 20, 20, 205));
                painter.drawRoundedRect(pillRect, 4.0, 4.0);
                painter.setPen(Qt::white);
                painter.drawText(pillRect, Qt::AlignCenter, badgeText);
                painter.restore();
            }
        }
        return;
    }
    int pixels = 0;
    const auto strip = cropStrip(&pixels);
    if (strip.isNull())
        return;
    const QRectF view = context.canvasToViewTransform.mapRect(strip);
    const QRectF imageView = context.canvasToViewTransform.mapRect(m_document.bounds());
    painter.save();
    painter.setClipRect(imageView, Qt::IntersectClip);
    if (pixels > 0) {
        painter.fillRect(view, QColor(240, 65, 65, 95));
        painter.setPen(Qt::white);
        painter.drawText(view, Qt::AlignCenter,
                         QCoreApplication::translate("ImageEditingWindow", "%1 px").arg(pixels));
    }
    const bool rows = m_mode == Mode::CropRows;
    const qreal minimum = rows ? imageView.top() : imageView.left();
    const qreal maximum = rows ? imageView.bottom() : imageView.right();
    // Keep the guide stroke inside the image clip even at the final pixel boundary.
    const qreal inset =
        std::min((maximum - minimum) / 2, 0.5 / painter.device()->devicePixelRatioF());
    const auto guideAt = [&](qreal position) {
        position = std::clamp(position, minimum + inset, maximum - inset);
        return rows ? QLineF(imageView.left(), position, imageView.right(), position)
                    : QLineF(position, imageView.top(), position, imageView.bottom());
    };
    const QLineF guides[] = {guideAt(rows ? view.top() : view.left()),
                             guideAt(rows ? view.bottom() : view.right())};
    // Match image splitting's selection-blue, one-logical-pixel dashed dividers.
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(QColor(0x40, 0x96, 0xff), 1.0);
    snowCanvasApplySelectionDashStyle(pen);
    painter.setPen(pen);
    painter.drawLines(guides, pixels > 0 ? 2 : 1);
    painter.restore();
}
} // namespace snow_shot::presentation
