#include "id3_metadata.h"

#include <QCoreApplication>
#include <QFile>

#include <iostream>

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    if (application.arguments().size() == 2) {
        const QByteArray path = QFile::encodeName(application.arguments().at(1));
        ID3Tag *tag = ID3Tag_New(); ID3Tag_Link(tag, path.constData());
        std::cout << readId3TextFrame(tag, ID3FID_TITLE).toStdString() << '\n'
                  << readId3TextFrame(tag, ID3FID_LEADARTIST).toStdString() << '\n'
                  << readId3TextFrame(tag, ID3FID_ALBUM).toStdString() << '\n';
        ID3Tag_Delete(tag);
        return 0;
    }
    // id3lib returns the UTF-16 byte sequence as big-endian numeric words even
    // for a little-endian ID3 frame. It can also report the field capacity and
    // leave unrelated values after the terminator.
    const unicode_t japanese[] = {0x1b61, 0x6e30, 0x7e30, 0x7e30, 0x6b30,
                                  0x0000, 0x1234, 0xabcd};
    if (decodeId3Unicode(japanese, std::size(japanese)) != QString::fromUtf8(u8"愛のままに")) {
        std::cerr << "Japanese ID3 decoding failed\n";
        return 1;
    }
    const unicode_t artist[] = {0x4200, 0x1920, 0x7a00, 0x0000, 0xbeef};
    if (decodeId3Unicode(artist, std::size(artist)) != QString::fromUtf8(u8"B’z")) {
        std::cerr << "Mixed Latin/Unicode ID3 decoding failed\n";
        return 1;
    }
    return 0;
}
