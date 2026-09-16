#include "local_audio.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QtMath>

namespace {

QString tagValue(const QJsonObject &tags, const QString &name)
{
    for (auto it = tags.constBegin(); it != tags.constEnd(); ++it)
        if (it.key().compare(name, Qt::CaseInsensitive) == 0) return it.value().toString().trimmed();
    return {};
}

QStringList mp3Arguments(const QString &source, const QString &output,
                         const Mp3EncodingSettings &settings)
{
    QStringList arguments{QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-y"), QStringLiteral("-i"), source, QStringLiteral("-map"), QStringLiteral("0:a:0"),
        QStringLiteral("-vn"), QStringLiteral("-map_metadata"), QStringLiteral("0"),
        QStringLiteral("-codec:a"), QStringLiteral("libmp3lame"),
        QStringLiteral("-ar"), QStringLiteral("44100")};
    if (settings.rateMode == Mp3EncodingSettings::RateMode::ConstantBitrate)
        arguments << QStringLiteral("-b:a") << QStringLiteral("%1k").arg(settings.bitrateKbps);
    else
        arguments << QStringLiteral("-q:a") << QString::number(settings.vbrQuality);
    arguments << QStringLiteral("-compression_level") << QString::number(settings.encoderQuality);
    if (settings.channelMode == Mp3EncodingSettings::ChannelMode::Mono)
        arguments << QStringLiteral("-ac") << QStringLiteral("1");
    else
        arguments << QStringLiteral("-ac") << QStringLiteral("2")
                  << QStringLiteral("-joint_stereo")
                  << (settings.channelMode == Mp3EncodingSettings::ChannelMode::JointStereo
                      ? QStringLiteral("1") : QStringLiteral("0"));
    arguments << QStringLiteral("-id3v2_version") << QStringLiteral("3")
              << QStringLiteral("-f") << QStringLiteral("mp3") << output;
    return arguments;
}

} // namespace

QStringList LocalAudio::nameFilters()
{
    return {QStringLiteral("*.mp3"), QStringLiteral("*.MP3"),
            QStringLiteral("*.flac"), QStringLiteral("*.FLAC"),
            QStringLiteral("*.ogg"), QStringLiteral("*.OGG"),
            QStringLiteral("*.oga"), QStringLiteral("*.OGA"),
            QStringLiteral("*.opus"), QStringLiteral("*.OPUS"),
            QStringLiteral("*.m4a"), QStringLiteral("*.M4A"),
            QStringLiteral("*.aac"), QStringLiteral("*.AAC"),
            QStringLiteral("*.wav"), QStringLiteral("*.WAV"),
            QStringLiteral("*.wma"), QStringLiteral("*.WMA")};
}

bool LocalAudio::isSupportedFile(const QString &filename)
{
    static const QStringList extensions{
        QStringLiteral("mp3"), QStringLiteral("flac"), QStringLiteral("ogg"),
        QStringLiteral("oga"), QStringLiteral("opus"), QStringLiteral("m4a"),
        QStringLiteral("aac"), QStringLiteral("wav"), QStringLiteral("wma")};
    return QFileInfo(filename).isFile()
        && extensions.contains(QFileInfo(filename).suffix(), Qt::CaseInsensitive);
}

bool LocalAudio::probe(const QString &filename, LocalAudioMetadata *metadata, QString *error)
{
    if (!metadata || !QFileInfo(filename).isReadable()) {
        if (error) *error = QStringLiteral("The audio file is not readable: %1").arg(filename);
        return false;
    }
    if (QStandardPaths::findExecutable(QStringLiteral("ffprobe")).isEmpty()) {
        if (error) *error = QStringLiteral("FFprobe is not installed. Install it with: sudo apt install ffmpeg");
        return false;
    }
    QProcess process;
    process.start(QStringLiteral("ffprobe"), {
        QStringLiteral("-v"), QStringLiteral("error"),
        QStringLiteral("-select_streams"), QStringLiteral("a:0"),
        QStringLiteral("-show_entries"),
        QStringLiteral("stream=codec_name,sample_rate,channels,bit_rate:stream_tags:format=duration,bit_rate:format_tags"),
        QStringLiteral("-of"), QStringLiteral("json"), filename});
    if (!process.waitForStarted() || !process.waitForFinished(30000)
        || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString details = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
        if (error) *error = details.isEmpty() ? QStringLiteral("FFprobe could not read: %1").arg(filename) : details;
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(process.readAllStandardOutput(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) *error = QStringLiteral("FFprobe returned invalid metadata for: %1").arg(filename);
        return false;
    }
    const QJsonObject root = document.object();
    const QJsonObject format = root.value(QStringLiteral("format")).toObject();
    const QJsonArray streams = root.value(QStringLiteral("streams")).toArray();
    if (streams.isEmpty()) {
        if (error) *error = QStringLiteral("The file has no supported audio stream: %1").arg(filename);
        return false;
    }
    const QJsonObject stream = streams.first().toObject();
    const QJsonObject formatTags = format.value(QStringLiteral("tags")).toObject();
    const QJsonObject streamTags = stream.value(QStringLiteral("tags")).toObject();
    const auto tag = [&formatTags, &streamTags](const QString &name) {
        const QString formatValue = tagValue(formatTags, name);
        return formatValue.isEmpty() ? tagValue(streamTags, name) : formatValue;
    };
    LocalAudioMetadata result;
    result.title = tag(QStringLiteral("title"));
    result.artist = tag(QStringLiteral("artist"));
    result.album = tag(QStringLiteral("album"));
    result.albumArtist = tag(QStringLiteral("album_artist"));
    if (result.albumArtist.isEmpty()) result.albumArtist = tag(QStringLiteral("albumartist"));
    result.genre = tag(QStringLiteral("genre"));
    QString track = tag(QStringLiteral("track"));
    if (track.isEmpty()) track = tag(QStringLiteral("tracknumber"));
    result.trackNumber = track.section(QLatin1Char('/'), 0, 0).toInt();
    QString date = tag(QStringLiteral("date"));
    if (date.isEmpty()) date = tag(QStringLiteral("year"));
    result.year = date.left(4).toInt();
    result.codec = stream.value(QStringLiteral("codec_name")).toString();
    result.sampleRate = stream.value(QStringLiteral("sample_rate")).toString().toInt();
    result.channels = stream.value(QStringLiteral("channels")).toInt();
    qint64 bitrate = stream.value(QStringLiteral("bit_rate")).toString().toLongLong();
    if (bitrate <= 0) bitrate = format.value(QStringLiteral("bit_rate")).toString().toLongLong();
    result.bitrateKbps = static_cast<int>(bitrate / 1000);
    result.durationSeconds = qRound(format.value(QStringLiteral("duration")).toString().toDouble());
    const QFileInfo fileInfo(filename);
    const QRegularExpressionMatch libraryName = QRegularExpression(
        QStringLiteral("^(?:\\d+-)?(\\d+)\\s+-\\s+(.+)$")).match(fileInfo.completeBaseName());
    if (result.trackNumber <= 0 && libraryName.hasMatch())
        result.trackNumber = libraryName.captured(1).toInt();
    if (result.title.isEmpty()) result.title = libraryName.hasMatch()
        ? libraryName.captured(2) : fileInfo.completeBaseName();
    if (result.album.isEmpty()) result.album = fileInfo.dir().dirName();
    if (result.artist.isEmpty()) {
        QDir artistDirectory = fileInfo.dir();
        if (artistDirectory.cdUp()) result.artist = artistDirectory.dirName();
    }
    if (result.albumArtist.isEmpty()) result.albumArtist = result.artist;
    *metadata = result;
    return true;
}

bool LocalAudio::isWalkmanCompatibleMp3(const QString &filename, const LocalAudioMetadata &metadata)
{
    if (QFileInfo(filename).suffix().compare(QStringLiteral("mp3"), Qt::CaseInsensitive) != 0
        || metadata.codec.compare(QStringLiteral("mp3"), Qt::CaseInsensitive) != 0)
        return false;
    // Use the conservative legacy Network Walkman profile. Other valid MP3
    // sample rates are transcoded to 44.1 kHz to avoid device-side CANNOT PLAY.
    return metadata.channels >= 1 && metadata.channels <= 2
        && (metadata.sampleRate == 32000 || metadata.sampleRate == 44100
            || metadata.sampleRate == 48000);
}

bool LocalAudio::encodeWalkmanMp3(const QString &sourceFile, const QString &outputFile,
                                  const Mp3EncodingSettings &settings, QString *error)
{
    if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()) {
        if (error) *error = QStringLiteral("FFmpeg is not installed. Install it with: sudo apt install ffmpeg");
        return false;
    }
    if (!QDir().mkpath(QFileInfo(outputFile).absolutePath())) {
        if (error) *error = QStringLiteral("Could not create the temporary encoding directory.");
        return false;
    }
    QFile::remove(outputFile);
    QProcess process;
    process.start(QStringLiteral("ffmpeg"), mp3Arguments(sourceFile, outputFile, settings));
    if (!process.waitForStarted() || !process.waitForFinished(-1)
        || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString details = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
        if (error) *error = details.isEmpty() ? QStringLiteral("FFmpeg could not encode: %1").arg(sourceFile) : details;
        QFile::remove(outputFile);
        return false;
    }
    return QFileInfo(outputFile).isReadable() && QFileInfo(outputFile).size() > 0;
}
