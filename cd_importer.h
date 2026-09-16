#pragma once

#include <QList>
#include <QNetworkRequest>
#include <QString>

struct CdTrackInfo {
    int number = 0;
    int startSector = 0;
    int endSector = 0;
    QString title;
    QString artist;

    int durationSeconds() const { return qMax(0, endSector - startSector) / 75; }
};

struct CdDiscInfo {
    QString devicePath;
    QString album = QStringLiteral("Audio CD");
    QString albumArtist = QStringLiteral("Unknown Artist");
    int year = 0;
    QList<CdTrackInfo> tracks;
};

struct CdMetadataCandidate {
    QString album;
    QString albumArtist;
    QString date;
    QString country;
    QString releaseId;
    QList<CdTrackInfo> tracks;

    QString displayName() const;
};

enum class CdRipResult { Ok, AlreadyExists, Failed };

struct Mp3EncodingSettings {
    enum class Format { Mp3, Flac, OggVorbis };
    enum class RateMode { ConstantBitrate, VariableBitrate };
    enum class ChannelMode { JointStereo, Stereo, Mono };

    Format format = Format::Mp3;
    RateMode rateMode = RateMode::ConstantBitrate;
    ChannelMode channelMode = ChannelMode::JointStereo;
    int bitrateKbps = 192;
    int vbrQuality = 2;
    int encoderQuality = 2;
    int flacCompression = 8;
    int oggQuality = 6;

    QString summary() const;
    QString extension() const;
    QString formatName() const;
};

class CdImporter final
{
public:
    static QString pcmSampleFormat();
    static QString findDevice();
    static bool readDisc(const QString &devicePath, CdDiscInfo *disc, QString *error);
    static QNetworkRequest musicBrainzRequest(const CdDiscInfo &disc);
    static QList<CdMetadataCandidate> parseMusicBrainzResults(
        const QByteArray &json, const CdDiscInfo &disc, QString *error);
    static void applyMetadata(CdDiscInfo *disc, const CdMetadataCandidate &candidate);

    static QString outputPath(const CdDiscInfo &disc, const CdTrackInfo &track,
                              const QString &musicFolder);
    static QString outputPath(const CdDiscInfo &disc, const CdTrackInfo &track,
                              const QString &musicFolder, const Mp3EncodingSettings &settings);
    static QStringList encodingArguments(const CdDiscInfo &disc, const CdTrackInfo &track,
                                         const Mp3EncodingSettings &settings,
                                         const QString &outputFile);
    static CdRipResult ripTrack(const CdDiscInfo &disc, const CdTrackInfo &track,
                                const QString &musicFolder, const Mp3EncodingSettings &settings,
                                bool overwrite, QString *outputFile, QString *error);
};
