#include "cd_importer.h"

#include <QCoreApplication>

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
    return 0;
}
