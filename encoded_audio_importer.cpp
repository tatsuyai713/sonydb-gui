#include "encoded_audio_importer.h"
#include "library_path.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

namespace {

QStringList encodingArguments(const QString &sourceFile, const QString &temporary,
                              const EncodedImportTrack &track,
                              const Mp3EncodingSettings &settings)
{
    QStringList arguments{QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-i"), sourceFile, QStringLiteral("-vn"), QStringLiteral("-map_metadata"), QStringLiteral("-1"),
        QStringLiteral("-codec:a"), QStringLiteral("libmp3lame"), QStringLiteral("-ar"), QStringLiteral("44100")};
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
              << QStringLiteral("-metadata") << QStringLiteral("album=%1").arg(track.album)
              << QStringLiteral("-metadata") << QStringLiteral("album_artist=%1").arg(track.albumArtist);
    if (track.trackNumber > 0) {
        const QString number = track.totalTracks > 0
            ? QStringLiteral("%1/%2").arg(track.trackNumber).arg(track.totalTracks)
            : QString::number(track.trackNumber);
        arguments << QStringLiteral("-metadata") << QStringLiteral("track=%1").arg(number);
    }
    if (track.discNumber > 0)
        arguments << QStringLiteral("-metadata") << QStringLiteral("disc=%1").arg(track.discNumber);
    arguments << QStringLiteral("-f") << QStringLiteral("mp3") << temporary;
    return arguments;
}

QStringList oggTaggingArguments(const QString &sourceFile, const QString &temporary,
                                const EncodedImportTrack &track)
{
    QStringList arguments{QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-y"), QStringLiteral("-i"), sourceFile,
        QStringLiteral("-map"), QStringLiteral("0:a:0"), QStringLiteral("-vn"),
        QStringLiteral("-map_metadata"), QStringLiteral("-1"),
        QStringLiteral("-codec:a"), QStringLiteral("copy"),
        QStringLiteral("-metadata"), QStringLiteral("title=%1").arg(track.title),
        QStringLiteral("-metadata"), QStringLiteral("artist=%1").arg(track.artist),
        QStringLiteral("-metadata"), QStringLiteral("album=%1").arg(track.album),
        QStringLiteral("-metadata"), QStringLiteral("album_artist=%1").arg(track.albumArtist)};
    if (track.trackNumber > 0) {
        const QString number = track.totalTracks > 0
            ? QStringLiteral("%1/%2").arg(track.trackNumber).arg(track.totalTracks)
            : QString::number(track.trackNumber);
        arguments << QStringLiteral("-metadata") << QStringLiteral("track=%1").arg(number);
    }
    if (track.discNumber > 0)
        arguments << QStringLiteral("-metadata") << QStringLiteral("disc=%1").arg(track.discNumber);
    arguments << QStringLiteral("-f") << QStringLiteral("ogg") << temporary;
    return arguments;
}

} // namespace

QString EncodedAudioImporter::outputPath(const EncodedImportTrack &track, const QString &musicFolder)
{
    return outputPath(track, musicFolder, QStringLiteral("mp3"));
}

QString EncodedAudioImporter::outputPath(const EncodedImportTrack &track, const QString &musicFolder,
                                         const QString &extension)
{
    const QString artistPart = LibraryPath::safePart(track.albumArtist, QStringLiteral("Unknown Artist"));
    const QString artist = LibraryPath::existingDirectoryName(musicFolder, artistPart);
    const QString artistPath = QDir(musicFolder).filePath(artist);
    const QString albumPart = LibraryPath::safePart(track.album, QStringLiteral("Unknown Album"));
    const QString album = LibraryPath::existingDirectoryName(artistPath, albumPart);
    const QString title = LibraryPath::safePart(track.title, QStringLiteral("Unknown Track"));
    QString number = QString::number(qMax(0, track.trackNumber)).rightJustified(2, QLatin1Char('0'));
    if (track.discNumber > 1) number = QStringLiteral("%1-%2").arg(track.discNumber).arg(number);
    const QString suffix = extension.startsWith(QLatin1Char('.')) ? extension.mid(1) : extension;
    const QString albumPath = QDir(artistPath).filePath(album);
    const QString filename = LibraryPath::existingFileName(albumPath,
        QStringLiteral("%1 - %2.%3").arg(number, title, suffix));
    return QDir(albumPath).filePath(filename);
}

EncodedImportResult EncodedAudioImporter::placeLocalFile(
    const QString &sourceFile, const EncodedImportTrack &track,
    const QString &musicFolder, const QString &extension, bool overwrite,
    QString *outputFile, QString *error)
{
    const QFileInfo source(sourceFile);
    if (!source.isFile() || !source.isReadable() || source.isSymLink()) {
        if (error) *error = QStringLiteral("The audio input is not a readable local file: %1").arg(sourceFile);
        return EncodedImportResult::Failed;
    }
    const QString output = outputPath(track, musicFolder, extension);
    if (outputFile) *outputFile = output;
    if (QFileInfo::exists(output) && !overwrite) return EncodedImportResult::AlreadyExists;
    if (!QDir().mkpath(QFileInfo(output).absolutePath())) {
        if (error) *error = QStringLiteral("Could not create the output folder: %1").arg(QFileInfo(output).absolutePath());
        return EncodedImportResult::Failed;
    }
    const QString temporary = output + QStringLiteral(".sonydb-part");
    QFile::remove(temporary);
    if (extension.compare(QStringLiteral("ogg"), Qt::CaseInsensitive) == 0) {
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()) {
            if (error) *error = QStringLiteral("FFmpeg is required to write Ogg metadata. Install it with: sudo apt install ffmpeg");
            return EncodedImportResult::Failed;
        }
        QProcess tagger;
        tagger.start(QStringLiteral("ffmpeg"), oggTaggingArguments(
            source.absoluteFilePath(), temporary, track));
        if (!tagger.waitForStarted() || !tagger.waitForFinished(-1)
            || tagger.exitStatus() != QProcess::NormalExit || tagger.exitCode() != 0) {
            const QString details = QString::fromLocal8Bit(tagger.readAllStandardError()).trimmed();
            if (error) *error = details.isEmpty()
                ? QStringLiteral("FFmpeg could not write metadata to the Ogg file.") : details;
            QFile::remove(temporary);
            return EncodedImportResult::Failed;
        }
    } else if (!QFile::copy(source.absoluteFilePath(), temporary)) {
        if (error) *error = QStringLiteral("Could not copy the audio artifact to: %1").arg(temporary);
        QFile::remove(temporary);
        return EncodedImportResult::Failed;
    }
    if (overwrite && QFileInfo::exists(output) && !QFile::remove(output)) {
        if (error) *error = QStringLiteral("Could not replace the existing file: %1").arg(output);
        QFile::remove(temporary);
        return EncodedImportResult::Failed;
    }
    if (!QFile::rename(temporary, output)) {
        if (error) *error = QStringLiteral("Could not finalize the audio file: %1").arg(output);
        QFile::remove(temporary);
        return EncodedImportResult::Failed;
    }
    return EncodedImportResult::Ok;
}

EncodedImportResult EncodedAudioImporter::encodeLocalFile(
    const QString &sourceFile, const EncodedImportTrack &track,
    const QString &musicFolder, const Mp3EncodingSettings &settings,
    bool overwrite, QString *outputFile, QString *error)
{
    const QFileInfo source(sourceFile);
    if (!source.isFile() || !source.isReadable()) {
        if (error) *error = QStringLiteral("The audio input is not a readable local file: %1").arg(sourceFile);
        return EncodedImportResult::Failed;
    }
    const QString output = outputPath(track, musicFolder);
    if (outputFile) *outputFile = output;
    if (QFileInfo::exists(output) && !overwrite) return EncodedImportResult::AlreadyExists;
    if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()) {
        if (error) *error = QStringLiteral("FFmpeg is not installed. Install it with: sudo apt install ffmpeg");
        return EncodedImportResult::Failed;
    }
    if (!QDir().mkpath(QFileInfo(output).absolutePath())) {
        if (error) *error = QStringLiteral("Could not create the output folder: %1").arg(QFileInfo(output).absolutePath());
        return EncodedImportResult::Failed;
    }
    const QString temporary = output + QStringLiteral(".sonydb-part");
    QFile::remove(temporary);
    QProcess encoder;
    encoder.start(QStringLiteral("ffmpeg"), encodingArguments(source.absoluteFilePath(), temporary, track, settings));
    if (!encoder.waitForStarted() || !encoder.waitForFinished(-1)
        || encoder.exitStatus() != QProcess::NormalExit || encoder.exitCode() != 0) {
        const QString details = QString::fromLocal8Bit(encoder.readAllStandardError()).trimmed();
        if (error) *error = details.isEmpty() ? QStringLiteral("FFmpeg could not encode the audio input.") : details;
        QFile::remove(temporary);
        return EncodedImportResult::Failed;
    }
    if (overwrite && QFileInfo::exists(output) && !QFile::remove(output)) {
        if (error) *error = QStringLiteral("Could not replace the existing file: %1").arg(output);
        QFile::remove(temporary);
        return EncodedImportResult::Failed;
    }
    if (!QFile::rename(temporary, output)) {
        if (error) *error = QStringLiteral("Could not finalize the encoded file: %1").arg(output);
        QFile::remove(temporary);
        return EncodedImportResult::Failed;
    }
    return EncodedImportResult::Ok;
}

EncodedImportSummary EncodedAudioImporter::importLocalFiles(
    const QList<EncodedImportJob> &jobs, const QString &musicFolder,
    const Mp3EncodingSettings &settings, bool overwrite)
{
    EncodedImportSummary summary;
    for (const EncodedImportJob &job : jobs) {
        QString output;
        QString error;
        switch (encodeLocalFile(job.sourceFile, job.track, musicFolder, settings,
                                overwrite, &output, &error)) {
        case EncodedImportResult::Ok:
            ++summary.completed;
            break;
        case EncodedImportResult::AlreadyExists:
            ++summary.skipped;
            break;
        case EncodedImportResult::Failed:
            ++summary.failed;
            summary.lastError = error;
            break;
        }
    }
    return summary;
}
