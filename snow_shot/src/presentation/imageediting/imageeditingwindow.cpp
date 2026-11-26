// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/presentation/imageeditingwindow.h"
#include "snowimageqtcodec.h"
#include "snow_shot/presentation/screenshotexportcoordinator.h"
#include "snow_shot/presentation/components/windowconfirmation.h"
#include "snow_shot/shortcuts/annotationoptionshortcut.h"
#include "snow_shot/presentation/screenshotpinnedrenderpreparation.h"
#include "snow_shot/presentation/canvasdrawingpreferences.h"
#include "imageeditingexportcontroller.h"
#include "snow_shot/presentation/imageeditingcanvas.h"
#include "snow_shot/presentation/components/canvasactiontoolbar.h"
#include "snow_shot/presentation/components/imagedropsurface.h"
#include "snow_shot/presentation/collagedocument.h"
#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include "antd_icons.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/presentation/screenshotdrawingtoolbaroptions.h"
#include "snow_shot/presentation/screenshotcanvascolorsampler.h"
#include "snow_shot/presentation/screenshotcanvascolorsamplerwindow.h"
#include "widgets/color_picker.h"
#include "widgets/select.h"
#include <QKeyEvent>
#include <QKeySequence>
#include <QtMath>
#include <QMouseEvent>
#include "snow_shot/presentation/screenshotcanvasoperations.h"
#include "snow_shot/presentation/screenshotcanvastools.h"
#include "snow_shot/presentation/screenshotstylebinding.h"
#include "snow_shot/presentation/screenshotfloatingtoolpalettewindow.h"
#include "snow_shot/presentation/pinnedgeometry.h"
#include "snow_shot/presentation/screenshottoolpalettehost.h"
#include "snow_shot/presentation/screenshotrecognitionsessioncontroller.h"
#include "snow_shot/presentation/screenshotrecognitionwindow.h"
#include "snow_shot/presentation/screenshotmessageservice.h"
#include "snow_shot/presentation/screenshotautofiltercontroller.h"
#include "snow_shot/presentation/canvashistoryshortcuts.h"
#include "snow_shot/presentation/pinnedwindowgroupmanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/storage/pinnedwindowrepository.h"
#include "widgets/modal.h"
#include "widgets/message.h"
#include "icon_renderer.h"
#include "widgets/button.h"
#include "widgets/input_number.h"
#include "widgets/slider.h"
#include <QApplication>
#include <QClipboard>
#include <QColorSpace>
#include <QContextMenuEvent>
#include <QCursor>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QPainter>
#include <QScreen>
#include <QScopedValueRollback>
#include <QScopeGuard>
#include <QSignalBlocker>
#include <QThread>
#include <QStackedLayout>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QPixmap>
#include <QPushButton>
#include <QScrollBar>
#include <QSlider>
#include <QTransform>
#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QDataStream>
#include <QCryptographicHash>
#include <algorithm>
#include <utility>

namespace snow_shot::presentation {
using adqt::widgets::AdButton;
using adqt::widgets::AdInputNumber;
using adqt::widgets::AdMessage;
using adqt::widgets::AdMessageHandle;
using adqt::widgets::AdMessageService;
using adqt::widgets::AdModal;
using adqt::widgets::AdSlider;
using Session = ScreenshotRecognitionSessionController;
using Tool = ScreenshotToolPalette::Tool;
using ExportOperation = ImageEditingExportController::Operation;
namespace {
const char* selectedFileErrorSource(platform::SelectedFileError error) {
    switch (error) {
    case platform::SelectedFileError::PermissionDenied:
        return QT_TRANSLATE_NOOP("ImageEditingWindow",
                                 "Allow Snow Shot to access Finder in System Settings > Privacy & "
                                 "Security > Automation, then try again");
    case platform::SelectedFileError::Timeout:
        return QT_TRANSLATE_NOOP("ImageEditingWindow",
                                 "The file manager took too long to return the selected files. "
                                 "Please try again");
    case platform::SelectedFileError::Unavailable:
        return QT_TRANSLATE_NOOP("ImageEditingWindow",
                                 "The file manager is unavailable. Open it and try again");
    case platform::SelectedFileError::QueryFailed:
    case platform::SelectedFileError::None:
        return QT_TRANSLATE_NOOP("ImageEditingWindow",
                                 "Could not read the selected image files. Please try again");
    }
    return nullptr;
}

QStringList imageFilePaths(const QStringList& paths,
                           const ScreenshotClipboardContentReader::CancellationCheck& cancelled) {
    QStringList images;
    const auto extensions = ScreenshotClipboardContentReader::supportedImageFileExtensions();
    for (const auto& path : paths) {
        if (extensions.contains(QFileInfo(path).suffix().toLower()))
            images.append(path);
    }
    QStringList result;
    for (const auto& file : ScreenshotClipboardContentReader::snapshotLocalFiles(
             images, cancelled, ScreenshotClipboardContentReader::FilePurpose::ImageImport))
        result.append(file.absolutePath);
    return result;
}

QString imageFileFilter() {
    QStringList patterns;
    for (const auto& suffix : ScreenshotClipboardContentReader::supportedImageFileExtensions())
        patterns.append(QStringLiteral("*.") + suffix);
    return ImageEditingWindow::tr("Images") + QStringLiteral(" (%1)").arg(patterns.join(u' '));
}

class PreviewSurface final : public QWidget {
  public:
    explicit PreviewSurface(ImageEditingDocument& document, QWidget* parent = nullptr)
        : QWidget(parent), m_document(document) {
        connect(&styles::ThemeManager::instance(), &styles::ThemeManager::themeChanged, this,
                [this] { update(); });
        connect(&document, &ImageEditingDocument::baseImageChanged, this, [this] { update(); });
    }
    void paintEvent(QPaintEvent*) override {
        if (m_document.image().isNull())
            return;
        QPainter painter(this);
        const auto theme = styles::ThemeManager::instance().themeColorScheme().map;
        constexpr int tile = 10;
        if (m_background != theme.colorBgContainer || m_foreground != theme.colorFillSecondary ||
            m_checkerboard.style() == Qt::NoBrush) {
            m_background = theme.colorBgContainer;
            m_foreground = theme.colorFillSecondary;
            QPixmap pattern(tile * 2, tile * 2);
            pattern.fill(m_background);
            QPainter patternPainter(&pattern);
            patternPainter.fillRect(QRect(tile, 0, tile, tile), m_foreground);
            patternPainter.fillRect(QRect(0, tile, tile, tile), m_foreground);
            patternPainter.end();
            m_checkerboard = QBrush(pattern);
        }
        painter.fillRect(rect(), m_checkerboard);
    }

  private:
    const ImageEditingDocument& m_document;
    QBrush m_checkerboard;
    QColor m_background;
    QColor m_foreground;
};
ScreenshotToolPalette::Options editorOptions() {
    auto options = screenshotDrawingToolbarOptions();
    options.moveToolPresentation = ScreenshotToolPalette::MoveToolPresentation::Pan;
    options.showCropTools = true;
    options.showAirDropButton = true;
    options.actions = ScreenshotToolPalette::CopyAction | ScreenshotToolPalette::PinAction;
    options.copyButtonWithNeutralIcon = false;
    options.separatorBeforeConfirm = false;
    options.showDrawingModeShortcutOnConfirm = false;
    return options;
}
} // namespace

namespace {
// SnowView Rev0.1: whole-raster color adjustment behind the image toolbar.
// brightness/contrast/saturation take -100..100, sharpen takes 0..100.
QImage snowViewAdjustedImage(const QImage& source, int brightness, int contrast, int saturation,
                             int sharpen) {
    if (source.isNull())
        return {};
    QImage image = source.convertToFormat(QImage::Format_ARGB32);
    const double offset = brightness * 2.55;
    const double factor = 1.0 + contrast / 100.0;
    const double saturationFactor = 1.0 + saturation / 100.0;
    uchar table[256];
    for (int i = 0; i < 256; ++i)
        table[i] = static_cast<uchar>(std::clamp((i - 128) * factor + 128 + offset, 0.0, 255.0));
    for (int y = 0; y < image.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = line[x];
            int red = table[qRed(pixel)];
            int green = table[qGreen(pixel)];
            int blue = table[qBlue(pixel)];
            if (saturation != 0) {
                const int gray = qGray(red, green, blue);
                red = std::clamp(qRound(gray + (red - gray) * saturationFactor), 0, 255);
                green = std::clamp(qRound(gray + (green - gray) * saturationFactor), 0, 255);
                blue = std::clamp(qRound(gray + (blue - gray) * saturationFactor), 0, 255);
            }
            line[x] = qRgba(red, green, blue, qAlpha(pixel));
        }
    }
    if (sharpen > 0 && image.width() > 2 && image.height() > 2) {
        const double k = sharpen / 100.0 * 0.75;
        const QImage original = image;
        const auto sharpened = [k](int center, int up, int down, int left, int right) {
            return std::clamp(qRound(center * (1.0 + 4.0 * k) - k * (up + down + left + right)), 0,
                              255);
        };
        for (int y = 1; y < image.height() - 1; ++y) {
            auto* line = reinterpret_cast<QRgb*>(image.scanLine(y));
            const auto* above = reinterpret_cast<const QRgb*>(original.constScanLine(y - 1));
            const auto* mid = reinterpret_cast<const QRgb*>(original.constScanLine(y));
            const auto* below = reinterpret_cast<const QRgb*>(original.constScanLine(y + 1));
            for (int x = 1; x < image.width() - 1; ++x) {
                line[x] = qRgba(sharpened(qRed(mid[x]), qRed(above[x]), qRed(below[x]),
                                          qRed(mid[x - 1]), qRed(mid[x + 1])),
                                sharpened(qGreen(mid[x]), qGreen(above[x]), qGreen(below[x]),
                                          qGreen(mid[x - 1]), qGreen(mid[x + 1])),
                                sharpened(qBlue(mid[x]), qBlue(above[x]), qBlue(below[x]),
                                          qBlue(mid[x - 1]), qBlue(mid[x + 1])),
                                qAlpha(mid[x]));
            }
        }
    }
    return image;
}

// SnowView Rev0.1: per-channel histogram stretch (outer 1% clipped) plus mild sharpening.
QImage snowViewAutoEnhancedImage(const QImage& source) {
    if (source.isNull())
        return {};
    const QImage image = source.convertToFormat(QImage::Format_ARGB32);
    int low[3] = {0, 0, 0};
    int high[3] = {255, 255, 255};
    const qint64 clip =
        std::max<qint64>(1, static_cast<qint64>(image.width()) * image.height() / 100);
    for (int channel = 0; channel < 3; ++channel) {
        qint64 histogram[256] = {};
        for (int y = 0; y < image.height(); ++y) {
            const auto* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
            for (int x = 0; x < image.width(); ++x)
                ++histogram[channel == 0   ? qRed(line[x])
                            : channel == 1 ? qGreen(line[x])
                                           : qBlue(line[x])];
        }
        qint64 seen = 0;
        for (int i = 0; i < 256; ++i) {
            seen += histogram[i];
            if (seen >= clip) {
                low[channel] = i;
                break;
            }
        }
        seen = 0;
        for (int i = 255; i >= 0; --i) {
            seen += histogram[i];
            if (seen >= clip) {
                high[channel] = i;
                break;
            }
        }
    }
    QImage stretched = image;
    for (int y = 0; y < stretched.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(stretched.scanLine(y));
        for (int x = 0; x < stretched.width(); ++x) {
            const auto stretch = [](int value, int lo, int hi) {
                return hi <= lo ? value : std::clamp((value - lo) * 255 / (hi - lo), 0, 255);
            };
            line[x] = qRgba(stretch(qRed(line[x]), low[0], high[0]),
                            stretch(qGreen(line[x]), low[1], high[1]),
                            stretch(qBlue(line[x]), low[2], high[2]), qAlpha(line[x]));
        }
    }
    return snowViewAdjustedImage(stretched, 0, 0, 0, 25);
}

// SnowView Rev0.2: map image extensions to snow_image formats and decode thumbnails
// via snow_shot::image_codec::decodeThumbnail so static Qt builds display JPEG/WebP/HEIC/AVIF/JXL.
snow::image::Format snowViewFormatForPath(const QString& path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QStringLiteral("png") || suffix == QStringLiteral("apng"))
        return snow::image::Format::png;
    if (suffix == QStringLiteral("jpg") || suffix == QStringLiteral("jpeg"))
        return snow::image::Format::jpeg;
    if (suffix == QStringLiteral("bmp"))
        return snow::image::Format::bmp;
    if (suffix == QStringLiteral("gif"))
        return snow::image::Format::gif;
    if (suffix == QStringLiteral("webp"))
        return snow::image::Format::webp;
    if (suffix == QStringLiteral("tif") || suffix == QStringLiteral("tiff"))
        return snow::image::Format::tiff;
    if (suffix == QStringLiteral("avif"))
        return snow::image::Format::avif;
    if (suffix == QStringLiteral("heic") || suffix == QStringLiteral("heif"))
        return snow::image::Format::heif;
    if (suffix == QStringLiteral("jxl"))
        return snow::image::Format::jxl;
    return snow::image::Format::unknown;
}

QImage snowViewDecodeThumbnail(const QString& path, QSize targetPixelSize,
                               const std::function<bool()>& cancelled) {
    constexpr qint64 kMaxThumbInputBytes = 128LL * 1024LL * 1024LL;
    if (targetPixelSize.isEmpty() || (cancelled && cancelled()))
        return {};
    QFile file(path);
    const qint64 fileSize = file.size();
    if (fileSize <= 0 || fileSize > kMaxThumbInputBytes || !file.open(QIODevice::ReadOnly))
        return {};
    const QByteArray bytes = file.read(fileSize + 1);
    if (bytes.isEmpty() || bytes.size() != fileSize || (cancelled && cancelled()))
        return {};
    QImage decoded = snow_shot::image_codec::decodeThumbnail(bytes, snowViewFormatForPath(path),
                                                             targetPixelSize, nullptr, cancelled);
    if (!decoded.isNull())
        return decoded;
    if (cancelled && cancelled())
        return {};
    QImageReader reader(path);
    reader.setAutoTransform(true);
    if (const QSize size = reader.size(); size.isValid())
        reader.setScaledSize(size.scaled(targetPixelSize, Qt::KeepAspectRatio));
    return reader.read();
}

// Theme-aware floating panel matching CanvasActionToolbar visuals for the Photoshop-style Crop bar.
class SnowViewCropBarPanel final : public QWidget {
  public:
    explicit SnowViewCropBarPanel(QWidget* parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_NoMousePropagation);
        connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged,
                this, [this] { update(); });
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
};
} // namespace

struct ImageEditingWindow::Impl {
    ImageEditingWindow& q;
    ImageEditingServices services;
    ImageEditingDocument document;
    AdModal* modal;
    PreviewSurface* surface;
    ImageEditingCanvas* canvas;
    QStackedLayout* stack;
    components::ImageDropSurface* emptyState;
    components::ImageDropSurface* dropHint;
    AdButton* addImages;
    QPointer<QFileDialog> fileDialog;
    bool choosingFiles = false;
    std::shared_ptr<ImageStitchingImportReservation> fileReservation;
    ScreenshotFloatingToolPaletteWindow* floating;
    QPointer<QWidget> editorWindow;
    ScreenshotToolPaletteHost* host;
    ScreenshotToolPalette* palette;
    AdButton *fit, *actual;
    // SnowView Rev0.2: folder filmstrip (async snow_image thumbnails), metadata bar,
    // unified top canvas toolbar (transforms + crop + enhance + zoom), and Photoshop crop bar.
    QWidget* contentPane = nullptr;
    QListWidget* filmstrip = nullptr;
    QLabel* metadataBar = nullptr;
    QString currentFilePath;
    QString filmstripDirPath;
    bool filmstripSyncing = false;
    ScreenshotExportQueue filmstripQueue;
    quint64 filmstripGeneration = 0;
    components::CanvasActionToolbar* imageToolbar = nullptr;
    AdButton* flipHorizontalButton = nullptr;
    AdButton* flipVerticalButton = nullptr;
    AdButton* rotateClockwiseButton = nullptr;
    AdButton* rotateCounterclockwiseButton = nullptr;
    AdButton* cropButton = nullptr;
    AdButton* adjustButton = nullptr;
    AdButton* autoEnhanceButton = nullptr;
    AdButton* grayscaleButton = nullptr;
    SnowViewCropBarPanel* cropBar = nullptr;
    AdSlider* cropAngleSlider = nullptr;
    AdInputNumber* cropAngleInput = nullptr;
    QLabel* cropSizeLabel = nullptr;
    AdButton* cropResetButton = nullptr;
    AdButton* cropSwapButton = nullptr;
    AdButton* cropCancelButton = nullptr;
    AdButton* cropApplyButton = nullptr;
    enum class CropRatioPreset { Free, Original, Square, Ratio4x3, Ratio16x9, Ratio3x2 };
    CropRatioPreset cropPreset = CropRatioPreset::Free;
    bool cropRatioSwapped = false;
    std::array<AdButton*, 6> cropPresetButtons{};
    QPointer<AdMessageHandle> statusMessage;
    WindowShortcutManager shortcuts;
    QVector<WindowShortcutManager::BindingHandle> commandBindings;
    ScreenshotAutoFilterController* autoFilter;
    ScreenshotRecognitionWindow* recognitionView = nullptr;
    bool recognitionVisible = false;
    QPointer<Session> session;
    struct RecognitionState {
        QPointer<Session> controller;
        QHash<QString, QByteArray> saved;
    };
    QHash<quint64, RecognitionState> sessions;
    QPointer<AdModal> confirmation;
    ScreenshotExportJobHandle loadJob, cropJob;
    std::shared_ptr<ScreenshotExportArtifact> loadArtifact;
    ImageEditingExportController exporter;
    WheelStepAccumulator wheel;
    QPoint toolbarOffset;
    bool manuallyPlaced = false;
    bool placingToolbar = false, draggingToolbar = false;
    bool windowInteractionActive = false;
    bool loading = false, cropBusy = false, closing = false;
    bool translateWhenReady = false;
    ScreenshotRecognitionResults initialRecognition;
    std::unique_ptr<ScreenshotCanvasColorSamplingSession> colorSampling;
    std::shared_ptr<ScreenshotExportArtifact> colorArtifact;
    QImage colorImage;
    quint64 colorGeneration = 0;
    quint64 loadGeneration = 0;
    enum class Notice { None, Loading, LoadError, Exporting, Exported };
    Notice notice = Notice::None;
    const char* errorSource = nullptr;

    Impl(ImageEditingWindow& owner, ImageEditingServices service)
        : q(owner), services(std::move(service)), document(&q), modal(new AdModal(&q)),
          surface(new PreviewSurface(document)), canvas(new ImageEditingCanvas(document, surface)),
          stack(new QStackedLayout(surface)),
          floating(new ScreenshotFloatingToolPaletteWindow(editorOptions())),
          host(floating->paletteHost()), palette(host->palette()),
          filmstripQueue(ScreenshotExportCoordinator::shared(), &q), shortcuts(&q),
          autoFilter(new ScreenshotAutoFilterController(
              [this] { return document.bounds(); }, [this](auto done) { done(document.image()); },
              &q)),
          exporter(services.printing, &q, services.pin, services.airDrop) {
        surface->setObjectName(QStringLiteral("imageEditingPreview"));
        surface->installEventFilter(&q);
        canvas->installEventFilter(&q);
        surface->setAcceptDrops(true);
        stack->setContentsMargins(0, 0, 0, 0);
        stack->addWidget(canvas);
        emptyState = new components::ImageDropSurface(false, surface);
        emptyState->setObjectName(QStringLiteral("imageEditingEmptyState"));
        addImages = emptyState->addImageButton();
        addImages->setObjectName(QStringLiteral("imageEditingAddImages"));
        stack->addWidget(emptyState);
        stack->setCurrentWidget(emptyState);
        dropHint = new components::ImageDropSurface(true, surface);
        dropHint->setObjectName(QStringLiteral("imageEditingDropHint"));
        dropHint->hide();
        QObject::connect(addImages, &AdButton::clicked, &q, [this] { chooseImages(); });
        QObject::connect(&styles::ThemeManager::instance(), &styles::ThemeManager::themeChanged, &q,
                         [this] {
                             emptyState->refreshTheme();
                             dropHint->refreshTheme();
                         });
        auto* overlay = new QVBoxLayout(canvas);
        overlay->setContentsMargins(16, 16, 16, 16);
        overlay->setSpacing(8);
        // SnowView Rev0.2: unified top canvas toolbar (transforms + Photoshop crop + enhance +
        // zoom) with larger 18px icons inside 32x32 action buttons.
        imageToolbar = new components::CanvasActionToolbar(canvas, QSize(32, 32), QSize(18, 18));
        imageToolbar->setObjectName(QStringLiteral("snowViewImageToolbar"));
        flipHorizontalButton = imageToolbar->addAction(icons::custom::outlined::FlipHorizontal(),
                                                       "snowViewFlipHorizontal");
        flipVerticalButton = imageToolbar->addAction(icons::custom::outlined::FlipVertical(),
                                                     "snowViewFlipVertical");
        rotateClockwiseButton = imageToolbar->addAction(adqt::icons::antd::outlined::RotateRight(),
                                                        "snowViewRotateClockwise");
        rotateCounterclockwiseButton = imageToolbar->addAction(
            adqt::icons::antd::outlined::RotateLeft(), "snowViewRotateCounterclockwise");
        cropButton = imageToolbar->addAction(icons::custom::outlined::Crop(), "snowViewCrop");
        cropButton->setCheckable(true);
        imageToolbar->addSeparator();
        adjustButton =
            imageToolbar->addAction(adqt::icons::antd::outlined::Control(), "snowViewAdjust");
        autoEnhanceButton = imageToolbar->addAction(adqt::icons::antd::outlined::Thunderbolt(),
                                                    "snowViewAutoEnhance");
        grayscaleButton =
            imageToolbar->addAction(adqt::icons::antd::outlined::BgColors(), "snowViewGrayscale");
        imageToolbar->addSeparator();
        fit = imageToolbar->addAction(adqt::icons::antd::outlined::Fullscreen(), "imageEditingFit");
        actual = imageToolbar->addAction(icons::custom::outlined::ActualSize(),
                                         "imageEditingActualSize");
        imageToolbar->hide();
        overlay->addWidget(imageToolbar, 0, Qt::AlignHCenter);

        // SnowView Rev0.2: Photoshop-style Crop options bar directly under the unified top toolbar.
        cropBar = new SnowViewCropBarPanel(canvas);
        cropBar->setObjectName(QStringLiteral("snowViewCropBar"));
        auto* cropLayout = new QHBoxLayout(cropBar);
        cropLayout->setContentsMargins(8, 4, 8, 4);
        cropLayout->setSpacing(4);
        const std::array<const char*, 6> presetNames = {"snowViewCropFree", "snowViewCropOriginal",
                                                        "snowViewCrop1x1",  "snowViewCrop4x3",
                                                        "snowViewCrop16x9", "snowViewCrop3x2"};
        for (std::size_t i = 0; i < cropPresetButtons.size(); ++i) {
            auto* btn = new AdButton(cropBar);
            btn->setObjectName(QString::fromLatin1(presetNames[i]));
            btn->setSizeClass(AdButton::SizeClass::Small);
            btn->setButtonStyle(AdButton::ButtonStyle::Text);
            btn->setCheckable(true);
            btn->setFocusPolicy(Qt::StrongFocus);
            cropLayout->addWidget(btn);
            cropPresetButtons[i] = btn;
            const auto presetValue = static_cast<CropRatioPreset>(i);
            QObject::connect(btn, &AdButton::clicked, &q,
                             [this, presetValue] { setCropRatioPreset(presetValue); });
        }
        cropSwapButton = new AdButton(cropBar);
        cropSwapButton->setObjectName(QStringLiteral("snowViewCropSwap"));
        cropSwapButton->setIconRef(adqt::icons::antd::outlined::Swap());
        cropSwapButton->setSizeClass(AdButton::SizeClass::Small);
        cropSwapButton->setButtonStyle(AdButton::ButtonStyle::Text);
        cropSwapButton->setFixedSize(26, 26);
        cropSwapButton->setReferenceIconSize({14, 14});
        cropSwapButton->setFocusPolicy(Qt::StrongFocus);
        cropLayout->addWidget(cropSwapButton);

        cropAngleSlider = new AdSlider(cropBar);
        cropAngleSlider->setObjectName(QStringLiteral("snowViewCropAngleSlider"));
        cropAngleSlider->setRange(-45, 45);
        cropAngleSlider->setSingleStep(1);
        cropAngleSlider->setPageStep(5);
        cropAngleSlider->setTracking(true);
        cropAngleSlider->setWheelEnabled(true);
        cropAngleSlider->setTooltipEnabled(false);
        cropAngleSlider->setFixedWidth(88);
        cropAngleSlider->setValue(0);
        cropLayout->addWidget(cropAngleSlider);

        cropAngleInput = new AdInputNumber(cropBar);
        cropAngleInput->setObjectName(QStringLiteral("snowViewCropAngleInput"));
        cropAngleInput->setControlSize(AdInputNumber::ControlSize::Small);
        cropAngleInput->setDecimals(0);
        cropAngleInput->setRange(-180, 180);
        cropAngleInput->setSingleStep(1);
        cropAngleInput->setStepButtonLayout(AdInputNumber::StepButtonLayout::Compact);
        cropAngleInput->setSuffixText(QStringLiteral("°"));
        cropAngleInput->setKeyboardTracking(false);
        cropAngleInput->setWheelStepEnabled(true);
        cropAngleInput->setFixedWidth(74);
        cropAngleInput->setValue(0);
        cropLayout->addWidget(cropAngleInput);

        cropSizeLabel = new QLabel(cropBar);
        cropSizeLabel->setObjectName(QStringLiteral("snowViewCropSizeLabel"));
        cropSizeLabel->setContentsMargins(6, 0, 6, 0);
        cropLayout->addWidget(cropSizeLabel);

        cropResetButton = new AdButton(cropBar);
        cropResetButton->setObjectName(QStringLiteral("snowViewCropReset"));
        cropResetButton->setIconRef(adqt::icons::antd::outlined::Undo());
        cropResetButton->setSizeClass(AdButton::SizeClass::Small);
        cropResetButton->setButtonStyle(AdButton::ButtonStyle::Text);
        cropResetButton->setFixedSize(26, 26);
        cropResetButton->setReferenceIconSize({14, 14});
        cropResetButton->setFocusPolicy(Qt::StrongFocus);
        cropLayout->addWidget(cropResetButton);

        cropCancelButton = new AdButton(cropBar);
        cropCancelButton->setObjectName(QStringLiteral("snowViewCropCancel"));
        cropCancelButton->setIconRef(icons::custom::outlined::CancelCrop());
        cropCancelButton->setSizeClass(AdButton::SizeClass::Small);
        cropCancelButton->setButtonStyle(AdButton::ButtonStyle::Text);
        cropCancelButton->setFixedSize(28, 28);
        cropCancelButton->setReferenceIconSize({16, 16});
        cropCancelButton->setFocusPolicy(Qt::StrongFocus);
        cropLayout->addWidget(cropCancelButton);

        cropApplyButton = new AdButton(cropBar);
        cropApplyButton->setObjectName(QStringLiteral("snowViewCropApply"));
        cropApplyButton->setIconRef(icons::custom::outlined::ApplyCrop());
        cropApplyButton->setSizeClass(AdButton::SizeClass::Small);
        cropApplyButton->setButtonStyle(AdButton::ButtonStyle::Solid);
        cropApplyButton->setAccentRole(AdButton::AccentRole::Primary);
        cropApplyButton->setFixedSize(28, 28);
        cropApplyButton->setReferenceIconSize({16, 16});
        cropApplyButton->setFocusPolicy(Qt::StrongFocus);
        cropLayout->addWidget(cropApplyButton);

        cropBar->hide();
        overlay->addWidget(cropBar, 0, Qt::AlignHCenter);
        overlay->addStretch(1);
        floating->setObjectName(QStringLiteral("imageEditingToolbar"));
        floating->installEventFilter(&q);
        modal->setObjectName(QStringLiteral("imageEditingModal"));
        modal->setMode(AdModal::Mode::Window);
        modal->setCentered(true);
        modal->setWindowModeDetached(true);
        modal->setWindowModality(Qt::NonModal);
        modal->setWindowResizable(true);
        modal->setWindowTaskbarVisible(true);
        modal->setWindowMinimizeButtonVisible(true);
        modal->setWindowMaximizeButtonVisible(true);
        modal->setWindowAlwaysOnTopButtonVisible(true);
        modal->setWindowAlwaysOnTop(true);
        modal->setWindowIcon(adqt::icons::makeIcon(icons::custom::app::ApplicationIcon()));
        modal->setFooterVisible(false);
        modal->setCloseOnEscape(false);
        modal->setClosePolicy(AdModal::ClosePolicy::Manual);
        modal->setCloseOnMaskClick(false);
        // SnowView Rev0.1: wrap the preview surface with the folder filmstrip and
        // the metadata bar below it inside one modal content pane.
        contentPane = new QWidget;
        contentPane->setObjectName(QStringLiteral("snowViewContentPane"));
        auto* paneLayout = new QVBoxLayout(contentPane);
        paneLayout->setContentsMargins(0, 0, 0, 0);
        paneLayout->setSpacing(0);
        paneLayout->addWidget(surface, 1);
        filmstrip = new QListWidget(contentPane);
        filmstrip->setObjectName(QStringLiteral("snowViewFilmstrip"));
        filmstrip->setViewMode(QListView::IconMode);
        filmstrip->setFlow(QListView::LeftToRight);
        filmstrip->setWrapping(false);
        filmstrip->setMovement(QListView::Static);
        filmstrip->setResizeMode(QListView::Adjust);
        filmstrip->setUniformItemSizes(true);
        filmstrip->setSpacing(6);
        filmstrip->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
        filmstrip->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        filmstrip->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        filmstrip->setSelectionMode(QAbstractItemView::SingleSelection);
        filmstrip->setIconSize(QSize(80, 56));
        filmstrip->installEventFilter(&q);
        filmstrip->viewport()->installEventFilter(&q);
        updateFilmstripMetrics();
        filmstrip->hide();
        paneLayout->addWidget(filmstrip);
        metadataBar = new QLabel(contentPane);
        metadataBar->setObjectName(QStringLiteral("snowViewMetadataBar"));
        metadataBar->setContentsMargins(10, 4, 10, 4);
        metadataBar->hide();
        paneLayout->addWidget(metadataBar);
        modal->setContentWidget(contentPane);
        QObject::connect(filmstrip, &QListWidget::currentItemChanged, &q,
                         [this](QListWidgetItem* item, QListWidgetItem*) {
                             if (filmstripSyncing || !item)
                                 return;
                             navigateToFile(item->data(Qt::UserRole).toString());
                         });
        QObject::connect(&document, &ImageEditingDocument::baseImageChanged, &q, [this] {
            imageToolbar->setVisible(!document.image().isNull());
            refreshMetadata();
        });
        QObject::connect(modal, &AdModal::closeRequested, &q, [this](auto) { q.requestClose(); });
        QObject::connect(modal, &AdModal::windowInteractionStarted, &q,
                         [this] { beginWindowInteraction(); });
        QObject::connect(modal, &AdModal::windowInteractionFinished, &q,
                         [this] { endWindowInteraction(); });
        QObject::connect(host, &ScreenshotToolPaletteHost::visibleContentChanged, &q, [this] {
            // The shared window preserves a dragged main row as its settings change.
            if (!manuallyPlaced)
                placeToolbar();
        });
        QObject::connect(floating, &ScreenshotFloatingToolPaletteWindow::dpiScaleCommitCompleted,
                         &q, [this] { placeToolbar(); });
        QObject::connect(host, &ScreenshotToolPaletteHost::dragStarted, &q, [this] {
            manuallyPlaced = true;
            draggingToolbar = true;
            rememberToolbarOffset();
        });
        QObject::connect(floating, &ScreenshotFloatingToolPaletteWindow::dragFinished, &q, [this] {
            manuallyPlaced = true;
            draggingToolbar = false;
            rememberToolbarOffset();
        });
        QObject::connect(palette, &ScreenshotToolPalette::cropRowsRequested, &q,
                         [this] { drawing(ImageEditingCanvas::Mode::CropRows); });
        QObject::connect(palette, &ScreenshotToolPalette::cropColumnsRequested, &q,
                         [this] { drawing(ImageEditingCanvas::Mode::CropColumns); });
        QObject::connect(fit, &AdButton::clicked, canvas, [this] { canvas->fit(true); });
        QObject::connect(actual, &AdButton::clicked, canvas, &ImageEditingCanvas::actualSize);
        // SnowView Rev0.1: base-image transform actions.
        QObject::connect(flipHorizontalButton, &AdButton::clicked, &q, [this] { flipImage(true); });
        QObject::connect(flipVerticalButton, &AdButton::clicked, &q, [this] { flipImage(false); });
        QObject::connect(rotateClockwiseButton, &AdButton::clicked, &q,
                         [this] { rotateImage(true); });
        QObject::connect(rotateCounterclockwiseButton, &AdButton::clicked, &q,
                         [this] { rotateImage(false); });
        QObject::connect(cropButton, &AdButton::clicked, &q, [this] { toggleCropMode(); });
        QObject::connect(cropSwapButton, &AdButton::clicked, &q, [this] {
            cropRatioSwapped = !cropRatioSwapped;
            applyCropRatioPreset();
        });
        QObject::connect(cropResetButton, &AdButton::clicked, &q, [this] {
            cropPreset = CropRatioPreset::Free;
            cropRatioSwapped = false;
            canvas->setCropAngleDegrees(0);
            applyCropRatioPreset();
        });
        QObject::connect(cropAngleSlider, &AdSlider::valueChanged, &q,
                         [this](double value) { canvas->setCropAngleDegrees(qRound(value)); });
        QObject::connect(cropAngleInput, &AdInputNumber::valueChanged, &q,
                         [this](double value) { canvas->setCropAngleDegrees(qRound(value)); });
        QObject::connect(cropCancelButton, &AdButton::clicked, &q,
                         [this] { canvas->cancelCropRect(); });
        QObject::connect(cropApplyButton, &AdButton::clicked, &q, [this] { canvas->commitCrop(); });
        QObject::connect(adjustButton, &AdButton::clicked, &q, [this] { showAdjustDialog(); });
        QObject::connect(autoEnhanceButton, &AdButton::clicked, &q, [this] { autoEnhanceImage(); });
        QObject::connect(grayscaleButton, &AdButton::clicked, &q, [this] { grayscaleImage(); });
        canvas->cropRectChanged = [this](const QRect& rect, int angle) {
            updateCropReadout(rect, angle);
        };
        canvas->cropRectCommitted = [this](const QRectF& box, int angle) {
            applyCropRect(box, angle);
        };
        canvas->cropRectCancelled = [this] { drawing(ImageEditingCanvas::Mode::Pan); };
        QObject::connect(palette, &ScreenshotToolPalette::moveRequested, &q,
                         [this] { drawing(ImageEditingCanvas::Mode::Pan); });
        new ScreenshotStyleBinding(*palette, *canvas, &q);
        bindScreenshotCanvasOperations(palette, *canvas, &q);
        bindScreenshotCanvasToolRequests(*palette, q, [this](SnowCanvasTool tool) {
            drawing(ImageEditingCanvas::Mode::Draw);
            canvas->setCanvasTool(tool);
            if (tool == SnowCanvasTool::AutoFilter)
                autoFilter->validate();
        });
        palette->setDrawTemplateCallbacks(
            [this] { return document.runtime().serializeSelectedDrawTemplate(); },
            [this](const QByteArray& data) { canvas->insertDrawTemplate(data, canvas->center()); });
        QObject::connect(palette, &ScreenshotToolPalette::watermarkPreviewChanged, canvas,
                         &SnowCanvasWidget::previewCanvasWatermarkConfig);
        QObject::connect(palette, &ScreenshotToolPalette::spotlightPreviewChanged, canvas,
                         &SnowCanvasWidget::previewCanvasSpotlightConfig);
        QObject::connect(palette, &ScreenshotToolPalette::textStylePopupInteractionBegan, canvas,
                         &SnowCanvasWidget::beginTextStylePopupInteraction);
        QObject::connect(palette, &ScreenshotToolPalette::textStylePopupInteractionEnded, &q,
                         [this] { canvas->endTextStylePopupInteraction(surface); });
        QObject::connect(canvas, &SnowCanvasWidget::activeToolChanged, &q, [this] {
            if (canvas->mode() == ImageEditingCanvas::Mode::Draw)
                if (auto tool = paletteToolForCanvasTool(canvas->canvasTool()))
                    host->setActiveTool(*tool);
        });
        QObject::connect(canvas, &SnowCanvasWidget::styleToolbarStateChanged, &q,
                         [this] { syncPalette(); });
        QObject::connect(canvas, &SnowCanvasWidget::historyStateChanged, &q,
                         [this] { syncPalette(); });
        QObject::connect(&document, &ImageEditingDocument::changed, &q, [this] {
            pruneRecognitionSessions();
            syncPalette();
        });
        QObject::connect(&document, &ImageEditingDocument::baseImageChanged, &q, [this] {
            const QPointer<ImageEditingWindow> lifetime(&q);
            const auto previous = session;
            if (previous) {
                previous->deactivate();
                if (!lifetime)
                    return;
                if (previous)
                    previous->cancelWorkflow();
                if (!lifetime)
                    return;
                if (previous)
                    previous->releaseTargetImage();
                if (session == previous)
                    session = nullptr;
            }
            autoFilter->resetSession();
            if (!lifetime)
                return;
            syncAvailability();
        });
        autoFilter->attachCanvas(canvas);
        QObject::connect(autoFilter, &ScreenshotAutoFilterController::availabilityChanged, palette,
                         &ScreenshotToolPalette::setAutoFilterAvailable);
        QObject::connect(palette, &ScreenshotToolPalette::autoFilterCategoryRequested, autoFilter,
                         &ScreenshotAutoFilterController::fillCategory);
        QObject::connect(autoFilter, &ScreenshotAutoFilterController::detectionFailed, &q,
                         [this](const QString& error) { message(error); });
        canvas->styleWheel = [this](QWheelEvent* event) {
            return handleScreenshotStyleWheel(*palette, *canvas, *event, wheel);
        };
        canvas->historyRequested = [this](bool redo) { undo(redo); };
        canvas->cropRequested = [this](auto axis, int first, int last) { crop(axis, first, last); };
        canvas->viewportChanged = [this] { syncRecognitionGeometry(); };
        QObject::connect(palette, &ScreenshotToolPalette::undoRequested, &q,
                         [this] { undo(false); });
        QObject::connect(palette, &ScreenshotToolPalette::redoRequested, &q,
                         [this] { undo(true); });
        new CanvasHistoryShortcuts(
            shortcuts, &q,
            [this](const auto&) {
                return !document.image().isNull() && !loading && !cropBusy &&
                       !canvas->hasActiveTextEditing() &&
                       !(colorSampling && colorSampling->active());
            },
            [this](const QString& action) {
                undo(action == QStringLiteral("redo"));
                return true;
            });
        bindCanvasDrawingPreferences(document.runtime(), q);
        reloadShortcuts();
        auto& storage = storage::ApplicationStorage::instance();
        if (storage.isInitialized()) {
            QObject::connect(
                &storage.configuration(), &storage::ConfigurationStore::valueChanged, &q,
                [this](const QString& key, const QJsonValue&) {
                    if (key == QStringLiteral("pin_to_screen/action_tools_layout")) {
                        palette->setActionToolsLayout(storage::ScreenshotToolbarSettings().layout(
                            storage::ScreenshotToolbarLayoutKind::PinnedActionTools));
                        placeToolbar();
                    } else if (key == QStringLiteral("screenshot_toolbar/layout")) {
                        palette->setToolbarLayout(storage::ScreenshotToolbarSettings().layout(
                            storage::ScreenshotToolbarLayoutKind::DrawingTools));
                        placeToolbar();
                    } else if (key == QStringLiteral("screenshot_ui/toolbar_size")) {
                        // The floating window commits its scale before this owner callback.
                        // Reanchor after that commit, rather than its intermediate host layout.
                        placeToolbar();
                    } else if (key.startsWith(QStringLiteral("drawing_shortcuts/")) ||
                               key.startsWith(QStringLiteral("screenshot_shortcuts/"))) {
                        reloadShortcuts();
                    }
                });
        }
        QObject::connect(
            palette, &ScreenshotToolPalette::canvasColorSamplingRequested, &q,
            [this](adqt::widgets::AdColorPicker* picker) { beginColorSampling(picker); });
        bindRecognition();
        QObject::connect(palette, &ScreenshotToolPalette::copyRequested, &q,
                         [this] { exportContent(ExportOperation::Copy); });
        QObject::connect(palette, &ScreenshotToolPalette::pinRequested, &q,
                         [this] { exportContent(ExportOperation::Pin); });
        QObject::connect(palette, &ScreenshotToolPalette::saveRequested, &q,
                         [this] { exportContent(ExportOperation::Save); });
        QObject::connect(palette, &ScreenshotToolPalette::quickSaveRequested, &q,
                         [this] { exportContent(ExportOperation::QuickSave); });
        QObject::connect(palette, &ScreenshotToolPalette::printRequested, &q,
                         [this] { exportContent(ExportOperation::Print); });
        QObject::connect(palette, &ScreenshotToolPalette::cloudUploadRequested, &q,
                         [this] { exportContent(ExportOperation::Upload); });
#ifdef Q_OS_MACOS
        QObject::connect(palette, &ScreenshotToolPalette::airDropRequested, &q,
                         [this] { exportContent(ExportOperation::AirDrop); });
#endif
        WindowShortcutManager::Binding paste;
        paste.id = QStringLiteral("imageEditingPaste");
        for (const auto& sequence : QKeySequence::keyBindings(QKeySequence::Paste)) {
            if (sequence.count() == 1)
                paste.keyCombinations.append(sequence[0]);
        }
        paste.priority = WindowShortcutManager::StandardPriority::WindowCommand;
        paste.canActivate = [this](const auto& context) {
            const auto* receiver = qobject_cast<QWidget*>(context.receiver);
            return canPaste() && receiver &&
                   (receiver->window() == editorWindow || receiver->window() == floating) &&
                   !WindowShortcutManager::focusAcceptsTextInput(context.focusWidget) &&
                   !canvas->hasActiveTextEditing();
        };
        paste.activate = [this](const auto&) {
            q.paste();
            return true;
        };
        static_cast<void>(shortcuts.addBinding(&q, std::move(paste)));

        const auto addCropShortcut =
            [this](const char* id, QList<QKeyCombination> keys, Qt::KeyboardModifiers extraMods,
                   std::function<bool()> canRun, std::function<bool()> run) {
                WindowShortcutManager::Binding binding;
                binding.id = QString::fromLatin1(id);
                binding.keyCombinations = std::move(keys);
                binding.allowedAdditionalModifiers = extraMods;
                binding.priority = WindowShortcutManager::StandardPriority::WindowCommand;
                binding.canActivate = [this, canRun = std::move(canRun)](const auto& context) {
                    const auto* receiver = qobject_cast<QWidget*>(context.receiver);
                    return receiver &&
                           (receiver->window() == editorWindow || receiver->window() == floating) &&
                           !WindowShortcutManager::focusAcceptsTextInput(context.focusWidget) &&
                           !canvas->hasActiveTextEditing() && !shortcuts.inputSuspended() &&
                           !confirmation && canRun();
                };
                binding.activate = [run = std::move(run)](const auto&) { return run(); };
                static_cast<void>(shortcuts.addBinding(&q, std::move(binding)));
            };
        addCropShortcut(
            "snowViewCropToggle", {QKeyCombination(Qt::Key_C)}, Qt::NoModifier,
            [this] {
                return !document.image().isNull() && !loading && !cropBusy &&
                       !(colorSampling && colorSampling->active()) && !recognitionVisible;
            },
            [this] {
                toggleCropMode();
                return true;
            });
        addCropShortcut(
            "snowViewCropCommit", {QKeyCombination(Qt::Key_Return), QKeyCombination(Qt::Key_Enter)},
            Qt::KeypadModifier,
            [this] { return canvas->mode() == ImageEditingCanvas::Mode::CropRect; },
            [this] { return canvas->commitCrop(); });
        addCropShortcut(
            "snowViewCropCancel", {QKeyCombination(Qt::Key_Escape)}, Qt::NoModifier,
            [this] { return canvas->mode() == ImageEditingCanvas::Mode::CropRect; },
            [this] {
                canvas->cancelCropRect();
                return true;
            });
        addCropShortcut(
            "snowViewFilmstripPrev", {QKeyCombination(Qt::Key_Left)}, Qt::NoModifier,
            [this] {
                return canvas->mode() != ImageEditingCanvas::Mode::CropRect &&
                       !document.image().isNull() && !loading && !cropBusy && filmstrip &&
                       filmstrip->count() > 1 && !(colorSampling && colorSampling->active());
            },
            [this] {
                stepFilmstrip(-1);
                return true;
            });
        addCropShortcut(
            "snowViewFilmstripNext", {QKeyCombination(Qt::Key_Right)}, Qt::NoModifier,
            [this] {
                return canvas->mode() != ImageEditingCanvas::Mode::CropRect &&
                       !document.image().isNull() && !loading && !cropBusy && filmstrip &&
                       filmstrip->count() > 1 && !(colorSampling && colorSampling->active());
            },
            [this] {
                stepFilmstrip(1);
                return true;
            });
        retranslate();
    }
    bool canPaste() const {
        return modal->isOpen() && !closing && !loading && !choosingFiles && !cropBusy &&
               !exporter.busy() && !confirmation && !shortcuts.inputSuspended() &&
               !(colorSampling && colorSampling->active());
    }
    bool canImport() const {
        return !closing && !loading && !choosingFiles && document.image().isNull();
    }
    void chooseImages() {
        if (!canImport())
            return;
        choosingFiles = true;
        const QPointer<ImageEditingWindow> guard(&q);
        auto complete = [guard](QStringList paths) {
            if (!guard || guard->m_impl->closing)
                return;
            guard->m_impl->choosingFiles = false;
            guard->openFiles(paths);
        };
        if (services.chooseFiles) {
            services.chooseFiles(surface->window(), std::move(complete));
            return;
        }
        auto* dialog = new QFileDialog(surface->window());
        fileDialog = dialog;
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(ImageEditingWindow::tr("Add images"));
        dialog->setFileMode(QFileDialog::ExistingFiles);
        dialog->setNameFilter(imageFileFilter());
        QObject::connect(dialog, &QFileDialog::finished, &q,
                         [this, dialog, complete = std::move(complete)](int result) mutable {
                             fileDialog.clear();
                             complete(result == QDialog::Accepted ? dialog->selectedFiles()
                                                                  : QStringList{});
                         });
        dialog->open();
    }
    ~Impl() {
        filmstripQueue.cancel();
        if (editorWindow)
            editorWindow->removeEventFilter(&q);
        floating->removeEventFilter(&q);
        surface->removeEventFilter(&q);
        canvas->removeEventFilter(&q);
        if (filmstrip) {
            filmstrip->removeEventFilter(&q);
            if (filmstrip->viewport())
                filmstrip->viewport()->removeEventFilter(&q);
        }
        for (const auto& state : sessions)
            if (state.controller)
                delete state.controller;
        delete autoFilter;
        QObject::disconnect(canvas, nullptr, &q, nullptr);
        floating->setTransientOwnerWindow(nullptr);
        delete floating;
        QObject::disconnect(modal, nullptr, &q, nullptr);
        delete modal;
    }
    void reloadShortcuts() {
        for (const auto handle : commandBindings)
            static_cast<void>(shortcuts.removeBinding(handle));
        commandBindings.clear();
        const auto registerBindings = [this](const auto& settings, bool drawing) {
            for (auto it = settings.cbegin(); it != settings.cend(); ++it) {
                if (!drawing &&
                    !QStringList{
                        QStringLiteral("table_recognition"), QStringLiteral("qr_code_recognition"),
                        QStringLiteral("text_recognition"), QStringLiteral("text_recognition_copy"),
                        QStringLiteral("text_translation"), QStringLiteral("copy_to_clipboard"),
                        QStringLiteral("save_as_file"), QStringLiteral("quick_save"),
                        QStringLiteral("print"), QStringLiteral("upload_to_cloud"),
                        QStringLiteral("move_tool"), QStringLiteral("pin_to_screen")}
                         .contains(it.key()))
                    continue;
                WindowShortcutManager::Binding binding;
                binding.id = (drawing ? QStringLiteral("image-editing.drawing.")
                                      : QStringLiteral("image-editing.action.")) +
                             it.key();
                binding.shortcutBindings = it.value();
                binding.priority =
                    drawing ? WindowShortcutManager::StandardPriority::DrawingShortcut
                            : WindowShortcutManager::StandardPriority::ScreenshotShortcut;
                binding.canActivate = [this, id = it.key()](const auto& context) {
                    if (document.image().isNull() || loading || cropBusy || closing ||
                        (colorSampling && colorSampling->active()) ||
                        WindowShortcutManager::focusAcceptsTextInput(context.focusWidget) ||
                        canvas->hasActiveTextEditing())
                        return false;
                    if (id == QStringLiteral("text_recognition_copy"))
                        return palette->canActivateScreenshotShortcut(
                            QStringLiteral("text_recognition"));
                    if (id != snow_shot::shortcuts::kCycleAnnotationToolOptions)
                        return true;
                    return !WindowShortcutManager::inputTargetsPopup(context) &&
                           palette->canActivateDrawingShortcut(id);
                };
                binding.activate = [this, id = it.key(), drawing](const auto&) {
                    if (id == QStringLiteral("text_recognition_copy")) {
                        activate(Session::Mode::Text, false,
                                 ScreenshotClipboardService::reservePublication(
                                     QApplication::clipboard()));
                        return true;
                    }
                    return drawing ? palette->activateDrawingShortcut(id)
                                   : palette->activateScreenshotShortcut(id);
                };
                commandBindings.append(shortcuts.addBinding(&q, std::move(binding)));
            }
        };
        registerBindings(storage::DrawingShortcutSettings().allShortcuts(), true);
        registerBindings(storage::ScreenshotShortcutSettings().allShortcuts(), false);
    }
    void cancelColorSampling() {
        ++colorGeneration;
        if (colorArtifact)
            colorArtifact->cancel();
        colorArtifact.reset();
        colorImage = {};
        if (colorSampling)
            colorSampling->end();
        canvas->setCursor(canvas->mode() == ImageEditingCanvas::Mode::Pan ? Qt::OpenHandCursor
                                                                          : Qt::CrossCursor);
    }
    void beginColorSampling(adqt::widgets::AdColorPicker* picker) {
        if (!picker || loading || cropBusy)
            return;
        cancelColorSampling();
        if (!colorSampling)
            colorSampling = std::make_unique<ScreenshotCanvasColorSamplingSession>(q);
        colorSampling->begin(picker, [this] { cancelColorSampling(); });
        canvas->setCursor(ScreenshotCanvasColorSamplerWindow::samplingCursor());
        canvas->setFocus();
        const auto source = document.exportSource();
        if (!source.isValid()) {
            cancelColorSampling();
            return;
        }
        colorArtifact = std::make_shared<ScreenshotExportArtifact>(source);
        const auto generation = colorGeneration;
        if (!colorArtifact->requestImage(&q, [this, generation](auto result) {
                if (generation != colorGeneration || closing)
                    return;
                colorImage = std::move(result.image);
                if (colorImage.isNull())
                    cancelColorSampling();
            }))
            cancelColorSampling();
    }
    bool colorEvent(QEvent* event) {
        if (!colorSampling || !colorSampling->active())
            return false;
        if (event->type() == QEvent::KeyPress) {
            if (static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape)
                cancelColorSampling();
            return true;
        }
        if (event->type() != QEvent::MouseMove && event->type() != QEvent::MouseButtonPress &&
            event->type() != QEvent::MouseButtonRelease)
            return event->type() == QEvent::Wheel || event->type() == QEvent::TabletPress ||
                   event->type() == QEvent::TabletMove || event->type() == QEvent::TabletRelease;
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::RightButton) {
            cancelColorSampling();
            return true;
        }
        const QPointF point = document.imagePixelAt(
            canvas->canvasToViewTransform().inverted().map(mouse->position()));
        const auto preview = ScreenshotCanvasColorSampler::previewFromPhysicalRaster(
            colorImage, colorImage.rect(), QPoint(qFloor(point.x()), qFloor(point.y())));
        if (!preview.isNull())
            colorSampling->updatePreview(preview, mouse->globalPosition().toPoint());
        else
            colorSampling->hidePreview();
        if (event->type() == QEvent::MouseButtonRelease && mouse->button() == Qt::LeftButton &&
            !preview.isNull())
            static_cast<void>(colorSampling->commit(preview, [this] { cancelColorSampling(); }));
        return true;
    }
    void showNotice(Notice value) {
        switch (value) {
        case Notice::Loading:
            message(ImageEditingWindow::tr("Loading image…"), AdMessage::Type::Loading);
            break;
        case Notice::LoadError:
            message(ImageEditingWindow::tr("The image could not be loaded"));
            break;
        case Notice::Exporting:
            message(ImageEditingWindow::tr("Exporting…"), AdMessage::Type::Loading);
            break;
        case Notice::Exported:
            message(ImageEditingWindow::tr("Export completed"), AdMessage::Type::Success);
            break;
        case Notice::None:
            message({});
            break;
        }
        notice = value;
    }
    void retranslate() {
        modal->setWindowTitle(QCoreApplication::translate("ImageEditingWindow", "Snow Shot"));
        floating->setWindowTitle(modal->windowTitle());
        fit->setToolTip(ImageEditingWindow::tr("Fit image to window"));
        fit->setAccessibleName(fit->toolTip());
        actual->setToolTip(ImageEditingWindow::tr("View at actual size (100%)"));
        actual->setAccessibleName(actual->toolTip());
        flipHorizontalButton->setToolTip(
            QCoreApplication::translate("ImageEditingWindow", "Flip horizontally"));
        flipHorizontalButton->setAccessibleName(flipHorizontalButton->toolTip());
        flipVerticalButton->setToolTip(
            QCoreApplication::translate("ImageEditingWindow", "Flip vertically"));
        flipVerticalButton->setAccessibleName(flipVerticalButton->toolTip());
        rotateClockwiseButton->setToolTip(
            QCoreApplication::translate("ImageEditingWindow", "Rotate clockwise"));
        rotateClockwiseButton->setAccessibleName(rotateClockwiseButton->toolTip());
        rotateCounterclockwiseButton->setToolTip(
            QCoreApplication::translate("ImageEditingWindow", "Rotate counterclockwise"));
        rotateCounterclockwiseButton->setAccessibleName(rotateCounterclockwiseButton->toolTip());
        cropButton->setToolTip(QCoreApplication::translate("ImageEditingWindow", "Crop image (C)"));
        cropButton->setAccessibleName(cropButton->toolTip());
        adjustButton->setToolTip(QCoreApplication::translate(
            "ImageEditingWindow", "Adjust brightness, contrast, saturation and sharpness"));
        adjustButton->setAccessibleName(adjustButton->toolTip());
        autoEnhanceButton->setToolTip(
            QCoreApplication::translate("ImageEditingWindow", "Auto enhance"));
        autoEnhanceButton->setAccessibleName(autoEnhanceButton->toolTip());
        grayscaleButton->setToolTip(
            QCoreApplication::translate("ImageEditingWindow", "Convert to grayscale"));
        grayscaleButton->setAccessibleName(grayscaleButton->toolTip());
        if (cropPresetButtons[0]) {
            cropPresetButtons[0]->setText(
                QCoreApplication::translate("ImageEditingWindow", "Free"));
            cropPresetButtons[1]->setText(
                QCoreApplication::translate("ImageEditingWindow", "Original"));
            cropPresetButtons[2]->setText(QStringLiteral("1:1"));
            cropPresetButtons[3]->setText(QStringLiteral("4:3"));
            cropPresetButtons[4]->setText(QStringLiteral("16:9"));
            cropPresetButtons[5]->setText(QStringLiteral("3:2"));
            cropSwapButton->setToolTip(
                QCoreApplication::translate("ImageEditingWindow", "Swap width and height ratio"));
            cropSwapButton->setAccessibleName(cropSwapButton->toolTip());
            cropAngleSlider->setToolTip(QCoreApplication::translate(
                "ImageEditingWindow", "Straighten or rotate crop frame (-45° to +45°, 1° step)"));
            cropAngleSlider->setAccessibleName(cropAngleSlider->toolTip());
            cropAngleInput->setToolTip(QCoreApplication::translate(
                "ImageEditingWindow", "Crop rotation angle (-180° to +180°, 1° step)"));
            cropAngleInput->setAccessibleName(cropAngleInput->toolTip());
            cropResetButton->setToolTip(
                QCoreApplication::translate("ImageEditingWindow", "Reset crop box"));
            cropResetButton->setAccessibleName(cropResetButton->toolTip());
            cropCancelButton->setToolTip(
                QCoreApplication::translate("ImageEditingWindow", "Cancel crop (Esc)"));
            cropCancelButton->setAccessibleName(cropCancelButton->toolTip());
            cropApplyButton->setToolTip(
                QCoreApplication::translate("ImageEditingWindow", "Apply crop (Enter)"));
            cropApplyButton->setAccessibleName(cropApplyButton->toolTip());
        }
        emptyState->title->setText(ImageEditingWindow::tr("Add images to start image editing"));
        emptyState->description->setText(
            ImageEditingWindow::tr("Drag image files here, or choose add images"));
        emptyState->setAccessibleName(emptyState->title->text());
        emptyState->setAccessibleDescription(emptyState->description->text());
        addImages->setText(ImageEditingWindow::tr("Add images"));
        addImages->setAccessibleName(addImages->text());
        dropHint->title->setText(ImageEditingWindow::tr("Drop images to edit"));
        dropHint->description->setText(ImageEditingWindow::tr("Open each image in its own window"));
        dropHint->setAccessibleName(dropHint->title->text());
        dropHint->setAccessibleDescription(dropHint->description->text());
        if (fileDialog) {
            fileDialog->setWindowTitle(ImageEditingWindow::tr("Add images"));
            fileDialog->setNameFilter(imageFileFilter());
        }
        if (notice != Notice::None)
            showNotice(notice);
        else if (errorSource)
            sourceError(errorSource);
        if (confirmation)
            translateConfirmation();
        placeToolbar();
    }
    void message(QString text, AdMessage::Type type = AdMessage::Type::Error) {
        notice = Notice::None;
        errorSource = nullptr;
        if (text.isEmpty()) {
            if (statusMessage)
                statusMessage->close();
            statusMessage.clear();
            return;
        }
        AdMessage::Request request;
        request.key = QStringLiteral("imageEditingStatus");
        request.type = type;
        request.content = std::move(text);
        if (type == AdMessage::Type::Loading)
            request.durationMs = 0;
        statusMessage = AdMessageService::instance(surface->window())->open(std::move(request));
    }
    void sourceError(const char* source) {
        message(QCoreApplication::translate("ImageEditingWindow", source));
        errorSource = source;
    }
    void cancelLoading() {
        loadJob.cancel();
        if (loadArtifact)
            loadArtifact->cancel();
        loadArtifact.reset();
    }
    void setLoading(bool value) {
        loading = value;
        dropHint->hide();
        surface->setEnabled(!loading);
        floating->setEnabled(!loading && !cropBusy);
        // Keep the preview in the modal layout so loading never changes its geometry.
        if (loading) {
            show();
            showNotice(Notice::Loading);
        } else {
            showNotice(Notice::None);
        }
    }
    void beginWindowInteraction() {
        if (closing || windowInteractionActive)
            return;
        windowInteractionActive = true;
        cancelColorSampling();
        floating->cancelDrag();
        for (auto* picker : palette->findChildren<adqt::widgets::AdColorPicker*>())
            picker->setPopupVisible(false);
        for (auto* select : palette->findChildren<adqt::widgets::AdSelect*>())
            select->setPopupVisible(false);
        floating->hide();
        for (auto* child : floating->findChildren<QWidget*>())
            if (child->isWindow())
                child->hide();
    }
    void endWindowInteraction() {
        if (!windowInteractionActive)
            return;
        windowInteractionActive = false;
        // Match pin windows: settle at the default anchor after a move or resize.
        manuallyPlaced = false;
        placeToolbar();
    }
    void placeToolbar() {
        if (!editorWindow || closing || placingToolbar || windowInteractionActive)
            return;
        const QScopedValueRollback<bool> placing(placingToolbar, true);
        if (!editorWindow->isVisible() || editorWindow->isMinimized() ||
            document.image().isNull()) {
            floating->cancelDrag();
            floating->hide();
            return;
        }
        if (draggingToolbar)
            return;
        auto* screen = editorWindow->screen();
        const auto frame = editorWindow->frameGeometry();
        const auto bounds = screen ? screen->geometry() : frame;
        floating->setPlacementContext(screen, bounds,
                                      screen ? pinnedScreenGeometry(*screen) : frame);
        floating->prepareForDisplay();
        const auto geometry = floating->placementSnapshot();
        if (!geometry.bottom.isValid())
            return;
        if (manuallyPlaced) {
            // Keep the user's main row fixed when secondary rows grow or change direction.
            const auto position =
                frame.topLeft() + toolbarOffset - host->mainToolbarContentRect().topLeft();
            floating->moveContentTo(floating->constrainedContentPosition(position));
        } else {
            // SnowView Rev0.2: default floating annotation toolbar above the filmstrip/metadata
            // area at the bottom-center of the preview surface, keeping it draggable.
            const QRect anchorSurface =
                (surface && !surface->size().isEmpty())
                    ? QRect(surface->mapToGlobal(QPoint(0, 0)), surface->size())
                    : frame;
            const auto& topLayout =
                geometry.top.mainToolbarContentRect.isEmpty() ? geometry.bottom : geometry.top;
            const auto& topMain = topLayout.mainToolbarContentRect;
            const QRect topOccupied =
                topLayout.occupiedContentRect.isEmpty() ? topMain : topLayout.occupiedContentRect;
            constexpr int kSurfaceBottomInset = 12;
            const QPoint topDesired(
                anchorSurface.left() + (anchorSurface.width() - topMain.width()) / 2 -
                    topMain.left(),
                anchorSurface.bottom() - kSurfaceBottomInset - topMain.bottom());
            const bool fitsTop = topOccupied.translated(topDesired).top() >= bounds.top();
            const bool useAbove = fitsTop || geometry.bottom.mainToolbarContentRect.isEmpty();
            floating->setStyleToolbarAboveMain(useAbove);
            floating->resetPhysicalSizeInvariant();
            const auto& chosen = useAbove ? topLayout : geometry.bottom;
            const auto& chosenMain = chosen.mainToolbarContentRect;
            const QPoint basePos(
                anchorSurface.left() + (anchorSurface.width() - chosenMain.width()) / 2 -
                    chosenMain.left(),
                anchorSurface.bottom() - kSurfaceBottomInset - chosenMain.bottom());
            floating->moveContentTo(floating->constrainedContentPosition(basePos));
        }
        floating->show();
    }
    void rememberToolbarOffset() {
        if (editorWindow)
            toolbarOffset = floating->contentPosition() + host->mainToolbarContentRect().topLeft() -
                            editorWindow->frameGeometry().topLeft();
    }
    void show() {
        if (!modal->isOpen()) {
            auto* screen = QGuiApplication::screenAt(QCursor::pos());
            if (!screen)
                screen = QGuiApplication::primaryScreen();
            if (screen) {
                modal->setWindowScreen(screen);
                const auto available = screen->availableGeometry().size() - QSize(40, 40);
                modal->setWindowPreferredSize(QSize(1200, 800).boundedTo(available));
                modal->setWindowMinimumSize(QSize(900, 600).boundedTo(available));
            }
        }
        modal->present();
        if (editorWindow != surface->window()) {
            if (editorWindow)
                editorWindow->removeEventFilter(&q);
            editorWindow = surface->window();
            // Keep the toolbar native surface independent of the modal's content tree.
            // Reparenting that tree can destroy child HWNDs without hiding their QWidgets.
            floating->setTransientOwnerWindow(editorWindow);
            editorWindow->installEventFilter(&q);
            shortcuts.addScopeWindow(editorWindow);
            shortcuts.addScopeWindow(floating);
        }
        floating->prepareForDisplay();
        placeToolbar();
    }
    void syncAvailability() {
        const auto providers =
            services.providers ? services.providers() : ScreenshotPinnedRecognitionProviders{};
        const auto cached = session                     ? session->cachedRecognitionResults()
                            : document.baseState() == 0 ? initialRecognition
                                                        : ScreenshotRecognitionResults{};
        const bool sized = screenshotOcrImageWithinPixelLimit(document.image().size());
        const auto textSession = sessions.value(document.baseState()).controller;
        palette->setOcrEnabled((sized && providers.recognition) || cached.text.has_value() ||
                               (textSession && textSession->hasTextResult()));
        palette->setTableEnabled((sized && providers.tableRecognition) ||
                                 !cached.tableEntries.isEmpty() || cached.table.has_value());
        palette->setQrEnabled((sized && providers.qrRecognition) || cached.qr.has_value());
        palette->setLatexState((sized && providers.tableRecognition) ||
                                   !cached.latexEntries.isEmpty() || cached.latex.has_value(),
                               session && session->busy(Session::Mode::Latex));
        palette->setImageConversionEnabled((sized && providers.tableRecognition) ||
                                           !cached.conversions.isEmpty());
    }
    void syncPalette() {
        if (session && session->active())
            return;
        palette->setHistoryState({document.runtime().canUndo(), document.runtime().canRedo()});
        palette->setStyleToolbarState(canvas->canvasStyleToolbarState());
        palette->setWatermarkConfig(canvas->canvasWatermarkConfig());
        palette->setSpotlightConfig(canvas->canvasSpotlightConfig());
    }
    void drawing(ImageEditingCanvas::Mode mode) {
        if (loading || cropBusy || document.image().isNull())
            return;
        cancelColorSampling();
        if (session)
            session->deactivate();
        translateWhenReady = false;
        stack->setCurrentWidget(canvas);
        static_cast<void>(canvas->resetEditingStatePreservingTool());
        if (mode == ImageEditingCanvas::Mode::CropRect) {
            cropPreset = CropRatioPreset::Free;
            cropRatioSwapped = false;
            canvas->setCropAspectRatio(std::nullopt);
        }
        canvas->setMode(mode);
        const bool cropRectActive = canvas->mode() == ImageEditingCanvas::Mode::CropRect;
        syncCropUiState(cropRectActive);
        palette->setCropToolsState(mode == ImageEditingCanvas::Mode::CropRows,
                                   mode == ImageEditingCanvas::Mode::CropColumns);
        fit->setEnabled(true);
        actual->setEnabled(true);
        if (mode == ImageEditingCanvas::Mode::Pan)
            host->setActiveTool(Tool::Move);
        else if (mode != ImageEditingCanvas::Mode::Draw)
            host->clearActiveTool();
        canvas->setFocus();
        syncAvailability();
        syncPalette();
        placeToolbar();
    }
    void syncCropUiState(bool active) {
        if (cropButton) {
            cropButton->setChecked(active);
            cropButton->setButtonStyle(active ? AdButton::ButtonStyle::Solid
                                              : AdButton::ButtonStyle::Text);
            cropButton->setAccentRole(active ? AdButton::AccentRole::Primary
                                             : AdButton::AccentRole::Neutral);
        }
        if (cropBar) {
            cropBar->setVisible(active);
            if (active) {
                cropBar->raise();
                updateCropReadout(canvas->cropPixelRect(), canvas->cropAngleDegrees());
            }
        }
        for (std::size_t i = 0; i < cropPresetButtons.size(); ++i) {
            if (!cropPresetButtons[i])
                continue;
            const bool selected = active && static_cast<CropRatioPreset>(i) == cropPreset;
            cropPresetButtons[i]->setChecked(selected);
            cropPresetButtons[i]->setButtonStyle(selected ? AdButton::ButtonStyle::Solid
                                                          : AdButton::ButtonStyle::Text);
            cropPresetButtons[i]->setAccentRole(selected ? AdButton::AccentRole::Primary
                                                         : AdButton::AccentRole::Neutral);
        }
        if (cropSwapButton)
            cropSwapButton->setEnabled(active && cropPreset != CropRatioPreset::Free &&
                                       cropPreset != CropRatioPreset::Square);
    }
    void toggleCropMode() {
        if (loading || cropBusy || document.image().isNull())
            return;
        if (canvas->mode() == ImageEditingCanvas::Mode::CropRect) {
            drawing(ImageEditingCanvas::Mode::Pan);
            return;
        }
        if (baseImageForTransform().isNull())
            return;
        drawing(ImageEditingCanvas::Mode::CropRect);
    }
    void setCropRatioPreset(CropRatioPreset preset) {
        if (preset == cropPreset && preset != CropRatioPreset::Free)
            cropRatioSwapped = !cropRatioSwapped;
        else
            cropRatioSwapped = false;
        cropPreset = preset;
        applyCropRatioPreset();
    }
    void applyCropRatioPreset() {
        std::optional<double> ratio;
        const QSize imgSize = document.image().size();
        switch (cropPreset) {
        case CropRatioPreset::Free:
            ratio = std::nullopt;
            break;
        case CropRatioPreset::Original:
            if (imgSize.height() > 0)
                ratio = static_cast<double>(imgSize.width()) / imgSize.height();
            break;
        case CropRatioPreset::Square:
            ratio = 1.0;
            break;
        case CropRatioPreset::Ratio4x3:
            ratio = 4.0 / 3.0;
            break;
        case CropRatioPreset::Ratio16x9:
            ratio = 16.0 / 9.0;
            break;
        case CropRatioPreset::Ratio3x2:
            ratio = 3.0 / 2.0;
            break;
        }
        if (ratio && *ratio > 0.0 && cropRatioSwapped && cropPreset != CropRatioPreset::Square)
            ratio = 1.0 / *ratio;
        canvas->setCropAspectRatio(ratio);
        syncCropUiState(canvas->mode() == ImageEditingCanvas::Mode::CropRect);
        canvas->setFocus();
    }
    void updateCropReadout(const QRect& rect, int angleDegrees) {
        if (cropSizeLabel)
            cropSizeLabel->setText(
                QStringLiteral("%1 × %2 px").arg(rect.width()).arg(rect.height()));
        if (cropAngleInput) {
            const QSignalBlocker blocker(cropAngleInput);
            if (qRound(cropAngleInput->value()) != angleDegrees)
                cropAngleInput->setValue(angleDegrees);
        }
        if (cropAngleSlider) {
            const QSignalBlocker blocker(cropAngleSlider);
            const int clamped = std::clamp(angleDegrees, -45, 45);
            if (qRound(cropAngleSlider->value()) != clamped)
                cropAngleSlider->setValue(clamped);
        }
    }
    void applyCropRect(const QRectF& pixelBox, int angleDegrees) {
        if (cropBusy || loading || closing)
            return;
        const QImage base = baseImageForTransform();
        drawing(ImageEditingCanvas::Mode::Pan);
        if (base.isNull() || !pixelBox.isValid() || pixelBox.isEmpty())
            return;
        if (angleDegrees == 0) {
            const QRect rounded(qRound(pixelBox.left()), qRound(pixelBox.top()),
                                std::max(1, qRound(pixelBox.width())),
                                std::max(1, qRound(pixelBox.height())));
            const QRect bounded = rounded.intersected(base.rect());
            if (bounded.isEmpty() || bounded == base.rect())
                return;
            const QImage cropped = base.copy(bounded);
            if (!cropped.isNull()) {
                document.applyBaseImage(cropped);
                canvas->fit();
            }
            return;
        }
        const int outW = std::max(1, qRound(pixelBox.width()));
        const int outH = std::max(1, qRound(pixelBox.height()));
        const QImage::Format targetFormat =
            base.hasAlphaChannel() ? QImage::Format_ARGB32_Premultiplied : QImage::Format_RGB32;
        QImage cropped(outW, outH, targetFormat);
        cropped.setDevicePixelRatio(base.devicePixelRatio());
        cropped.setColorSpace(base.colorSpace());
        cropped.fill(base.hasAlphaChannel() ? Qt::transparent : Qt::black);
        {
            QPainter painter(&cropped);
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            painter.translate(outW * 0.5, outH * 0.5);
            painter.rotate(-static_cast<qreal>(angleDegrees));
            painter.translate(-pixelBox.center());
            painter.drawImage(QPointF(0.0, 0.0), base);
        }
        if (base.format() != targetFormat && base.format() != QImage::Format_Invalid &&
            base.format() != QImage::Format_Indexed8 && base.format() != QImage::Format_Mono &&
            base.format() != QImage::Format_MonoLSB) {
            cropped = cropped.convertToFormat(base.format());
        }
        if (!cropped.isNull()) {
            document.applyBaseImage(cropped);
            canvas->fit();
        }
    }
    void setCropBusy(bool busy) {
        cropBusy = busy;
        canvas->setEnabled(!busy);
        floating->setEnabled(!busy && !loading);
    }
    void crop(ImageEditingDocument::CropAxis axis, int first, int last) {
        if (cropBusy || loading || closing)
            return;
        auto result = std::make_shared<ImageEditingDocument::Crop>();
        setCropBusy(true);
        cropJob = ScreenshotExportCoordinator::shared().submit(
            &q, ScreenshotExportCoordinator::Priority::Foreground,
            [image = document.image(), axis, first, last, result](const auto& cancel) {
                if (!cancel.isCancellationRequested())
                    *result =
                        ImageEditingDocument::prepareCrop(image, axis, first, last, [&cancel] {
                            return cancel.isCancellationRequested();
                        });
                return ScreenshotExportTaskResult{};
            },
            [this, result](auto) {
                setCropBusy(false);
                if (!closing)
                    static_cast<void>(document.applyCrop(std::move(*result)));
            });
        if (!cropJob.isValid()) {
            setCropBusy(false);
        }
    }
    // SnowView Rev0.1: keep filmstrip selection synced with currentFilePath without re-triggering
    // navigation.
    void syncFilmstripSelection() {
        if (!filmstrip)
            return;
        filmstripSyncing = true;
        int matchRow = -1;
        if (!currentFilePath.isEmpty()) {
            const QString target = QFileInfo(currentFilePath).absoluteFilePath();
            for (int row = 0; row < filmstrip->count(); ++row) {
                if (QString::compare(filmstrip->item(row)->data(Qt::UserRole).toString(), target,
                                     Qt::CaseInsensitive) == 0) {
                    matchRow = row;
                    break;
                }
            }
        }
        if (matchRow >= 0)
            filmstrip->setCurrentRow(matchRow);
        filmstripSyncing = false;
    }
    void updateFilmstripMetrics() {
        if (!filmstrip)
            return;
        const QSize thumbSize = filmstrip->iconSize();
        const int textHeight = filmstrip->fontMetrics().height();
        const QSize itemSize(std::max(94, thumbSize.width() + 14),
                             thumbSize.height() + textHeight + 18);
        for (int row = 0; row < filmstrip->count(); ++row) {
            if (auto* item = filmstrip->item(row))
                item->setSizeHint(itemSize);
        }
        const int hbarHeight = filmstrip->horizontalScrollBar()
                                   ? filmstrip->horizontalScrollBar()->sizeHint().height()
                                   : 0;
        const int totalHeight = itemSize.height() + hbarHeight + filmstrip->frameWidth() * 2 + 12;
        if (filmstrip->height() != totalHeight)
            filmstrip->setFixedHeight(totalHeight);
    }
    void stepFilmstrip(int delta) {
        if (!filmstrip || filmstrip->count() <= 1)
            return;
        const int current = filmstrip->currentRow();
        const int next =
            std::clamp((current >= 0 ? current : 0) + delta, 0, filmstrip->count() - 1);
        if (next != current)
            if (auto* item = filmstrip->item(next))
                navigateToFile(item->data(Qt::UserRole).toString());
    }
    // SnowView Rev0.2: filmstrip of the current file's folder, asynchronously decoded with
    // snow_image so static Release builds render JPEG/WebP/HEIC/AVIF/JXL previews smoothly.
    void refreshFilmstrip() {
        if (!filmstrip)
            return;
        if (currentFilePath.isEmpty()) {
            filmstripQueue.cancel();
            ++filmstripGeneration;
            filmstripSyncing = true;
            filmstrip->clear();
            filmstripDirPath.clear();
            filmstrip->hide();
            metadataBar->hide();
            filmstripSyncing = false;
            return;
        }
        const QFileInfo current(currentFilePath);
        const QString dirPath = current.absolutePath();
        if (!filmstripDirPath.isEmpty() &&
            QString::compare(filmstripDirPath, dirPath, Qt::CaseInsensitive) == 0 &&
            filmstrip->count() > 0) {
            syncFilmstripSelection();
            filmstrip->setVisible(filmstrip->count() > 1);
            refreshMetadata();
            return;
        }
        filmstripQueue.cancel();
        const quint64 generation = ++filmstripGeneration;
        filmstripSyncing = true;
        filmstrip->clear();
        filmstripDirPath = dirPath;
        const QDir dir(dirPath);
        const QStringList filters{
            QStringLiteral("*.png"),  QStringLiteral("*.jpg"),  QStringLiteral("*.jpeg"),
            QStringLiteral("*.bmp"),  QStringLiteral("*.gif"),  QStringLiteral("*.webp"),
            QStringLiteral("*.tif"),  QStringLiteral("*.tiff"), QStringLiteral("*.avif"),
            QStringLiteral("*.heic"), QStringLiteral("*.heif"), QStringLiteral("*.jxl")};
        const QFileInfoList entries =
            dir.entryInfoList(filters, QDir::Files | QDir::Readable, QDir::Name);
        updateFilmstripMetrics();
        const QSize thumbSize = filmstrip->iconSize();
        const int textHeight = filmstrip->fontMetrics().height();
        const QSize itemSize(std::max(94, thumbSize.width() + 14),
                             thumbSize.height() + textHeight + 18);
        QStringList queuedPaths;
        queuedPaths.reserve(std::min<qsizetype>(entries.size(), 500));
        int currentRow = -1;
        int row = 0;
        for (const auto& entry : entries) {
            if (row >= 500)
                break;
            auto* item = new QListWidgetItem(filmstrip);
            const QString filePath = entry.absoluteFilePath();
            item->setData(Qt::UserRole, filePath);
            item->setToolTip(entry.fileName());
            item->setText(entry.fileName());
            item->setSizeHint(itemSize);
            queuedPaths.append(filePath);
            if (QString::compare(filePath, current.absoluteFilePath(), Qt::CaseInsensitive) == 0)
                currentRow = row;
            ++row;
        }
        if (currentRow >= 0)
            filmstrip->setCurrentRow(currentRow);
        filmstripSyncing = false;
        filmstrip->setVisible(entries.size() > 1);
        refreshMetadata();

        if (queuedPaths.size() <= 1)
            return;

        // Decode visible neighborhood around currentRow outward first.
        QVector<int> order;
        order.reserve(queuedPaths.size());
        const int anchor = currentRow >= 0 ? currentRow : 0;
        order.append(anchor);
        for (int offset = 1; offset < queuedPaths.size(); ++offset) {
            if (anchor + offset < queuedPaths.size())
                order.append(anchor + offset);
            if (anchor - offset >= 0)
                order.append(anchor - offset);
        }
        const qreal dpr = std::max<qreal>(1.0, filmstrip->devicePixelRatioF());
        const QSize pixelTarget(std::max(1, qRound(thumbSize.width() * dpr)),
                                std::max(1, qRound(thumbSize.height() * dpr)));
        for (const int idx : order) {
            const QString path = queuedPaths.at(idx);
            auto decoded = std::make_shared<QImage>();
            static_cast<void>(filmstripQueue.enqueue(
                ScreenshotExportCoordinator::Priority::Background,
                [path, pixelTarget, decoded](const auto& cancel) {
                    *decoded = snowViewDecodeThumbnail(
                        path, pixelTarget, [&cancel] { return cancel.isCancellationRequested(); });
                    return ScreenshotExportTaskResult{};
                },
                [this, generation, idx, path, dpr, decoded](auto) {
                    if (closing || generation != filmstripGeneration || !filmstrip || idx < 0 ||
                        idx >= filmstrip->count() || decoded->isNull())
                        return;
                    auto* targetItem = filmstrip->item(idx);
                    if (!targetItem || QString::compare(targetItem->data(Qt::UserRole).toString(),
                                                        path, Qt::CaseInsensitive) != 0)
                        return;
                    QPixmap pixmap = QPixmap::fromImage(*decoded);
                    pixmap.setDevicePixelRatio(dpr);
                    targetItem->setIcon(pixmap);
                }));
        }
    }
    // SnowView Rev0.1: one-line metadata for the current file under the filmstrip.
    void refreshMetadata() {
        if (!metadataBar)
            return;
        if (currentFilePath.isEmpty() || document.image().isNull()) {
            metadataBar->hide();
            return;
        }
        const QFileInfo info(currentFilePath);
        const QSize size = document.image().size();
        QString text =
            QStringLiteral("%1  •  %2 × %3 px  •  %4  •  %5")
                .arg(info.fileName(), QString::number(size.width()), QString::number(size.height()),
                     QLocale().formattedDataSize(info.size()),
                     info.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        if (filmstrip && filmstrip->count() > 1 && filmstrip->currentRow() >= 0)
            text += QStringLiteral("  •  ") +
                    QCoreApplication::translate("ImageEditingWindow", "%1 of %2")
                        .arg(filmstrip->currentRow() + 1)
                        .arg(filmstrip->count());
        metadataBar->setText(text);
        metadataBar->show();
    }
    void navigateToFile(const QString& path) {
        if (path.isEmpty() || QString::compare(path, currentFilePath, Qt::CaseInsensitive) == 0 ||
            closing)
            return;
        q.confirmDiscard([this, path](bool accepted) {
            if (closing)
                return;
            if (accepted)
                q.presentFile(path);
            else
                syncFilmstripSelection();
        });
    }
    // SnowView Rev0.1: transforms act on the base raster. With annotations present
    // there is no scene-wide transform in the canvas runtime, so ask the user to
    // remove them (or work on an exported copy) instead of silently misaligning them.
    QImage baseImageForTransform() {
        if (document.image().isNull() || loading || cropBusy)
            return {};
        if (document.runtime().hasDocumentContent()) {
            message(QCoreApplication::translate(
                "ImageEditingWindow",
                "Remove the annotations before transforming the image, or export the "
                "flattened image and open it again"));
            return {};
        }
        return document.image();
    }
    void flipImage(bool horizontal) {
        const QImage base = baseImageForTransform();
        if (!base.isNull())
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
            document.applyBaseImage(base.flipped(horizontal ? Qt::Horizontal : Qt::Vertical));
#else
            document.applyBaseImage(base.mirrored(horizontal, !horizontal));
#endif
    }
    void rotateImage(bool clockwise) {
        const QImage base = baseImageForTransform();
        if (base.isNull())
            return;
        QTransform transform;
        transform.rotate(clockwise ? 90 : -90);
        document.applyBaseImage(base.transformed(transform));
    }
    void grayscaleImage() {
        const QImage base = baseImageForTransform();
        if (!base.isNull())
            document.applyBaseImage(base.convertToFormat(QImage::Format_Grayscale8)
                                        .convertToFormat(QImage::Format_ARGB32));
    }
    void autoEnhanceImage() {
        const QImage base = baseImageForTransform();
        if (!base.isNull())
            document.applyBaseImage(snowViewAutoEnhancedImage(base));
    }
    void showAdjustDialog() {
        const QImage base = baseImageForTransform();
        if (base.isNull())
            return;
        QDialog dialog(surface->window());
        dialog.setWindowTitle(QCoreApplication::translate("ImageEditingWindow", "Adjust image"));
        dialog.setModal(true);
        auto* layout = new QVBoxLayout(&dialog);
        auto* preview = new QLabel(&dialog);
        preview->setAlignment(Qt::AlignCenter);
        layout->addWidget(preview);
        const QImage previewBase =
            base.scaled(440, 300, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        const auto addSlider = [&dialog, layout](const QString& label, int minimum, int maximum) {
            auto* row = new QHBoxLayout;
            auto* caption = new QLabel(label, &dialog);
            caption->setMinimumWidth(90);
            row->addWidget(caption);
            auto* slider = new QSlider(Qt::Horizontal, &dialog);
            slider->setRange(minimum, maximum);
            slider->setValue(0);
            row->addWidget(slider, 1);
            layout->addLayout(row);
            return slider;
        };
        auto* brightness =
            addSlider(QCoreApplication::translate("ImageEditingWindow", "Brightness"), -100, 100);
        auto* contrast =
            addSlider(QCoreApplication::translate("ImageEditingWindow", "Contrast"), -100, 100);
        auto* saturation =
            addSlider(QCoreApplication::translate("ImageEditingWindow", "Saturation"), -100, 100);
        auto* sharpen =
            addSlider(QCoreApplication::translate("ImageEditingWindow", "Sharpen"), 0, 100);
        const auto updatePreview = [preview, previewBase, brightness, contrast, saturation,
                                    sharpen] {
            preview->setPixmap(QPixmap::fromImage(
                snowViewAdjustedImage(previewBase, brightness->value(), contrast->value(),
                                      saturation->value(), sharpen->value())));
        };
        for (auto* slider : {brightness, contrast, saturation, sharpen})
            QObject::connect(slider, &QSlider::valueChanged, &dialog,
                             [updatePreview](int) { updatePreview(); });
        updatePreview();
        auto* buttons = new QHBoxLayout;
        buttons->addStretch(1);
        auto* cancelButton =
            new QPushButton(QCoreApplication::translate("ImageEditingWindow", "Cancel"), &dialog);
        auto* applyButton =
            new QPushButton(QCoreApplication::translate("ImageEditingWindow", "Apply"), &dialog);
        applyButton->setDefault(true);
        buttons->addWidget(cancelButton);
        buttons->addWidget(applyButton);
        layout->addLayout(buttons);
        QObject::connect(cancelButton, &QPushButton::clicked, &dialog, &QDialog::reject);
        QObject::connect(applyButton, &QPushButton::clicked, &dialog, &QDialog::accept);
        if (dialog.exec() == QDialog::Accepted &&
            (brightness->value() != 0 || contrast->value() != 0 || saturation->value() != 0 ||
             sharpen->value() != 0))
            document.applyBaseImage(snowViewAdjustedImage(base, brightness->value(),
                                                          contrast->value(), saturation->value(),
                                                          sharpen->value()));
    }
    void undo(bool redo) {
        if (cropBusy || loading)
            return;
        if (session && session->active()) {
            if (session->tableModeActive()) {
                if (redo)
                    session->redoTableEdit();
                else
                    session->undoTableEdit();
            } else {
                if (redo)
                    session->redoTextEdit();
                else
                    session->undoTextEdit();
            }
        } else {
            if (canvas->mode() == ImageEditingCanvas::Mode::CropRect)
                drawing(ImageEditingCanvas::Mode::Pan);
            static_cast<void>(canvas->resetEditingStatePreservingTool());
            auto transition = std::make_shared<ImageEditingDocument::HistoryTransition>(
                document.historyTransition(redo));
            if (transition->state == transition->nextState) {
                document.applyHistoryTransition(std::move(*transition));
            } else {
                setCropBusy(true);
                cropJob = ScreenshotExportCoordinator::shared().submit(
                    &q, ScreenshotExportCoordinator::Priority::Foreground,
                    [transition](const auto& cancel) {
                        if (!cancel.isCancellationRequested())
                            ImageEditingDocument::prepareHistoryTransition(*transition, [&cancel] {
                                return cancel.isCancellationRequested();
                            });
                        return ScreenshotExportTaskResult{};
                    },
                    [this, transition](auto) {
                        setCropBusy(false);
                        if (!closing)
                            document.applyHistoryTransition(std::move(*transition));
                    });
                if (!cropJob.isValid())
                    setCropBusy(false);
            }
        }
        syncPalette();
    }
    void bindRecognition();
    void activate(Session::Mode mode, bool translate = false,
                  ScreenshotClipboardService::PublicationId copySourceOnReady = 0);
    Session* ensureSession();
    void pruneRecognitionSessions(bool replaceDocument = false) {
        for (auto it = sessions.begin(); it != sessions.end();) {
            if (!replaceDocument && it->controller && document.retainsBaseHistoryState(it.key())) {
                ++it;
                continue;
            }
            const auto retired = it->controller;
            if (retired)
                retired->retireContentAttachment();
            it = sessions.erase(it);
            if (retired) {
                if (session == retired)
                    session = nullptr;
                QObject::disconnect(retired, nullptr, &q, nullptr);
                // A history mutation can arrive through a session callback. Retire its
                // ownership now, then let destruction cancel work off that callback stack.
                retired->deleteLater();
            }
        }
    }
    ScreenshotRecognitionWindow* ensureRecognitionView();
    void syncRecognitionGeometry();
    void syncRecognitionDisplay();
    [[nodiscard]] bool showsRecognitionImage() const;
    bool routeRecognitionViewportEvent(QObject* watched, QEvent* event);
    QByteArray recognitionCheckpoint(Session* target) const {
        if (!target)
            return {};
        return target->contentCheckpoints().value(target->activeContentKey());
    }
    bool exportContent(ExportOperation operation);
    void translateConfirmation() {
        confirmation->setWindowTitle(ImageEditingWindow::tr("Discard changes?"));
        confirmation->setText(ImageEditingWindow::tr("This editing window has unsaved changes"));
        confirmation->setAcceptText(ImageEditingWindow::tr("Discard changes"));
        confirmation->setRejectText(ImageEditingWindow::tr("Continue editing"));
    }
};

bool ImageEditingWindow::Impl::showsRecognitionImage() const {
    return recognitionVisible && recognitionView && session && session->active() &&
           session->mode() == Session::Mode::Text &&
           (session->showOriginalImage() || recognitionView->showsOcrLayer());
}

void ImageEditingWindow::Impl::syncRecognitionGeometry() {
    if (!recognitionVisible || !recognitionView)
        return;
    const QRect viewport = canvas->rect();
    const auto bounds = canvas->canvasToViewTransform().inverted().mapRect(QRectF(viewport));
    static_cast<void>(recognitionView->updateSelectionGeometry(viewport, bounds));
}

void ImageEditingWindow::Impl::syncRecognitionDisplay() {
    const bool showImage = showsRecognitionImage();
    const bool showFiltered = showImage && !session->showOriginalImage();
    canvas->setRecognitionDisplay(recognitionVisible, !recognitionVisible || showImage,
                                  showFiltered);
    const bool showToolbar = !document.image().isNull() && (!recognitionVisible || showImage);
    imageToolbar->setVisible(showToolbar);
    if (showToolbar)
        imageToolbar->raise();
    if (cropButton)
        cropButton->setEnabled(!recognitionVisible);
    fit->setEnabled(!recognitionVisible || showImage);
    actual->setEnabled(!recognitionVisible || showImage);
}

bool ImageEditingWindow::Impl::routeRecognitionViewportEvent(QObject* watched, QEvent* event) {
    const auto* widget = qobject_cast<QWidget*>(watched);
    if (!showsRecognitionImage() || !widget ||
        (widget != recognitionView && !recognitionView->isAncestorOf(widget)))
        return false;
    if (event->type() == QEvent::Wheel) {
        const auto* wheelEvent = static_cast<QWheelEvent*>(event);
        const QPointF position = canvas->mapFromGlobal(wheelEvent->globalPosition());
        QWheelEvent forwarded(position, wheelEvent->globalPosition(), wheelEvent->pixelDelta(),
                              wheelEvent->angleDelta(), wheelEvent->buttons(),
                              wheelEvent->modifiers(), wheelEvent->phase(), wheelEvent->inverted(),
                              wheelEvent->source(), wheelEvent->pointingDevice());
        QCoreApplication::sendEvent(canvas, &forwarded);
    } else if (event->type() == QEvent::MouseButtonPress ||
               event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::MouseMove) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        const bool middle = event->type() == QEvent::MouseMove
                                ? mouse->buttons().testFlag(Qt::MiddleButton)
                                : mouse->button() == Qt::MiddleButton;
        if (!middle)
            return false;
        const QPointF position = canvas->mapFromGlobal(mouse->globalPosition());
        QMouseEvent forwarded(mouse->type(), position, canvas->mapTo(canvas->window(), position),
                              mouse->globalPosition(), mouse->button(), mouse->buttons(),
                              mouse->modifiers(), mouse->pointingDevice());
        QCoreApplication::sendEvent(canvas, &forwarded);
    } else {
        return false;
    }
    event->accept();
    return true;
}

ScreenshotRecognitionWindow* ImageEditingWindow::Impl::ensureRecognitionView() {
    if (!recognitionView) {
        ScreenshotRecognitionWindowActions actions;
        actions.handleCancel = [this] { drawing(ImageEditingCanvas::Mode::Pan); };
        actions.handleCopy = [this] { exportContent(ExportOperation::Copy); };
        actions.copyOcrResult = [this] { return exportContent(ExportOperation::Copy); };
        actions.handleTextEdited = [this](const QString& text) {
            if (session && session->ownsContentAttachment())
                session->setTextDraft(text);
        };
        actions.handleUndoTextEdit = [this] { undo(false); };
        actions.handleRedoTextEdit = [this] { undo(true); };
        actions.handleTableCommandStateChanged = [this](const auto& state) {
            if (session && session->ownsContentAttachment())
                session->handleTableCommandState(state);
        };
        recognitionView = new ScreenshotRecognitionWindow(
            actions, canvas, ScreenshotRecognitionWindow::PresentationMode::EmbeddedChild,
            &shortcuts);
        recognitionView->setObjectName(QStringLiteral("imageEditingRecognition"));
        recognitionView->setOriginalImagePreviewSuppressed(true);
        recognitionView->setEmbeddedPointerEventHandler([this](QObject* watched, QEvent* event) {
            return routeRecognitionViewportEvent(watched, event);
        });
        recognitionView->setTextSelectionFocusPreserver([this](QWidget* widget) {
            return widget && (widget->window() == floating || widget == imageToolbar ||
                              imageToolbar->isAncestorOf(widget));
        });
        QObject::connect(recognitionView, &ScreenshotRecognitionWindow::resultPageChanged, &q,
                         [this] { syncRecognitionDisplay(); });
        QObject::connect(
            recognitionView, &ScreenshotRecognitionWindow::embeddedContextMenuRequested, &q,
            [this](const QPoint& global) {
                if (!recognitionView->boxSelectionEnabled())
                    return;
                QContextMenuEvent event(QContextMenuEvent::Other,
                                        recognitionView->mapFromGlobal(global), global);
                QCoreApplication::sendEvent(recognitionView, &event);
            });
    }
    ScreenshotRecognitionWindow::Config config;
    config.presentationMode = ScreenshotRecognitionWindow::PresentationMode::EmbeddedChild;
    config.screen = surface->screen();
    config.geometry = canvas->rect();
    config.canvasSelection =
        canvas->canvasToViewTransform().inverted().mapRect(QRectF(config.geometry));
    config.transientOwner = surface->window();
    static_cast<void>(recognitionView->present(config));
    return recognitionView;
}

Session* ImageEditingWindow::Impl::ensureSession() {
    const auto providers =
        services.providers ? services.providers() : ScreenshotPinnedRecognitionProviders{};
    if (!session)
        session = sessions.value(document.baseState()).controller;
    if (session) {
        session->setTarget(
            {QStringLiteral("image-editing:%1:%2").arg(quintptr(&q)).arg(document.baseState()),
             document.image(), document.bounds()});
        session->setProviders(providers.recognition, providers.qrRecognition,
                              providers.tableRecognition);
        return session;
    }
    ScreenshotRecognitionSessionActions actions;
    const auto owner = std::make_shared<QPointer<Session>>();
    actions.isCurrentSession = [this, owner] { return *owner && session == *owner; };
    actions.ensureContent = [this] { return ensureRecognitionView(); };
    actions.applyOcrPresentation = [this](auto presentation) {
        canvas->setRecognitionBackground();
        recognitionView->setOcrPresentation(std::move(presentation));
    };
    actions.applyOcrBackgroundImage = [this](auto, QImage image, QRectF bounds) {
        canvas->setRecognitionBackground(std::move(image),
                                         bounds.isValid() ? bounds : document.bounds());
    };
    actions.ocrBackgroundColor = [this] { return surface->palette().window().color(); };
    actions.applyFormattedText = [this](auto doc) {
        if (recognitionView && session && session->ownsContentAttachment())
            recognitionView->showFormattedText(std::move(doc));
    };
    actions.clearOcrBackground = [this] { canvas->setRecognitionBackground(); };
    actions.setRecognitionVisualState = [this](bool active) {
        recognitionVisible = active;
        if (recognitionView) {
            recognitionView->setVisible(active);
            if (active)
                recognitionView->setFocus(Qt::OtherFocusReason);
        }
        syncRecognitionGeometry();
        syncRecognitionDisplay();
        placeToolbar();
    };
    actions.setTextEditingState = [this](bool available, bool editing, bool undo, bool redo) {
        palette->setTextEditingState(available, editing, undo, redo);
        palette->setOriginalImageToggleEnabled(available);
    };
    actions.setTextTranslationState = [this](bool a, bool b, bool c, bool d, bool e, bool f,
                                             bool g) {
        palette->setTextTranslationState(a, b, c, d, e, f, g);
    };
    actions.setTableEditingState = [this](bool a, bool b, bool c, bool d, bool e, bool f) {
        palette->setTableEditingState(a, b, c, d, e, f);
    };
    actions.setLatexEditingState = [this](bool a, bool b, bool c) {
        palette->setLatexEditingState(a, b, c);
    };
    actions.setBusyState = [this](bool a, bool b, bool c) {
        palette->setOcrBusy(a);
        palette->setTableBusy(b);
        palette->setQrBusy(c);
    };
    actions.hideLoading = [this] {
        ScreenshotMessageService::destroyFor(surface->window(),
                                             QStringLiteral("imageEditingRecognition"));
    };
    actions.showStatus = [this](const QString& text, bool error) {
        if (text.isEmpty())
            return;
        AdMessage::Request request;
        request.key = QStringLiteral("imageEditingRecognitionStatus");
        request.type = error ? AdMessage::Type::Error : AdMessage::Type::Warning;
        request.content = text;
        AdMessageService::instance(surface->window())->open(std::move(request));
    };
    actions.showModelDownload = [this](const QString& text) {
        ScreenshotMessageService::loadingFor(surface->window(),
                                             QStringLiteral("imageEditingModelDownload"), text);
    };
    actions.showRecognition = [this](const QString& text) {
        ScreenshotMessageService::loadingFor(surface->window(),
                                             QStringLiteral("imageEditingRecognition"), text);
    };
    actions.hideModelDownload = [this] {
        ScreenshotMessageService::destroyFor(surface->window(),
                                             QStringLiteral("imageEditingModelDownload"));
    };
    actions.translationSettingsOwner = [this] { return surface->window(); };
    actions.setTextTransformState = [this](const QString& a, const QString& b) {
        palette->setTextTransformSelections(a, b);
    };
    actions.setTextTargetLanguage = [this](const QString& text) {
        palette->setTextTargetLanguage(text);
    };
    actions.setTextTranslationServices = [this](const QVector<SnowShotChatModel>& models,
                                                const QString& selection, bool modelsLoading) {
        palette->setTextTranslationServices(models, selection, modelsLoading);
    };
    actions.setShowOriginalImage = [this](bool show) {
        palette->setShowOriginalImage(show);
        syncRecognitionDisplay();
    };
    actions.renderTranslationImage = [this] {
        if (!recognitionView)
            return QImage{};
        auto snapshot = recognitionView->imageSnapshot(
            document.image(), document.bounds(), canvas->recognitionBackground(),
            canvas->recognitionBackgroundRect(), ScreenshotResultStyle{}, true);
        return snapshot ? renderScreenshotRecognitionImage(*snapshot) : QImage{};
    };
    actions.setConversionState = [this](bool active, bool busy, auto format) {
        palette->setImageConversionBusy(
            active && busy && format == SnowShotImageConversionFormat::Markdown,
            active && busy && format == SnowShotImageConversionFormat::Html);
    };
    actions.setRecognitionModelState = [this](int mode, const auto& state) {
        const auto tool = mode == int(Session::Mode::Document) ? Tool::Document
                          : mode == int(Session::Mode::Table)  ? Tool::Table
                          : mode == int(Session::Mode::Latex)  ? Tool::Latex
                          : mode == int(Session::Mode::Html)   ? Tool::Html
                                                               : Tool::Markdown;
        palette->setRecognitionModelState(tool, state);
    };
    session = new Session(providers.recognition, providers.qrRecognition,
                          providers.tableRecognition, std::move(actions), &q);
    *owner = session;
    session->setIndependentEditing(true);
    session->setTarget(
        {QStringLiteral("image-editing:%1:%2").arg(quintptr(&q)).arg(document.baseState()),
         document.image(), document.bounds()});
    if (document.baseState() == 0) {
        auto seed = initialRecognition;
        seed.key =
            QStringLiteral("image-editing:%1:%2").arg(quintptr(&q)).arg(document.baseState());
        session->seedRecognitionResults(std::move(seed));
    }
    auto* created = session.data();
    sessions.insert(document.baseState(), {created, created->contentCheckpoints()});
    QObject::connect(created, &Session::recognitionResultsChanged, &q, [this, created] {
        if (session == created)
            syncAvailability();
    });
    QObject::connect(created, &Session::textResultChanged, &q, [this, created](bool ready) {
        if (ready && session == created) {
            if (translateWhenReady) {
                translateWhenReady = false;
                created->beginTextTranslation();
            }
        }
    });
    return session;
}
void ImageEditingWindow::Impl::activate(
    Session::Mode mode, bool translate,
    ScreenshotClipboardService::PublicationId copySourceOnReady) {
    if (loading || cropBusy || document.image().isNull())
        return;
    const QPointer<ImageEditingWindow> lifetime(&q);
    cancelColorSampling();
    if (!lifetime)
        return;
    static_cast<void>(canvas->resetEditingStatePreservingTool());
    if (!lifetime)
        return;
    canvas->setMode(ImageEditingCanvas::Mode::Pan);
    translateWhenReady = translate;
    const QPointer<Session> current = ensureSession();
    if (!lifetime || !current || current != session)
        return;
    current->setShowOriginalImage(false);
    if (!lifetime || !current || current != session)
        return;
    current->activate(mode, copySourceOnReady);
    if (!lifetime || !current || current != session)
        return;
    syncAvailability();
    palette->setCropToolsState(false, false);
    if (translate && translateWhenReady && session->hasTextResult()) {
        translateWhenReady = false;
        session->beginTextTranslation();
        if (!lifetime || !current || current != session)
            return;
    }
    const Tool tool = mode == Session::Mode::Text ? (translate ? Tool::TextTranslation : Tool::Ocr)
                      : mode == Session::Mode::Document ? Tool::Document
                      : mode == Session::Mode::Table    ? Tool::Table
                      : mode == Session::Mode::Qr       ? Tool::Qr
                      : mode == Session::Mode::Latex    ? Tool::Latex
                      : mode == Session::Mode::Html     ? Tool::Html
                                                        : Tool::Markdown;
    host->setActiveTool(tool);
    placeToolbar();
}
void ImageEditingWindow::Impl::bindRecognition() {
    QObject::connect(palette, &ScreenshotToolPalette::ocrRequested, &q,
                     [this] { activate(Session::Mode::Text); });
    QObject::connect(palette, &ScreenshotToolPalette::textTranslationRequested, &q,
                     [this] { activate(Session::Mode::Text, true); });
    QObject::connect(palette, &ScreenshotToolPalette::documentRequested, &q,
                     [this] { activate(Session::Mode::Document); });
    QObject::connect(palette, &ScreenshotToolPalette::tableRequested, &q,
                     [this] { activate(Session::Mode::Table); });
    QObject::connect(palette, &ScreenshotToolPalette::qrRequested, &q,
                     [this] { activate(Session::Mode::Qr); });
    QObject::connect(palette, &ScreenshotToolPalette::latexRequested, &q,
                     [this] { activate(Session::Mode::Latex); });
    QObject::connect(palette, &ScreenshotToolPalette::markdownRequested, &q,
                     [this] { activate(Session::Mode::Markdown); });
    QObject::connect(palette, &ScreenshotToolPalette::htmlRequested, &q,
                     [this] { activate(Session::Mode::Html); });
    QObject::connect(
        palette, &ScreenshotToolPalette::showOriginalImageRequested, &q, [this](bool show) {
            if (session && session->mode() == Session::Mode::Text && session->hasTextResult())
                session->setShowOriginalImage(show);
        });
    QObject::connect(palette, &ScreenshotToolPalette::textEditRequested, &q, [this] {
        const QPointer<ImageEditingWindow> lifetime(&q);
        const auto current = session;
        if (!current)
            return;
        current->setShowOriginalImage(false);
        if (!lifetime || !current || current != session)
            return;
        const bool openEditor = !current->editing() || current->translating();
        if (current->editing() || current->translating())
            current->endTextEditing();
        if (lifetime && current && current == session && openEditor)
            current->beginTextEditing();
        if (lifetime && current && current == session)
            host->setActiveTool(Tool::Ocr);
    });
    QObject::connect(palette, &ScreenshotToolPalette::textTranslateRequested, &q, [this] {
        const QPointer<ImageEditingWindow> lifetime(&q);
        const auto current = session;
        if (!current)
            return;
        current->setShowOriginalImage(false);
        if (!lifetime || !current || current != session)
            return;
        if (current->translating()) {
            current->endTextEditing();
            if (lifetime && current && current == session)
                host->setActiveTool(Tool::Ocr);
        } else {
            current->beginTextTranslation();
        }
    });
#define SESSION_ACTION(signal, method)                                                             \
    QObject::connect(palette, &ScreenshotToolPalette::signal, &q, [this] {                         \
        if (session)                                                                               \
            session->method();                                                                     \
    })
    SESSION_ACTION(tableMergeRequested, mergeTableSelection);
    SESSION_ACTION(tableSplitRequested, splitTableSelection);
    SESSION_ACTION(tableResetRequested, resetTable);
    QObject::connect(palette, &ScreenshotToolPalette::openDocumentFileRequested, &q, [this] {
        const QPointer<ImageEditingWindow> lifetime(&q);
        if (session)
            static_cast<void>(session->openDocumentFile([this, lifetime] {
                if (lifetime)
                    drawing(ImageEditingCanvas::Mode::Pan);
            }));
    });
    QObject::connect(palette, &ScreenshotToolPalette::openSpreadsheetFileRequested, &q, [this] {
        const QPointer<ImageEditingWindow> lifetime(&q);
        if (session && session->openSpreadsheetFile() && lifetime)
            drawing(ImageEditingCanvas::Mode::Pan);
    });
    SESSION_ACTION(textResetRequested, resetTextEditing);
    SESSION_ACTION(textSettingsRequested, openTranslationSettings);
    SESSION_ACTION(textTranslationServicesRequested, requestTextTranslationServices);
#undef SESSION_ACTION
#define SESSION_VALUE(signal, method)                                                              \
    QObject::connect(palette, &ScreenshotToolPalette::signal, &q, [this](const QString& value) {   \
        if (session)                                                                               \
            session->method(value);                                                                \
    })
    SESSION_VALUE(textFormattingRequested, applyTextFormatting);
    SESSION_VALUE(textPunctuationRequested, applyTextPunctuation);
    SESSION_VALUE(textTargetLanguageRequested, applyTextTargetLanguage);
    SESSION_VALUE(textTranslationServiceRequested, applyTextTranslationService);
    SESSION_VALUE(recognitionModelChanged, setRecognitionModel);
#undef SESSION_VALUE
    palette->setOriginalImageToggleEnabled(false);
}

bool ImageEditingWindow::Impl::exportContent(ExportOperation operation) {
    if (loading || cropBusy || !exporter.canStart(operation) || closing ||
        document.image().isNull())
        return false;

    const QPointer<ImageEditingWindow> lifetime(&q);
    const bool copying = operation == ExportOperation::Copy;
    ImageEditingExportController::CopyOptions copyOptions;
    if (copying) {
        copyOptions.publication =
            ScreenshotClipboardService::reservePublication(QApplication::clipboard());
        copyOptions.requestedAt = QDateTime::currentDateTime();
        if (storage::ScreenshotSettings().copyImageFileToClipboard())
            copyOptions.file = ScreenshotCloudUploadService::currentOptions();
        exporter.cancelCopy();
        if (!lifetime || closing)
            return false;
    }
    if (!q.preparePendingEdits() ||
        (copying && !ScreenshotClipboardService::publicationCurrent(copyOptions.publication)))
        return false;

    const QPointer<Session> recognition =
        operation != ExportOperation::Pin && session && session->active() ? session : nullptr;
    const QString contentKey = recognition ? recognition->activeContentKey() : QString{};
    const auto state = document.baseState();
    const auto finish = [this, lifetime, operation, recognition, contentKey,
                         state](const QByteArray& checkpoint, bool recognitionContent, bool success,
                                QString error) {
        if (!lifetime || closing)
            return;
        if (operation == ExportOperation::Upload)
            palette->setCloudUploadBusy(false);
        if (success && !checkpoint.isEmpty()) {
            if (recognitionContent) {
                const auto entry = sessions.find(state);
                if (recognition && entry != sessions.end() && entry->controller == recognition)
                    entry->saved.insert(contentKey, checkpoint);
            } else {
                document.markExported(checkpoint);
            }
        }
        if (!lifetime || closing)
            return;
        if (!exporter.busy()) {
            if (success)
                showNotice(Notice::Exported);
            else if (!error.isEmpty())
                message(std::move(error));
            else if (notice == Notice::Exporting)
                message({});
        }
        if (lifetime && !closing)
            emit q.exportFinished(success);
    };
    const auto completion = [finish](QByteArray checkpoint, bool recognitionContent) {
        return [finish, checkpoint = std::move(checkpoint), recognitionContent](bool success,
                                                                                QString error) {
            finish(checkpoint, recognitionContent, success, std::move(error));
        };
    };

    if (copying) {
        const auto publish = [this, lifetime,
                              copyOptions](ImageEditingExportController::CopyContent content,
                                           ImageEditingExportController::Completion done) {
            if (!lifetime || closing ||
                !ScreenshotClipboardService::publicationCurrent(copyOptions.publication))
                return false;
            if (!exporter.busy())
                showNotice(Notice::Exporting);
            if (!lifetime || closing ||
                !ScreenshotClipboardService::publicationCurrent(copyOptions.publication))
                return false;
            exporter.startCopy(std::move(content), copyOptions, surface->window(), std::move(done));
            return true;
        };
        if (!recognition) {
            QString error;
            auto source = document.exportSource(&error);
            if (source.isValid())
                return publish(std::move(source), completion(document.checkpoint(), false));
            finish({}, false, false, error);
            return false;
        }
        const auto checkpoint = recognitionCheckpoint(recognition);
#if SNOW_SHOT_ENABLE_DOCUMENT_RECOGNITION
        if (recognition->mode() == Session::Mode::Document) {
            auto request = recognition->documentCopyRequest();
            if (request.document)
                return publish(std::move(request), completion(checkpoint, true));
            finish({}, true, false,
                   ImageEditingWindow::tr("No recognition result is available to export"));
            return false;
        }
#endif
        const bool ocrOverlay =
            recognition->mode() == Session::Mode::Text && !recognition->editing();
        const auto draft = ocrOverlay ? recognition->textDraft() : QString{};
        if (recognition->prepareSmartClipboard(
                nullptr, false,
                [this, lifetime, recognition, publish, completion, checkpoint, draft](auto mime) {
                    if (!lifetime || !recognition || recognition != session ||
                        !recognition->ownsContentAttachment())
                        return;
                    const auto saved = mime->text() == draft ? checkpoint : QByteArray{};
                    publish(std::move(mime), completion(saved, true));
                }))
            return true;
        auto mime = recognition->recognitionClipboardMimeData();
        if (!lifetime || !recognition || recognition != session)
            return false;
        if (!mime) {
            finish({}, true, false,
                   ImageEditingWindow::tr("No recognition result is available to export"));
            return false;
        }
        if (mime->hasImage()) {
            auto image = qvariant_cast<QImage>(mime->imageData());
            return publish(ScreenshotExportSource::fromImage(std::move(image)),
                           completion({}, true));
        }
        const auto saved =
            ocrOverlay && (!mime->hasText() || mime->text() != draft) ? QByteArray{} : checkpoint;
        return publish(std::move(mime), completion(saved, true));
    }

    ImageEditingExportController::Content content;
    QByteArray checkpoint;
    bool recognitionContent = false;
    if (recognition &&
        (operation == ExportOperation::Save || operation == ExportOperation::QuickSave)) {
        auto snapshot = recognition->fileExportSnapshot();
        if (!lifetime || !recognition || recognition != session)
            return false;
        if (snapshot) {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
            if (snapshot->isMissingTableResult()) {
                finish({}, true, false, ScreenshotRecognitionFileExport::noResultMessage());
                return false;
            }
#endif
            checkpoint = recognitionCheckpoint(recognition);
            content = std::move(*snapshot);
            recognitionContent = true;
        }
    }
    if (!recognitionContent) {
        auto snapshot = recognition
                            ? recognition->imageExportSnapshot(document.image(), document.bounds(),
                                                               canvas->recognitionBackground(),
                                                               canvas->recognitionBackgroundRect())
                            : std::nullopt;
        if (snapshot) {
            content = ScreenshotExportSource::fromRecognitionImage(std::move(*snapshot));
        } else {
            QString error;
            auto source = document.exportSource(&error);
            if (!source.isValid()) {
                finish({}, false, false, error);
                return false;
            }
            checkpoint = document.checkpoint();
            content = std::move(source);
        }
    }
    showNotice(Notice::Exporting);
    if (!lifetime || closing)
        return false;
    palette->setCloudUploadBusy(operation == ExportOperation::Upload);
    exporter.start(operation, std::move(content), surface->window(), floating,
                   completion(std::move(checkpoint), recognitionContent));
    return true;
}

ImageEditingWindow::ImageEditingWindow(ImageEditingServices services, QObject* parent)
    : QObject(parent) {
    // Construction can deliver child events before Impl has finished installing the UI.
    m_impl = std::make_unique<Impl>(*this, std::move(services));
}
ImageEditingWindow::~ImageEditingWindow() {
    shutdown();
}
void ImageEditingWindow::presentEmpty() {
    auto& d = *m_impl;
    if (d.closing || !d.document.image().isNull())
        return;
    // SnowView Rev0.1: no file backs the empty state.
    d.currentFilePath.clear();
    d.refreshFilmstrip();
    ++d.loadGeneration;
    d.cancelLoading();
    d.errorSource = nullptr;
    d.setLoading(false);
    d.stack->setCurrentWidget(d.emptyState);
    d.show();
}
void ImageEditingWindow::openFiles(const QStringList& paths) {
    auto& d = *m_impl;
    if (!d.canImport() || paths.isEmpty())
        return;
    const auto generation = ++d.loadGeneration;
    d.cancelLoading();
    d.errorSource = nullptr;
    d.setLoading(true);
    auto files = std::make_shared<QStringList>();
    d.loadJob = ScreenshotExportCoordinator::shared().submit(
        this, ScreenshotExportCoordinator::Priority::Background,
        [paths, files](const auto& cancellation) {
            *files = imageFilePaths(
                paths, [&cancellation] { return cancellation.isCancellationRequested(); });
            return ScreenshotExportTaskResult{};
        },
        [this, files, generation](auto result) {
            auto& impl = *m_impl;
            if (impl.closing || impl.loadGeneration != generation)
                return;
            impl.setLoading(false);
            if (!result.succeeded()) {
                impl.showNotice(Impl::Notice::LoadError);
                return;
            }
            if (files->isEmpty())
                return;
            const QPointer<ImageEditingWindow> guard(this);
            presentFile(files->first());
            if (guard && !m_impl->closing && files->size() > 1)
                emit additionalFilesRequested(files->mid(1));
        });
    if (!d.loadJob.isValid()) {
        d.setLoading(false);
        d.showNotice(Impl::Notice::LoadError);
    }
}
void ImageEditingWindow::paste() {
    auto& d = *m_impl;
    if (!d.canPaste())
        return;
    auto* clipboard = QApplication::clipboard();
    auto paths = ScreenshotClipboardContentReader::localImageFilePaths(clipboard->mimeData());
    // SnowView Rev0.1: pasted pixels have no backing file; pasted files re-enter
    // through presentFile and set the path there.
    if (paths.isEmpty()) {
        d.currentFilePath.clear();
        d.refreshFilmstrip();
    }
    auto reservation = reserveImageStitchingImport(0);
    bool rejected = !reservation;
    std::optional<ScreenshotClipboardContentSnapshot> snapshot;
    if (paths.isEmpty()) {
        snapshot = ScreenshotClipboardContentReader::snapshotImage(
            clipboard, d.surface->devicePixelRatioF(), [&reservation, &rejected](qint64 bytes) {
                rejected = rejected || !reservation->resize(bytes);
                return !rejected;
            });
        if (!snapshot && !rejected)
            return;
    }
    loadImportedImage(std::move(paths), std::move(snapshot), std::move(reservation), rejected);
}

void ImageEditingWindow::loadImportedImage(
    QStringList paths, std::optional<ScreenshotClipboardContentSnapshot> snapshot,
    std::shared_ptr<ImageStitchingImportReservation> reservation, bool reservationRejected) {
    auto& d = *m_impl;
    struct PastedImage {
        ImageEditingSource source;
        QStringList additionalFiles;
        std::shared_ptr<ImageStitchingImportReservation> reservation;
        bool rejected = false;
    };
    auto pasted = std::make_shared<PastedImage>();
    pasted->reservation = std::move(reservation);
    pasted->rejected = reservationRejected || !pasted->reservation;
    const auto generation = ++d.loadGeneration;
    d.cancelLoading();
    d.errorSource = nullptr;
    d.setLoading(true);
    const auto finish = [this, pasted, generation](ScreenshotExportTaskResult result) {
        auto& impl = *m_impl;
        if (impl.closing || impl.loadGeneration != generation)
            return;
        const QPointer<ImageEditingWindow> guard(this);
        if (!result.succeeded() || pasted->source.image.isNull()) {
            impl.setLoading(false);
            impl.sourceError(
                pasted->rejected
                    ? QT_TRANSLATE_NOOP(
                          "ImageEditingWindow",
                          "The image is too large or there is not enough memory to load it")
                    : QT_TRANSLATE_NOOP("ImageEditingWindow", "The image could not be loaded"));
            if (!guard)
                return;
            emit loaded(false);
            if (guard && !impl.closing && impl.loadGeneration == generation &&
                !pasted->additionalFiles.isEmpty())
                emit additionalFilesRequested(pasted->additionalFiles);
            return;
        }
        confirmDiscard([guard, pasted, generation](bool accepted) {
            if (!guard || guard->m_impl->closing || guard->m_impl->loadGeneration != generation)
                return;
            if (!accepted) {
                guard->m_impl->setLoading(false);
                return;
            }
            guard->present(std::move(pasted->source));
            if (!guard || guard->m_impl->closing || guard->m_impl->loadGeneration != generation + 1)
                return;
            guard->m_impl->fileReservation = std::move(pasted->reservation);
            if (!pasted->additionalFiles.isEmpty())
                emit guard->additionalFilesRequested(pasted->additionalFiles);
        });
    };
    if (pasted->rejected) {
        finish({});
        return;
    }
    d.loadJob = ScreenshotExportCoordinator::shared().submit(
        this, ScreenshotExportCoordinator::Priority::Foreground,
        [paths, snapshot = std::move(snapshot), pasted](const auto& cancellation) mutable {
            const auto cancelled = [&cancellation] {
                return cancellation.isCancellationRequested();
            };
            const auto allocate = [pasted](qint64 bytes) {
                pasted->rejected = pasted->rejected || !pasted->reservation->resize(bytes);
                return !pasted->rejected;
            };
            if (snapshot) {
                auto content = ScreenshotClipboardContentReader::decode(std::move(*snapshot),
                                                                        cancelled, allocate);
                if (content)
                    pasted->source.image = std::move(content->image);
            } else {
                const auto files = imageFilePaths(paths, cancelled);
                if (!files.isEmpty()) {
                    const auto captured = ScreenshotClipboardContentReader::snapshotLocalFiles(
                        {files.first()}, cancelled,
                        ScreenshotClipboardContentReader::FilePurpose::ImageImport);
                    if (!captured.isEmpty())
                        pasted->source.image = ScreenshotClipboardContentReader::decodeImageFile(
                            captured.first(), cancelled, allocate);
                    pasted->source.originalFileName = QFileInfo(files.first()).fileName();
                    pasted->additionalFiles = files.mid(1);
                }
            }
            if (!pasted->source.image.isNull()) {
                const auto bytes = imageStitchingImageImportBytes(pasted->source.image.size());
                pasted->rejected =
                    bytes == 0 || !allocate(std::max(bytes, pasted->source.image.sizeInBytes()));
                if (pasted->rejected)
                    pasted->source.image = {};
            }
            pasted->source.pixelSize = pasted->source.image.size();
            pasted->source.canvasRect = QRectF(QPointF(), QSizeF(pasted->source.pixelSize));
            pasted->source.flattened = true;
            return ScreenshotExportTaskResult{};
        },
        finish);
    if (!d.loadJob.isValid())
        finish({});
}
void ImageEditingWindow::presentFile(QString path) {
    auto& d = *m_impl;
    if (d.closing)
        return;
    // SnowView Rev0.1: remember the opened file and show its folder in the filmstrip.
    d.currentFilePath = path;
    d.refreshFilmstrip();
    const auto generation = ++d.loadGeneration;
    d.cancelLoading();
    d.errorSource = nullptr;
    d.setLoading(true);
    struct FileImage {
        ImageEditingSource source;
        std::shared_ptr<ImageStitchingImportReservation> reservation;
        bool rejected = false;
    };
    auto file = std::make_shared<FileImage>();
    d.loadJob = ScreenshotExportCoordinator::shared().submit(
        this, ScreenshotExportCoordinator::Priority::Foreground,
        [path = std::move(path), file](const auto& cancellation) {
            const auto cancelled = [&cancellation] {
                return cancellation.isCancellationRequested();
            };
            file->source.originalFileName = QFileInfo(path).fileName();
            file->reservation = reserveImageStitchingImport(0);
            file->rejected = !file->reservation;
            const auto files = ScreenshotClipboardContentReader::snapshotLocalFiles(
                {path}, cancelled, ScreenshotClipboardContentReader::FilePurpose::ImageImport);
            if (!file->rejected && !files.isEmpty()) {
                file->source.image = ScreenshotClipboardContentReader::decodeImageFile(
                    files.first(), cancelled, [file](qint64 bytes) {
                        if (!file->reservation->resize(bytes)) {
                            file->rejected = true;
                            return false;
                        }
                        return true;
                    });
                if (!file->source.image.isNull()) {
                    const auto bytes = imageStitchingImageImportBytes(file->source.image.size());
                    file->rejected = bytes == 0 || !file->reservation->resize(std::max(
                                                       bytes, file->source.image.sizeInBytes()));
                    if (file->rejected)
                        file->source.image = {};
                }
            }
            file->source.pixelSize = file->source.image.size();
            file->source.canvasRect = QRectF(QPointF(), QSizeF(file->source.pixelSize));
            file->source.flattened = true;
            return ScreenshotExportTaskResult{};
        },
        [this, file, generation](auto result) {
            if (m_impl->closing || m_impl->loadGeneration != generation)
                return;
            if (!result.succeeded() || file->source.image.isNull()) {
                file->source.error =
                    file->rejected
                        ? tr("The image is too large or there is not enough memory to load it")
                        : tr("The image could not be loaded");
            }
            const bool success = result.succeeded() && !file->source.image.isNull();
            const QPointer<ImageEditingWindow> guard(this);
            present(std::move(file->source));
            if (!guard || m_impl->closing || m_impl->loadGeneration != generation + 1)
                return;
            if (file->rejected)
                m_impl->errorSource = QT_TRANSLATE_NOOP(
                    "ImageEditingWindow",
                    "The image is too large or there is not enough memory to load it");
            if (success)
                m_impl->fileReservation = std::move(file->reservation);
        });
    if (!d.loadJob.isValid()) {
        ImageEditingSource failure;
        failure.error = tr("The image could not be loaded");
        present(std::move(failure));
    }
}
void ImageEditingWindow::present(ImageEditingSource source) {
    auto& d = *m_impl;
    if (d.closing)
        return;
    d.errorSource = nullptr;
    const auto generation = ++d.loadGeneration;
    d.cancelLoading();
    if (source.image.isNull() && source.layeredImage.isValid()) {
        d.setLoading(true);
        auto result = std::make_shared<ImageEditingSource>(std::move(source));
        d.loadJob = ScreenshotExportCoordinator::shared().submit(
            this, ScreenshotExportCoordinator::Priority::Foreground,
            [result](const auto& cancel) {
                if (!cancel.isCancellationRequested())
                    result->image = renderImageEditingBaseImage(
                        result->layeredImage, result->canvasRect, result->pixelSize);
                result->layeredImage = {};
                return ScreenshotExportTaskResult{};
            },
            [this, result, generation](auto) {
                if (!m_impl->closing && m_impl->loadGeneration == generation)
                    present(std::move(*result));
            });
        if (d.loadJob.isValid())
            return;
    }
    if (!source.textAnnotations.empty() && source.error.isEmpty()) {
        d.setLoading(true);
        auto result = std::make_shared<ImageEditingSource>(std::move(source));
        d.loadJob = ScreenshotExportCoordinator::shared().submit(
            this, ScreenshotExportCoordinator::Priority::Foreground,
            [result](const auto& cancel) {
                if (!cancel.isCancellationRequested()) {
                    result->image = renderScreenshotTextAnnotations(
                        result->image, result->canvasRect, result->textAnnotations);
                    if (result->image.isNull())
                        result->error = ImageEditingWindow::tr("The image could not be loaded");
                    result->textAnnotations = {};
                }
                return ScreenshotExportTaskResult{};
            },
            [this, result, generation](auto) {
                if (!m_impl->closing && m_impl->loadGeneration == generation)
                    present(std::move(*result));
            });
        if (d.loadJob.isValid())
            return;
        source = std::move(*result);
        source.error = tr("The image could not be loaded");
    }
    if (source.resultStyle && source.error.isEmpty()) {
        d.setLoading(true);
        d.loadArtifact =
            std::make_shared<ScreenshotExportArtifact>(imageEditingCompositeSource(source));
        if (d.loadArtifact->requestImage(this, [this, source = std::move(source),
                                                generation](auto result) mutable {
                if (m_impl->closing || m_impl->loadGeneration != generation)
                    return;
                [[maybe_unused]] const auto artifact = std::move(m_impl->loadArtifact);
                present(completeImageEditingComposite(std::move(source), std::move(result.image)));
            }))
            return;
        source.error = tr("The image could not be loaded");
    }
    d.setLoading(false);
    d.show();
    if (!source.error.isEmpty() || !d.document.load(source)) {
        if (source.error.isEmpty() || source.error == tr("The image could not be loaded") ||
            source.error ==
                QCoreApplication::translate("ImageEditingWindow", "The image could not be loaded"))
            d.showNotice(Impl::Notice::LoadError);
        else
            d.message(source.error);
        emit loaded(false);
        return;
    }
    d.pruneRecognitionSessions(true);
    d.fileReservation.reset();
    d.initialRecognition = source.recognition;
    d.syncAvailability();
    d.stack->setCurrentWidget(d.canvas);
    d.canvas->fit();
    d.drawing(ImageEditingCanvas::Mode::Pan);
    emit loaded(true);
}
void ImageEditingWindow::presentSource(ScreenshotExportSource source, QString originalFileName) {
    auto& d = *m_impl;
    if (d.closing)
        return;
    // SnowView Rev0.1: captures and other in-memory sources have no backing file.
    d.currentFilePath.clear();
    d.refreshFilmstrip();
    const auto generation = ++d.loadGeneration;
    d.cancelLoading();
    d.setLoading(true);
    d.loadArtifact = std::make_shared<ScreenshotExportArtifact>(std::move(source));
    if (!d.loadArtifact->requestImage(
            this, [this, generation, originalFileName = std::move(originalFileName)](
                      ScreenshotExportImageResult result) {
                if (m_impl->closing || m_impl->loadGeneration != generation)
                    return;
                // Keep the artifact alive until its delivery callback returns.
                [[maybe_unused]] const auto artifact = std::move(m_impl->loadArtifact);
                ImageEditingSource editingSource;
                editingSource.image = std::move(result.image);
                editingSource.pixelSize = editingSource.image.size();
                editingSource.canvasRect = QRectF(QPointF(), QSizeF(editingSource.pixelSize));
                editingSource.originalFileName = originalFileName;
                editingSource.flattened = true;
                if (editingSource.image.isNull())
                    editingSource.error = tr("The image could not be loaded");
                present(std::move(editingSource));
            })) {
        ImageEditingSource failure;
        failure.error = tr("The image could not be loaded");
        present(std::move(failure));
    }
}
void ImageEditingWindow::presentHistory(storage::CaptureHistoryRecord record) {
    auto& d = *m_impl;
    if (d.closing)
        return;
    const auto generation = ++d.loadGeneration;
    d.cancelLoading();
    d.setLoading(true);
    auto result = std::make_shared<ImageEditingSource>();
    d.loadJob = ScreenshotExportCoordinator::shared().submit(
        this, ScreenshotExportCoordinator::Priority::Foreground,
        [record = std::move(record), result](const auto& cancel) {
            if (!cancel.isCancellationRequested())
                *result = readImageEditingHistory(
                    storage::ApplicationStorage::instance().captureHistory(), record);
            return ScreenshotExportTaskResult{};
        },
        [this, result, generation](auto) {
            if (!m_impl->closing && m_impl->loadGeneration == generation)
                present(std::move(*result));
        });
    if (!d.loadJob.isValid()) {
        ImageEditingSource failure;
        failure.error = tr("The image could not be loaded");
        present(std::move(failure));
    }
}
void ImageEditingWindow::presentPinnedRecord(QString id) {
    auto& d = *m_impl;
    if (d.closing)
        return;
    const auto generation = ++d.loadGeneration;
    d.cancelLoading();
    d.setLoading(true);
    if (d.services.pins)
        if (auto* pin = d.services.pins->liveWindow(id)) {
            const QPointer<ImageEditingWindow> guard(this);
            pin->requestRenderedImageSnapshot(
                [guard, generation](ScreenshotPinnedImageSnapshot snapshot) {
                    if (!guard || guard->m_impl->closing ||
                        guard->m_impl->loadGeneration != generation)
                        return;
                    if (!snapshot.error.isEmpty() || !snapshot.source.isValid()) {
                        ImageEditingSource failure;
                        failure.error =
                            snapshot.error.isEmpty()
                                ? ImageEditingWindow::tr("The image could not be loaded")
                                : std::move(snapshot.error);
                        guard->present(std::move(failure));
                        return;
                    }
                    guard->presentSource(std::move(snapshot.source),
                                         std::move(snapshot.originalFileName));
                },
                ScreenshotPinnedRenderMode::SourceResolution);
            return;
        }
    const auto preparation = captureScreenshotPinnedRenderPreparation();
    auto result = std::make_shared<ImageEditingSource>();
    d.loadJob = ScreenshotExportCoordinator::shared().submit(
        this, ScreenshotExportCoordinator::Priority::Foreground,
        [id = std::move(id), result, preparation](const auto& cancel) {
            if (!cancel.isCancellationRequested()) {
                auto record =
                    storage::ApplicationStorage::instance().pinnedWindows().loadRecord(id);
                if (record)
                    *result = readImageEditingPinned(std::move(*record), preparation);
            }
            return ScreenshotExportTaskResult{};
        },
        [this, result, generation](auto) {
            if (!m_impl->closing && m_impl->loadGeneration == generation)
                present(std::move(*result));
        });
    if (!d.loadJob.isValid()) {
        ImageEditingSource failure;
        failure.error = tr("The image could not be loaded");
        present(std::move(failure));
    }
}
bool ImageEditingWindow::dirty() const {
    if (m_impl->cropBusy)
        return true;
    if (m_impl->document.dirty())
        return true;
    for (const auto& state : m_impl->sessions)
        if (state.controller && state.controller->contentCheckpoints() != state.saved)
            return true;
    return false;
}
bool ImageEditingWindow::loading() const {
    return m_impl->loading;
}
QByteArray ImageEditingWindow::contentCheckpoint() const {
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream << m_impl->document.checkpoint() << m_impl->cropBusy;
    auto states = m_impl->sessions.keys();
    std::sort(states.begin(), states.end());
    for (auto state : states) {
        const auto session = m_impl->sessions.value(state).controller;
        if (!session)
            continue;
        const auto contents = session->contentCheckpoints();
        auto keys = contents.keys();
        std::sort(keys.begin(), keys.end());
        for (const auto& key : keys)
            stream << state << key << contents.value(key);
    }
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}
bool ImageEditingWindow::preparePendingEdits(bool requireCanvasCommit) {
    if (m_impl->closing)
        return false;
    const QPointer<ImageEditingWindow> lifetime(this);
    const bool canvasPrepared = m_impl->canvas->resetEditingStatePreservingTool();
    if (!lifetime || m_impl->closing || (requireCanvasCommit && !canvasPrepared))
        return false;
    if (m_impl->session && m_impl->session->active() && m_impl->session->tableModeActive() &&
        m_impl->recognitionView)
        m_impl->recognitionView->commitActiveTableEdit();
    return lifetime && !m_impl->closing;
}
void ImageEditingWindow::confirmDiscard(std::function<void(bool)> done) {
    if (!preparePendingEdits()) {
        done(false);
        return;
    }
    auto& d = *m_impl;
    if (!dirty()) {
        done(true);
        return;
    }
    if (d.confirmation) {
        done(false);
        return;
    }
    auto* dialog = modal_window::createConfirmation(this, d.surface->window());
    d.confirmation = dialog;

    d.translateConfirmation();
    connect(dialog, &AdModal::finished, this, [this, done = std::move(done)](auto code) {
        m_impl->confirmation = nullptr;
        done(code == AdModal::DialogCode::Accepted);
    });
    dialog->setOpen(true);
    if (dialog->acceptButton()) {
        dialog->acceptButton()->setAutoDefault(false);
        dialog->acceptButton()->setDefault(false);
    }
    if (dialog->rejectButton()) {
        dialog->rejectButton()->setAutoDefault(true);
        dialog->setInitialFocusWidget(dialog->rejectButton());
        dialog->rejectButton()->setDefault(true);
        dialog->rejectButton()->setFocus();
    }
}
void ImageEditingWindow::requestClose() {
    if (m_impl->closing)
        return;
    confirmDiscard([this](bool discard) {
        if (discard) {
            const QPointer<ImageEditingWindow> lifetime(this);
            shutdown();
            if (lifetime)
                emit closed();
        }
    });
}
void ImageEditingWindow::shutdown() {
    if (!m_impl || m_impl->closing)
        return;
    const QPointer<ImageEditingWindow> lifetime(this);
    auto& d = *m_impl;
    const QPointer<AdModal> confirmation = d.confirmation;
    d.closing = true;
    d.dropHint->hide();
    if (d.fileDialog) {
        d.fileDialog->reject();
        if (!lifetime)
            return;
    }
    d.showNotice(Impl::Notice::None);
    if (!lifetime)
        return;
    d.floating->cancelDrag();
    d.floating->hide();
    if (!lifetime)
        return;
    d.cancelColorSampling();
    if (!lifetime)
        return;
    d.autoFilter->resetSession();
    if (!lifetime)
        return;
    d.cancelLoading();
    d.cropJob.cancel();
    d.filmstripQueue.cancel();
    d.exporter.cancel();
    if (!lifetime)
        return;
    const auto sessions = d.sessions;
    for (const auto& state : sessions) {
        const auto session = state.controller;
        if (session)
            session->invalidate();
        if (!lifetime)
            return;
    }
    // Rejecting invokes client completion, which can destroy this editor and its owner.
    // Resolve it before hiding its owner can trigger the modal's owner-hide callback.
    if (confirmation)
        confirmation->reject();
    if (lifetime)
        d.modal->setOpen(false);
}
bool ImageEditingWindow::eventFilter(QObject* object, QEvent* event) {
    if (m_impl && m_impl->filmstrip &&
        (object == m_impl->filmstrip || object == m_impl->filmstrip->viewport()) &&
        event->type() == QEvent::Wheel) {
        auto* wheel = static_cast<QWheelEvent*>(event);
        if (auto* hbar = m_impl->filmstrip->horizontalScrollBar()) {
            const int pixelStep =
                wheel->pixelDelta().x() != 0 ? wheel->pixelDelta().x() : wheel->pixelDelta().y();
            const int angleStep =
                wheel->angleDelta().x() != 0 ? wheel->angleDelta().x() : wheel->angleDelta().y();
            const int delta = pixelStep != 0 ? pixelStep : qRound(angleStep / 120.0 * 72.0);
            if (delta != 0) {
                hbar->setValue(hbar->value() - delta);
                wheel->accept();
                return true;
            }
        }
    }
    if (m_impl && object == m_impl->floating && event->type() == QEvent::Close) {
        event->ignore();
        requestClose();
        return true;
    }
    if (m_impl && object == m_impl->canvas && m_impl->colorEvent(event)) {
        event->accept();
        return true;
    }
    if (m_impl && object == m_impl->surface) {
        auto& d = *m_impl;
        if (event->type() == QEvent::DragLeave) {
            d.dropHint->hide();
            event->accept();
            return true;
        }
        if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove ||
            event->type() == QEvent::Drop) {
            auto* drop = static_cast<QDropEvent*>(event);
            const auto paths =
                ScreenshotClipboardContentReader::localImageFilePaths(drop->mimeData());
            if (!d.canImport() || !(drop->possibleActions() & Qt::CopyAction) || paths.isEmpty()) {
                d.dropHint->hide();
                event->ignore();
                return true;
            }
            drop->setDropAction(Qt::CopyAction);
            drop->accept();
            if (event->type() == QEvent::Drop) {
                d.dropHint->hide();
                openFiles(paths);
            } else {
                d.dropHint->setGeometry(d.surface->rect());
                d.dropHint->raise();
                d.dropHint->show();
            }
            return true;
        }
        if (event->type() == QEvent::Resize) {
            d.dropHint->setGeometry(d.surface->rect());
            d.placeToolbar();
        }
        if (event->type() == QEvent::LanguageChange)
            m_impl->retranslate();
        if (event->type() == QEvent::FontChange || event->type() == QEvent::ApplicationFontChange ||
            event->type() == QEvent::DevicePixelRatioChange ||
            event->type() == QEvent::ScreenChangeInternal)
            QTimer::singleShot(0, this, [this] {
                if (!m_impl->closing) {
                    m_impl->updateFilmstripMetrics();
                    m_impl->placeToolbar();
                }
            });
    }
    if (m_impl && object == m_impl->editorWindow) {
        if (event->type() == QEvent::Hide ||
            (event->type() == QEvent::WindowStateChange && m_impl->editorWindow->isMinimized())) {
            m_impl->floating->cancelDrag();
            m_impl->floating->hide();
        } else if (event->type() == QEvent::Move || event->type() == QEvent::Resize ||
                   event->type() == QEvent::Show || event->type() == QEvent::WindowStateChange ||
                   event->type() == QEvent::ScreenChangeInternal ||
                   event->type() == QEvent::DevicePixelRatioChange) {
            m_impl->placeToolbar();
        }
    }
    return QObject::eventFilter(object, event);
}
ImageEditingDocument* ImageEditingWindow::document() const {
    return &m_impl->document;
}
ImageEditingCanvas* ImageEditingWindow::canvas() const {
    return m_impl->canvas;
}
ScreenshotToolPalette* ImageEditingWindow::palette() const {
    return m_impl->palette;
}
AdModal* ImageEditingWindow::modal() const {
    return m_impl->modal;
}
Session* ImageEditingWindow::recognition() const {
    return m_impl->session;
}
ImageEditingWindowCoordinator::ImageEditingWindowCoordinator(ImageEditingServices services,
                                                             QObject* parent)
    : QObject(parent), m_services(std::move(services)) {
    qApp->installEventFilter(this);
}
ImageEditingWindowCoordinator::~ImageEditingWindowCoordinator() {
    shutdown();
}
ImageEditingWindow* ImageEditingWindowCoordinator::createWindow() {
    auto* window = new ImageEditingWindow(m_services, this);
    m_windows.append(window);
    ++m_windowRevision;
    connect(window, &ImageEditingWindow::closed, this, [this, window] {
        if (m_windows.removeAll(window) != 0)
            ++m_windowRevision;
        window->deleteLater();
    });
    connect(window, &QObject::destroyed, this, [this, window] {
        if (m_windows.removeIf(
                [window](const auto& entry) { return !entry || entry.data() == window; }) != 0)
            ++m_windowRevision;
    });
    connect(window, &ImageEditingWindow::additionalFilesRequested, this,
            [this](const QStringList& paths) {
                for (const auto& path : paths) {
                    if (auto* receiving = createWindow())
                        receiving->presentFile(path);
                }
            });
    return window;
}
void ImageEditingWindowCoordinator::presentFiles(const QStringList& paths) {
    if (auto* window = createWindow()) {
        window->presentEmpty();
        window->openFiles(paths);
    }
}
void ImageEditingWindowCoordinator::presentDroppedImage(
    ScreenshotClipboardContentSnapshot snapshot,
    std::shared_ptr<ImageStitchingImportReservation> reservation) {
    if (auto* window = createWindow()) {
        window->presentEmpty();
        window->loadImportedImage({}, std::move(snapshot), std::move(reservation), false);
    }
}
void ImageEditingWindowCoordinator::presentSelectedFiles(
    std::shared_ptr<platform::SelectedFileBackend> backend, platform::SelectedFileTarget target) {
    auto* window = createWindow();
    if (!window)
        return;
    window->presentEmpty();
    if (!backend || !backend->isValidTarget(target))
        return;
    auto& d = *window->m_impl;
    const auto generation = ++d.loadGeneration;
    d.setLoading(true);
    auto selection = std::make_shared<platform::SelectedFileResult>();
    d.loadJob = ScreenshotExportCoordinator::shared().submit(
        window, ScreenshotExportCoordinator::Priority::Background,
        [backend = std::move(backend), target, selection](const auto& cancellation) {
            *selection = backend->selectedFiles(
                target, [&cancellation] { return cancellation.isCancellationRequested(); });
            return ScreenshotExportTaskResult{};
        },
        [window, selection, generation](auto result) {
            auto& impl = *window->m_impl;
            if (impl.closing || impl.loadGeneration != generation)
                return;
            impl.setLoading(false);
            if (!result.succeeded() || selection->error != platform::SelectedFileError::None) {
                impl.sourceError(selectedFileErrorSource(selection->error));
                return;
            }
            window->openFiles(selection->paths);
        });
    if (!d.loadJob.isValid()) {
        d.setLoading(false);
        d.showNotice(ImageEditingWindow::Impl::Notice::LoadError);
    }
}
void ImageEditingWindowCoordinator::presentHistory(const storage::CaptureHistoryRecord& record) {
    if (auto* window = createWindow())
        window->presentHistory(record);
}
bool ImageEditingWindowCoordinator::presentDocument(ImageEditingSource source,
                                                    std::function<void(bool)> completion) {
    auto* window = createWindow();
    if (!window)
        return false;
    connect(window, &ImageEditingWindow::loaded, window, std::move(completion),
            Qt::SingleShotConnection);
    window->present(std::move(source));
    return true;
}
void ImageEditingWindowCoordinator::presentSource(ScreenshotExportSource source) {
    if (auto* window = createWindow())
        window->presentSource(std::move(source));
}
void ImageEditingWindowCoordinator::presentPinnedRecord(const QString& id) {
    if (auto* window = createWindow())
        window->presentPinnedRecord(id);
}
void ImageEditingWindowCoordinator::confirmShutdown(std::function<void(bool)> done) {
    if (m_reviewPending) {
        done(false);
        return;
    }
    m_reviewPending = true;
    auto approved = std::make_shared<QHash<ImageEditingWindow*, QByteArray>>();
    const QPointer<ImageEditingWindowCoordinator> lifetime(this);
    auto advance = std::make_shared<std::function<void(bool)>>();
    const std::weak_ptr<std::function<void(bool)>> weakAdvance = advance;
    *advance = [lifetime, approved, weakAdvance, done = std::move(done)](bool accepted) {
        const auto continuation = weakAdvance.lock();
        if (!continuation)
            return;
        QPointer<ImageEditingWindow> pending;
        if (accepted && lifetime) {
            const auto windows = lifetime->m_windows;
            for (auto window : windows) {
                if (!window)
                    continue;
                const bool prepared = window->preparePendingEdits();
                if (!lifetime)
                    break;
                if (prepared && window && window->dirty() &&
                    approved->value(window) != window->contentCheckpoint()) {
                    pending = window;
                    break;
                }
            }
        }
        if (!accepted || !lifetime || !pending) {
            if (lifetime)
                lifetime->m_reviewPending = false;
            auto complete = done;
            *continuation = {};
            complete(accepted && lifetime);
            return;
        }
        pending->confirmDiscard([pending, approved, continuation](bool accepted) {
            if (accepted && pending)
                approved->insert(pending, pending->contentCheckpoint());
            auto next = *continuation;
            next(accepted);
        });
    };
    auto first = *advance;
    first(true);
}
bool ImageEditingWindowCoordinator::hasUnsavedChanges() const {
    return std::any_of(m_windows.cbegin(), m_windows.cend(),
                       [](const auto& window) { return window && window->dirty(); });
}
bool ImageEditingWindowCoordinator::prepareForUpdateHandoff() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_handoffPreparing || m_reviewPending)
        return false;
    const QPointer<ImageEditingWindowCoordinator> lifetime(this);
    m_handoffPreparing = true;
    const auto preparing = qScopeGuard([lifetime] {
        if (lifetime)
            lifetime->m_handoffPreparing = false;
    });
    const auto revision = m_windowRevision;
    const auto windows = m_windows;
    for (auto window : windows) {
        if (!window)
            return false;
        const bool prepared = window->preparePendingEdits(true);
        if (!lifetime || !window || !prepared)
            return false;
        if (m_windowRevision != revision || m_reviewPending || window->dirty())
            return false;
    }
    return m_windowRevision == revision && !m_reviewPending && !hasUnsavedChanges();
}
void ImageEditingWindowCoordinator::shutdown() {
    const auto windows = std::exchange(m_windows, {});
    if (!windows.isEmpty())
        ++m_windowRevision;
    for (auto window : windows) {
        if (!window)
            continue;
        window->shutdown();
        if (window)
            window->deleteLater();
    }
}
bool ImageEditingWindowCoordinator::eventFilter(QObject* object, QEvent* event) {
    if (object == qApp && event->type() == QEvent::Quit && !m_quitApproved) {
        if (!m_quitPending) {
            m_quitPending = true;
            QTimer::singleShot(0, this, [this] {
                const QPointer<ImageEditingWindowCoordinator> lifetime(this);
                confirmShutdown([lifetime](bool accepted) {
                    if (!lifetime)
                        return;
                    lifetime->m_quitPending = false;
                    if (accepted) {
                        lifetime->m_quitApproved = true;
                        QCoreApplication::quit();
                    }
                });
            });
        }
        return true;
    }
    return QObject::eventFilter(object, event);
}
} // namespace snow_shot::presentation
