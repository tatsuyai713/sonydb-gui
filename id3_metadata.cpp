#include "sonydb.h"
#include "id3_metadata.h"

#include <QByteArray>

#include <cstdlib>
#include <vector>

QString decodeId3Unicode(const unicode_t *text, size_t capacity)
{
    if (!text || capacity == 0) return {};
    char *utf8 = utf16_to_ansi(reinterpret_cast<const utf16char *>(text),
                               static_cast<long>(capacity), true);
    const QString value = utf8 ? QString::fromUtf8(utf8).trimmed() : QString();
    free(utf8);
    return value;
}

QString readId3TextFrame(ID3Tag *tag, ID3_FrameID id)
{
    ID3Frame *frame = ID3Tag_FindFrameWithID(tag, id);
    ID3Field *field = frame ? ID3Frame_GetField(frame, ID3FN_TEXT) : nullptr;
    if (!field || ID3Field_Size(field) == 0) return {};

    std::vector<unicode_t> unicode(ID3Field_Size(field) + 1, 0);
    if (ID3Field_GetUNICODE(field, unicode.data(), unicode.size()) > 0) {
        const QString value = decodeId3Unicode(unicode.data(), unicode.size());
        if (!value.isEmpty()) return value;
    }
    QByteArray buffer(static_cast<int>(ID3Field_Size(field)) + 1, '\0');
    ID3Field_GetASCII(field, buffer.data(), buffer.size());
    return QString::fromLatin1(buffer.constData()).trimmed();
}
