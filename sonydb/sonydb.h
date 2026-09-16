/*
** Sonydb.h
**
** Made by (julien)
*/

#ifndef SONYDB_H
# define SONYDB_H

#ifdef __GNUC__
#undef _WIN32
#endif

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#ifdef __MINGW32__
#include <io.h>
#endif
#endif
#include <stdio.h>
#include <cstdint>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <algorithm>
#include <unistd.h>

using namespace std;

#if !defined(_WIN32) && !defined(__MINGW32__)
typedef long long __int64;
#endif
typedef std::uint16_t utf16char;

#define UINT32_SWAP_BE_LE(val) ((std::uint32_t) ( \
(((std::uint32_t) (val) & (std::uint32_t) 0x000000ffU) << 24) | \
(((std::uint32_t) (val) & (std::uint32_t) 0x0000ff00U) <<  8) | \
(((std::uint32_t) (val) & (std::uint32_t) 0x00ff0000U) >>  8) | \
(((std::uint32_t) (val) & (std::uint32_t) 0xff000000U) >> 24)))

#define UINT16_SWAP_BE_LE(val) ((std::uint16_t) ( \
(((std::uint16_t) (val) & (std::uint16_t) 0x00ffU) << 8) | \
(((std::uint16_t) (val) & (std::uint16_t) 0xff00U) >> 8)))

#define SYNCHSAFE_B1(val) (((std::uint32_t) (val) >> 21) & (std::uint32_t) 0x000007F)
#define SYNCHSAFE_B2(val) (((std::uint32_t) (val) >> 14) & (std::uint32_t) 0x000007F)
#define SYNCHSAFE_B3(val) (((std::uint32_t) (val) >> 7) & (std::uint32_t) 0x000007F)
#define SYNCHSAFE_B4(val) (((std::uint32_t) (val) & (std::uint32_t) 0x0000007F))

#define NOT_SYNCHSAFE_B1(val) (std::uint8_t) (((val) & (std::uint32_t) 0xff000000U) >> 24);
#define NOT_SYNCHSAFE_B2(val) (std::uint8_t) (((val) & (std::uint32_t) 0x00ff0000U) >> 16);
#define NOT_SYNCHSAFE_B3(val) (std::uint8_t) (((val) & (std::uint32_t) 0x0000ff00U) >>  8);
#define NOT_SYNCHSAFE_B4(val) (std::uint8_t) ((val) & (std::uint32_t)  0x000000ffU);

// Text in SonyDb's public char-based API is UTF-8. OMGAUDIO stores text as
// UTF-16, normally in big-endian byte order.
utf16char *ansi_to_utf16(const char  *str, long len, bool endian);
char *utf16_to_ansi(const utf16char *str, long len, bool endian);

#define TAGSIZE 128
#define OUTPUT_TAGSIZE 128

#define ON_DEVICE	      0
#define ADD_TO_DEVICE	      1
#define REMOVE_FROM_DEVICE    2
// 3 is reserved for MODIFIED
#define EMPTYTRACK            4

#define NOT_LOADED	      0
#define LOADED		      1
#define MODIFIED	      2

#define ENCODING_USE_NONE	      0
#define ENCODING_USE_TABLE	      1
#define ENCODING_USE_KEY	      2

#define SONY_PROTECTION_LSI_DRM       0x0001
#define SONY_PROTECTION_ENCRYPTED_MP3 0xfffe
#define SONY_PROTECTION_NONE          0xffff

#define EXPORT_OK                 0
#define EXPORT_NOT_FOUND          1
#define EXPORT_ALREADY_EXISTS     2
#define EXPORT_FAILED             3

#define DATABASE_HEADER_SIZE 0


typedef struct {
     char *artist;
     char *title;
     char *album;
     char *filename;
     char *genre;
     utf16char *wArtist;
     utf16char *wTitle;
     utf16char *wAlbum;
     //utf16char *wFilename;
     utf16char *wGenre;
     int songlen; // seconds?
     int track_nr;
     int year;
     
     int       sonyDbOrder;
     int       statusOfSong; //0 was present on player, 1 was not present on player needs & to be added, 2 was present & needs to be removed
     // Four-byte OMGAUDIO codec field: codec, flags, MPEG parameters, mode.
     std::uint32_t    encoding;
     // Per-track OpenMG protection. This must not be replaced globally when
     // rewriting 04CNTINF because existing ATRAC tracks may use LSI DRM.
     std::uint16_t    protection;
     // 05CIDLST contains a track-specific 48-byte record for LSI DRM titles.
     // Non-LSI titles, including encrypted MP3, use an all-zero record.
     std::uint8_t     cidRecord[48];
     bool             hasCidRecord;
} Song;

typedef struct {
	 int  index;
     char *name;
     vector<Song*> songs;
} Playlist;

typedef struct
{
  std::uint8_t magic[4];      /* "magic file descriptor" */
  std::uint8_t cte[4];        /* Constant value */
  std::uint8_t count;         /* Number of object pointers */
  std::uint8_t padding[7];    /* padding to 16 bytes */
} sonyFileHeader;

typedef struct
{
  std::uint8_t  magic[4];      /* magic (same as object) */
  std::uint32_t offset;        /* offset of the object (from the beginning)*/
  std::uint32_t length;        /* size of object in bytes */
  std::uint32_t padding;       /* padding to 16 bytes */
} sonyObjectPointer;

typedef struct
{
  std::uint8_t magic[4];       /* magic (same as object pointer) */
  std::uint16_t count;         /* record count */
  std::uint16_t size;		/* record size */
  std::uint32_t padding[2];    /* padding to 16 bytes */
} sonyObject;

typedef struct
{
 std::uint8_t  fileType[4];
 std::uint32_t trackEncoding;
 std::uint32_t trackLength;
 std::uint16_t nbTagRecords;
 std::uint16_t sizeTagRecords;
} sonyTrack;

typedef struct
{
     std::uint8_t tagType[4];
     std::uint8_t tagEncoding[2];
}sonyTrackTag;

bool sortByIndex(Song *a, Song *b);
bool sortByTrackNumber(Song *a, Song *b);
bool sortByAlbumName(Song *a, Song *b);
bool sortByArtistName(Song *a, Song *b);
bool sortByTitleName(Song *a, Song *b);
bool sortByGenreName(Song *a, Song *b);
bool sortPlaylist(Playlist s, Playlist s2);
bool sortByPlaylistIndex(Playlist s, Playlist s2);


class SonyDb
{
  private:

     
     int  id;
     int  lastTrackIndex;
     int  nbTrackToDel;
     int  nbTrackToAdd;
     char* driveLetter;    
     int  trackListLoaded; //0 not loaded, 1 loaded, 2 modified
     char *deviceName;
     char *decodeTableFilename;
     bool useAllTags; //if you want to have "all albums" "all genre" etc...

     /* disk space */
     __int64 addTrackTotalByte;
     __int64 delTrackTotalByte;
     __int64 usedSpaceDisk;
     __int64 freeSpaceDisk;
     __int64 neededSpaceValue;
     __int64 totalDiskSpaceValue;
     char *totalDiskSpace;
     char *totalUsedSpaceAfterApply;
     char *freeDiskSpaceAfterApply;
     char *addTrackTotalByteString;
     char *delTrackTotalByteString;
     char *neededSpace;

     /* estimated time */
     int bytePerSec; //estimated value will be calculated after first file as been transfered
     __int64 totalByteLeftToWrite;

     /* encoding decoding*/
     int  getTrackNumber(char *filename); //read the track number directly from the omg header
     std::uint32_t DvId;
     int  codeType; //0 no code, 1 decodeKeys.dat, 2 DvId.dat
     bool deviceKeyRequired;

     /* copy progress */
     bool copying; //currently getting or adding Oma files, or rewriting db
     bool databaseDirty;
     int  copyIndex; //current file index
     float copyPercent;// progress of the current file (in percent)


     /* device related */
     void freeAllTracks();
     void freeAllPlaylist();
     bool writeDatabase(vector<Song *> songsToSend);
     char *GetOMAFilename(int id);


     /* common header files */
     bool getHeader(sonyFileHeader *fh, FILE *f);
     bool getObjectPointer(sonyObjectPointer *op, FILE *f);
     bool getObject(sonyObject *obj, FILE *f);
     bool getTrack(FILE *f, Song *output);
     bool writeHeader(sonyFileHeader *h, FILE *f);
     bool writeHeader(sonyFileHeader *h, FILE *f, int count);
     bool writeObjectPointer(sonyObjectPointer *p, FILE *f);
     bool writeObject(sonyObject *obj, FILE *f);
     bool writeTrackHeader(sonyTrack *t, FILE *f);
     bool writeTrackTag(sonyTrackTag *tt, char *input, FILE *f);

     /* file writers */
     bool write_00GTRLST();
     bool write_01TREEXX(vector<Song *> songsToSend, vector<Song *> list, int type);
     bool write_01TREE22();
     bool write_02TREINF(vector<Song *> songsToSend);
     bool write_03GINFXX(vector<Song *> songsToSend, int type);
     bool write_03GINF22();
     bool write_04CNTINF(vector<Song *> songsToSend);
     bool write_05CIDLST(vector<Song *> songsToSend);
     bool write_TrackNumber(vector<Song *> songsToSend);


     /* decoder encoder */
     std::uint8_t codeTable[1024];
     bool loadCodeTable(int id);
     bool addOMA(Song *s, int destination);
     void deleteOMA(char *filename);

     /* directory */
     void createDir(int highestValue);

     //debug
     FILE *fp;
     vector<Song>     songs;
     vector<Song>     songs_temporary;
     vector<Playlist> playlist;
     vector<Playlist> playlist_temporary;

  public:

     SonyDb();
     ~SonyDb();

     bool writeTracks();
     int  readAllTracks();
     int  readAllPlaylist();
     
     vector<Song*> getSongs();
     vector<Song*> getSongsInPlaylist(int source);
     vector<Playlist*> getPlaylist();
     bool deletePlaylist(int source, bool removeSongs);

     bool addSong(Song *s); //add to the database
     int  delSong(Song *s); //del to the database
	 bool updSong(Song *s); //update to the database
     bool addSongCopy(const Song &song);
     bool removeSong(int order, const char *filename);
     bool updateSong(int order, const char *filename, const Song &values);
     bool hasPendingChanges() const;
     bool getOMA(Song *s, char *destination);//download oma to mp3
     bool getOMAToFile(Song *s, const char *outputFile);
     std::string exportPathForSong(int order, const char *destination) const;
     int exportSong(int order, const char *destination, bool overwrite,
                    std::string *outputPath = 0);
     int exportSongToFile(int order, const char *outputFile, bool overwrite);

     
     /*encode decoder*/
     void setTable(char *decodeTableFileName, int type);
     bool setDeviceKeyFile(const char *fileName);
     void clearDeviceKey();
     bool isDeviceKeyConfigured() const;
     bool requiresDeviceKey() const;
     std::string findDeviceKeyFile() const;
     int getUnprotectedMp3Count() const;
     bool repairUnprotectedMp3Tracks();

     /* misc */
     bool isPresent();
     bool isCopying();
     int  progressIndex();
     int  getCopyPercent();
     bool detectPlayer();
     bool detectPlayer(char* drive);
     bool detectPlayerStorage();
     bool detectPlayerStorage(char* drive);
     bool initializePlayer();
     char* getDriveLetter();
     int  getNumberOfTracks();
     char *getDeviceName();
     int  getId();
     void setId(int newId);
     int  getNbTrackToDel();
     int  getNbTrackToAdd();
     int  isTrackListLoaded();
     void setUseAllTags(bool value); //if you want to have "all albums" "all genre" etc...
     bool getUseAllTags();

     /* disk space*/
     void updateDiskSpaceInfo();
     char *getTotalDiskSpace();
     char *getTotalUsedSpaceAfterApply();
     char *getFreeDiskSpaceAfterApply();
     char *getSizeTrackToAdd();
     char *getSizeTrackToDel();
     char *getNeededSpace();
     int  getNeededSpaceValue();

     /* playlist */
     bool addPlaylist(Playlist *p);
     int  getNbPlaylist();


};

#endif /* !SONYDB_H */
