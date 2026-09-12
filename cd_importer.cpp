#include "cd_importer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <cerrno>
#include <cstring>

#ifdef Q_OS_LINUX
#include <fcntl.h>
#include <linux/cdrom.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace {

constexpr int AudioSectorBytes = 2352;

QString artistCredit(const QJsonArray &credit)
{
    QString value;
    for (const QJsonValue &partValue : credit) {
        const QJsonObject part = partValue.toObject();
        QString name = part.value(QStringLiteral("name")).toString();
        if (name.isEmpty()) name = part.value(QStringLiteral("artist")).toObject()
                                      .value(QStringLiteral("name")).toString();
        value += name;
        value += part.value(QStringLiteral("joinphrase")).toString();
    }
    return value.trimmed();
}

QString safePathPart(QString value, const QString &fallback)
{
    value = value.trimmed();
    for (QChar &character : value) {
        if (character == QLatin1Char('/') || character == QLatin1Char('\\')
            || character.unicode() < 0x20) character = QLatin1Char('_');
    }
    while (value.endsWith(QLatin1Char('.')) || value.endsWith(QLatin1Char(' '))) value.chop(1);
    if (value.isEmpty() || value == QStringLiteral(".") || value == QStringLiteral("..")) return fallback;
    return value;
}

QString tocString(const CdDiscInfo &disc)
{
    if (disc.tracks.isEmpty()) return {};
    QStringList fields{QStringLiteral("1"), QString::number(disc.tracks.size()),
                       QString::number(disc.tracks.constLast().endSector + 150)};
    for (const CdTrackInfo &track : disc.tracks)
        fields << QString::number(track.startSector + 150);
    return fields.join(QLatin1Char('+'));
}

} // namespace

QString CdMetadataCandidate::displayName() const
{
    QStringList details;
    if (!date.isEmpty()) details << date;
    if (!country.isEmpty()) details << country;
    return QStringLiteral("%1 — %2%3").arg(album, albumArtist,
        details.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(details.join(QStringLiteral(", "))));
}

QString Mp3EncodingSettings::summary() const
{
    const QString rate = rateMode == RateMode::ConstantBitrate
        ? QStringLiteral("CBR %1 kbps").arg(bitrateKbps)
        : QStringLiteral("VBR V%1").arg(vbrQuality);
    QString channels = QStringLiteral("Joint Stereo");
    if (channelMode == ChannelMode::Stereo) channels = QStringLiteral("Stereo");
    else if (channelMode == ChannelMode::Mono) channels = QStringLiteral("Mono");
    return QStringLiteral("%1 · %2 · 44.1 kHz").arg(rate, channels);
}

QString CdImporter::pcmSampleFormat()
{
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
    return QStringLiteral("s16le");
#else
    return QStringLiteral("s16be");
#endif
}

QString CdImporter::findDevice()
{
#ifdef Q_OS_LINUX
    const QStringList candidates = [] {
        QStringList paths{QStringLiteral("/dev/cdrom"), QStringLiteral("/dev/dvd")};
        for (int i = 0; i < 16; ++i) paths << QStringLiteral("/dev/sr%1").arg(i);
        paths.removeDuplicates();
        return paths;
    }();
    for (const QString &path : candidates) {
        if (QFileInfo::exists(path)) return QFileInfo(path).canonicalFilePath().isEmpty()
            ? path : QFileInfo(path).canonicalFilePath();
    }
#endif
    return {};
}

bool CdImporter::readDisc(const QString &devicePath, CdDiscInfo *disc, QString *error)
{
    if (!disc) return false;
#ifndef Q_OS_LINUX
    if (error) *error = QStringLiteral("Audio CD reading is supported on Linux only.");
    return false;
#else
    const QByteArray encoded = QFile::encodeName(devicePath);
    const int fd = ::open(encoded.constData(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        if (error) *error = QStringLiteral("Cannot open %1: %2").arg(devicePath, QString::fromLocal8Bit(std::strerror(errno)));
        return false;
    }
    struct CloseFd { int fd; ~CloseFd() { if (fd >= 0) ::close(fd); } } closer{fd};

    cdrom_tochdr header{};
    if (::ioctl(fd, CDROMREADTOCHDR, &header) < 0) {
        if (error) *error = QStringLiteral("No readable audio CD was found in %1: %2")
            .arg(devicePath, QString::fromLocal8Bit(std::strerror(errno)));
        return false;
    }

    struct TocTrack { int number; int startSector; bool audio; };
    QList<TocTrack> tocTracks;
    for (int number = header.cdth_trk0; number <= header.cdth_trk1; ++number) {
        cdrom_tocentry entry{};
        entry.cdte_track = static_cast<unsigned char>(number);
        entry.cdte_format = CDROM_LBA;
        if (::ioctl(fd, CDROMREADTOCENTRY, &entry) < 0) {
            if (error) *error = QStringLiteral("Could not read the CD table of contents: %1")
                .arg(QString::fromLocal8Bit(std::strerror(errno)));
            return false;
        }
        tocTracks << TocTrack{number, entry.cdte_addr.lba, !(entry.cdte_ctrl & CDROM_DATA_TRACK)};
    }
    cdrom_tocentry leadout{};
    leadout.cdte_track = CDROM_LEADOUT;
    leadout.cdte_format = CDROM_LBA;
    const bool hasAudio = std::any_of(tocTracks.cbegin(), tocTracks.cend(),
        [](const TocTrack &track) { return track.audio; });
    if (::ioctl(fd, CDROMREADTOCENTRY, &leadout) < 0 || !hasAudio) {
        if (error) *error = !hasAudio ? QStringLiteral("The disc contains no audio tracks.")
            : QStringLiteral("Could not read the CD lead-out sector.");
        return false;
    }

    CdDiscInfo result;
    result.devicePath = devicePath;
    for (int index = 0; index < tocTracks.size(); ++index) {
        if (!tocTracks.at(index).audio) continue;
        CdTrackInfo track;
        track.number = tocTracks.at(index).number;
        track.startSector = tocTracks.at(index).startSector;
        track.endSector = index + 1 < tocTracks.size() ? tocTracks.at(index + 1).startSector : leadout.cdte_addr.lba;
        track.title = QStringLiteral("Track %1").arg(track.number, 2, 10, QLatin1Char('0'));
        track.artist = result.albumArtist;
        result.tracks << track;
    }
    *disc = result;
    return true;
#endif
}

QNetworkRequest CdImporter::musicBrainzRequest(const CdDiscInfo &disc)
{
    QUrl url(QStringLiteral("https://musicbrainz.org/ws/2/discid/-"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("toc"), tocString(disc));
    query.addQueryItem(QStringLiteral("inc"), QStringLiteral("recordings+artist-credits"));
    query.addQueryItem(QStringLiteral("cdstubs"), QStringLiteral("no"));
    query.addQueryItem(QStringLiteral("fmt"), QStringLiteral("json"));
    url.setQuery(query);
    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("User-Agent", "SonyDb-GUI/0.2 (https://github.com/mattn/sonydb)");
    return request;
}

QList<CdMetadataCandidate> CdImporter::parseMusicBrainzResults(
    const QByteArray &json, const CdDiscInfo &disc, QString *error)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (document.isNull() || !document.isObject()) {
        if (error) *error = QStringLiteral("MusicBrainz returned invalid data: %1").arg(parseError.errorString());
        return {};
    }

    QList<CdMetadataCandidate> candidates;
    const QJsonArray releases = document.object().value(QStringLiteral("releases")).toArray();
    for (const QJsonValue &releaseValue : releases) {
        const QJsonObject release = releaseValue.toObject();
        const QString releaseArtist = artistCredit(release.value(QStringLiteral("artist-credit")).toArray());
        for (const QJsonValue &mediumValue : release.value(QStringLiteral("media")).toArray()) {
            const QJsonObject medium = mediumValue.toObject();
            const QJsonArray tracks = medium.value(QStringLiteral("tracks")).toArray();
            const int trackCount = medium.value(QStringLiteral("track-count")).toInt(tracks.size());
            if (trackCount != disc.tracks.size() || tracks.size() != disc.tracks.size()) continue;

            CdMetadataCandidate candidate;
            candidate.album = release.value(QStringLiteral("title")).toString(QStringLiteral("Audio CD"));
            candidate.albumArtist = releaseArtist.isEmpty() ? QStringLiteral("Unknown Artist") : releaseArtist;
            candidate.date = release.value(QStringLiteral("date")).toString();
            candidate.country = release.value(QStringLiteral("country")).toString();
            candidate.releaseId = release.value(QStringLiteral("id")).toString();
            for (int index = 0; index < tracks.size(); ++index) {
                const QJsonObject trackObject = tracks.at(index).toObject();
                const QJsonObject recording = trackObject.value(QStringLiteral("recording")).toObject();
                CdTrackInfo track = disc.tracks.at(index);
                track.number = trackObject.value(QStringLiteral("position")).toInt(track.number);
                track.title = trackObject.value(QStringLiteral("title")).toString();
                if (track.title.isEmpty()) track.title = recording.value(QStringLiteral("title")).toString(track.title);
                track.artist = artistCredit(trackObject.value(QStringLiteral("artist-credit")).toArray());
                if (track.artist.isEmpty()) track.artist = artistCredit(recording.value(QStringLiteral("artist-credit")).toArray());
                if (track.artist.isEmpty()) track.artist = candidate.albumArtist;
                candidate.tracks << track;
            }
            candidates << candidate;
        }
    }
    if (candidates.isEmpty() && error)
        *error = QStringLiteral("No MusicBrainz release matched this CD table of contents.");
    return candidates;
}

void CdImporter::applyMetadata(CdDiscInfo *disc, const CdMetadataCandidate &candidate)
{
    if (!disc || candidate.tracks.size() != disc->tracks.size()) return;
    disc->album = candidate.album;
    disc->albumArtist = candidate.albumArtist;
    disc->year = candidate.date.left(4).toInt();
    for (int index = 0; index < disc->tracks.size(); ++index) {
        disc->tracks[index].title = candidate.tracks.at(index).title;
        disc->tracks[index].artist = candidate.tracks.at(index).artist;
        disc->tracks[index].number = candidate.tracks.at(index).number;
    }
}

QString CdImporter::outputPath(const CdDiscInfo &disc, const CdTrackInfo &track,
                               const QString &musicFolder)
{
    const QString artist = safePathPart(disc.albumArtist, QStringLiteral("Unknown Artist"));
    const QString album = safePathPart(disc.album, QStringLiteral("Unknown Album"));
    const QString title = safePathPart(track.title, QStringLiteral("Track %1").arg(track.number));
    return QDir(musicFolder).filePath(QStringLiteral("%1/%2/%3 - %4.mp3")
        .arg(artist, album, QString::number(track.number).rightJustified(2, QLatin1Char('0')), title));
}

CdRipResult CdImporter::ripTrack(const CdDiscInfo &disc, const CdTrackInfo &track,
                                 const QString &musicFolder, const Mp3EncodingSettings &settings,
                                 bool overwrite, QString *outputFile, QString *error)
{
    const QString output = outputPath(disc, track, musicFolder);
    if (outputFile) *outputFile = output;
    if (QFileInfo::exists(output) && !overwrite) return CdRipResult::AlreadyExists;
    if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()) {
        if (error) *error = QStringLiteral("FFmpeg is not installed. Install it with: sudo apt install ffmpeg");
        return CdRipResult::Failed;
    }
    if (!QDir().mkpath(QFileInfo(output).absolutePath())) {
        if (error) *error = QStringLiteral("Could not create the output folder: %1").arg(QFileInfo(output).absolutePath());
        return CdRipResult::Failed;
    }

    const QString temporary = output + QStringLiteral(".sonydb-part");
    QFile::remove(temporary);
    // Linux CDROMREADAUDIO supplies host-order samples. A wrong byte order
    // swaps every 16-bit sample into full-scale noise.
    QStringList arguments{QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-f"), pcmSampleFormat(), QStringLiteral("-ar"), QStringLiteral("44100"),
        QStringLiteral("-ac"), QStringLiteral("2"), QStringLiteral("-i"), QStringLiteral("pipe:0"),
        QStringLiteral("-map_metadata"), QStringLiteral("-1"), QStringLiteral("-codec:a"), QStringLiteral("libmp3lame")};
    if (settings.rateMode == Mp3EncodingSettings::RateMode::ConstantBitrate)
        arguments << QStringLiteral("-b:a") << QStringLiteral("%1k").arg(settings.bitrateKbps);
    else
        arguments << QStringLiteral("-q:a") << QString::number(settings.vbrQuality);
    arguments << QStringLiteral("-compression_level") << QString::number(settings.encoderQuality);
    if (settings.channelMode == Mp3EncodingSettings::ChannelMode::Mono)
        arguments << QStringLiteral("-ac") << QStringLiteral("1");
    else
        arguments << QStringLiteral("-joint_stereo")
                  << (settings.channelMode == Mp3EncodingSettings::ChannelMode::JointStereo
                      ? QStringLiteral("1") : QStringLiteral("0"));
    arguments << QStringLiteral("-id3v2_version") << QStringLiteral("3")
              << QStringLiteral("-metadata") << QStringLiteral("title=%1").arg(track.title)
              << QStringLiteral("-metadata") << QStringLiteral("artist=%1").arg(track.artist)
              << QStringLiteral("-metadata") << QStringLiteral("album=%1").arg(disc.album)
              << QStringLiteral("-metadata") << QStringLiteral("album_artist=%1").arg(disc.albumArtist)
              << QStringLiteral("-metadata") << QStringLiteral("track=%1/%2").arg(track.number).arg(disc.tracks.size());
    if (disc.year > 0) arguments << QStringLiteral("-metadata") << QStringLiteral("date=%1").arg(disc.year);
    arguments << QStringLiteral("-f") << QStringLiteral("mp3") << temporary;

    QProcess encoder;
    encoder.setProgram(QStringLiteral("ffmpeg"));
    encoder.setArguments(arguments);
    encoder.start();
    if (!encoder.waitForStarted()) {
        if (error) *error = QStringLiteral("Could not start FFmpeg: %1").arg(encoder.errorString());
        return CdRipResult::Failed;
    }

#ifndef Q_OS_LINUX
    encoder.kill(); encoder.waitForFinished();
    if (error) *error = QStringLiteral("Audio CD reading is supported on Linux only.");
    return CdRipResult::Failed;
#else
    const QByteArray encodedDevice = QFile::encodeName(disc.devicePath);
    const int fd = ::open(encodedDevice.constData(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        encoder.kill(); encoder.waitForFinished();
        if (error) *error = QStringLiteral("Cannot open %1: %2").arg(disc.devicePath, QString::fromLocal8Bit(std::strerror(errno)));
        return CdRipResult::Failed;
    }
    struct CloseFd { int fd; ~CloseFd() { if (fd >= 0) ::close(fd); } } closer{fd};

    QByteArray audio(AudioSectorBytes * 16, Qt::Uninitialized);
    int sector = track.startSector;
    bool readOk = true;
    while (sector < track.endSector) {
        const int frames = std::min(16, track.endSector - sector);
        cdrom_read_audio request{};
        request.addr.lba = sector;
        request.addr_format = CDROM_LBA;
        request.nframes = frames;
        request.buf = reinterpret_cast<unsigned char *>(audio.data());
        if (::ioctl(fd, CDROMREADAUDIO, &request) < 0) {
            if (error) *error = QStringLiteral("Could not read track %1 at sector %2: %3")
                .arg(track.number).arg(sector).arg(QString::fromLocal8Bit(std::strerror(errno)));
            readOk = false;
            break;
        }
        qint64 written = 0;
        const qint64 bytes = static_cast<qint64>(frames) * AudioSectorBytes;
        while (written < bytes) {
            const qint64 amount = encoder.write(audio.constData() + written, bytes - written);
            if (amount < 0 || (!encoder.waitForBytesWritten(30000) && encoder.state() == QProcess::NotRunning)) {
                if (error) *error = QStringLiteral("FFmpeg stopped while encoding track %1.").arg(track.number);
                readOk = false;
                break;
            }
            written += qMax<qint64>(0, amount);
        }
        if (!readOk) break;
        sector += frames;
    }
    encoder.closeWriteChannel();
    if (!readOk) encoder.kill();
    if (!encoder.waitForFinished(-1) || encoder.exitStatus() != QProcess::NormalExit || encoder.exitCode() != 0) {
        if (readOk && error) {
            const QString ffmpegError = QString::fromLocal8Bit(encoder.readAllStandardError()).trimmed();
            *error = ffmpegError.isEmpty() ? QStringLiteral("FFmpeg could not encode track %1.").arg(track.number) : ffmpegError;
        }
        QFile::remove(temporary);
        return CdRipResult::Failed;
    }
    if (!readOk) { QFile::remove(temporary); return CdRipResult::Failed; }

    const QByteArray oldName = QFile::encodeName(temporary);
    const QByteArray newName = QFile::encodeName(output);
    if (::rename(oldName.constData(), newName.constData()) != 0) {
        if (error) *error = QStringLiteral("Could not finalize %1: %2").arg(output, QString::fromLocal8Bit(std::strerror(errno)));
        QFile::remove(temporary);
        return CdRipResult::Failed;
    }
    return CdRipResult::Ok;
#endif
}
