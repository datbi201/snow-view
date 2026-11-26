// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_shot/presentation/screenshotrecognitionresults.h"
#include "snow_shot/presentation/screenshotimagesource.h"
#include "snow_shot/presentation/screenshotexportartifact.h"
#include "snow_shot/presentation/screenshotpinnedtextpreparation.h"
#include "snow_shot/presentation/screenshotpinnedrenderpreparation.h"
#include "snow_shot/storage/capturehistoryrepository.h"
#include "snow_shot/storage/pinnedwindowtypes.h"
#include <QObject>
#include <QHash>

namespace snow_shot::presentation {

struct ImageEditingSource {
    QImage image;
    ScreenshotImageSource layeredImage;
    QSize pixelSize;
    QRectF canvasRect;
    QByteArray canvasSession;
    QByteArray canvasHistory;
    SnowCanvasSmartEraseSnapshot smartErase;
    QList<SnowCanvasBaseImageSource> samplingSources;
    ScreenshotRecognitionResults recognition;
    QString originalFileName;
    QString error;
    bool flattened = false;
    // A pending pin appearance is composed before importing into a rectangular document.
    std::optional<ScreenshotResultStyle> resultStyle;
    // Baked into the base raster before applying a pin appearance or publishing the document.
    ScreenshotTextAnnotationRenderSnapshot textAnnotations;
};

ImageEditingSource readImageEditingHistory(storage::CaptureHistoryRepository& repository,
                                           const storage::CaptureHistoryRecord& record);
ImageEditingSource readImageEditingPinned(storage::PinnedWindowRecord record,
                                          const ScreenshotPinnedTextPreparation& preparation);
ImageEditingSource readImageEditingPinned(storage::PinnedWindowRecord record,
                                          const ScreenshotPinnedRenderPreparation& preparation);
// Restrict the base raster, baking pending text marks before a pin appearance.
// Retain the drawing document and its canvas origin.
void restrictImageEditingSource(ImageEditingSource& source, const QRectF& visible);

[[nodiscard]] QImage renderImageEditingBaseImage(const ScreenshotImageSource& source,
                                                 const QRectF& canvasRect, const QSize& pixelSize);
[[nodiscard]] ScreenshotExportSource imageEditingCompositeSource(const ImageEditingSource& source);
[[nodiscard]] ScreenshotExportSource imageEditingExportSource(const ImageEditingSource& source);
[[nodiscard]] ImageEditingSource completeImageEditingComposite(ImageEditingSource source,
                                                               QImage image);

class ImageEditingDocument final : public QObject {
    Q_OBJECT
  public:
    enum class CropAxis { Rows, Columns };
    struct Crop {
        CropAxis axis = CropAxis::Rows;
        int start = 0;
        int length = 0;
        QSize beforeSize;
        QImage removed;
        QImage result;
        qint64 sourceKey = 0;
    };
    explicit ImageEditingDocument(QObject* parent = nullptr);
    bool load(const ImageEditingSource& source);
    SnowCanvasRuntime& runtime() {
        return m_runtime;
    }
    const QImage& image() const {
        return m_image;
    }
    // SnowView: live non-destructive preview image (used while dragging Adjust sliders).
    const QImage& displayImage() const {
        return m_previewImage.isNull() ? m_image : m_previewImage;
    }
    bool hasPreviewImage() const {
        return !m_previewImage.isNull();
    }
    void setPreviewImage(QImage image);
    void clearPreviewImage();
    static constexpr int kMaxVectorUndoCommandsAfterFlatten = 10;
    QRectF bounds() const;
    QPointF imagePixelAt(QPointF canvasPoint) const;
    quint64 baseState() const {
        return m_runtime.hostHistoryState();
    }
    bool retainsBaseHistoryState(quint64 state) const {
        return state == 0 || state == baseState() || m_crops.contains(state) ||
               m_baseEdits.contains(state);
    }
    QList<SnowCanvasBaseImageSource> baseImageSources() const;
    // SnowView: synchronously render m_image + live runtime annotations at 1:1 resolution.
    [[nodiscard]] QImage renderFlattenedBaseImage() const;
    static Crop prepareCrop(const QImage& image, CropAxis axis, int first, int last,
                            const std::function<bool()>& cancelled = {});
    bool applyCrop(Crop crop);
    bool crop(CropAxis axis, int first, int last);
    // SnowView: replace the whole base raster (flip, rotate, color adjust) as one
    // undoable history step, chained with crops in the same host history sequence.
    bool applyBaseImage(QImage image);
    // SnowView: replace base raster with transformed/flattened pixels, clear live vectors,
    // and retain the pre-flatten vector layer undoable for up to 10 subsequent commands.
    bool applyFlattenedBaseImage(QImage image);
    void setCanvasGestureActive(bool active);
    [[nodiscard]] bool canUndo() const;
    [[nodiscard]] bool canRedo() const;
    struct HistoryTransition {
        bool redo = false;
        quint64 state = 0;
        quint64 nextState = 0;
        quint64 revision = 0;
        qint64 sourceKey = 0;
        QImage image;
        Crop crop;
        // SnowView: true when this transition restores a whole-raster base edit;
        // `image` then already holds the final raster for the target state.
        bool baseEdit = false;
        // SnowView: true when stepping backward/forward across a flatten boundary.
        bool flattenBoundary = false;
    };
    HistoryTransition historyTransition(bool redo) const;
    static void prepareHistoryTransition(HistoryTransition& transition,
                                         const std::function<bool()>& cancelled = {});
    bool applyHistoryTransition(HistoryTransition transition);
    bool undo();
    bool redo();
    QByteArray checkpoint() const;
    bool dirty() const;
    void markExported(QByteArray checkpoint);
    ImageEditingSource snapshot() const;
    ScreenshotExportSource exportSource(QString* error = nullptr) const;

  signals:
    void changed();
    void baseImageChanged();
    void previewImageChanged();

  private:
    struct CropEntry {
        quint64 before = 0;
        Crop crop;
    };
    // SnowView: a whole-raster base edit kept as before/after images so undo and
    // redo restore exact pixels without an inverse operation.
    struct BaseEditEntry {
        quint64 before = 0;
        QImage beforeImage;
        QImage afterImage;
    };
    struct FlattenUndoEntry {
        QByteArray preVectorHistory;
        QImage preImage;
        QList<SnowCanvasBaseImageSource> preSamplingSources;
        SnowCanvasSmartEraseSnapshot preSmartErase;
        quint64 preEpochId = 0;
        int stepsFromFlatten = 0;
    };
    struct FlattenRedoEntry {
        FlattenUndoEntry flattenEntry;
        QByteArray postVectorHistory;
        QImage postImage;
        QList<SnowCanvasBaseImageSource> postSamplingSources;
        SnowCanvasSmartEraseSnapshot postSmartErase;
        quint64 postEpochId = 0;
        quint64 postSerial = 0;
        QHash<quint64, CropEntry> postCrops;
        QHash<quint64, BaseEditEntry> postBaseEdits;
    };
    bool travel(bool redo);
    void updateSources();
    void pruneCrops();
    void recordPostFlattenCommand();
    void adjustFlattenStepDistances(int delta);
    SnowCanvasRuntime m_runtime;
    QImage m_image;
    QImage m_previewImage;
    QList<SnowCanvasBaseImageSource> m_samplingSources;
    QString m_originalFileName;
    QPointF m_origin;
    QSizeF m_unitsPerPixel;
    QHash<quint64, CropEntry> m_crops;
    QHash<quint64, BaseEditEntry> m_baseEdits;
    QList<FlattenUndoEntry> m_flattenUndoStack;
    QList<FlattenRedoEntry> m_flattenRedoStack;
    quint64 m_serial = 0;
    quint64 m_flattenEpochId = 0;
    quint64 m_nextFlattenEpochId = 0;
    bool m_inHostMutation = false;
    bool m_traveling = false;
    bool m_canvasGestureActive = false;
    bool m_pendingCanvasGestureCommand = false;
    QByteArray m_saved;
    mutable QByteArray m_checkpointContent;
    mutable QByteArray m_checkpoint;
    mutable quint64 m_checkpointBaseState = 0;
    mutable quint64 m_checkpointFlattenEpochId = 0;
    struct ExportCache {
        QByteArray checkpoint;
        QByteArray smartEraseKey;
        ScreenshotExportSource source;
    };
    mutable ExportCache m_exportCache;
};
} // namespace snow_shot::presentation
