// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "snow_shot/presentation/imageeditingdocument.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_draw_engine_qt/snow_canvas_custom_renderer.h"
#include <optional>

class CanvasStatusReadout;

namespace snow_shot::presentation {
class ImageEditingCanvas final : public SnowCanvasWidget, private SnowCanvasCustomRenderer {
  public:
    enum class Mode { Pan, Draw, CropRows, CropColumns, CropRect };
    explicit ImageEditingCanvas(ImageEditingDocument& document, QWidget* parent = nullptr);
    ~ImageEditingCanvas() override;
    void setMode(Mode mode);
    Mode mode() const {
        return m_mode;
    }
    void setCropAspectRatio(std::optional<double> ratio);
    [[nodiscard]] std::optional<double> cropAspectRatio() const {
        return m_cropAspectRatio;
    }
    void setCropAngleDegrees(int degrees);
    [[nodiscard]] int cropAngleDegrees() const {
        return m_cropAngleDeg;
    }
    void resetCropRect();
    [[nodiscard]] QRect cropPixelRect() const;
    [[nodiscard]] QRectF cropPixelBox() const {
        return m_cropRect;
    }
    bool commitCrop();
    void cancelCropRect();
    void fit(bool showZoomReadout = false);
    void actualSize();
    void setRecognitionDisplay(bool active, bool showImage, bool showFiltered);
    void setRecognitionBackground(QImage image = {}, const QRectF& canvasRect = {});
    [[nodiscard]] const QImage& recognitionBackground() const {
        return m_recognitionBackground;
    }
    [[nodiscard]] const QRectF& recognitionBackgroundRect() const {
        return m_recognitionBackgroundRect;
    }
    double zoom() const {
        return m_zoom;
    }
    QPointF center() const {
        return m_center;
    }
    std::function<void(ImageEditingDocument::CropAxis, int, int)> cropRequested;
    std::function<void(const QRect&, int)> cropRectChanged;
    std::function<void(const QRectF&, int)> cropRectCommitted;
    std::function<void()> cropRectCancelled;
    std::function<bool(QWheelEvent*)> styleWheel;
    std::function<void(bool)> historyRequested;
    std::function<void()> viewportChanged;

  protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    enum class CropHandle {
        None,
        TopLeft,
        Top,
        TopRight,
        Right,
        BottomRight,
        Bottom,
        BottomLeft,
        Left,
        Rotate,
        Move,
        Create,
    };
    void syncImage();
    void syncCamera(bool showZoomReadout = false);
    double actualSizeZoom() const;
    QPointF pixelAt(QPointF view) const;
    QRectF cropCanvasRect(const QRectF& pixelRect) const;
    QRectF cropViewRect() const;
    std::pair<QPointF, QPointF> cropPixelAxes() const;
    std::array<QPointF, 4> cropViewCorners() const;
    CropHandle cropHandleAt(const QPointF& viewPos) const;
    QCursor cropCursorForHandle(CropHandle handle, bool dragging) const;
    void applyCropAngleConstraints();
    void initializeCropRect(std::optional<double> ratio);
    void updateCropDrag(const QPointF& pixelPos, Qt::KeyboardModifiers modifiers);
    void notifyCropRectChanged();
    QRectF cropStrip(int* pixelCount = nullptr) const;
    QRect cropPreviewDamage() const;
    std::uint64_t contentRevision() const override;
    QList<SnowCanvasBaseImageSource> baseImageSources() const override;
    std::optional<SnowCanvasFilterRenderReference> filterRenderReference() const override;
    void renderBeforeCanvas(QPainter&, const SnowCanvasRenderContext&) override;
    void renderOriginalBackground(QPainter&, const SnowCanvasRenderContext&) override;
    void renderAfterCanvas(QPainter&, const SnowCanvasRenderContext&) override;
    ImageEditingDocument& m_document;
    CanvasStatusReadout* m_zoomReadout;
    Mode m_mode = Mode::Pan;
    QPointF m_center;
    double m_zoom = 1;
    bool m_fitted = false;
    bool m_panning = false;
    bool m_cropping = false;
    std::optional<QPointF> m_pointerView;
    QPointF m_last;
    QPointF m_firstPixel;
    QPointF m_lastPixel;
    QRectF m_cropRect;
    QSizeF m_cropBaseSize;
    int m_cropAngleDeg = 0;
    std::optional<double> m_cropAspectRatio;
    CropHandle m_cropDragHandle = CropHandle::None;
    CropHandle m_cropHoverHandle = CropHandle::None;
    QPointF m_cropDragStartPixel;
    QRectF m_cropDragStartRect;
    std::optional<double> m_cropDragRatio;
    double m_cropDragStartPointerAngle = 0.0;
    int m_cropDragStartAngle = 0;
    QImage m_recognitionBackground;
    QRectF m_recognitionBackgroundRect;
    bool m_recognitionActive = false;
    bool m_showImage = true;
    bool m_showFiltered = false;
};
} // namespace snow_shot::presentation
