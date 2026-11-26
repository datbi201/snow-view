// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/presentation/imageeditingdocument.h"
#include "snow_shot/presentation/screenshotdefaultstyles.h"
#include "snow_draw_engine_qt/snow_canvas_image.h"
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QScopedValueRollback>
#include <QSet>
#include <cstring>
#include <QPainter>
#include <algorithm>

namespace snow_shot::presentation {
namespace {
QList<SnowCanvasBaseImageSource>
exteriorSamplingSources(const QList<SnowCanvasBaseImageSource>& sources, const QRectF& bounds) {
    QList<SnowCanvasBaseImageSource> result;
    for (const auto& source : sources) {
        if (source.image.isNull() || source.canvasRect.isEmpty())
            continue;
        const auto coverage = source.coverage.isEmpty()
                                  ? source.canvasRect
                                  : source.canvasRect.intersected(source.coverage);
        const auto overlap = coverage.intersected(bounds);
        if (overlap.isEmpty()) {
            if (!coverage.isEmpty())
                result.append({source.image, source.canvasRect, coverage, source.smartEraseSource});
            continue;
        }
        // Keep only immutable context outside the editable image. In particular, transparent
        // pixels and strips removed by later crops must never reveal the original image below.
        const QRectF pieces[]{
            {coverage.left(), coverage.top(), coverage.width(), overlap.top() - coverage.top()},
            {coverage.left(), overlap.bottom(), coverage.width(),
             coverage.bottom() - overlap.bottom()},
            {coverage.left(), overlap.top(), overlap.left() - coverage.left(), overlap.height()},
            {overlap.right(), overlap.top(), coverage.right() - overlap.right(), overlap.height()}};
        for (const auto& piece : pieces)
            if (!piece.isEmpty())
                result.append({source.image, source.canvasRect, piece, source.smartEraseSource});
    }
    return result;
}

QImage removeStrip(const QImage& image, const ImageEditingDocument::Crop& crop,
                   const std::function<bool()>& cancelled) {
    if (image.isNull() || image.depth() < 8 || (cancelled && cancelled()))
        return {};
    const bool rows = crop.axis == ImageEditingDocument::CropAxis::Rows;
    const int extent = rows ? image.height() : image.width();
    if (crop.start < 0 || crop.length <= 0 || crop.length >= extent ||
        crop.start > extent - crop.length)
        return {};
    const QSize size(image.width() - (rows ? 0 : crop.length),
                     image.height() - (rows ? crop.length : 0));
    QImage result = snowCanvasAllocateImage(size, image.format());
    if (result.isNull())
        return {};
    result.setColorSpace(image.colorSpace());
    result.setColorTable(image.colorTable());
    const qsizetype pixelBytes = image.depth() / 8;
    for (int y = 0; y < result.height(); ++y) {
        if ((y & 63) == 0 && cancelled && cancelled())
            return {};
        uchar* destination = result.scanLine(y);
        if (rows) {
            std::memcpy(destination, image.constScanLine(y < crop.start ? y : y + crop.length),
                        static_cast<std::size_t>(result.width() * pixelBytes));
        } else {
            const uchar* source = image.constScanLine(y);
            std::memcpy(destination, source, static_cast<std::size_t>(crop.start * pixelBytes));
            std::memcpy(destination + crop.start * pixelBytes,
                        source + (crop.start + crop.length) * pixelBytes,
                        static_cast<std::size_t>((result.width() - crop.start) * pixelBytes));
        }
    }
    return result;
}

QImage joinStrip(const QImage& current, const ImageEditingDocument::Crop& crop,
                 const std::function<bool()>& cancelled) {
    if (cancelled && cancelled())
        return {};
    QImage restored = snowCanvasAllocateImage(crop.beforeSize, current.format());
    if (restored.isNull())
        return {};
    restored.setColorSpace(current.colorSpace());
    restored.setColorTable(current.colorTable());
    const qsizetype pixelBytes = current.depth() / 8;
    for (int y = 0; y < restored.height(); ++y) {
        if ((y & 63) == 0 && cancelled && cancelled())
            return {};
        uchar* destination = restored.scanLine(y);
        if (crop.axis == ImageEditingDocument::CropAxis::Rows) {
            const uchar* source = y < crop.start ? current.constScanLine(y)
                                  : y < crop.start + crop.length
                                      ? crop.removed.constScanLine(y - crop.start)
                                      : current.constScanLine(y - crop.length);
            std::memcpy(destination, source,
                        static_cast<std::size_t>(restored.width() * pixelBytes));
        } else {
            const uchar* source = current.constScanLine(y);
            std::memcpy(destination, source, static_cast<std::size_t>(crop.start * pixelBytes));
            std::memcpy(destination + crop.start * pixelBytes, crop.removed.constScanLine(y),
                        static_cast<std::size_t>(crop.length * pixelBytes));
            std::memcpy(destination + (crop.start + crop.length) * pixelBytes,
                        source + crop.start * pixelBytes,
                        static_cast<std::size_t>((current.width() - crop.start) * pixelBytes));
        }
    }
    return restored;
}
} // namespace

ImageEditingDocument::ImageEditingDocument(QObject* parent)
    : QObject(parent), m_runtime(SnowCanvasRuntimeConfig{screenshotCanvasStyleDefaults()}) {
    m_runtime.setDocumentChangedHandler([this] {
        m_exportCache = {};
        if (!m_traveling && !m_inHostMutation) {
            m_flattenRedoStack.clear();
            if (!m_flattenUndoStack.isEmpty()) {
                if (m_canvasGestureActive)
                    m_pendingCanvasGestureCommand = true;
                else
                    recordPostFlattenCommand();
            }
        }
        pruneCrops();
        emit changed();
    });
}

bool ImageEditingDocument::load(const ImageEditingSource& source) {
    if (source.image.isNull() || !source.canvasRect.isValid() || source.resultStyle)
        return false;
    QImage image = source.image.depth() < 8 ? source.image.convertToFormat(QImage::Format_ARGB32)
                                            : source.image;
    if (image.isNull())
        return false;
    // Restore builds and validates a replacement runtime before publishing it. A second
    // runtime and deep clone would duplicate the complete document and undo history.
    const bool restored =
        !source.canvasSession.isEmpty()   ? m_runtime.restoreDocumentSession(source.canvasSession)
        : !source.canvasHistory.isEmpty() ? m_runtime.restoreDocumentHistory(source.canvasHistory)
                                          : m_runtime.reset();
    if (!restored)
        return false;
    m_image = std::move(image);
    m_previewImage = {};
    if (m_image.devicePixelRatio() != 1)
        m_image.setDevicePixelRatio(1);
    m_originalFileName = source.originalFileName;
    m_origin = source.canvasRect.topLeft();
    m_unitsPerPixel = QSizeF(source.canvasRect.width() / m_image.width(),
                             source.canvasRect.height() / m_image.height());
    m_samplingSources = exteriorSamplingSources(source.samplingSources, source.canvasRect);
    m_crops.clear();
    m_baseEdits.clear();
    m_flattenUndoStack.clear();
    m_flattenRedoStack.clear();
    m_serial = 0;
    m_flattenEpochId = 0;
    m_canvasGestureActive = false;
    m_pendingCanvasGestureCommand = false;
    updateSources();
    m_runtime.seedSmartEraseSnapshot(source.smartErase);
    m_saved = checkpoint();
    emit baseImageChanged();
    emit changed();
    return true;
}

QRectF ImageEditingDocument::bounds() const {
    return QRectF(m_origin, QSizeF(m_image.width() * m_unitsPerPixel.width(),
                                   m_image.height() * m_unitsPerPixel.height()));
}
QPointF ImageEditingDocument::imagePixelAt(QPointF point) const {
    return {(point.x() - m_origin.x()) / m_unitsPerPixel.width(),
            (point.y() - m_origin.y()) / m_unitsPerPixel.height()};
}

void ImageEditingDocument::setPreviewImage(QImage image) {
    if (m_previewImage.isNull() && image.isNull())
        return;
    m_previewImage = std::move(image);
    emit previewImageChanged();
}

void ImageEditingDocument::clearPreviewImage() {
    if (m_previewImage.isNull())
        return;
    m_previewImage = {};
    emit previewImageChanged();
}

QImage ImageEditingDocument::renderFlattenedBaseImage() const {
    if (m_image.isNull())
        return {};
    if (!m_runtime.hasDocumentContent())
        return m_image;
    const QColorSpace colorSpace = m_image.colorSpace();
    const qreal dpr = m_image.devicePixelRatio();
    const QRectF canvasRect = bounds();
    const auto base = baseImageSources();
    const auto smartEraseSeed = m_runtime.smartEraseSnapshot();
    if (!smartEraseSeed.cacheKey().isEmpty()) {
        const_cast<SnowCanvasRuntime&>(m_runtime).prepareSmartEraseForExport(smartEraseSeed, base,
                                                                             canvasRect);
    }
    QList<CanvasExportSource> sources;
    sources.reserve(base.size());
    for (const auto& source : base)
        sources.append({source.image, source.canvasRect, source.coverage});
    QImage rendered = const_cast<SnowCanvasRuntime&>(m_runtime).renderToImage(
        canvasRect, m_image.size(), sources, m_image);
    if (rendered.isNull())
        return {};
    if (rendered.devicePixelRatio() != dpr)
        rendered.setDevicePixelRatio(dpr);
    if (colorSpace.isValid() && !rendered.colorSpace().isValid())
        rendered.setColorSpace(colorSpace);
    return rendered;
}

ImageEditingDocument::Crop
ImageEditingDocument::prepareCrop(const QImage& image, CropAxis axis, int first, int last,
                                  const std::function<bool()>& cancelled) {
    Crop crop;
    crop.axis = axis;
    crop.sourceKey = image.cacheKey();
    crop.beforeSize = image.size();
    const int extent = axis == CropAxis::Rows ? image.height() : image.width();
    first = std::clamp(first, 0, extent);
    last = std::clamp(last, 0, extent);
    crop.start = std::min(first, last);
    crop.length = std::abs(first - last);
    if (image.isNull() || crop.length == 0 || crop.length >= extent || (cancelled && cancelled()))
        return crop;
    const bool rows = axis == CropAxis::Rows;
    crop.result = removeStrip(image, crop, cancelled);
    if (crop.result.isNull())
        return crop;
    crop.removed = image.copy(rows ? QRect(0, crop.start, image.width(), crop.length)
                                   : QRect(crop.start, 0, crop.length, image.height()));
    if (crop.removed.isNull() || (cancelled && cancelled())) {
        crop.result = {};
        crop.removed = {};
        return crop;
    }
    return crop;
}

bool ImageEditingDocument::applyCrop(Crop crop) {
    if (crop.result.isNull() || crop.sourceKey != m_image.cacheKey())
        return false;
    m_previewImage = {};
    const auto before = baseState();
    const auto token = ++m_serial;
    {
        QScopedValueRollback mutationGuard(m_inHostMutation, true);
        if (!m_runtime.commitHostHistoryState(token))
            return false;
    }
    m_flattenRedoStack.clear();
    m_image = std::move(crop.result);
    m_crops.insert(token, CropEntry{before, std::move(crop)});
    recordPostFlattenCommand();
    pruneCrops();
    updateSources();
    emit baseImageChanged();
    emit changed();
    return true;
}
bool ImageEditingDocument::crop(CropAxis axis, int first, int last) {
    return applyCrop(prepareCrop(m_image, axis, first, last));
}
bool ImageEditingDocument::applyBaseImage(QImage image) {
    if (image.isNull() || m_image.isNull() || image.cacheKey() == m_image.cacheKey())
        return false;
    m_previewImage = {};
    const auto before = baseState();
    const auto token = ++m_serial;
    {
        QScopedValueRollback mutationGuard(m_inHostMutation, true);
        if (!m_runtime.commitHostHistoryState(token))
            return false;
    }
    m_flattenRedoStack.clear();
    m_baseEdits.insert(token, BaseEditEntry{before, m_image, image});
    m_image = std::move(image);
    recordPostFlattenCommand();
    pruneCrops();
    updateSources();
    emit baseImageChanged();
    emit changed();
    return true;
}
bool ImageEditingDocument::applyFlattenedBaseImage(QImage image) {
    if (image.isNull() || m_image.isNull())
        return false;
    QByteArray serialized = m_runtime.serializeDocumentHistory();
    if (serialized.isEmpty())
        return false;
    FlattenUndoEntry entry;
    entry.preVectorHistory = std::move(serialized);
    entry.preImage = m_image;
    entry.preSamplingSources = m_samplingSources;
    entry.preSmartErase = m_runtime.smartEraseSnapshot();
    entry.preEpochId = m_flattenEpochId;
    entry.stepsFromFlatten = 0;

    m_previewImage = {};
    m_canvasGestureActive = false;
    m_pendingCanvasGestureCommand = false;
    m_flattenRedoStack.clear();
    {
        QScopedValueRollback mutationGuard(m_inHostMutation, true);
        if (!m_runtime.clearDocumentPreservingViewports())
            return false;
    }
    // Existing outer flatten checkpoints count this flatten+transform as 1 command.
    recordPostFlattenCommand();
    m_flattenUndoStack.append(std::move(entry));
    m_crops.clear();
    m_baseEdits.clear();
    m_image = std::move(image);
    m_flattenEpochId = ++m_nextFlattenEpochId;
    updateSources();
    emit baseImageChanged();
    emit changed();
    return true;
}
void ImageEditingDocument::setCanvasGestureActive(bool active) {
    if (m_canvasGestureActive == active)
        return;
    m_canvasGestureActive = active;
    if (active) {
        m_pendingCanvasGestureCommand = false;
    } else if (std::exchange(m_pendingCanvasGestureCommand, false)) {
        recordPostFlattenCommand();
        emit changed();
    }
}
bool ImageEditingDocument::canUndo() const {
    return m_runtime.canUndo() || !m_flattenUndoStack.isEmpty();
}
bool ImageEditingDocument::canRedo() const {
    return m_runtime.canRedo() || !m_flattenRedoStack.isEmpty();
}
void ImageEditingDocument::recordPostFlattenCommand() {
    for (auto& entry : m_flattenUndoStack)
        ++entry.stepsFromFlatten;
    while (!m_flattenUndoStack.isEmpty() &&
           m_flattenUndoStack.first().stepsFromFlatten > kMaxVectorUndoCommandsAfterFlatten) {
        m_flattenUndoStack.removeFirst();
    }
}
void ImageEditingDocument::adjustFlattenStepDistances(int delta) {
    for (auto& entry : m_flattenUndoStack)
        entry.stepsFromFlatten = std::max(0, entry.stepsFromFlatten + delta);
}
ImageEditingDocument::HistoryTransition ImageEditingDocument::historyTransition(bool redo) const {
    HistoryTransition transition;
    transition.redo = redo;
    transition.state = baseState();
    transition.nextState = m_runtime.hostHistoryState(redo ? 1 : -1);
    transition.revision = m_runtime.documentRevision();
    transition.sourceKey = m_image.cacheKey();
    if (!redo && !m_runtime.canUndo() && !m_flattenUndoStack.isEmpty()) {
        transition.flattenBoundary = true;
        return transition;
    }
    if (redo && !m_runtime.canRedo() && !m_flattenRedoStack.isEmpty()) {
        transition.flattenBoundary = true;
        return transition;
    }
    if (transition.nextState != transition.state) {
        const auto key = redo ? transition.nextState : transition.state;
        const auto entry = m_crops.constFind(key);
        if (entry != m_crops.cend()) {
            transition.image = m_image;
            transition.crop = entry->crop;
        } else if (const auto edit = m_baseEdits.constFind(key); edit != m_baseEdits.cend()) {
            transition.baseEdit = true;
            transition.image = redo ? edit->afterImage : edit->beforeImage;
        }
    }
    return transition;
}
void ImageEditingDocument::prepareHistoryTransition(HistoryTransition& transition,
                                                    const std::function<bool()>& cancelled) {
    if (transition.flattenBoundary || transition.image.isNull() || transition.baseEdit)
        return;
    transition.image = transition.redo ? removeStrip(transition.image, transition.crop, cancelled)
                                       : joinStrip(transition.image, transition.crop, cancelled);
}
bool ImageEditingDocument::applyHistoryTransition(HistoryTransition transition) {
    if (m_image.cacheKey() != transition.sourceKey ||
        m_runtime.documentRevision() != transition.revision)
        return false;
    if (transition.flattenBoundary) {
        m_previewImage = {};
        m_canvasGestureActive = false;
        m_pendingCanvasGestureCommand = false;
        QScopedValueRollback travelingGuard(m_traveling, true);
        if (!transition.redo) {
            if (m_runtime.canUndo() || m_flattenUndoStack.isEmpty())
                return false;
            const FlattenUndoEntry entry = m_flattenUndoStack.takeLast();
            QByteArray postHistory = m_runtime.serializeDocumentHistory();
            if (!m_runtime.restoreDocumentHistoryPreservingEditorStyles(entry.preVectorHistory)) {
                m_flattenUndoStack.append(entry);
                return false;
            }
            FlattenRedoEntry redoEntry;
            redoEntry.flattenEntry = entry;
            redoEntry.postVectorHistory = std::move(postHistory);
            redoEntry.postImage = m_image;
            redoEntry.postSamplingSources = m_samplingSources;
            redoEntry.postSmartErase = m_runtime.smartEraseSnapshot();
            redoEntry.postEpochId = m_flattenEpochId;
            redoEntry.postSerial = m_serial;
            redoEntry.postCrops = m_crops;
            redoEntry.postBaseEdits = m_baseEdits;
            m_flattenRedoStack.append(std::move(redoEntry));
            adjustFlattenStepDistances(-1);
            m_image = entry.preImage;
            m_samplingSources = entry.preSamplingSources;
            m_runtime.seedSmartEraseSnapshot(entry.preSmartErase);
            m_flattenEpochId = entry.preEpochId;
            m_crops.clear();
            m_baseEdits.clear();
            updateSources();
            emit baseImageChanged();
            emit changed();
            return true;
        }
        if (m_runtime.canRedo() || m_flattenRedoStack.isEmpty())
            return false;
        const FlattenRedoEntry redoEntry = m_flattenRedoStack.takeLast();
        const bool restored = !redoEntry.postVectorHistory.isEmpty()
                                  ? m_runtime.restoreDocumentHistoryPreservingEditorStyles(
                                        redoEntry.postVectorHistory)
                                  : m_runtime.clearDocumentPreservingViewports();
        if (!restored) {
            m_flattenRedoStack.append(redoEntry);
            return false;
        }
        adjustFlattenStepDistances(1);
        m_flattenUndoStack.append(redoEntry.flattenEntry);
        m_image = redoEntry.postImage;
        m_samplingSources = redoEntry.postSamplingSources;
        m_runtime.seedSmartEraseSnapshot(redoEntry.postSmartErase);
        m_flattenEpochId = redoEntry.postEpochId;
        m_serial = redoEntry.postSerial;
        m_crops = redoEntry.postCrops;
        m_baseEdits = redoEntry.postBaseEdits;
        updateSources();
        emit baseImageChanged();
        emit changed();
        return true;
    }
    if (baseState() != transition.state ||
        m_runtime.hostHistoryState(transition.redo ? 1 : -1) != transition.nextState)
        return false;
    if (transition.state != transition.nextState && transition.image.isNull())
        return false;
    m_previewImage = {};
    m_canvasGestureActive = false;
    m_pendingCanvasGestureCommand = false;
    bool ok = false;
    {
        QScopedValueRollback travelingGuard(m_traveling, true);
        ok = transition.redo ? m_runtime.redo() : m_runtime.undo();
    }
    if (!ok)
        return false;
    adjustFlattenStepDistances(transition.redo ? 1 : -1);
    if (transition.state != transition.nextState) {
        m_image = std::move(transition.image);
        updateSources();
        emit baseImageChanged();
    }
    emit changed();
    return true;
}
bool ImageEditingDocument::travel(bool redo) {
    auto transition = historyTransition(redo);
    prepareHistoryTransition(transition);
    return applyHistoryTransition(std::move(transition));
}
void ImageEditingDocument::pruneCrops() {
    if (m_runtime.canRedo())
        return;
    QSet<quint64> retained;
    for (quint64 state = baseState(); state;) {
        if (const auto crop = m_crops.constFind(state); crop != m_crops.cend()) {
            retained.insert(state);
            state = crop->before;
            continue;
        }
        const auto edit = m_baseEdits.constFind(state);
        if (edit == m_baseEdits.cend())
            break;
        retained.insert(state);
        state = edit->before;
    }
    // A host entry is published just before its Qt delta is installed.
    if (baseState() && !retained.contains(baseState()))
        return;
    for (auto it = m_crops.begin(); it != m_crops.end();)
        if (!retained.contains(it.key()))
            it = m_crops.erase(it);
        else
            ++it;
    for (auto it = m_baseEdits.begin(); it != m_baseEdits.end();)
        if (!retained.contains(it.key()))
            it = m_baseEdits.erase(it);
        else
            ++it;
}
bool ImageEditingDocument::undo() {
    return travel(false);
}
bool ImageEditingDocument::redo() {
    return travel(true);
}
void ImageEditingDocument::updateSources() {
    m_exportCache = {};
    m_runtime.setBaseImageSources(baseImageSources());
}
QList<SnowCanvasBaseImageSource> ImageEditingDocument::baseImageSources() const {
    auto sources = m_samplingSources;
    sources.append({m_image, bounds(), {}});
    return sources;
}
QByteArray ImageEditingDocument::checkpoint() const {
    const QByteArray bytes = m_runtime.documentCheckpoint();
    if (bytes.isEmpty())
        return QByteArrayLiteral("unserializable:") +
               QByteArray::number(m_runtime.documentRevision()) + ":" +
               QByteArray::number(baseState()) + ":" + QByteArray::number(m_flattenEpochId);
    const auto state = baseState();
    const auto epoch = m_flattenEpochId;
    if (!m_checkpointContent.isEmpty() && bytes.constData() == m_checkpointContent.constData() &&
        state == m_checkpointBaseState && epoch == m_checkpointFlattenEpochId)
        return m_checkpoint;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(bytes);
    hash.addData(QByteArray::number(state));
    if (epoch != 0) {
        hash.addData(QByteArrayLiteral(":flatten:"));
        hash.addData(QByteArray::number(epoch));
    }
    m_checkpoint = hash.result();
    // Share the runtime's bounded cache; retaining the bytes also makes identity comparisons
    // safe when a different runtime happens to have the same document revision.
    m_checkpointContent = bytes.size() <= 16 * 1024 * 1024 ? bytes : QByteArray{};
    m_checkpointBaseState = state;
    m_checkpointFlattenEpochId = epoch;
    return m_checkpoint;
}
bool ImageEditingDocument::dirty() const {
    return !m_image.isNull() && checkpoint() != m_saved;
}
void ImageEditingDocument::markExported(QByteArray checkpoint) {
    m_saved = std::move(checkpoint);
    emit changed();
}
ScreenshotExportSource ImageEditingDocument::exportSource(QString* error) const {
    if (error)
        error->clear();
    if (m_image.isNull())
        return {};
    if (!m_runtime.hasDocumentContent()) {
        m_exportCache = {};
        return ScreenshotExportSource::fromImage(m_image);
    }
    const auto content = checkpoint();
    const auto smartEraseKey = m_runtime.smartEraseSnapshot().cacheKey();
    if (m_exportCache.source.isValid() && m_exportCache.checkpoint == content &&
        m_exportCache.smartEraseKey == smartEraseKey)
        return m_exportCache.source;
    m_exportCache = {};
    const auto source = snapshot();
    if (!source.error.isEmpty()) {
        if (error)
            *error = source.error;
        return {};
    }
    auto output = imageEditingExportSource(source);
    // Copies share immutable rendered pixels, while each export owns its own cancellation.
    // Retain one result only, and avoid keeping an additional tall-image raster between actions.
    constexpr qint64 maximumRetainedBytes = 64 * 1024 * 1024;
    if (qint64(m_image.width()) * m_image.height() <= maximumRetainedBytes / 4)
        m_exportCache = {content, smartEraseKey, output};
    return output;
}

ImageEditingSource ImageEditingDocument::snapshot() const {
    ImageEditingSource result;
    result.image = m_image;
    result.originalFileName = m_originalFileName;
    result.canvasRect = bounds();
    result.canvasSession = m_runtime.serializeDocumentSnapshot();
    if (result.canvasSession.isEmpty())
        result.error = QCoreApplication::translate(
            "ImageEditingWindow", "The editing content could not be prepared for export");
    result.smartErase = m_runtime.smartEraseSnapshot();
    result.samplingSources = baseImageSources();
    return result;
}
} // namespace snow_shot::presentation
