#include "library_path.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QTextStream>

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QTextStream out(stdout);
    QTextStream err(stderr);
    if (application.arguments().size() != 2) {
        err << "Usage: sonydb-normalize-library MUSIC_FOLDER\n";
        return 2;
    }
    const QString root = QFileInfo(application.arguments().at(1)).absoluteFilePath();
    LibraryPath::NormalizationResult result;
    QString error;
    if (!LibraryPath::normalizeDirectoryTree(root, &result, &error)) {
        err << error << '\n';
        return 1;
    }
    out << "Renamed directories: " << result.renamedDirectories << '\n'
        << "Merged directories: " << result.mergedDirectories << '\n'
        << "Moved files: " << result.movedFiles << '\n'
        << "Conflicts preserved: " << result.conflicts.size() << '\n';
    for (const QString &conflict : result.conflicts) out << "Conflict: " << conflict << '\n';
    return result.conflicts.isEmpty() ? 0 : 3;
}
