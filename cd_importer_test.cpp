#include "cd_importer.h"
#include "encoded_audio_importer.h"
#include "library_path.h"
#include "local_audio.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <iostream>

namespace {

bool require(bool condition, const char *message)
{
    if (!condition) std::cerr << message << '\n';
    return condition;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
    if (!require(CdImporter::pcmSampleFormat() == QStringLiteral("s16le"),
                 "CD audio must use little-endian PCM on this host")) return 1;
#endif
    if (application.arguments().contains(QStringLiteral("--probe"))
        || application.arguments().contains(QStringLiteral("--rip-probe"))) {
        const QString device = CdImporter::findDevice();
        CdDiscInfo physicalDisc; QString probeError;
        if (device.isEmpty() || !CdImporter::readDisc(device, &physicalDisc, &probeError)) {
            std::cerr << probeError.toStdString() << '\n';
            return 2;
        }
        std::cout << device.toStdString() << ": " << physicalDisc.tracks.size()
                  << " audio track(s)\n"
                  << CdImporter::musicBrainzRequest(physicalDisc).url().toString(QUrl::FullyEncoded).toStdString() << '\n';
        const int ripIndex = application.arguments().indexOf(QStringLiteral("--rip-probe"));
        if (ripIndex >= 0) {
            if (ripIndex + 1 >= application.arguments().size()) {
                std::cerr << "--rip-probe requires an output directory\n";
                return 2;
            }
            QString output;
            Mp3EncodingSettings encoding;
            encoding.bitrateKbps = 128;
            const CdRipResult result = CdImporter::ripTrack(physicalDisc, physicalDisc.tracks.first(),
                application.arguments().at(ripIndex + 1), encoding, true, &output, &probeError);
            if (result != CdRipResult::Ok) {
                std::cerr << probeError.toStdString() << '\n';
                return 2;
            }
            std::cout << output.toStdString() << '\n';
        }
        return 0;
    }
    CdDiscInfo disc;
    disc.devicePath = QStringLiteral("/dev/sr0");
    disc.tracks = {{1, 0, 13500, QStringLiteral("Track 01"), QStringLiteral("Unknown Artist")},
                   {2, 13500, 27000, QStringLiteral("Track 02"), QStringLiteral("Unknown Artist")}};

    const QByteArray response = R"({
      "releases": [{
        "id": "release-id", "title": "Example / Album", "date": "2025-03-02", "country": "JP",
        "artist-credit": [{"name": "Various Artists"}],
        "media": [{"track-count": 2, "tracks": [
          {"position": 1, "title": "First Song", "artist-credit": [{"name": "Artist One"}]},
          {"position": 2, "recording": {"title": "Second Song", "artist-credit": [{"name": "Artist Two"}]}}
        ]}]
      }]
    })";
    QString error;
    const QList<CdMetadataCandidate> candidates = CdImporter::parseMusicBrainzResults(response, disc, &error);
    if (!require(candidates.size() == 1, "Expected one MusicBrainz candidate")) return 1;
    CdImporter::applyMetadata(&disc, candidates.first());
    if (!require(disc.albumArtist == QStringLiteral("Various Artists"), "Album artist was not applied")) return 1;
    if (!require(disc.tracks.at(1).artist == QStringLiteral("Artist Two"), "Per-track artist was not applied")) return 1;
    const QString path = CdImporter::outputPath(disc, disc.tracks.first(), QStringLiteral("/music"));
    if (!require(path == QStringLiteral("/music/Various Artists/Example _ Album/01 - First Song.mp3"),
                 "Unexpected library output path")) return 1;
    if (!require(CdImporter::musicBrainzRequest(disc).url().query().contains(QStringLiteral("toc=")),
                 "MusicBrainz request has no TOC")) return 1;
    Mp3EncodingSettings encoding;
    if (!require(encoding.summary().contains(QStringLiteral("CBR 192 kbps")),
                 "Unexpected default MP3 settings")) return 1;
    encoding.rateMode = Mp3EncodingSettings::RateMode::VariableBitrate;
    encoding.vbrQuality = 0;
    if (!require(encoding.summary().contains(QStringLiteral("VBR V0")),
                 "VBR MP3 settings were not reflected in the summary")) return 1;
    encoding.format = Mp3EncodingSettings::Format::Flac;
    encoding.flacCompression = 10;
    if (!require(CdImporter::outputPath(disc, disc.tracks.first(), QStringLiteral("/music"), encoding)
                     .endsWith(QStringLiteral("01 - First Song.flac")),
                 "FLAC output path has the wrong extension")) return 1;
    const QStringList flacArguments = CdImporter::encodingArguments(
        disc, disc.tracks.first(), encoding, QStringLiteral("output.part"));
    if (!require(flacArguments.contains(QStringLiteral("flac"))
                 && !flacArguments.contains(QStringLiteral("libmp3lame")),
                 "FLAC encoder arguments are incorrect")) return 1;
    encoding.format = Mp3EncodingSettings::Format::OggVorbis;
    encoding.oggQuality = 7;
    if (!require(CdImporter::outputPath(disc, disc.tracks.first(), QStringLiteral("/music"), encoding)
                     .endsWith(QStringLiteral("01 - First Song.ogg")),
                 "Ogg output path has the wrong extension")) return 1;
    const QStringList oggArguments = CdImporter::encodingArguments(
        disc, disc.tracks.first(), encoding, QStringLiteral("output.part"));
    if (!require(oggArguments.contains(QStringLiteral("libvorbis"))
                 && oggArguments.contains(QStringLiteral("ogg")),
                 "Ogg Vorbis encoder arguments are incorrect")) return 1;
    encoding.format = Mp3EncodingSettings::Format::Mp3;

    EncodedImportTrack imported;
    imported.title = QStringLiteral("Future / Track");
    imported.artist = QStringLiteral("Track Artist");
    imported.album = QStringLiteral("Example Album");
    imported.albumArtist = QStringLiteral("Various Artists");
    imported.trackNumber = 3;
    imported.discNumber = 2;

    {
        QTemporaryDir normalizedLibrary;
        if (!require(normalizedLibrary.isValid(), "Could not create Unicode path test directory")) return 1;
        const QString existingAlbum = normalizedLibrary.filePath(
            QStringLiteral("B\u2019z/B\u2019z The Best \u201cPleasure \u2161\u201d"));
        if (!require(QDir().mkpath(existingAlbum), "Could not create existing Unicode library path")) return 1;
        EncodedImportTrack equivalent;
        equivalent.albumArtist = QStringLiteral("\uff22'\uff3a");
        equivalent.album = QStringLiteral("B\u02bcz The Best \"Pleasure II\"");
        equivalent.title = QStringLiteral("Cafe\u0301");
        equivalent.trackNumber = 1;
        const QString resolved = EncodedAudioImporter::outputPath(
            equivalent, normalizedLibrary.path(), QStringLiteral("ogg"));
        if (!require(resolved == existingAlbum + QStringLiteral("/01 - Caf\u00e9.ogg"),
                     "Equivalent Unicode path did not reuse the existing folders")) return 1;
    }
    {
        QTemporaryDir migrationRoot;
        if (!require(migrationRoot.isValid(), "Could not create folder migration test directory")) return 1;
        const QString oldPath = migrationRoot.filePath(
            QStringLiteral("\uff22'\uff3a/The Ballads \uff5eLove & B\u02bcz\uff5e"));
        if (!require(QDir().mkpath(oldPath), "Could not create folders requiring normalization")) return 1;
        LibraryPath::NormalizationResult result;
        QString migrationError;
        if (!require(LibraryPath::normalizeDirectoryTree(migrationRoot.path(), &result, &migrationError),
                     "Existing folder normalization failed")) return 1;
        if (!require(QFileInfo(migrationRoot.filePath(
                         QStringLiteral("B\u2019Z/The Ballads ~Love & B\u2019z~"))).isDir()
                     && result.renamedDirectories == 2 && result.conflicts.isEmpty(),
                     "Existing folders were not normalized consistently")) return 1;
    }
    {
        QTemporaryDir mergeRoot;
        if (!require(mergeRoot.isValid(), "Could not create folder merge test directory")) return 1;
        const QString ascii = mergeRoot.filePath(QStringLiteral("B'z/Album"));
        const QString curly = mergeRoot.filePath(QStringLiteral("B\u2019z/Album"));
        if (!require(QDir().mkpath(ascii) && QDir().mkpath(curly),
                     "Could not create equivalent folder trees")) return 1;
        QFile first(QDir(ascii).filePath(QStringLiteral("01.ogg")));
        QFile second(QDir(curly).filePath(QStringLiteral("02.ogg")));
        if (!require(first.open(QIODevice::WriteOnly) && first.write("one") == 3
                     && second.open(QIODevice::WriteOnly) && second.write("two") == 3,
                     "Could not create folder merge fixtures")) return 1;
        first.close(); second.close();
        LibraryPath::NormalizationResult result;
        QString mergeError;
        if (!require(LibraryPath::normalizeDirectoryTree(mergeRoot.path(), &result, &mergeError),
                     "Equivalent folder merge failed")) return 1;
        const QString merged = mergeRoot.filePath(QStringLiteral("B\u2019z/Album"));
        if (!require(QFileInfo::exists(QDir(merged).filePath(QStringLiteral("01.ogg")))
                     && QFileInfo::exists(QDir(merged).filePath(QStringLiteral("02.ogg")))
                     && !QFileInfo::exists(mergeRoot.filePath(QStringLiteral("B'z")))
                     && result.mergedDirectories > 0 && result.conflicts.isEmpty(),
                     "Equivalent folders were not merged without data loss")) return 1;
    }
    imported.totalTracks = 12;
    if (!require(EncodedAudioImporter::outputPath(imported, QStringLiteral("/music"))
                     == QStringLiteral("/music/Various Artists/Example Album/2-03 - Future _ Track.mp3"),
                 "Unexpected encoded import output path")) return 1;
    if (!require(EncodedAudioImporter::outputPath(imported, QStringLiteral("/music"), QStringLiteral("ogg"))
                     == QStringLiteral("/music/Various Artists/Example Album/2-03 - Future _ Track.ogg"),
                 "Unexpected Ogg import output path")) return 1;

    imported.albumArtist = QStringLiteral("B'z");
    imported.album = QStringLiteral("The Ballads ～Love & B\u02BCz～");
    imported.title = QStringLiteral("B\u2018z Track");
    imported.trackNumber = 1;
    imported.discNumber = 1;
    if (!require(EncodedAudioImporter::outputPath(imported, QStringLiteral("/music"), QStringLiteral("ogg"))
                     == QStringLiteral("/music/B\u2019z/The Ballads ~Love & B\u2019z~/01 - B\u2019z Track.ogg"),
                 "Apostrophe variants were not normalized in the library path")) return 1;

    imported.title = QStringLiteral("Future / Track");
    imported.album = QStringLiteral("Example Album");
    imported.albumArtist = QStringLiteral("Various Artists");
    imported.trackNumber = 3;
    imported.discNumber = 2;

    if (!QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()) {
        QTemporaryDir temporary;
        if (!require(temporary.isValid(), "Could not create an Ogg placement test directory")) return 1;
        const QString input = temporary.filePath(QStringLiteral("source.ogg"));
        QProcess generator;
        generator.start(QStringLiteral("ffmpeg"), {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"),
            QStringLiteral("error"), QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
            QStringLiteral("anullsrc=r=44100:cl=stereo"), QStringLiteral("-t"), QStringLiteral("0.05"), input});
        if (!require(generator.waitForFinished(30000) && generator.exitCode() == 0,
                     "Could not create Ogg placement input")) return 1;
        QString output;
        QString error;
        if (!require(EncodedAudioImporter::placeLocalFile(input, imported, temporary.path(),
                         QStringLiteral("ogg"), false, &output, &error) == EncodedImportResult::Ok,
                     "Ogg placement failed")) return 1;
        LocalAudioMetadata tagged;
        if (!require(QFileInfo(output).fileName() == QStringLiteral("2-03 - Future _ Track.ogg")
                     && QFileInfo(output).size() > 0 && LocalAudio::probe(output, &tagged, &error),
                     "Ogg placement wrote an unexpected file")) return 1;
        if (!require(tagged.title == imported.title && tagged.artist == imported.artist
                     && tagged.album == imported.album && tagged.albumArtist == imported.albumArtist
                     && tagged.trackNumber == imported.trackNumber,
                     "Ogg placement did not write transfer metadata")) return 1;
        if (!require(EncodedAudioImporter::placeLocalFile(input, imported, temporary.path(),
                         QStringLiteral("ogg"), false, &output, &error) == EncodedImportResult::AlreadyExists,
                     "Ogg placement duplicate handling failed")) return 1;
    }

    if (!QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()) {
        QTemporaryDir temporary;
        if (!require(temporary.isValid(), "Could not create an encoder test directory")) return 1;
        for (const auto format : {Mp3EncodingSettings::Format::Flac,
                                  Mp3EncodingSettings::Format::OggVorbis}) {
            Mp3EncodingSettings cdEncoding;
            cdEncoding.format = format;
            const QString encoded = temporary.filePath(
                format == Mp3EncodingSettings::Format::Flac
                    ? QStringLiteral("cd-test.flac.part") : QStringLiteral("cd-test.ogg.part"));
            QProcess cdEncoder;
            cdEncoder.start(QStringLiteral("ffmpeg"), CdImporter::encodingArguments(
                disc, disc.tracks.first(), cdEncoding, encoded));
            if (!require(cdEncoder.waitForStarted(30000), "Could not start CD format encoder")) return 1;
            cdEncoder.write(QByteArray(8820, '\0'));
            cdEncoder.closeWriteChannel();
            if (!require(cdEncoder.waitForFinished(30000) && cdEncoder.exitCode() == 0
                         && QFileInfo(encoded).size() > 0,
                         "CD FLAC/Ogg encoding failed")) return 1;
        }
        const QString input = temporary.filePath(QStringLiteral("input.wav"));
        QProcess generator;
        generator.start(QStringLiteral("ffmpeg"), {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"),
            QStringLiteral("error"), QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
            QStringLiteral("anullsrc=r=44100:cl=stereo"), QStringLiteral("-t"), QStringLiteral("0.05"), input});
        if (!require(generator.waitForFinished(30000) && generator.exitCode() == 0,
                     "Could not create encoder test audio")) return 1;
        QString output;
        encoding.rateMode = Mp3EncodingSettings::RateMode::ConstantBitrate;
        encoding.bitrateKbps = 128;
        if (!require(EncodedAudioImporter::encodeLocalFile(input, imported, temporary.path(), encoding,
                         false, &output, &error) == EncodedImportResult::Ok,
                     "Future import encoder failed")) return 1;
        if (!require(QFileInfo(output).size() > 0, "Future import encoder created no MP3 data")) return 1;
        if (!require(EncodedAudioImporter::encodeLocalFile(input, imported, temporary.path(), encoding,
                         false, &output, &error) == EncodedImportResult::AlreadyExists,
                     "Future import duplicate handling failed")) return 1;
        EncodedImportTrack second = imported;
        second.trackNumber = 4;
        const EncodedImportSummary summary = EncodedAudioImporter::importLocalFiles(
            {{input, imported}, {input, second}}, temporary.path(), encoding, false);
        if (!require(summary.completed == 1 && summary.skipped == 1 && summary.failed == 0,
                     "Future batch import accounting failed")) return 1;
    }
    return 0;
}
