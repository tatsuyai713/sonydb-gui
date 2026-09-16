#include "local_audio.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>

#include <iostream>

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition "\n"; return 1; \
} } while (false)

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    CHECK(LocalAudio::isSupportedFile(QStringLiteral("example.FLAC")) == false);
    QTemporaryDir directory;
    CHECK(directory.isValid());
    const QString source = QDir(directory.path()).filePath(QStringLiteral("owned source.wav"));
    QProcess generator;
    generator.start(QStringLiteral("ffmpeg"), {QStringLiteral("-v"), QStringLiteral("error"),
        QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
        QStringLiteral("sine=frequency=440:duration=0.2"), QStringLiteral("-ar"),
        QStringLiteral("44100"), QStringLiteral("-ac"), QStringLiteral("2"),
        QStringLiteral("-metadata"), QStringLiteral("title=Owned Track"),
        QStringLiteral("-y"), source});
    CHECK(generator.waitForFinished(30000));
    CHECK(generator.exitCode() == 0);
    CHECK(LocalAudio::isSupportedFile(source));

    LocalAudioMetadata sourceInfo;
    QString error;
    CHECK(LocalAudio::probe(source, &sourceInfo, &error));
    CHECK(sourceInfo.title == QStringLiteral("Owned Track"));
    CHECK(sourceInfo.codec == QStringLiteral("pcm_s16le"));
    CHECK(!LocalAudio::isWalkmanCompatibleMp3(source, sourceInfo));

    Mp3EncodingSettings settings;
    settings.bitrateKbps = 192;
    const QString output = QDir(directory.path()).filePath(QStringLiteral("transfer.mp3"));
    CHECK(LocalAudio::encodeWalkmanMp3(source, output, settings, &error));
    LocalAudioMetadata outputInfo;
    CHECK(LocalAudio::probe(output, &outputInfo, &error));
    CHECK(LocalAudio::isWalkmanCompatibleMp3(output, outputInfo));
    CHECK(QFileInfo(source).exists());

    const QString ogg = QDir(directory.path()).filePath(QStringLiteral("owned source.ogg"));
    QProcess oggGenerator;
    oggGenerator.start(QStringLiteral("ffmpeg"), {QStringLiteral("-v"), QStringLiteral("error"),
        QStringLiteral("-i"), source, QStringLiteral("-metadata"), QStringLiteral("title=Ogg Title"),
        QStringLiteral("-metadata"), QStringLiteral("artist=Ogg Artist"),
        QStringLiteral("-metadata"), QStringLiteral("album=Ogg Album"),
        QStringLiteral("-metadata"), QStringLiteral("album_artist=Ogg Album Artist"),
        QStringLiteral("-metadata"), QStringLiteral("tracknumber=7/12"), QStringLiteral("-y"), ogg});
    CHECK(oggGenerator.waitForFinished(30000));
    CHECK(oggGenerator.exitCode() == 0);
    LocalAudioMetadata oggInfo;
    CHECK(LocalAudio::probe(ogg, &oggInfo, &error));
    CHECK(oggInfo.codec == QStringLiteral("vorbis"));
    CHECK(oggInfo.title == QStringLiteral("Ogg Title"));
    CHECK(oggInfo.artist == QStringLiteral("Ogg Artist"));
    CHECK(oggInfo.album == QStringLiteral("Ogg Album"));
    CHECK(oggInfo.albumArtist == QStringLiteral("Ogg Album Artist"));
    CHECK(oggInfo.trackNumber == 7);
    CHECK(!LocalAudio::isWalkmanCompatibleMp3(ogg, oggInfo));

    const QString untaggedFolder = QDir(directory.path()).filePath(QStringLiteral("Path Artist/Path Album"));
    CHECK(QDir().mkpath(untaggedFolder));
    const QString untagged = QDir(untaggedFolder).filePath(QStringLiteral("2-09 - Path Title.ogg"));
    QProcess untaggedGenerator;
    untaggedGenerator.start(QStringLiteral("ffmpeg"), {QStringLiteral("-v"), QStringLiteral("error"),
        QStringLiteral("-i"), source, QStringLiteral("-map_metadata"), QStringLiteral("-1"),
        QStringLiteral("-y"), untagged});
    CHECK(untaggedGenerator.waitForFinished(30000));
    CHECK(untaggedGenerator.exitCode() == 0);
    LocalAudioMetadata inferred;
    CHECK(LocalAudio::probe(untagged, &inferred, &error));
    CHECK(inferred.title == QStringLiteral("Path Title"));
    CHECK(inferred.artist == QStringLiteral("Path Artist"));
    CHECK(inferred.album == QStringLiteral("Path Album"));
    CHECK(inferred.trackNumber == 9);
    return 0;
}
