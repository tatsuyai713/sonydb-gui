#pragma once

#include "cd_importer.h"

#include <QList>
#include <QString>

// Metadata and encoding stages shared by imports whose audio input is already
// available as a normal local media file.
struct EncodedImportTrack {
    QString title;
    QString artist;
    QString album;
    QString albumArtist;
    int trackNumber = 0;
    int discNumber = 0;
    int totalTracks = 0;
};

enum class EncodedImportResult { Ok, AlreadyExists, Failed };

struct EncodedImportJob {
    QString sourceFile;
    EncodedImportTrack track;
};

struct EncodedImportSummary {
    int completed = 0;
    int skipped = 0;
    int failed = 0;
    QString lastError;
};

class EncodedAudioImporter final
{
public:
    static QString outputPath(const EncodedImportTrack &track, const QString &musicFolder);
    static QString outputPath(const EncodedImportTrack &track, const QString &musicFolder,
                              const QString &extension);
    static EncodedImportResult placeLocalFile(
        const QString &sourceFile, const EncodedImportTrack &track,
        const QString &musicFolder, const QString &extension, bool overwrite,
        QString *outputFile, QString *error);
    static EncodedImportResult encodeLocalFile(
        const QString &sourceFile, const EncodedImportTrack &track,
        const QString &musicFolder, const Mp3EncodingSettings &settings,
        bool overwrite, QString *outputFile, QString *error);
    static EncodedImportSummary importLocalFiles(
        const QList<EncodedImportJob> &jobs, const QString &musicFolder,
        const Mp3EncodingSettings &settings, bool overwrite);
};
