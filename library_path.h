#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>

namespace LibraryPath {

inline QString safePart(QString value, const QString &fallback)
{
    value = value.normalized(QString::NormalizationForm_KC).trimmed();
    QString normalized;
    normalized.reserve(value.size());
    bool previousSpace = false;
    for (QChar &character : value) {
        if (character.isSpace()) {
            if (!previousSpace) normalized += QLatin1Char(' ');
            previousSpace = true;
            continue;
        }
        previousSpace = false;
        switch (character.unicode()) {
        case 0x0027: // apostrophe
        case 0x02bc: // modifier letter apostrophe
        case 0x2018: // left single quotation mark
        case 0x2019: // right single quotation mark
        case 0xff07: // fullwidth apostrophe
            normalized += QChar(0x2019); break;
        case 0x0022: case 0x201c: case 0x201d: case 0x301d: case 0x301e: case 0xff02:
            normalized += QLatin1Char('"'); break;
        case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212:
            normalized += QLatin1Char('-'); break;
        case 0x007e: case 0x301c: case 0xff5e:
            normalized += QLatin1Char('~'); break;
        default:
            if (character == QLatin1Char('/') || character == QLatin1Char('\\')
                || character.unicode() < 0x20) normalized += QLatin1Char('_');
            else normalized += character;
            break;
        }
    }
    value = normalized.trimmed();
    while (value.endsWith(QLatin1Char('.')) || value.endsWith(QLatin1Char(' '))) value.chop(1);
    if (value.isEmpty() || value == QStringLiteral(".") || value == QStringLiteral("..")) return fallback;
    return value;
}

inline QString comparisonKey(QString value)
{
    value = value.normalized(QString::NormalizationForm_KC).toCaseFolded();
    QString result;
    result.reserve(value.size());
    bool previousSpace = false;
    for (QChar character : value) {
        const ushort code = character.unicode();
        if (character.isSpace()) {
            if (!previousSpace) result += QLatin1Char(' ');
            previousSpace = true;
            continue;
        }
        previousSpace = false;
        switch (code) {
        case 0x0027: case 0x02bc: case 0x2018: case 0x2019: case 0xff07:
            result += QLatin1Char('\''); break;
        case 0x0022: case 0x201c: case 0x201d: case 0x301d: case 0x301e: case 0xff02:
            result += QLatin1Char('"'); break;
        case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212:
            result += QLatin1Char('-'); break;
        case 0x007e: case 0x301c: case 0xff5e:
            result += QLatin1Char('~'); break;
        default:
            result += character; break;
        }
    }
    return result.trimmed().normalized(QString::NormalizationForm_KC);
}

inline QString existingEquivalentName(const QString &parent, const QString &candidate,
                                      QDir::Filters type)
{
    const QDir directory(parent);
    const QFileInfo exact(directory.filePath(candidate));
    if ((type.testFlag(QDir::Dirs) && exact.isDir())
        || (type.testFlag(QDir::Files) && exact.isFile())) return candidate;
    const QString key = comparisonKey(candidate);
    const QFileInfoList entries = directory.entryInfoList(type | QDir::NoDotAndDotDot,
                                                           QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &entry : entries)
        if (comparisonKey(entry.fileName()) == key) return entry.fileName();
    return candidate;
}

inline QString existingDirectoryName(const QString &parent, const QString &candidate)
{
    return existingEquivalentName(parent, candidate, QDir::Dirs);
}

inline QString existingFileName(const QString &parent, const QString &candidate)
{
    return existingEquivalentName(parent, candidate, QDir::Files);
}

struct NormalizationResult {
    int renamedDirectories = 0;
    int mergedDirectories = 0;
    int movedFiles = 0;
    QStringList conflicts;
};

inline bool mergeDirectoryContents(const QString &source, const QString &target,
                                   NormalizationResult *result, QString *error);

inline bool normalizeDirectoryTree(const QString &root, NormalizationResult *result, QString *error)
{
    QDir directory(root);
    if (!directory.exists()) {
        if (error) *error = QStringLiteral("Directory does not exist: %1").arg(root);
        return false;
    }
    const QFileInfoList children = directory.entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                                                            QDir::Name);
    for (const QFileInfo &child : children) {
        QString source = child.absoluteFilePath();
        if (!normalizeDirectoryTree(source, result, error)) return false;
        const QString canonicalName = safePart(child.fileName(), QStringLiteral("Unknown"));
        const QString target = directory.filePath(canonicalName);
        if (source == target) continue;
        if (QFileInfo(target).exists()) {
            if (!QFileInfo(target).isDir()) {
                if (result) result->conflicts << target;
                continue;
            }
            if (!mergeDirectoryContents(source, target, result, error)) return false;
            if (result) ++result->mergedDirectories;
        } else if (!QDir().rename(source, target)) {
            if (error) *error = QStringLiteral("Could not rename %1 to %2").arg(source, target);
            return false;
        } else if (result) {
            ++result->renamedDirectories;
        }
    }
    return true;
}

inline bool mergeDirectoryContents(const QString &source, const QString &target,
                                   NormalizationResult *result, QString *error)
{
    const QFileInfoList entries = QDir(source).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &entry : entries) {
        const QString canonical = safePart(entry.fileName(), entry.isDir()
            ? QStringLiteral("Unknown") : QStringLiteral("Unknown File"));
        const QString destination = QDir(target).filePath(canonical);
        if (entry.isDir() && !entry.isSymLink()) {
            if (QFileInfo(destination).exists()) {
                if (!QFileInfo(destination).isDir()
                    || !mergeDirectoryContents(entry.absoluteFilePath(), destination, result, error)) return false;
                if (result) ++result->mergedDirectories;
            } else if (!QDir().rename(entry.absoluteFilePath(), destination)) {
                if (error) *error = QStringLiteral("Could not move %1 to %2").arg(entry.absoluteFilePath(), destination);
                return false;
            } else if (result) {
                ++result->renamedDirectories;
            }
        } else if (QFileInfo(destination).exists()) {
            if (result) result->conflicts << destination;
        } else if (!QFile::rename(entry.absoluteFilePath(), destination)) {
            if (error) *error = QStringLiteral("Could not move %1 to %2").arg(entry.absoluteFilePath(), destination);
            return false;
        } else if (result) {
            ++result->movedFiles;
        }
    }
    if (!QDir().rmdir(source)) {
        if (!QDir(source).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty()) return true;
        if (error) *error = QStringLiteral("Could not remove merged directory: %1").arg(source);
        return false;
    }
    return true;
}

} // namespace LibraryPath
