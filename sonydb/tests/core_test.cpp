#include "sonydb.h"

// Including id3lib after the core header is intentional: this verifies that
// the public SonyDb types no longer collide with id3lib's global typedefs.
#include <id3.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (false)

namespace {

void releaseSong(Song *song)
{
    free(song->album);
    free(song->artist);
    free(song->title);
    free(song->genre);
    free(song->filename);
    free(song);
}

} // namespace

int main()
{
    const char *unicodeText = u8"宇多田ヒカル – First Love 🎵";
    utf16char *unicode = ansi_to_utf16(unicodeText, 128, true);
    CHECK(unicode != nullptr);
    char *roundTrip = utf16_to_ansi(unicode, 128, true);
    CHECK(roundTrip != nullptr);
    CHECK(std::strcmp(roundTrip, unicodeText) == 0);
    free(unicode);
    free(roundTrip);

    SonyDb database;
    Song input{};
    input.artist = const_cast<char *>("Artist");
    input.album = const_cast<char *>("Album");
    input.title = const_cast<char *>("Title");
    input.genre = const_cast<char *>("Rock");
    input.filename = const_cast<char *>("/tmp/sonydb-core-test.mp3");
    input.track_nr = 3;
    input.songlen = 125;
    input.year = 2026;
    input.statusOfSong = ADD_TO_DEVICE;

    CHECK(database.addSongCopy(input));
    CHECK(database.hasPendingChanges());
    CHECK(database.getNumberOfTracks() == 1);

    Song edited = input;
    edited.title = const_cast<char *>("Edited Title");
    edited.track_nr = 4;
    CHECK(database.updateSong(0, input.filename, edited));

    std::vector<Song *> songs = database.getSongs();
    CHECK(songs.size() == 1);
    CHECK(std::strcmp(songs.front()->title, "Edited Title") == 0);
    CHECK(songs.front()->track_nr == 4);
    releaseSong(songs.front());

    CHECK(database.removeSong(0, input.filename));
    CHECK(database.getNumberOfTracks() == 0);
    CHECK(database.hasPendingChanges());

    char directoryTemplate[] = "/tmp/sonydb-core-test-XXXXXX";
    char *directory = mkdtemp(directoryTemplate);
    CHECK(directory != nullptr);
    CHECK(database.detectPlayerStorage(directory));
    CHECK(database.initializePlayer());
    CHECK(std::filesystem::is_regular_file(
        std::filesystem::path(directory) / "OMGAUDIO" / "04CNTINF.DAT"));

    const std::filesystem::path source = std::filesystem::path(directory) / "source.mp3";
    std::string mp3(4096, '\0');
    mp3[0] = static_cast<char>(0xff);
    mp3[1] = static_cast<char>(0xfb);
    mp3[2] = static_cast<char>(0x90);
    mp3[3] = 0;
    std::ofstream(source, std::ios::binary).write(mp3.data(), mp3.size());
    input.artist = const_cast<char *>(u8"宇多田ヒカル");
    input.album = const_cast<char *>(u8"初恋");
    input.title = const_cast<char *>(u8"花束を君に");
    input.genre = const_cast<char *>(u8"ポップ");
    input.filename = const_cast<char *>(source.c_str());
    CHECK(database.addSongCopy(input));
    CHECK(database.writeTracks());

    SonyDb reopened;
    CHECK(reopened.detectPlayer(directory));
    CHECK(reopened.readAllTracks() == 1);
    std::vector<Song *> reopenedSongs = reopened.getSongs();
    CHECK(reopenedSongs.size() == 1);
    CHECK(std::strcmp(reopenedSongs.front()->artist, u8"宇多田ヒカル") == 0);
    CHECK(std::strcmp(reopenedSongs.front()->album, u8"初恋") == 0);
    CHECK(std::strcmp(reopenedSongs.front()->title, u8"花束を君に") == 0);
    CHECK(std::strcmp(reopenedSongs.front()->genre, u8"ポップ") == 0);
    releaseSong(reopenedSongs.front());

    const std::filesystem::path oma = std::filesystem::path(directory) /
        "OMGAUDIO" / "10F00" / "10000001.OMA";
    std::ifstream omaInput(oma, std::ios::binary);
    std::string omaHeader(4096 + 96, '\0');
    omaInput.read(omaHeader.data(), omaHeader.size());
    CHECK(omaInput.gcount() == static_cast<std::streamsize>(omaHeader.size()));
    CHECK(omaHeader.compare(0, 3, "ea3") == 0);
    CHECK(static_cast<unsigned char>(omaHeader[21]) == 0x82);
    CHECK(static_cast<unsigned char>(omaHeader[22]) == 0xb1);
    CHECK(omaHeader.compare(4096, 3, "EA3") == 0);
    CHECK(static_cast<unsigned char>(omaHeader[4096 + 32]) == 0x03);
    CHECK(static_cast<unsigned char>(omaHeader[4096 + 33]) == 0x80);
    CHECK(static_cast<unsigned char>(omaHeader[4096 + 34]) == 0xd9);
    CHECK(static_cast<unsigned char>(omaHeader[4096 + 35]) == 0x00);

    std::ifstream contentInfo(std::filesystem::path(directory) /
        "OMGAUDIO" / "04CNTINF.DAT", std::ios::binary);
    contentInfo.seekg(48 + 4);
    char databaseCodec[4]{};
    contentInfo.read(databaseCodec, sizeof(databaseCodec));
    CHECK(static_cast<unsigned char>(databaseCodec[0]) == 0x03);
    CHECK(static_cast<unsigned char>(databaseCodec[1]) == 0x80);
    CHECK(static_cast<unsigned char>(databaseCodec[2]) == 0xd9);
    CHECK(static_cast<unsigned char>(databaseCodec[3]) == 0x00);

    const std::filesystem::path destination = std::filesystem::path(directory) / "export";
    std::filesystem::create_directory(destination);
    std::string exported;
    CHECK(database.exportSong(1, destination.c_str(), false, &exported) == EXPORT_OK);
    CHECK(std::filesystem::path(exported) ==
        destination / u8"宇多田ヒカル" / u8"初恋" / u8"03 - 花束を君に.mp3");
    CHECK(std::filesystem::is_regular_file(exported));
    std::ifstream restoredInput(exported, std::ios::binary);
    unsigned char restoredHeader[10]{};
    restoredInput.read(reinterpret_cast<char *>(restoredHeader), sizeof(restoredHeader));
    const std::uint32_t restoredTagSize =
        ((restoredHeader[6] & 0x7f) << 21) | ((restoredHeader[7] & 0x7f) << 14) |
        ((restoredHeader[8] & 0x7f) << 7) | (restoredHeader[9] & 0x7f);
    restoredInput.seekg(10 + restoredTagSize);
    unsigned char frameHeader[2]{};
    restoredInput.read(reinterpret_cast<char *>(frameHeader), sizeof(frameHeader));
    CHECK(frameHeader[0] == 0xff && frameHeader[1] == 0xfb);
    CHECK(database.exportSong(1, destination.c_str(), false) == EXPORT_ALREADY_EXISTS);
    CHECK(database.exportSong(1, destination.c_str(), true) == EXPORT_OK);

    const std::filesystem::path secondSource = std::filesystem::path(directory) / "second.mp3";
    std::filesystem::copy_file(source, secondSource);
    Song compilationTrack = input;
    compilationTrack.artist = const_cast<char *>("Second Artist");
    compilationTrack.title = const_cast<char *>("Second Title");
    compilationTrack.filename = const_cast<char *>(secondSource.c_str());
    compilationTrack.track_nr = 5;
    CHECK(database.addSongCopy(compilationTrack));
    CHECK(database.writeTracks());
    CHECK(std::filesystem::path(database.exportPathForSong(1, destination.c_str())) ==
        destination / "Various Artists" / u8"初恋" / u8"03 - 花束を君に.mp3");
    CHECK(std::filesystem::path(database.exportPathForSong(2, destination.c_str())) ==
        destination / "Various Artists" / u8"初恋" / "05 - Second Title.mp3");
    CHECK(database.exportSong(2, destination.c_str(), false) == EXPORT_OK);

    // Generation 3 players encrypt MP3 payloads with their device-specific
    // DvID. Verify the on-device marker, XOR stream, CIDL semantics, export,
    // and the fail-closed behavior when the key is unavailable.
    char protectedTemplate[] = "/tmp/sonydb-protected-test-XXXXXX";
    char *protectedDirectory = mkdtemp(protectedTemplate);
    CHECK(protectedDirectory != nullptr);
    SonyDb protectedDb;
    CHECK(protectedDb.detectPlayerStorage(protectedDirectory));
    CHECK(protectedDb.initializePlayer());
    const std::filesystem::path keyFile = std::filesystem::path(protectedDirectory) / "DvID.dat";
    const unsigned char keyBytes[14] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                         0x12, 0x34, 0x56, 0x78};
    std::ofstream(keyFile, std::ios::binary).write(
        reinterpret_cast<const char *>(keyBytes), sizeof(keyBytes));
    CHECK(protectedDb.setDeviceKeyFile(keyFile.c_str()));
    Song protectedTrack = input;
    protectedTrack.artist = const_cast<char *>("Protected Artist");
    protectedTrack.album = const_cast<char *>("Protected Album");
    protectedTrack.title = const_cast<char *>("Protected Title");
    protectedTrack.filename = const_cast<char *>(source.c_str());
    CHECK(protectedDb.addSongCopy(protectedTrack));
    CHECK(protectedDb.writeTracks());

    const std::filesystem::path protectedOma = std::filesystem::path(protectedDirectory) /
        "OMGAUDIO" / "10F00" / "10000001.OMA";
    std::ifstream protectedInput(protectedOma, std::ios::binary);
    protectedInput.seekg(4096 + 6);
    unsigned char protectionBytes[2]{};
    protectedInput.read(reinterpret_cast<char *>(protectionBytes), sizeof(protectionBytes));
    CHECK(protectionBytes[0] == 0xff && protectionBytes[1] == 0xfe);
    protectedInput.seekg(4096 + 96);
    unsigned char encryptedFrame[4]{};
    protectedInput.read(reinterpret_cast<char *>(encryptedFrame), sizeof(encryptedFrame));
    const std::uint32_t streamKey = (0x2465U + 0x5296E435U) ^ 0x12345678U;
    CHECK(encryptedFrame[0] == static_cast<unsigned char>(0xff ^ (streamKey >> 24)));
    CHECK(encryptedFrame[1] == static_cast<unsigned char>(0xfb ^ ((streamKey >> 16) & 0xff)));

    std::ifstream protectedInfo(std::filesystem::path(protectedDirectory) /
        "OMGAUDIO" / "04CNTINF.DAT", std::ios::binary);
    protectedInfo.seekg(48 + 2);
    protectedInfo.read(reinterpret_cast<char *>(protectionBytes), sizeof(protectionBytes));
    CHECK(protectionBytes[0] == 0xff && protectionBytes[1] == 0xfe);
    std::ifstream protectedCid(std::filesystem::path(protectedDirectory) /
        "OMGAUDIO" / "05CIDLST.DAT", std::ios::binary);
    protectedCid.seekg(48);
    char cidBytes[48]{};
    protectedCid.read(cidBytes, sizeof(cidBytes));
    for (char byte : cidBytes) CHECK(byte == 0);
	for (int index = 0; index < 48; ++index) cidBytes[index] = static_cast<char>(index + 1);
	{
		std::fstream editableCid(std::filesystem::path(protectedDirectory) /
			"OMGAUDIO" / "05CIDLST.DAT", std::ios::in | std::ios::out | std::ios::binary);
		editableCid.seekp(48);
		editableCid.write(cidBytes, sizeof(cidBytes));
	}
	// Simulate a file created by the broken clear-MP3 path, while retaining the
	// generation-3 control marker used by a real NW-E405.
	std::ofstream(std::filesystem::path(protectedDirectory) / "OMGAUDIO" / "00010021.DAT",
		std::ios::binary).put('\0');
	{
		std::fstream clearOma(protectedOma, std::ios::in | std::ios::out | std::ios::binary);
		clearOma.seekp(4096 + 6);
		const char clearProtection[2] = {static_cast<char>(0xff), static_cast<char>(0xff)};
		clearOma.write(clearProtection, sizeof(clearProtection));
		clearOma.seekp(4096 + 96);
		clearOma.write(mp3.data(), mp3.size());
	}

    SonyDb protectedReopened;
    CHECK(protectedReopened.detectPlayer(protectedDirectory));
    CHECK(protectedReopened.readAllTracks() == 1);
    CHECK(protectedReopened.requiresDeviceKey());
	CHECK(protectedReopened.getUnprotectedMp3Count() == 1);
    const std::filesystem::path blockedSource = std::filesystem::path(protectedDirectory) / "blocked.mp3";
    std::filesystem::copy_file(source, blockedSource);
    Song blockedTrack = protectedTrack;
    blockedTrack.title = const_cast<char *>("Blocked Without Key");
    blockedTrack.filename = const_cast<char *>(blockedSource.c_str());
    CHECK(protectedReopened.addSongCopy(blockedTrack));
    CHECK(!protectedReopened.writeTracks());
	CHECK(protectedReopened.removeSong(0, blockedSource.c_str()));
	CHECK(protectedReopened.readAllTracks() == 1);
    CHECK(protectedReopened.setDeviceKeyFile(keyFile.c_str()));
	CHECK(protectedReopened.repairUnprotectedMp3Tracks());
	CHECK(protectedReopened.getUnprotectedMp3Count() == 0);
	CHECK(protectedReopened.addSongCopy(blockedTrack));
    CHECK(protectedReopened.writeTracks());
	std::ifstream rewrittenCid(std::filesystem::path(protectedDirectory) /
		"OMGAUDIO" / "05CIDLST.DAT", std::ios::binary);
	rewrittenCid.seekg(48);
	char preservedCid[48]{};
	rewrittenCid.read(preservedCid, sizeof(preservedCid));
	CHECK(std::memcmp(cidBytes, preservedCid, sizeof(cidBytes)) == 0);
    const std::filesystem::path protectedExport = std::filesystem::path(protectedDirectory) / "export.mp3";
    CHECK(protectedReopened.exportSongToFile(1, protectedExport.c_str(), false) == EXPORT_OK);
    std::ifstream protectedRestored(protectedExport, std::ios::binary);
    unsigned char protectedId3[10]{};
    protectedRestored.read(reinterpret_cast<char *>(protectedId3), sizeof(protectedId3));
    const std::uint32_t protectedTagSize =
        ((protectedId3[6] & 0x7f) << 21) | ((protectedId3[7] & 0x7f) << 14) |
        ((protectedId3[8] & 0x7f) << 7) | (protectedId3[9] & 0x7f);
    protectedRestored.seekg(10 + protectedTagSize);
    protectedRestored.read(reinterpret_cast<char *>(frameHeader), sizeof(frameHeader));
    CHECK(frameHeader[0] == 0xff && frameHeader[1] == 0xfb);
    std::filesystem::remove_all(protectedDirectory);

    SonyDb invalidDatabase;
    CHECK(invalidDatabase.detectPlayerStorage(directory));
    const std::filesystem::path invalidSource = std::filesystem::path(directory) / "invalid.mp3";
    std::ofstream(invalidSource, std::ios::binary) << "not an mp3";
    Song invalidTrack = input;
    invalidTrack.title = const_cast<char *>("Invalid");
    invalidTrack.album = const_cast<char *>("Invalid");
    invalidTrack.filename = const_cast<char *>(invalidSource.c_str());
    CHECK(invalidDatabase.addSongCopy(invalidTrack));
    CHECK(!invalidDatabase.writeTracks());
    std::filesystem::remove_all(directory);
    return 0;
}
