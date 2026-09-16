#pragma once

#include "cd_importer.h"

#include <QString>
#include <QStringList>

struct LocalAudioMetadata {
    QString title;
    QString artist;
    QString album;
    QString albumArtist;
    QString genre;
    QString codec;
    int trackNumber = 0;
    int year = 0;
    int durationSeconds = 0;
    int bitrateKbps = 0;
    int sampleRate = 0;
    int channels = 0;
};

class LocalAudio final
{
public:
    static QStringList nameFilters();
    static bool isSupportedFile(const QString &filename);
    static bool probe(const QString &filename, LocalAudioMetadata *metadata, QString *error = nullptr);
    static bool isWalkmanCompatibleMp3(const QString &filename, const LocalAudioMetadata &metadata);
    static bool encodeWalkmanMp3(const QString &sourceFile, const QString &outputFile,
                                 const Mp3EncodingSettings &settings, QString *error = nullptr);
};
