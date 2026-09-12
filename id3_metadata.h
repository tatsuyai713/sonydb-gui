#pragma once

#include <id3.h>

#include <QString>

QString decodeId3Unicode(const unicode_t *text, size_t capacity);
QString readId3TextFrame(ID3Tag *tag, ID3_FrameID id);
