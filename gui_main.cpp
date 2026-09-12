#include "sonydb.h"
#include "cd_importer.h"
#include "id3_metadata.h"

#include <id3.h>

#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QThread>
#include <QToolBar>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <iterator>
#include <memory>

namespace {

enum DeviceColumn { StateColumn, TitleColumn, ArtistColumn, AlbumColumn,
                    TrackColumn, DurationColumn, DeviceColumnCount };
constexpr int SongRole = Qt::UserRole + 1;

struct SongData {
    int order = 0;
    int status = ON_DEVICE;
    int track = 0;
    int duration = 0;
    int year = 0;
    std::uint32_t encoding = 0;
    QString title;
    QString artist;
    QString album;
    QString genre;
    QString filename;
};

QString fromSony(const char *value) { return value ? QString::fromUtf8(value) : QString(); }
QByteArray toSony(const QString &value) { return value.toUtf8(); }

void releaseSong(Song *song)
{
    if (!song) return;
    free(song->album); free(song->artist); free(song->title);
    free(song->genre); free(song->filename); free(song);
}

QString normalizeGenre(const QString &genre)
{
    static const char *names[] = {
        "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge",
        "Hip-Hop", "Jazz", "Metal", "New Age", "Oldies", "Other", "Pop", "R&B",
        "Rap", "Reggae", "Rock", "Techno", "Industrial", "Alternative", "Ska",
        "Death Metal", "Pranks", "Soundtrack", "Euro-Techno", "Ambient", "Trip-Hop",
        "Vocal", "Jazz+Funk", "Fusion", "Trance", "Classical"
    };
    QString value = genre.trimmed();
    if (value.startsWith('(') && value.endsWith(')')) value = value.mid(1, value.size() - 2);
    bool ok = false;
    const int index = value.toInt(&ok);
    return ok && index >= 0 && index < static_cast<int>(std::size(names))
        ? QString::fromLatin1(names[index]) : genre.trimmed();
}

int estimateMp3Duration(const QString &filename)
{
    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly)) return 0;
    QByteArray data = file.read(256 * 1024);
    qsizetype start = 0;
    if (data.size() >= 10 && data.startsWith("ID3")) {
        const auto byte = [&data](int i) { return static_cast<unsigned char>(data.at(i)); };
        start = 10 + ((byte(6) & 0x7f) << 21) + ((byte(7) & 0x7f) << 14)
            + ((byte(8) & 0x7f) << 7) + (byte(9) & 0x7f);
        if (start >= data.size()) { file.seek(start); data = file.read(256 * 1024); start = 0; }
    }
    static const int mpeg1[] = {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
    static const int mpeg2[] = {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0};
    for (qsizetype i = start; i + 3 < data.size(); ++i) {
        const auto b0 = static_cast<unsigned char>(data.at(i));
        const auto b1 = static_cast<unsigned char>(data.at(i + 1));
        const auto b2 = static_cast<unsigned char>(data.at(i + 2));
        if (b0 != 0xff || (b1 & 0xe0) != 0xe0 || (b1 & 0x06) != 0x02) continue;
        const int bitrate = ((b1 >> 3) & 3) == 3 ? mpeg1[(b2 >> 4) & 15] : mpeg2[(b2 >> 4) & 15];
        if (bitrate > 0) return static_cast<int>((QFileInfo(filename).size() * 8) / (bitrate * 1000));
    }
    return 0;
}

struct Mp3StreamInfo {
    int bitrateKbps = 0;
    int sampleRate = 0;
    int channels = 0;
};

Mp3StreamInfo mp3StreamInfo(const QString &filename)
{
    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QByteArray data = file.read(256 * 1024);
    qsizetype start = 0;
    if (data.size() >= 10 && data.startsWith("ID3")) {
        const auto byte = [&data](int i) { return static_cast<unsigned char>(data.at(i)); };
        start = 10 + ((byte(6) & 0x7f) << 21) + ((byte(7) & 0x7f) << 14)
            + ((byte(8) & 0x7f) << 7) + (byte(9) & 0x7f);
        if (start >= data.size()) { file.seek(start); data = file.read(256 * 1024); start = 0; }
    }
    static const int mpeg1Bitrates[] = {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
    static const int mpeg2Bitrates[] = {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0};
    static const int sampleRates[] = {44100, 48000, 32000, 0};
    for (qsizetype i = start; i + 3 < data.size(); ++i) {
        const auto b0 = static_cast<unsigned char>(data.at(i));
        const auto b1 = static_cast<unsigned char>(data.at(i + 1));
        const auto b2 = static_cast<unsigned char>(data.at(i + 2));
        const auto b3 = static_cast<unsigned char>(data.at(i + 3));
        if (b0 != 0xff || (b1 & 0xe0) != 0xe0 || (b1 & 0x06) != 0x02) continue;
        const int version = (b1 >> 3) & 3;
        if (version == 1 || ((b2 >> 4) & 15) == 0 || ((b2 >> 4) & 15) == 15) continue;
        Mp3StreamInfo info;
        info.bitrateKbps = version == 3 ? mpeg1Bitrates[(b2 >> 4) & 15] : mpeg2Bitrates[(b2 >> 4) & 15];
        info.sampleRate = sampleRates[(b2 >> 2) & 3];
        if (version == 2) info.sampleRate /= 2;
        else if (version == 0) info.sampleRate /= 4;
        info.channels = ((b3 >> 6) & 3) == 3 ? 1 : 2;
        return info;
    }
    return {};
}

SongData metadataForFile(const QString &filename)
{
    SongData data;
    data.status = ADD_TO_DEVICE;
    data.filename = QFileInfo(filename).absoluteFilePath();
    data.title = QFileInfo(filename).completeBaseName();
    const QByteArray path = QFile::encodeName(data.filename);
    if (ID3Tag *tag = ID3Tag_New()) {
        ID3Tag_Link(tag, path.constData());
        const QString title = readId3TextFrame(tag, ID3FID_TITLE);
        if (!title.isEmpty()) data.title = title;
        data.artist = readId3TextFrame(tag, ID3FID_LEADARTIST);
        data.album = readId3TextFrame(tag, ID3FID_ALBUM);
        data.genre = normalizeGenre(readId3TextFrame(tag, ID3FID_CONTENTTYPE));
        data.track = readId3TextFrame(tag, ID3FID_TRACKNUM).section('/', 0, 0).toInt();
        data.year = readId3TextFrame(tag, ID3FID_YEAR).left(4).toInt();
        data.duration = readId3TextFrame(tag, ID3FID_SONGLEN).toInt() / 1000;
        ID3Tag_Delete(tag);
    }
    if (data.duration <= 0) data.duration = estimateMp3Duration(data.filename);
    return data;
}

SongData songData(const Song *song)
{
    return {song->sonyDbOrder, song->statusOfSong, song->track_nr, song->songlen,
            song->year, song->encoding, fromSony(song->title), fromSony(song->artist), fromSony(song->album),
            fromSony(song->genre), fromSony(song->filename)};
}

QString durationText(int seconds)
{
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

QString statusText(int status)
{
    switch (status) {
    case ADD_TO_DEVICE: return QObject::tr("Pending addition");
    case REMOVE_FROM_DEVICE: return QObject::tr("Pending removal");
    default: return QObject::tr("On device");
    }
}

QFrame *makePanel(QWidget *content, const QString &title, const QString &subtitle)
{
    auto *panel = new QFrame;
    panel->setObjectName(QStringLiteral("panel"));
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(16, 14, 16, 16);
    auto *heading = new QLabel(title); heading->setObjectName(QStringLiteral("panelTitle"));
    auto *caption = new QLabel(subtitle); caption->setObjectName(QStringLiteral("panelCaption"));
    layout->addWidget(heading); layout->addWidget(caption); layout->addSpacing(4); layout->addWidget(content, 1);
    return panel;
}

} // namespace

Q_DECLARE_METATYPE(SongData)

class MainWindow final : public QMainWindow
{
public:
    MainWindow()
    {
        setWindowTitle(tr("SonyDb — Walkman Music Transfer"));
        resize(1580, 800); setMinimumSize(1100, 600); setAcceptDrops(true);
        loadMp3Settings();
        buildToolbar(); buildContent(); applyModernStyle();
        network_ = new QNetworkAccessManager(this);

        const QString music = QSettings().value(QStringLiteral("musicFolder"),
            QStandardPaths::writableLocation(QStandardPaths::MusicLocation)).toString();
        setMusicFolder(music);
        refreshCd(false);
        const QString previous = QSettings().value(QStringLiteral("devicePath")).toString();
        if (!previous.isEmpty() && QFileInfo::exists(previous + QStringLiteral("/OMGAUDIO/04CNTINF.DAT")))
            connectDevice(previous);
        else detectDevice(false);
    }

protected:
    void dragEnterEvent(QDragEnterEvent *event) override
    { if (event->mimeData()->hasUrls()) event->acceptProposedAction(); }

    void dropEvent(QDropEvent *event) override
    {
        QStringList files;
        for (const QUrl &url : event->mimeData()->urls()) collectMp3Files(url.toLocalFile(), files);
        transferFiles(files); event->acceptProposedAction();
    }

    void closeEvent(QCloseEvent *event) override
    {
        if (!worker_) { event->accept(); return; }
        QMessageBox::information(this, tr("Transfer in progress"), tr("The application cannot close until the transfer is complete."));
        event->ignore();
    }

private:
    void buildToolbar()
    {
        auto *bar = addToolBar(tr("Main Actions"));
        bar->setMovable(false); bar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); bar->setIconSize({18, 18});
        cdRefreshAction_ = bar->addAction(style()->standardIcon(QStyle::SP_BrowserReload), tr("Refresh CD"));
        connect(cdRefreshAction_, &QAction::triggered, this, [this] { refreshCd(true); });
        mp3SettingsAction_ = bar->addAction(style()->standardIcon(QStyle::SP_FileDialogDetailedView), tr("MP3 Settings"));
        connect(mp3SettingsAction_, &QAction::triggered, this, [this] { configureMp3(); });
        encodingSummary_ = new QLabel(mp3Settings_.summary()); encodingSummary_->setObjectName(QStringLiteral("encodingSummary"));
        bar->addWidget(encodingSummary_); bar->addSeparator();
        auto *musicButton = bar->addAction(style()->standardIcon(QStyle::SP_DirOpenIcon), tr("Music Folder"));
        connect(musicButton, &QAction::triggered, this, [this] { chooseMusicFolder(); });
        musicPath_ = new QLineEdit; musicPath_->setReadOnly(true); musicPath_->setMinimumWidth(180); musicPath_->setMaximumWidth(300);
        bar->addWidget(musicPath_); bar->addSeparator();
        auto *deviceButton = bar->addAction(style()->standardIcon(QStyle::SP_DriveHDIcon), tr("Select Walkman"));
        connect(deviceButton, &QAction::triggered, this, [this] { chooseDevice(); });
        auto *detectButton = bar->addAction(style()->standardIcon(QStyle::SP_DialogApplyButton), tr("Auto Detect"));
        connect(detectButton, &QAction::triggered, this, [this] { detectDevice(); });
        refreshAction_ = bar->addAction(style()->standardIcon(QStyle::SP_BrowserReload), tr("Refresh"));
        connect(refreshAction_, &QAction::triggered, this, [this] { reload(); });
        bar->addSeparator();
        editAction_ = bar->addAction(style()->standardIcon(QStyle::SP_FileDialogDetailedView), tr("Edit Track Info"));
        connect(editAction_, &QAction::triggered, this, [this] { editSelected(); });
        deleteAction_ = bar->addAction(style()->standardIcon(QStyle::SP_TrashIcon), tr("Remove from Walkman"));
        connect(deleteAction_, &QAction::triggered, this, [this] { removeSelected(); });
    }

    void buildContent()
    {
        auto *central = new QWidget; auto *outer = new QVBoxLayout(central);
        outer->setContentsMargins(18, 18, 18, 12); outer->setSpacing(12);
        auto *connectionRow = new QHBoxLayout;
        deviceState_ = new QLabel(tr("● Not connected")); deviceState_->setObjectName(QStringLiteral("deviceState"));
        devicePath_ = new QLabel(tr("Connect a Walkman")); devicePath_->setObjectName(QStringLiteral("devicePath"));
        connectionRow->addWidget(deviceState_); connectionRow->addWidget(devicePath_); connectionRow->addStretch();
        groupBy_ = new QComboBox; groupBy_->addItems({tr("By Album"), tr("By Artist"), tr("By Genre"), tr("Ungrouped")});
        search_ = new QLineEdit; search_->setPlaceholderText(tr("Search Walkman")); search_->setClearButtonEnabled(true); search_->setMaximumWidth(250);
        connectionRow->addWidget(new QLabel(tr("View:"))); connectionRow->addWidget(groupBy_); connectionRow->addWidget(search_);
        outer->addLayout(connectionRow);

        sourceModel_ = new QFileSystemModel(this);
        sourceModel_->setNameFilters({QStringLiteral("*.mp3"), QStringLiteral("*.MP3")});
        sourceModel_->setNameFilterDisables(false); sourceModel_->setFilter(QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot);
        sourceTree_ = new QTreeView; sourceTree_->setModel(sourceModel_);
        sourceTree_->setSelectionMode(QAbstractItemView::ExtendedSelection); sourceTree_->setAnimated(true);
        sourceTree_->setSelectionBehavior(QAbstractItemView::SelectRows);
        sourceTree_->setContextMenuPolicy(Qt::CustomContextMenu);
        sourceTree_->setAlternatingRowColors(true); sourceTree_->setSortingEnabled(true); sourceTree_->sortByColumn(0, Qt::AscendingOrder);
        sourceTree_->setColumnHidden(2, true); sourceTree_->setColumnHidden(3, true);
        configureResizableHeader(sourceTree_->header(), QStringLiteral("headers/pc"), {360, 100, 0, 0});

        cdTree_ = new QTreeWidget; cdTree_->setColumnCount(4);
        cdTree_->setHeaderLabels({tr("#"), tr("Title"), tr("Artist"), tr("Duration")});
        cdTree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
        cdTree_->setSelectionBehavior(QAbstractItemView::SelectRows);
        cdTree_->setContextMenuPolicy(Qt::CustomContextMenu);
        cdTree_->setAlternatingRowColors(true); cdTree_->setAnimated(true);
        configureResizableHeader(cdTree_->header(), QStringLiteral("headers/cd"), {48, 230, 170, 82});

        deviceTree_ = new QTreeWidget; deviceTree_->setColumnCount(DeviceColumnCount);
        deviceTree_->setHeaderLabels({tr("Status"), tr("Title"), tr("Artist"), tr("Album"), tr("#"), tr("Duration")});
        deviceTree_->setSelectionMode(QAbstractItemView::ExtendedSelection); deviceTree_->setSelectionBehavior(QAbstractItemView::SelectRows);
        deviceTree_->setContextMenuPolicy(Qt::CustomContextMenu);
        deviceTree_->setAlternatingRowColors(true); deviceTree_->setAnimated(true); deviceTree_->setSortingEnabled(true);
        configureResizableHeader(deviceTree_->header(), QStringLiteral("headers/walkman"),
                                  {120, 240, 170, 210, 50, 82});

        auto *cdPanel = makePanel(cdTree_, tr("Audio CD"), tr("MusicBrainz metadata and track selection"));
        auto *leftPanel = makePanel(sourceTree_, tr("This PC"), tr("MP3 library and CD import destination"));
        auto *rightPanel = makePanel(deviceTree_, tr("Sony Walkman"), tr("Browse tracks stored on the device"));
        cdImportButton_ = new QToolButton; cdImportButton_->setObjectName(QStringLiteral("transferButton"));
        cdImportButton_->setIcon(style()->standardIcon(QStyle::SP_ArrowForward)); cdImportButton_->setIconSize({32, 32});
        cdImportButton_->setToolTip(tr("Import selected CD tracks as MP3 files")); cdImportButton_->setEnabled(false);
        auto *cdMiddle = new QWidget; cdMiddle->setFixedWidth(72); auto *cdMiddleLayout = new QVBoxLayout(cdMiddle);
        cdMiddleLayout->addStretch(); cdMiddleLayout->addWidget(cdImportButton_, 0, Qt::AlignCenter);
        auto *importLabel = new QLabel(tr("Import")); importLabel->setAlignment(Qt::AlignCenter);
        importLabel->setObjectName(QStringLiteral("arrowLabel")); cdMiddleLayout->addWidget(importLabel); cdMiddleLayout->addStretch();
        transferButton_ = new QToolButton; transferButton_->setObjectName(QStringLiteral("transferButton"));
        transferButton_->setIcon(style()->standardIcon(QStyle::SP_ArrowForward)); transferButton_->setIconSize({32, 32});
        transferButton_->setToolTip(tr("Transfer selected tracks to the Walkman")); transferButton_->setEnabled(false);
        restoreButton_ = new QToolButton; restoreButton_->setObjectName(QStringLiteral("restoreButton"));
        restoreButton_->setIcon(style()->standardIcon(QStyle::SP_ArrowBack)); restoreButton_->setIconSize({32, 32});
        restoreButton_->setToolTip(tr("Restore selected tracks to this PC")); restoreButton_->setEnabled(false);
        auto *middle = new QWidget; middle->setFixedWidth(72); auto *middleLayout = new QVBoxLayout(middle);
        middleLayout->addStretch(); middleLayout->addWidget(transferButton_, 0, Qt::AlignCenter);
        auto *arrowLabel = new QLabel(tr("Transfer")); arrowLabel->setAlignment(Qt::AlignCenter); arrowLabel->setObjectName(QStringLiteral("arrowLabel"));
        middleLayout->addWidget(arrowLabel); middleLayout->addSpacing(24);
        middleLayout->addWidget(restoreButton_, 0, Qt::AlignCenter);
        auto *restoreLabel = new QLabel(tr("Restore")); restoreLabel->setAlignment(Qt::AlignCenter); restoreLabel->setObjectName(QStringLiteral("arrowLabel"));
        middleLayout->addWidget(restoreLabel); middleLayout->addStretch();
        auto *splitter = new QSplitter; splitter->addWidget(cdPanel); splitter->addWidget(cdMiddle);
        splitter->addWidget(leftPanel); splitter->addWidget(middle); splitter->addWidget(rightPanel);
        splitter->setStretchFactor(0, 1); splitter->setStretchFactor(1, 0); splitter->setStretchFactor(2, 1);
        splitter->setStretchFactor(3, 0); splitter->setStretchFactor(4, 1);
        splitter->setSizes({390, 72, 430, 72, 500}); outer->addWidget(splitter, 1);
        progress_ = new QProgressBar; progress_->setTextVisible(false); progress_->setFixedHeight(4); progress_->setVisible(false);
        outer->addWidget(progress_); setCentralWidget(central);
        statusBar()->showMessage(tr("Import an audio CD to the PC, then transfer MP3 tracks to the Walkman"));

        connect(cdImportButton_, &QToolButton::clicked, this, [this] { importCdSelection(); });
        connect(cdTree_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &position) {
            showCdContextMenu(position);
        });
        connect(transferButton_, &QToolButton::clicked, this, [this] { transferSelection(); });
        connect(restoreButton_, &QToolButton::clicked, this, [this] { restoreSelection(); });
        connect(sourceTree_, &QTreeView::doubleClicked, this, [this](const QModelIndex &index) {
            if (!sourceModel_->isDir(index)) transferSelection();
        });
        connect(sourceTree_, &QTreeView::customContextMenuRequested, this, [this](const QPoint &position) {
            showPcContextMenu(position);
        });
        connect(groupBy_, &QComboBox::currentIndexChanged, this, [this] { populateDeviceTree(); });
        connect(search_, &QLineEdit::textChanged, this, [this](const QString &text) { filterDeviceTree(text); });
        connect(deviceTree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *, int) { editSelected(); });
        connect(deviceTree_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &position) {
            showDeviceContextMenu(position);
        });
        setDeviceControlsEnabled(false);
    }

    void applyModernStyle()
    {
        qApp->setStyle(QStringLiteral("Fusion"));
        qApp->setStyleSheet(QStringLiteral(R"(
QMainWindow, QWidget { background: #f4f6f9; color: #172033; font-size: 13px; }
QToolBar { background: #ffffff; border: 0; border-bottom: 1px solid #dde3ec; spacing: 5px; padding: 8px 12px; }
QToolButton { border: 0; border-radius: 7px; padding: 7px 9px; background: transparent; }
QToolButton:hover { background: #edf3ff; color: #1559c7; } QToolButton:disabled { color: #aab2c0; }
QLineEdit, QComboBox, QSpinBox { background: white; border: 1px solid #cfd7e4; border-radius: 7px; padding: 6px 9px; }
QLineEdit:focus, QComboBox:focus, QSpinBox:focus { border: 1px solid #3978e6; }
QFrame#panel { background: white; border: 1px solid #e0e5ed; border-radius: 12px; }
QLabel#panelTitle { font-size: 19px; font-weight: 650; color: #172033; border: 0; }
QLabel#panelCaption, QLabel#arrowLabel { color: #727e91; border: 0; }
QLabel#encodingSummary { color: #5e6879; padding: 0 6px; }
QLabel#deviceState { color: #9b2335; font-weight: 650; } QLabel#devicePath { color: #5e6879; }
QTreeView, QTreeWidget { background: white; border: 0; alternate-background-color: #f8faff; outline: 0; }
QTreeView::item, QTreeWidget::item { min-height: 26px; border-radius: 4px; }
QTreeView::item:selected, QTreeWidget::item:selected { background: #dce9ff; color: #123a78; }
QHeaderView::section { background: #f5f7fb; color: #5e6879; border: 0; border-bottom: 1px solid #e1e6ee; padding: 7px; font-weight: 600; }
QToolButton#transferButton, QToolButton#restoreButton { background: #246fe5; border-radius: 24px; min-width: 48px; min-height: 48px; padding: 0; }
QToolButton#transferButton:hover, QToolButton#restoreButton:hover { background: #1559c7; }
QToolButton#transferButton:disabled, QToolButton#restoreButton:disabled { background: #cbd3df; }
QStatusBar { background: white; border-top: 1px solid #dde3ec; color: #637083; }
QProgressBar { border: 0; background: #dfe5ef; } QProgressBar::chunk { background: #246fe5; }
)"));
    }

    void setDeviceControlsEnabled(bool enabled)
    {
        transferButton_->setEnabled(enabled && !worker_); editAction_->setEnabled(enabled && !worker_);
        restoreButton_->setEnabled(enabled && !worker_);
        deleteAction_->setEnabled(enabled && !worker_); refreshAction_->setEnabled(enabled && !worker_);
        groupBy_->setEnabled(enabled && !worker_); search_->setEnabled(enabled && !worker_);
    }

    void configureResizableHeader(QHeaderView *header, const QString &settingsKey,
                                   const QList<int> &defaultWidths)
    {
        header->setSectionResizeMode(QHeaderView::Interactive);
        header->setMinimumSectionSize(42);
        header->setSectionsMovable(true);
        header->setStretchLastSection(false);
        for (int section = 0; section < defaultWidths.size() && section < header->count(); ++section) {
            if (defaultWidths.at(section) > 0) header->resizeSection(section, defaultWidths.at(section));
        }
        const QByteArray savedState = QSettings().value(settingsKey).toByteArray();
        if (!savedState.isEmpty()) header->restoreState(savedState);
        const auto save = [header, settingsKey] {
            QSettings().setValue(settingsKey, header->saveState());
        };
        connect(header, &QHeaderView::sectionResized, this, save);
        connect(header, &QHeaderView::sectionMoved, this, save);
    }

    void setCdControlsEnabled(bool enabled)
    {
        if (cdRefreshAction_) cdRefreshAction_->setEnabled(enabled && !worker_);
        if (mp3SettingsAction_) mp3SettingsAction_->setEnabled(enabled && !worker_);
        if (cdImportButton_) cdImportButton_->setEnabled(enabled && !worker_ && !cdDisc_.tracks.isEmpty());
        if (cdTree_) cdTree_->setEnabled(enabled && !worker_);
    }

    void loadMp3Settings()
    {
        QSettings settings;
        mp3Settings_.rateMode = settings.value(QStringLiteral("mp3/rateMode"), 0).toInt() == 1
            ? Mp3EncodingSettings::RateMode::VariableBitrate : Mp3EncodingSettings::RateMode::ConstantBitrate;
        mp3Settings_.channelMode = static_cast<Mp3EncodingSettings::ChannelMode>(
            qBound(0, settings.value(QStringLiteral("mp3/channelMode"), 0).toInt(), 2));
        mp3Settings_.bitrateKbps = settings.value(QStringLiteral("mp3/bitrateKbps"), 192).toInt();
        if (!QList<int>{128, 160, 192, 256, 320}.contains(mp3Settings_.bitrateKbps)) mp3Settings_.bitrateKbps = 192;
        mp3Settings_.vbrQuality = qBound(0, settings.value(QStringLiteral("mp3/vbrQuality"), 2).toInt(), 5);
        mp3Settings_.encoderQuality = qBound(0, settings.value(QStringLiteral("mp3/encoderQuality"), 2).toInt(), 9);
    }

    void configureMp3()
    {
        QDialog dialog(this); dialog.setWindowTitle(tr("MP3 Encoding Settings"));
        auto *layout = new QVBoxLayout(&dialog); auto *form = new QFormLayout;
        auto *mode = new QComboBox;
        mode->addItem(tr("Constant bitrate (CBR)"), 0); mode->addItem(tr("Variable bitrate (VBR)"), 1);
        mode->setCurrentIndex(mp3Settings_.rateMode == Mp3EncodingSettings::RateMode::VariableBitrate ? 1 : 0);
        auto *bitrate = new QComboBox;
        for (int value : {128, 160, 192, 256, 320}) bitrate->addItem(tr("%1 kbps").arg(value), value);
        bitrate->setCurrentIndex(qMax(0, bitrate->findData(mp3Settings_.bitrateKbps)));
        auto *vbr = new QComboBox;
        for (int value = 0; value <= 5; ++value)
            vbr->addItem(value == 0 ? tr("V0 — Highest quality") : tr("V%1").arg(value), value);
        vbr->setCurrentIndex(mp3Settings_.vbrQuality);
        auto *channels = new QComboBox;
        channels->addItem(tr("Joint Stereo (recommended)"), 0);
        channels->addItem(tr("Stereo"), 1); channels->addItem(tr("Mono"), 2);
        channels->setCurrentIndex(static_cast<int>(mp3Settings_.channelMode));
        auto *quality = new QComboBox;
        quality->addItem(tr("Highest (slower)"), 0); quality->addItem(tr("High (recommended)"), 2);
        quality->addItem(tr("Fast"), 5);
        quality->setCurrentIndex(qMax(0, quality->findData(mp3Settings_.encoderQuality)));
        auto updateMode = [mode, bitrate, vbr] {
            const bool cbr = mode->currentData().toInt() == 0; bitrate->setEnabled(cbr); vbr->setEnabled(!cbr);
        };
        connect(mode, &QComboBox::currentIndexChanged, &dialog, updateMode); updateMode();
        form->addRow(tr("Rate control:"), mode); form->addRow(tr("CBR bitrate:"), bitrate);
        form->addRow(tr("VBR quality:"), vbr); form->addRow(tr("Channels:"), channels);
        form->addRow(tr("Encoder quality:"), quality);
        auto *sourceFormat = new QLabel(tr("Source format: 44.1 kHz, 16-bit CD audio"));
        sourceFormat->setWordWrap(true); layout->addLayout(form); layout->addWidget(sourceFormat);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject); layout->addWidget(buttons);
        if (dialog.exec() != QDialog::Accepted) return;
        mp3Settings_.rateMode = mode->currentData().toInt() == 1
            ? Mp3EncodingSettings::RateMode::VariableBitrate : Mp3EncodingSettings::RateMode::ConstantBitrate;
        mp3Settings_.bitrateKbps = bitrate->currentData().toInt();
        mp3Settings_.vbrQuality = vbr->currentData().toInt();
        mp3Settings_.channelMode = static_cast<Mp3EncodingSettings::ChannelMode>(channels->currentData().toInt());
        mp3Settings_.encoderQuality = quality->currentData().toInt();
        QSettings settings; settings.setValue(QStringLiteral("mp3/rateMode"), mode->currentData().toInt());
        settings.setValue(QStringLiteral("mp3/bitrateKbps"), mp3Settings_.bitrateKbps);
        settings.setValue(QStringLiteral("mp3/vbrQuality"), mp3Settings_.vbrQuality);
        settings.setValue(QStringLiteral("mp3/channelMode"), channels->currentData().toInt());
        settings.setValue(QStringLiteral("mp3/encoderQuality"), mp3Settings_.encoderQuality);
        encodingSummary_->setText(mp3Settings_.summary());
    }

    void populateCdTree()
    {
        cdTree_->clear();
        for (int index = 0; index < cdDisc_.tracks.size(); ++index) {
            const CdTrackInfo &track = cdDisc_.tracks.at(index);
            auto *item = new QTreeWidgetItem(cdTree_, {QString::number(track.number), track.title,
                track.artist, durationText(track.durationSeconds())});
            item->setData(0, Qt::UserRole, index);
        }
        cdTree_->selectAll();
        setCdControlsEnabled(true);
    }

    void showCdContextMenu(const QPoint &position)
    {
        QTreeWidgetItem *clicked = cdTree_->itemAt(position);
        if (!clicked) return;
        if (!clicked->isSelected()) {
            cdTree_->clearSelection(); clicked->setSelected(true); cdTree_->setCurrentItem(clicked);
        }
        QMenu menu(this);
        QAction *import = menu.addAction(style()->standardIcon(QStyle::SP_ArrowForward),
                                         tr("Import Selected Tracks to This PC"));
        import->setEnabled(!worker_ && !cdTree_->selectedItems().isEmpty());
        menu.addSeparator();
        QAction *selectAll = menu.addAction(tr("Select All Tracks"));
        QAction *settings = menu.addAction(tr("MP3 Settings…"));
        QAction *chosen = menu.exec(cdTree_->viewport()->mapToGlobal(position));
        if (chosen == import) importCdSelection();
        else if (chosen == selectAll) cdTree_->selectAll();
        else if (chosen == settings) configureMp3();
    }

    void showPcContextMenu(const QPoint &position)
    {
        const QModelIndex clicked = sourceTree_->indexAt(position);
        if (!clicked.isValid()) return;
        if (!sourceTree_->selectionModel()->isSelected(clicked)) {
            sourceTree_->selectionModel()->clearSelection();
            sourceTree_->selectionModel()->select(clicked,
                QItemSelectionModel::Select | QItemSelectionModel::Rows);
            sourceTree_->setCurrentIndex(clicked);
        }
        QMenu menu(this);
        QAction *information = menu.addAction(style()->standardIcon(QStyle::SP_MessageBoxInformation),
                                               tr("Show MP3 Information"));
        information->setEnabled(!selectedPcMp3Files().isEmpty());
        menu.addSeparator();
        QAction *transfer = menu.addAction(style()->standardIcon(QStyle::SP_ArrowForward),
                                            tr("Transfer Selected to Walkman"));
        transfer->setEnabled(!worker_ && db_.isPresent());
        QAction *chosen = menu.exec(sourceTree_->viewport()->mapToGlobal(position));
        if (chosen == information) showPcMp3Information();
        else if (chosen == transfer) transferSelection();
    }

    void showDeviceContextMenu(const QPoint &position)
    {
        QTreeWidgetItem *clicked = deviceTree_->itemAt(position);
        if (!clicked) return;
        const bool group = !clicked->data(StateColumn, SongRole).isValid();
        if (!group && !clicked->isSelected()) {
            deviceTree_->clearSelection(); clicked->setSelected(true); deviceTree_->setCurrentItem(clicked);
        }
        const int count = group ? clicked->childCount() : selectedDeviceSongs().size();
        if (count == 0) return;
        QMenu menu(this);
        QAction *restore = menu.addAction(style()->standardIcon(QStyle::SP_ArrowBack),
            group ? tr("Restore All Tracks in This Group") : tr("Restore Selected to This PC"));
        QAction *edit = menu.addAction(style()->standardIcon(QStyle::SP_FileDialogDetailedView),
                                        tr("Edit Track Info"));
        menu.addSeparator();
        QAction *remove = menu.addAction(style()->standardIcon(QStyle::SP_TrashIcon), group
            ? tr("Delete All Tracks in This Group") : tr("Delete Selected from Walkman"));
        restore->setEnabled(!worker_); edit->setEnabled(!worker_ && !group && count == 1);
        remove->setEnabled(!worker_);
        QAction *chosen = menu.exec(deviceTree_->viewport()->mapToGlobal(position));
        if (group && (chosen == restore || chosen == remove)) {
            deviceTree_->clearSelection();
            for (int index = 0; index < clicked->childCount(); ++index) clicked->child(index)->setSelected(true);
        }
        if (chosen == restore) restoreSelection();
        else if (chosen == edit) editSelected();
        else if (chosen == remove) removeSelected();
    }

    QStringList selectedPcMp3Files() const
    {
        QStringList files;
        for (const QModelIndex &index : sourceTree_->selectionModel()->selectedRows(0)) {
            const QFileInfo file(sourceModel_->filePath(index));
            if (file.isFile() && file.suffix().compare(QStringLiteral("mp3"), Qt::CaseInsensitive) == 0)
                files << file.absoluteFilePath();
        }
        files.removeDuplicates();
        return files;
    }

    void showPcMp3Information()
    {
        const QStringList files = selectedPcMp3Files();
        if (files.isEmpty()) return;
        QDialog dialog(this); dialog.setWindowTitle(tr("MP3 Information — %1 Track(s)").arg(files.size()));
        dialog.resize(1120, qMin(700, 180 + files.size() * 30));
        auto *layout = new QVBoxLayout(&dialog);
        auto *table = new QTreeWidget; table->setColumnCount(10);
        table->setHeaderLabels({tr("Title"), tr("Artist"), tr("Album"), tr("#"), tr("Duration"),
            tr("Bitrate"), tr("Sample Rate"), tr("Channels"), tr("Size"), tr("File")});
        table->setAlternatingRowColors(true); table->setSelectionBehavior(QAbstractItemView::SelectRows);
        for (const QString &file : files) {
            const SongData song = metadataForFile(file); const Mp3StreamInfo stream = mp3StreamInfo(file);
            auto *item = new QTreeWidgetItem(table, {song.title, song.artist, song.album,
                song.track > 0 ? QString::number(song.track) : QString(), durationText(song.duration),
                stream.bitrateKbps > 0 ? tr("%1 kbps").arg(stream.bitrateKbps) : tr("Unknown"),
                stream.sampleRate > 0 ? tr("%1 Hz").arg(stream.sampleRate) : tr("Unknown"),
                stream.channels == 1 ? tr("Mono") : stream.channels == 2 ? tr("Stereo") : tr("Unknown"),
                QLocale().formattedDataSize(QFileInfo(file).size()), file});
            item->setToolTip(9, file);
        }
        configureResizableHeader(table->header(), QStringLiteral("headers/mp3Information"),
                                  {220, 150, 190, 45, 75, 85, 100, 75, 85, 360});
        layout->addWidget(table);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        layout->addWidget(buttons); dialog.exec();
    }

    void refreshCd(bool showFailure)
    {
        if (worker_) return;
        const QString device = CdImporter::findDevice();
        CdDiscInfo disc; QString error;
        if (device.isEmpty() || !CdImporter::readDisc(device, &disc, &error)) {
            cdDisc_ = {}; cdTree_->clear(); setCdControlsEnabled(true);
            statusBar()->showMessage(tr("No readable audio CD detected"), 5000);
            if (showFailure) QMessageBox::warning(this, tr("Audio CD Not Found"),
                error.isEmpty() ? tr("No optical drive was found.") : error);
            return;
        }
        cdDisc_ = disc; populateCdTree();
        statusBar()->showMessage(tr("Looking up CD metadata on MusicBrainz…"));
        cdRefreshAction_->setEnabled(false);
        QNetworkReply *reply = network_->get(CdImporter::musicBrainzRequest(cdDisc_));
        connect(reply, &QNetworkReply::finished, this, [this, reply, disc] {
            const QByteArray payload = reply->readAll();
            const QString networkError = reply->error() == QNetworkReply::NoError ? QString() : reply->errorString();
            reply->deleteLater();
            if (cdDisc_.devicePath != disc.devicePath || cdDisc_.tracks.size() != disc.tracks.size()) return;
            QString metadataError;
            const QList<CdMetadataCandidate> candidates = networkError.isEmpty()
                ? CdImporter::parseMusicBrainzResults(payload, disc, &metadataError) : QList<CdMetadataCandidate>();
            if (candidates.isEmpty()) {
                statusBar()->showMessage(networkError.isEmpty() ? metadataError
                    : tr("MusicBrainz lookup failed: %1").arg(networkError), 8000);
                setCdControlsEnabled(true);
                return;
            }
            int selected = 0;
            if (candidates.size() > 1) {
                QStringList names; for (const CdMetadataCandidate &candidate : candidates) names << candidate.displayName();
                bool accepted = false;
                const QString choice = QInputDialog::getItem(this, tr("Select CD Metadata"),
                    tr("Multiple MusicBrainz releases match this CD:"), names, 0, false, &accepted);
                if (!accepted) { setCdControlsEnabled(true); return; }
                selected = names.indexOf(choice);
            }
            CdImporter::applyMetadata(&cdDisc_, candidates.at(qMax(0, selected)));
            populateCdTree();
            statusBar()->showMessage(tr("Loaded metadata from MusicBrainz: %1 — %2")
                .arg(cdDisc_.album, cdDisc_.albumArtist), 6000);
        });
    }

    void importCdSelection()
    {
        if (worker_) return;
        QList<CdTrackInfo> tracks;
        QSet<int> selectedIndexes;
        for (QTreeWidgetItem *item : cdTree_->selectedItems()) selectedIndexes.insert(item->data(0, Qt::UserRole).toInt());
        for (int index : selectedIndexes) if (index >= 0 && index < cdDisc_.tracks.size()) tracks << cdDisc_.tracks.at(index);
        std::sort(tracks.begin(), tracks.end(), [](const CdTrackInfo &left, const CdTrackInfo &right) {
            return left.number < right.number;
        });
        if (tracks.isEmpty()) {
            QMessageBox::information(this, tr("No CD Tracks Selected"), tr("Select one or more tracks in the Audio CD pane."));
            return;
        }
        const QString destination = musicPath_->text();
        if (!QFileInfo(destination).isDir() || !QFileInfo(destination).isWritable()) {
            QMessageBox::warning(this, tr("Cannot Save"), tr("The Music Folder is not writable: %1").arg(destination));
            return;
        }
        int duplicates = 0;
        for (const CdTrackInfo &track : tracks)
            if (QFileInfo::exists(CdImporter::outputPath(cdDisc_, track, destination))) ++duplicates;
        bool overwrite = false;
        if (duplicates > 0) {
            QMessageBox confirm(this); confirm.setIcon(QMessageBox::Question);
            confirm.setWindowTitle(tr("Files Already Exist"));
            confirm.setText(tr("%1 MP3 file(s) already exist at the destination. Overwrite them?").arg(duplicates));
            auto *overwriteButton = confirm.addButton(tr("Overwrite"), QMessageBox::AcceptRole);
            auto *skipButton = confirm.addButton(tr("Skip Duplicates"), QMessageBox::DestructiveRole);
            confirm.addButton(QMessageBox::Cancel); confirm.setDefaultButton(qobject_cast<QPushButton *>(skipButton));
            confirm.exec();
            if (confirm.clickedButton() == overwriteButton) overwrite = true;
            else if (confirm.clickedButton() != skipButton) return;
        } else if (QMessageBox::question(this, tr("Import Audio CD"),
            tr("Import %1 track(s) as MP3 files?\nFormat: %2\nDestination: %3")
                .arg(tracks.size()).arg(mp3Settings_.summary(), destination)) != QMessageBox::Yes) return;

        struct ImportResult { int completed = 0; int skipped = 0; int failed = 0; QString lastError; };
        const auto result = std::make_shared<ImportResult>();
        const CdDiscInfo disc = cdDisc_; const Mp3EncodingSettings encoding = mp3Settings_;
        setDeviceControlsEnabled(false); setCdControlsEnabled(false); sourceTree_->setEnabled(false);
        progress_->setRange(0, 0); progress_->setVisible(true);
        statusBar()->showMessage(tr("Reading the audio CD and encoding MP3 files…"));
        worker_ = QThread::create([disc, tracks, destination, encoding, overwrite, result] {
            for (const CdTrackInfo &track : tracks) {
                QString output, error;
                const CdRipResult status = CdImporter::ripTrack(disc, track, destination, encoding, overwrite, &output, &error);
                if (status == CdRipResult::Ok) ++result->completed;
                else if (status == CdRipResult::AlreadyExists) ++result->skipped;
                else { ++result->failed; result->lastError = error; }
            }
        });
        connect(worker_, &QThread::finished, this, [this, result, destination] {
            worker_->deleteLater(); worker_ = nullptr; progress_->setVisible(false);
            sourceTree_->setEnabled(true); setDeviceControlsEnabled(db_.isPresent()); setCdControlsEnabled(true);
            sourceTree_->setRootIndex(sourceModel_->setRootPath(destination)); sourceTree_->expandToDepth(1);
            QString message = tr("Imported: %1 track(s)\nSkipped: %2 track(s)\nFailed: %3 track(s)\nDestination: %4")
                .arg(result->completed).arg(result->skipped).arg(result->failed).arg(destination);
            if (!result->lastError.isEmpty()) message += tr("\n\nLast error: %1").arg(result->lastError);
            QMessageBox::information(this, tr("CD Import Complete"), message);
            statusBar()->showMessage(tr("CD import complete"), 5000);
        });
        worker_->start();
    }

    void chooseMusicFolder()
    {
        const QString folder = QFileDialog::getExistingDirectory(this, tr("Select Music Folder"), musicPath_->text());
        if (!folder.isEmpty()) setMusicFolder(folder);
    }

    void setMusicFolder(const QString &folder)
    {
        const QString path = QDir::cleanPath(folder); musicPath_->setText(path);
        QSettings().setValue(QStringLiteral("musicFolder"), path);
        sourceTree_->setRootIndex(sourceModel_->setRootPath(path)); sourceTree_->expandToDepth(0);
    }

    void chooseDevice()
    {
        const QString path = QFileDialog::getExistingDirectory(this, tr("Select Walkman Mount Point"));
        if (!path.isEmpty()) connectDevice(path);
    }

    void detectDevice(bool showFailure = true)
    {
        if (db_.detectPlayer()) {
            finishConnection(fromSony(db_.getDriveLetter()));
            return;
        }
        if (db_.detectPlayerStorage()) {
            initializeDevice(fromSony(db_.getDriveLetter()));
            return;
        }
        if (showFailure) QMessageBox::warning(this, tr("Device Not Found"),
            tr("No mounted Sony Network Walkman was detected.\n"
               "Reconnect the device or use Select Walkman to choose its mount point."));
    }

    void connectDevice(QString path)
    {
        path = QDir::cleanPath(path.trimmed()); QByteArray encoded = QFile::encodeName(path);
        if (!db_.detectPlayer(encoded.data())) {
            if (db_.detectPlayerStorage(encoded.data())) {
                initializeDevice(path);
                return;
            }
            QMessageBox::warning(this, tr("Cannot Connect"), tr("%1 is not a readable and writable mount point.").arg(path));
            return;
        }
        finishConnection(path);
    }

    void initializeDevice(const QString &path)
    {
        const auto answer = QMessageBox::warning(
            this, tr("Walkman Initialization Required"),
            tr("A Sony Network Walkman was detected at %1, but it has no music database.\n\n"
               "Create an empty OMGAUDIO database for SonyDb?\n"
               "Existing files will not be deleted.").arg(path),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;

        QApplication::setOverrideCursor(Qt::WaitCursor);
        const bool initialized = db_.initializePlayer();
        QApplication::restoreOverrideCursor();
        if (!initialized) {
            QMessageBox::critical(this, tr("Initialization Failed"),
                tr("The OMGAUDIO database could not be created on the Walkman.\n"
                   "Check the available space and write permissions."));
            return;
        }
        finishConnection(path);
    }

    void finishConnection(const QString &path)
    {
        mountedPath_ = path; QSettings().setValue(QStringLiteral("devicePath"), path);
        deviceState_->setText(tr("● Connected")); deviceState_->setStyleSheet(QStringLiteral("color: #21864b;"));
        devicePath_->setText(path); setDeviceControlsEnabled(true); reload();
    }

    void reload()
    {
        if (!db_.isPresent()) {
            setDeviceControlsEnabled(false); deviceTree_->clear(); deviceState_->setText(tr("● Disconnected"));
            deviceState_->setStyleSheet(QStringLiteral("color: #9b2335;")); return;
        }
        QApplication::setOverrideCursor(Qt::WaitCursor); const int count = db_.readAllTracks(); QApplication::restoreOverrideCursor();
        populateDeviceTree();
        devicePath_->setText(tr("%1  •  %2 tracks  •  Capacity %3  •  Free %4").arg(mountedPath_).arg(count)
            .arg(fromSony(db_.getTotalDiskSpace())).arg(fromSony(db_.getFreeDiskSpaceAfterApply())));
        statusBar()->showMessage(tr("Loaded the Walkman track list"), 4000);
    }

    QString groupName(const SongData &song) const
    {
        QString group;
        switch (groupBy_->currentIndex()) {
        case 0: group = song.album; break; case 1: group = song.artist; break;
        case 2: group = song.genre; break; default: return {};
        }
        return group.isEmpty() ? tr("(Not set)") : group;
    }

    void populateDeviceTree()
    {
        deviceTree_->setUpdatesEnabled(false); deviceTree_->setSortingEnabled(false); deviceTree_->clear();
        QMap<QString, QTreeWidgetItem *> groups;
        const std::vector<Song *> songs = db_.getSongs();
        for (Song *song : songs) {
            const SongData data = songData(song); QTreeWidgetItem *parent = nullptr; const QString group = groupName(data);
            if (!group.isEmpty()) {
                parent = groups.value(group);
                if (!parent) {
                    parent = new QTreeWidgetItem(deviceTree_, {group}); parent->setFirstColumnSpanned(true);
                    QFont font = parent->font(0); font.setWeight(QFont::DemiBold); parent->setFont(0, font);
                    parent->setForeground(0, QColor(QStringLiteral("#34435a"))); groups.insert(group, parent);
                }
            }
            auto *item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(deviceTree_);
            item->setText(StateColumn, statusText(data.status)); item->setText(TitleColumn, data.title);
            item->setText(ArtistColumn, data.artist); item->setText(AlbumColumn, data.album);
            item->setText(TrackColumn, QString::number(data.track)); item->setText(DurationColumn, durationText(data.duration));
            item->setData(StateColumn, SongRole, QVariant::fromValue(data)); item->setToolTip(TitleColumn, data.filename);
            if (data.status == ADD_TO_DEVICE) item->setForeground(StateColumn, QColor(QStringLiteral("#21864b")));
            else if (data.status == REMOVE_FROM_DEVICE) item->setForeground(StateColumn, QColor(QStringLiteral("#b63145")));
            releaseSong(song);
        }
        deviceTree_->setSortingEnabled(true); deviceTree_->sortItems(0, Qt::AscendingOrder); deviceTree_->expandAll();
        deviceTree_->setUpdatesEnabled(true); filterDeviceTree(search_->text());
    }

    void filterDeviceTree(const QString &text)
    {
        const QString needle = text.trimmed();
        for (int i = 0; i < deviceTree_->topLevelItemCount(); ++i) {
            QTreeWidgetItem *top = deviceTree_->topLevelItem(i);
            if (top->data(StateColumn, SongRole).isValid()) {
                top->setHidden(!needle.isEmpty() && !songMatches(top->data(StateColumn, SongRole).value<SongData>(), needle)); continue;
            }
            bool visibleGroup = false;
            for (int c = 0; c < top->childCount(); ++c) {
                QTreeWidgetItem *child = top->child(c);
                const bool visible = needle.isEmpty() || songMatches(child->data(StateColumn, SongRole).value<SongData>(), needle);
                child->setHidden(!visible); visibleGroup |= visible;
            }
            top->setHidden(!visibleGroup);
        }
    }

    static bool songMatches(const SongData &song, const QString &needle)
    {
        return song.title.contains(needle, Qt::CaseInsensitive) || song.artist.contains(needle, Qt::CaseInsensitive)
            || song.album.contains(needle, Qt::CaseInsensitive) || song.genre.contains(needle, Qt::CaseInsensitive);
    }

    void collectMp3Files(const QString &path, QStringList &files) const
    {
        const QFileInfo info(path);
        if (info.isFile()) {
            if (info.suffix().compare(QStringLiteral("mp3"), Qt::CaseInsensitive) == 0) files << info.absoluteFilePath();
            return;
        }
        QDirIterator iterator(path, {QStringLiteral("*.mp3"), QStringLiteral("*.MP3")}, QDir::Files, QDirIterator::Subdirectories);
        while (iterator.hasNext()) files << iterator.next();
    }

    void transferSelection()
    {
        QStringList files;
        for (const QModelIndex &index : sourceTree_->selectionModel()->selectedRows(0))
            collectMp3Files(sourceModel_->filePath(index), files);
        files.removeDuplicates(); transferFiles(files);
    }

    void transferFiles(const QStringList &files)
    {
        if (files.isEmpty()) { QMessageBox::information(this, tr("No Tracks Selected"), tr("Select MP3 files or folders in the This PC pane.")); return; }
        if (QMessageBox::question(this, tr("Transfer to Walkman"), tr("Transfer the selected %1 track(s) to the Walkman?").arg(files.size())) != QMessageBox::Yes) return;
        int added = 0; for (const QString &file : files) added += addSong(metadataForFile(file)) ? 1 : 0;
        populateDeviceTree();
        if (added == 0) { QMessageBox::information(this, tr("No Tracks to Add"), tr("The selected tracks are already registered.")); return; }
        applyChanges();
    }

    QString restoreDestination() const
    {
        const QModelIndex index = sourceTree_->currentIndex();
        if (!index.isValid()) return musicPath_->text();
        const QFileInfo selected(sourceModel_->filePath(index));
        return selected.isDir() ? selected.absoluteFilePath() : selected.absolutePath();
    }

    void restoreSelection()
    {
        const QList<SongData> selected = selectedDeviceSongs();
        QList<int> orders;
        int unsupportedTracks = 0;
        for (const SongData &song : selected) {
            if (song.status == ON_DEVICE && song.order > 0) {
                if ((song.encoding >> 24) != 0x03) ++unsupportedTracks;
                else orders << song.order;
            }
        }
        if (unsupportedTracks > 0) {
            QMessageBox::warning(this, tr("Unsupported Track Format"),
                tr("%1 selected track(s) use ATRAC/OpenMG audio. They cannot be restored as playable MP3 files without the original SonicStage encryption keys.")
                    .arg(unsupportedTracks));
            return;
        }
        if (orders.isEmpty()) {
            QMessageBox::information(this, tr("No Tracks Selected"),
                                     tr("Select tracks on the right to restore to this PC."));
            return;
        }

        const QString destination = restoreDestination();
        if (!QFileInfo(destination).isWritable()) {
            QMessageBox::warning(this, tr("Cannot Save"),
                                 tr("The destination is not writable: %1").arg(destination));
            return;
        }

        const QByteArray encodedDestination = QFile::encodeName(destination);
        int duplicateCount = 0;
        QSet<QString> outputPaths;
        for (int order : orders) {
            const std::string path = db_.exportPathForSong(order, encodedDestination.constData());
            const QString output = QFile::decodeName(path.c_str());
            if (QFileInfo::exists(output) || outputPaths.contains(output)) ++duplicateCount;
            outputPaths.insert(output);
        }

        bool overwrite = false;
        if (duplicateCount > 0) {
            QMessageBox confirm(this);
            confirm.setIcon(QMessageBox::Question);
            confirm.setWindowTitle(tr("Files Already Exist"));
            confirm.setText(tr("%1 MP3 file(s) already exist at the destination. Overwrite them?").arg(duplicateCount));
            auto *overwriteButton = confirm.addButton(tr("Overwrite"), QMessageBox::AcceptRole);
            auto *skipButton = confirm.addButton(tr("Skip Duplicates"), QMessageBox::DestructiveRole);
            confirm.addButton(QMessageBox::Cancel);
            confirm.setDefaultButton(qobject_cast<QPushButton *>(skipButton));
            confirm.exec();
            if (confirm.clickedButton() == overwriteButton) overwrite = true;
            else if (confirm.clickedButton() != skipButton) return;
        } else if (QMessageBox::question(this, tr("Restore to PC"),
                                         tr("Restore the selected %1 track(s) to this folder?\n%2")
                                             .arg(orders.size()).arg(destination)) != QMessageBox::Yes) {
            return;
        }

        struct ExportResult { int completed = 0; int skipped = 0; int failed = 0; };
        const auto result = std::make_shared<ExportResult>();
        const std::vector<int> exportOrders(orders.begin(), orders.end());
        const std::string outputDirectory = encodedDestination.constData();
        setDeviceControlsEnabled(false); setCdControlsEnabled(false); sourceTree_->setEnabled(false);
        progress_->setRange(0, 0); progress_->setVisible(true);
        deviceState_->setText(tr("● Restoring to PC"));
        worker_ = QThread::create([this, result, exportOrders, outputDirectory, overwrite] {
            for (int order : exportOrders) {
                const int status = db_.exportSong(order, outputDirectory.c_str(), overwrite);
                if (status == EXPORT_OK) ++result->completed;
                else if (status == EXPORT_ALREADY_EXISTS) ++result->skipped;
                else ++result->failed;
            }
        });
        connect(worker_, &QThread::finished, this, [this, result, destination] {
            worker_->deleteLater(); worker_ = nullptr; progress_->setVisible(false);
            sourceTree_->setEnabled(true); setDeviceControlsEnabled(db_.isPresent()); setCdControlsEnabled(true);
            deviceState_->setText(tr("● Connected"));
            QMessageBox::information(this, tr("Restore Complete"),
                tr("Restored: %1 track(s)\nSkipped: %2 track(s)\nFailed: %3 track(s)\nDestination: %4")
                    .arg(result->completed).arg(result->skipped).arg(result->failed).arg(destination));
        });
        worker_->start();
    }

    bool addSong(const SongData &data)
    {
        const QByteArray title = toSony(data.title), artist = toSony(data.artist), album = toSony(data.album);
        const QByteArray genre = toSony(data.genre), filename = QFile::encodeName(data.filename);
        Song song{};
        song.title = const_cast<char *>(title.constData()); song.artist = const_cast<char *>(artist.constData());
        song.album = const_cast<char *>(album.constData()); song.genre = const_cast<char *>(genre.constData());
        song.filename = const_cast<char *>(filename.constData()); song.track_nr = data.track;
        song.year = data.year; song.songlen = data.duration; song.statusOfSong = ADD_TO_DEVICE;
        return db_.addSongCopy(song);
    }

    QList<SongData> selectedDeviceSongs() const
    {
        QList<SongData> songs;
        for (QTreeWidgetItem *item : deviceTree_->selectedItems()) {
            const QVariant value = item->data(StateColumn, SongRole); if (value.isValid()) songs << value.value<SongData>();
        }
        return songs;
    }

    void editSelected()
    {
        const QList<SongData> selected = selectedDeviceSongs();
        if (selected.size() != 1) { QMessageBox::information(this, tr("Select a Track"), tr("Select one track to edit.")); return; }
        SongData data = selected.first(); QDialog dialog(this); dialog.setWindowTitle(tr("Edit Track Info"));
        auto *layout = new QVBoxLayout(&dialog); auto *form = new QFormLayout;
        auto *title = new QLineEdit(data.title), *artist = new QLineEdit(data.artist);
        auto *album = new QLineEdit(data.album), *genre = new QLineEdit(data.genre); auto *track = new QSpinBox;
        track->setRange(0, 9999); track->setValue(data.track);
        form->addRow(tr("Title:"), title); form->addRow(tr("Artist:"), artist);
        form->addRow(tr("Album:"), album); form->addRow(tr("Genre:"), genre); form->addRow(tr("Track number:"), track);
        layout->addLayout(form); auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject); layout->addWidget(buttons);
        if (dialog.exec() != QDialog::Accepted) return;
        const QByteArray newTitle = toSony(title->text()), newArtist = toSony(artist->text());
        const QByteArray newAlbum = toSony(album->text()), newGenre = toSony(genre->text()), filename = QFile::encodeName(data.filename);
        Song song{}; song.sonyDbOrder = data.order; song.statusOfSong = data.status;
        song.title = const_cast<char *>(newTitle.constData()); song.artist = const_cast<char *>(newArtist.constData());
        song.album = const_cast<char *>(newAlbum.constData()); song.genre = const_cast<char *>(newGenre.constData());
        song.filename = const_cast<char *>(filename.constData()); song.track_nr = track->value(); song.songlen = data.duration; song.year = data.year;
        if (db_.updateSong(data.order, filename.constData(), song)) { populateDeviceTree(); applyChanges(false); }
        else QMessageBox::warning(this, tr("Cannot Edit Track"), tr("The track information could not be updated."));
    }

    void removeSelected()
    {
        const QList<SongData> selected = selectedDeviceSongs(); if (selected.isEmpty()) return;
        if (QMessageBox::question(this, tr("Remove from Walkman"), tr("Remove the selected %1 track(s)?").arg(selected.size())) != QMessageBox::Yes) return;
        for (const SongData &data : selected) {
            const QByteArray title = toSony(data.title), artist = toSony(data.artist), album = toSony(data.album);
            const QByteArray filename = QFile::encodeName(data.filename); Song song{};
            song.sonyDbOrder = data.order; song.statusOfSong = data.status; song.title = const_cast<char *>(title.constData());
            song.artist = const_cast<char *>(artist.constData()); song.album = const_cast<char *>(album.constData());
            song.filename = const_cast<char *>(filename.constData()); db_.removeSong(data.order, filename.constData());
        }
        populateDeviceTree(); applyChanges(false);
    }

    QString backupDatabase(QString *error)
    {
        const QString source = mountedPath_ + QStringLiteral("/OMGAUDIO");
        const QString target = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
            + QStringLiteral("/backups/") + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"));
        if (!QDir().mkpath(target)) { *error = tr("Could not create the backup destination: %1").arg(target); return {}; }
        for (const QFileInfo &file : QDir(source).entryInfoList({QStringLiteral("*.DAT"), QStringLiteral("*.dat")}, QDir::Files)) {
            if (!QFile::copy(file.absoluteFilePath(), target + QLatin1Char('/') + file.fileName())) {
                *error = tr("Could not back up %1").arg(file.fileName()); return {};
            }
        }
        return target;
    }

    void applyChanges(bool showSuccess = true)
    {
        if (worker_ || !db_.hasPendingChanges()) return;
        if (db_.getNeededSpaceValue() > 0) {
            QMessageBox::warning(this, tr("Insufficient Space"), tr("The transfer requires %1 more free space.").arg(fromSony(db_.getNeededSpace()))); reload(); return;
        }
        QString backupError; const QString backup = backupDatabase(&backupError);
        if (backup.isEmpty()) { QMessageBox::critical(this, tr("Backup Failed"), backupError + tr("\nThe operation was canceled to protect the device database.")); reload(); return; }
        setDeviceControlsEnabled(false); setCdControlsEnabled(false); sourceTree_->setEnabled(false); progress_->setRange(0, 0); progress_->setVisible(true);
        deviceState_->setText(tr("● Transferring")); devicePath_->setText(tr("Do not disconnect the USB cable…"));
        const auto result = std::make_shared<bool>(false); worker_ = QThread::create([this, result] { *result = db_.writeTracks(); });
        connect(worker_, &QThread::finished, this, [this, result, backup, showSuccess] {
            worker_->deleteLater(); worker_ = nullptr; progress_->setVisible(false); sourceTree_->setEnabled(true);
            setDeviceControlsEnabled(db_.isPresent()); setCdControlsEnabled(true);
            if (*result) {
                deviceState_->setText(tr("● Connected")); reload();
                if (showSuccess) QMessageBox::information(this, tr("Transfer Complete"),
                    tr("The transfer is complete. Use Ubuntu's eject or unmount action before disconnecting the USB cable.\nDatabase backup: %1").arg(backup));
            } else {
                deviceState_->setText(tr("● Transfer Failed")); QMessageBox::critical(this, tr("Operation Failed"),
                    tr("The operation could not be completed. Keep the device connected and check the connection.\nDatabase backup: %1").arg(backup));
            }
        });
        worker_->start();
    }

    SonyDb db_;
    QFileSystemModel *sourceModel_ = nullptr; QTreeView *sourceTree_ = nullptr;
    QTreeWidget *cdTree_ = nullptr; QTreeWidget *deviceTree_ = nullptr;
    QLineEdit *musicPath_ = nullptr; QLabel *deviceState_ = nullptr; QLabel *devicePath_ = nullptr;
    QComboBox *groupBy_ = nullptr; QLineEdit *search_ = nullptr; QLabel *encodingSummary_ = nullptr;
    QToolButton *cdImportButton_ = nullptr; QToolButton *transferButton_ = nullptr; QToolButton *restoreButton_ = nullptr;
    QProgressBar *progress_ = nullptr; QAction *editAction_ = nullptr; QAction *deleteAction_ = nullptr;
    QAction *cdRefreshAction_ = nullptr; QAction *mp3SettingsAction_ = nullptr; QAction *refreshAction_ = nullptr;
    QThread *worker_ = nullptr; QNetworkAccessManager *network_ = nullptr;
    CdDiscInfo cdDisc_; Mp3EncodingSettings mp3Settings_; QString mountedPath_;
};

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("SonyDb GUI"));
    application.setOrganizationName(QStringLiteral("SonyDb"));
    application.setDesktopFileName(QStringLiteral("sonydb-gui"));
    MainWindow window; window.show(); return application.exec();
}
