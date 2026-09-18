/*
 ** SonyDB.cpp
 **
 ** Made by (julien)
 */

#include <stdio.h>
#include <string.h>
#include <fstream>
#include <filesystem>
#ifndef _WIN32
#include <fcntl.h>
#include <glob.h>
#include <mntent.h>
#include <limits.h>
#include <scsi/sg.h>
#include <set>
#include <system_error>
#include <sys/ioctl.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif
#include "sonydb.h"

#include <codecvt>
#include <locale>

using namespace std;

#ifndef _WIN32
#define strnicmp(x, y, z) strncasecmp(x, y, z)
#define stricmp(x, y) strcasecmp(x, y)
#endif

//SORT function for song vectors
#define SKIP_THE_AND_WHITESPACE(x) { while (!isalnum(*x) && *x) x++; if (!strnicmp(x,"the ",4)) x+=4; while (*x == ' ') x++; }
static int STRCMP_NULLOK(const char *pa, const char *pb)
{
	if (!pa) pa="";
	else SKIP_THE_AND_WHITESPACE(pa)

		if (!pb) pb="";
		else SKIP_THE_AND_WHITESPACE(pb)
			return stricmp(pa,pb);
}

static int STRCMP2_NULLOK(const char *pa, const char *pb)
{
	if (!pa) pa="";
	if (!pb) pb="";
	return stricmp(pa,pb);
}

static int STRNCMP_NULLOK(const char *pa, const char *pb, int size)
{
	if (!pa) pa="";  
	if (!pb) pb="";
	return strnicmp(pa, pb, size);
}

static std::u16string utf8_to_utf16_string(const char *text)
{
	if (!text) return {};
	try {
		std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> converter;
		return converter.from_bytes(text);
	} catch (const std::range_error &) {
		return {};
	}
}

static std::uint16_t oma_protection(const char *fileName)
{
	if (!fileName || !*fileName) return SONY_PROTECTION_NONE;
	FILE *file = fopen(fileName, "rb");
	if (!file) return SONY_PROTECTION_NONE;
	std::uint8_t header[10];
	if (fread(header, 1, sizeof(header), file) != sizeof(header) || memcmp(header, "ea3", 3) != 0)
	{
		fclose(file);
		return SONY_PROTECTION_NONE;
	}
	const long metadataSize = ((header[6] & 0x7f) << 21) | ((header[7] & 0x7f) << 14) |
		((header[8] & 0x7f) << 7) | (header[9] & 0x7f);
	std::uint8_t format[8];
	const bool valid = fseek(file, 10 + metadataSize, SEEK_SET) == 0 &&
		fread(format, 1, sizeof(format), file) == sizeof(format) && memcmp(format, "EA3", 3) == 0;
	fclose(file);
	return valid ? static_cast<std::uint16_t>((format[6] << 8) | format[7]) : SONY_PROTECTION_NONE;
}

static bool encrypt_unprotected_oma(const char *fileName, int trackId, std::uint32_t deviceId)
{
	FILE *input = fopen(fileName, "rb");
	if (!input) return false;
	std::uint8_t metadata[10];
	if (fread(metadata, 1, sizeof(metadata), input) != sizeof(metadata) ||
		memcmp(metadata, "ea3", 3) != 0)
	{
		fclose(input);
		return false;
	}
	const long metadataSize = ((metadata[6] & 0x7f) << 21) | ((metadata[7] & 0x7f) << 14) |
		((metadata[8] & 0x7f) << 7) | (metadata[9] & 0x7f);
	const long formatOffset = 10 + metadataSize;
	std::uint8_t format[96];
	if (fseek(input, formatOffset, SEEK_SET) != 0 ||
		fread(format, 1, sizeof(format), input) != sizeof(format) ||
		memcmp(format, "EA3", 3) != 0 || format[32] != 0x03 ||
		format[6] != 0xff || format[7] != 0xff)
	{
		fclose(input);
		return false;
	}
	const long audioOffset = formatOffset + static_cast<long>(sizeof(format));
	if (fseek(input, 0, SEEK_SET) != 0) { fclose(input); return false; }
	const string temporary = string(fileName) + ".sonydb-encrypt";
	const string original = string(fileName) + ".sonydb-clear-backup";
	FILE *output = fopen(temporary.c_str(), "wb");
	if (!output) { fclose(input); return false; }
	const std::uint32_t key = (0x2465U + static_cast<std::uint32_t>(trackId) * 0x5296E435U) ^ deviceId;
	std::uint8_t buffer[32768];
	long absolutePosition = 0;
	long audioPosition = 0;
	bool success = true;
	while (success)
	{
		const size_t count = fread(buffer, 1, sizeof(buffer), input);
		if (count == 0) break;
		for (size_t index = 0; index < count; ++index, ++absolutePosition)
		{
			if (absolutePosition == formatOffset + 6) buffer[index] = 0xff;
			else if (absolutePosition == formatOffset + 7) buffer[index] = 0xfe;
			else if (absolutePosition >= audioOffset)
			{
				const int shift = 24 - static_cast<int>((audioPosition % 4) * 8);
				buffer[index] ^= static_cast<std::uint8_t>((key >> shift) & 0xff);
				++audioPosition;
			}
		}
		success = fwrite(buffer, 1, count, output) == count;
	}
	if (ferror(input)) success = false;
	success = fclose(output) == 0 && success;
	fclose(input);
	if (!success) { remove(temporary.c_str()); return false; }
	remove(original.c_str());
	if (rename(fileName, original.c_str()) != 0) { remove(temporary.c_str()); return false; }
	if (rename(temporary.c_str(), fileName) != 0)
	{
		rename(original.c_str(), fileName);
		remove(temporary.c_str());
		return false;
	}
	remove(original.c_str());
	return true;
}



SonyDb::SonyDb()
{
	this->driveLetter = 0;
	this->deviceName = (char*)calloc(sizeof(char), 256);
	this->trackListLoaded = false;
	this->nbTrackToAdd = 0;
	this->nbTrackToDel = 0;
	this->lastTrackIndex = 1;
	this->codeType = ENCODING_USE_NONE;
	this->decodeTableFilename = 0;
	this->DvId = 0;
	this->deviceKeyRequired = false;
	this->copying = false;
	this->databaseDirty = false;
	this->copyIndex = 0;
	this->copyPercent = 0;
	this->addTrackTotalByte = 0;
	this->delTrackTotalByte = 0;
	this->freeSpaceDisk = 0;
	this->neededSpace = 0;
	this->totalDiskSpaceValue = 0;
	this->useAllTags = false;


	totalDiskSpace = (char*)calloc(sizeof(char), 64);
	freeDiskSpaceAfterApply = (char*)calloc(sizeof(char), 64);
	totalUsedSpaceAfterApply = (char*)calloc(sizeof(char), 64);
	delTrackTotalByteString = (char*)calloc(sizeof(char), 64);
	addTrackTotalByteString = (char*)calloc(sizeof(char), 64);
	neededSpace = (char*)calloc(sizeof(char), 64);

	//debug
	//fp = fopen("c:/Debug_ml_sony.log", "ab");
	//fprintf(fp,"+++++++++++++\n"); 
	//fflush(fp);
	fp = stderr;
}

//clean the song list before living
SonyDb::~SonyDb()
{
	//free track list 
	freeAllTracks();

	//free device Name
	free(this->deviceName);

	//free disk space info
	free(totalDiskSpace);
	free(totalUsedSpaceAfterApply);
	free(freeDiskSpaceAfterApply);
	free(delTrackTotalByteString);
	free(addTrackTotalByteString);
	free(neededSpace);

	//debug
	//fclose(fp);

	if (driveLetter) free(driveLetter);
	if (decodeTableFilename) free(decodeTableFilename);
}

//free track list
void SonyDb::freeAllTracks()
{
	for (vector<Song>::iterator i = songs.begin(); i != songs.end(); i++)
	{
		if ((*i).album) free((*i).album);
		if ((*i).artist) free((*i).artist);
		if ((*i).title) free((*i).title);
		if ((*i).genre) free((*i).genre);
		if ((*i).filename) free((*i).filename);
	}
	while (songs.size() > 0)
		songs.pop_back();
}

//free playlist
void SonyDb::freeAllPlaylist()
{
	vector<Song *> plSongs;

	for (vector<Playlist>::iterator pl = playlist.begin(); pl != playlist.end(); pl++)
	{
		plSongs = (*pl).songs;
		for (vector<Song*>::iterator i = plSongs.begin(); i != plSongs.end(); i++)
		{
			if ((*i)->statusOfSong == MODIFIED)
			{
				free((*i)->filename);
				free((*i)->artist);
				free((*i)->album);
				free((*i)->genre);
				free((*i)->title);
			}
		}
	}
	while (playlist.size() > 0)
		playlist.pop_back();
}

int  SonyDb::getId()
{
	return (this->id);
}

void SonyDb::setId(int newId)
{
	this->id = newId;
}

void SonyDb::setUseAllTags(bool value)
{
	useAllTags = value;
}

bool SonyDb::getUseAllTags()
{
	return (useAllTags);
}

vector<Playlist*> SonyDb::getPlaylist()
{    
	vector<Playlist*> ret;
	for (vector<Playlist>::iterator pl = playlist_temporary.begin(); pl != playlist_temporary.end(); pl++)
		ret.push_back(&(*pl));
	for (vector<Playlist>::iterator pl2 = playlist.begin(); pl2 != playlist.end(); pl2++)
		ret.push_back(&(*pl2));
	return (ret);
}

vector<Song*> SonyDb::getSongsInPlaylist(int source)
{
	vector<Song*> ret;
	Song *p;
	vector<Song *> plSongs;
	vector<Playlist>::iterator playlistBegin;
	vector<Playlist>::iterator playlistEnd;

	if (this->copying)
	{
		playlistBegin = playlist_temporary.begin();
		playlistEnd = playlist_temporary.end();
	}
	else
	{
		playlistBegin = playlist.begin();
		playlistEnd = playlist.end();
	}

	for (vector<Playlist>::iterator pl = playlistBegin; pl != playlistEnd; pl++)
	{
		if (pl->index  == source)
		{
			plSongs = pl->songs;
			for (vector<Song*>::iterator i = plSongs.begin(); i != plSongs.end(); i++)
			{
				if ((*i)->statusOfSong != EMPTYTRACK)
				{
					//new song
					p = (Song *)calloc(sizeof(Song),1);
					p->filename = strdup((*i)->filename);
					p->artist = strdup((*i)->artist);
					p->album = strdup((*i)->album);
					p->genre = strdup((*i)->genre);
					p->title = strdup((*i)->title);

					p->track_nr = (*i)->track_nr;
					p->songlen = (*i)->songlen;
					p->year = (*i)->year;
					p->statusOfSong = (*i)->statusOfSong;
					p->sonyDbOrder = (*i)->sonyDbOrder;
					p->encoding = (*i)->encoding;
					p->protection = (*i)->protection;
					p->hasCidRecord = (*i)->hasCidRecord;
					memcpy(p->cidRecord, (*i)->cidRecord, sizeof(p->cidRecord));
					ret.push_back(p);
				}
			}
			return (ret);
		}
	}

	//not found in temporary playlists so try in other playlist
	playlistBegin = playlist.begin();
	playlistEnd = playlist.end();

	for (vector<Playlist>::iterator pl2 = playlistBegin; pl2 != playlistEnd; pl2++)
	{
		if (pl2->index  == source)
		{
			plSongs = pl2->songs;
			for (vector<Song*>::iterator i = plSongs.begin(); i != plSongs.end(); i++)
			{
				if ((*i)->statusOfSong != EMPTYTRACK)
				{
					//new song
					p = (Song *)calloc(sizeof(Song),1);
					p->filename = strdup((*i)->filename);
					p->artist = strdup((*i)->artist);
					p->album = strdup((*i)->album);
					p->genre = strdup((*i)->genre);
					p->title = strdup((*i)->title);

					p->track_nr = (*i)->track_nr;
					p->songlen = (*i)->songlen;
					p->year = (*i)->year;
					p->statusOfSong = (*i)->statusOfSong;
					p->sonyDbOrder = (*i)->sonyDbOrder;
					p->encoding = (*i)->encoding;
					p->protection = (*i)->protection;
					p->hasCidRecord = (*i)->hasCidRecord;
					memcpy(p->cidRecord, (*i)->cidRecord, sizeof(p->cidRecord));
					ret.push_back(p);
				}
			}
			return (ret);
		}
	}

	return (ret);
}

bool SonyDb::deletePlaylist(int source, bool removeSongs)
{
	vector<Song *> plSongs;

	for (vector<Playlist>::iterator pl = playlist.begin(); pl != playlist.end(); pl++)
	{
		if (pl->index  == source)
		{
			plSongs = pl->songs;
			if (removeSongs)
			{
				for (vector<Song*>::iterator song = plSongs.begin(); song != plSongs.end(); song++)
				{
					delSong(*song);
				}
			}
			pl->songs.clear();
			playlist.erase(pl);
			return (true);
		}
	}
	return (false);

}

vector<Song*> SonyDb::getSongs()
{
	vector<Song*> ret;
	Song *p;

	if (this->copying)
	{
		for (vector<Song>::iterator i = songs_temporary.begin(); i != songs_temporary.end(); i++)
		{
			if ((*i).statusOfSong != EMPTYTRACK)
			{
				//new song
				p = (Song *)calloc(sizeof(Song),1);
				p->filename = strdup((*i).filename);
				p->artist = strdup((*i).artist);
				p->album = strdup((*i).album);
				p->genre = strdup((*i).genre);
				p->title = strdup((*i).title);

				p->track_nr = (*i).track_nr;
				p->songlen = (*i).songlen;
				p->year = (*i).year;
				p->statusOfSong = (*i).statusOfSong;
				p->sonyDbOrder = (*i).sonyDbOrder;
				p->encoding = (*i).encoding;
				p->protection = (*i).protection;
				p->hasCidRecord = (*i).hasCidRecord;
				memcpy(p->cidRecord, (*i).cidRecord, sizeof(p->cidRecord));
				ret.push_back(p);
			}
		}
	}
	else
	{
		for (vector<Song>::iterator i = songs.begin(); i != songs.end(); i++)
		{
			if ((*i).statusOfSong != EMPTYTRACK)
			{
				//new song
				p = (Song *)calloc(sizeof(Song),1);
				p->filename = strdup((*i).filename);
				p->artist = strdup((*i).artist);
				p->album = strdup((*i).album);
				p->genre = strdup((*i).genre);
				p->title = strdup((*i).title);

				p->track_nr = (*i).track_nr;
				p->songlen = (*i).songlen;
				p->year = (*i).year;
				p->statusOfSong = (*i).statusOfSong;
				p->sonyDbOrder = (*i).sonyDbOrder;
				p->encoding = (*i).encoding;
				p->protection = (*i).protection;
				p->hasCidRecord = (*i).hasCidRecord;
				memcpy(p->cidRecord, (*i).cidRecord, sizeof(p->cidRecord));
				ret.push_back(p);
			}
		}
	}
	return (ret);
}


int SonyDb::progressIndex()
{
	return (this->copyIndex);
}


int SonyDb::isTrackListLoaded()
{
	return (this->trackListLoaded);
}

int SonyDb::getNbTrackToDel()
{
	return (this->nbTrackToDel);
}

int SonyDb::getNbTrackToAdd()
{
	return (this->nbTrackToAdd);
}

bool SonyDb::updSong(Song *songToUpd)
{
	if (this->copying)
	{
		for (vector<Song>::iterator song = songs_temporary.begin(); song != songs_temporary.end(); song++)
		{
			//song is the same
			if (song->sonyDbOrder == songToUpd->sonyDbOrder)
			{
				if (song->statusOfSong != REMOVE_FROM_DEVICE &&
					song->statusOfSong != EMPTYTRACK)
				{
					song->artist = strdup(songToUpd->artist);
					song->album = strdup(songToUpd->album);
					song->genre = strdup(songToUpd->genre);
					song->title = strdup(songToUpd->title);

					song->track_nr = songToUpd->track_nr;
					song->songlen = songToUpd->songlen;
					song->year = songToUpd->year;
					databaseDirty = true;
					return (true);
				}
				return (false);
			}
		}
	}
	else
	{
		for (vector<Song>::iterator song = songs.begin(); song != songs.end(); song++)
		{
			//song is the same
			if (song->sonyDbOrder == songToUpd->sonyDbOrder)
			{
				if (song->statusOfSong != REMOVE_FROM_DEVICE &&
					song->statusOfSong != EMPTYTRACK)
				{
					song->artist = strdup(songToUpd->artist);
					song->album = strdup(songToUpd->album);
					song->genre = strdup(songToUpd->genre);
					song->title = strdup(songToUpd->title);

					song->track_nr = songToUpd->track_nr;
					song->songlen = songToUpd->songlen;
					song->year = songToUpd->year;
					databaseDirty = true;
					return (true);
				}
				return (false);
			}
		}
	}
	return (0);
}

int SonyDb::delSong(Song *songToDel)
{
	if (this->copying)
	{
		for (vector<Song>::iterator song = songs_temporary.begin(); song != songs_temporary.end(); song++)
		{
			//song is the same
			if ((STRCMP2_NULLOK((*song).album, (songToDel)->album) == 0) &&
					(STRCMP2_NULLOK((*song).artist, (songToDel)->artist) == 0) &&
					(STRCMP2_NULLOK((*song).title, (songToDel)->title) == 0))
			{
				if ((*song).statusOfSong == ADD_TO_DEVICE) //song is not yet on the device
				{
					//remove it from the list
					songs_temporary.erase(song);
					nbTrackToAdd--;
					//size of the song
					struct stat results;
					if (stat((*songToDel).filename, &results) == 0)
						addTrackTotalByte -= results.st_size;
					databaseDirty = true;
					return (2);
				}
			}
		}
	}
	else
	{
		for (vector<Song>::iterator song = songs.begin(); song != songs.end(); song++)
		{
			//song is the same
			if ((STRCMP2_NULLOK((*song).album, (songToDel)->album) == 0) &&
					(STRCMP2_NULLOK((*song).artist, (songToDel)->artist) == 0) &&
					(STRCMP2_NULLOK((*song).title, (songToDel)->title) == 0))
			{
				if ((*song).statusOfSong == ADD_TO_DEVICE) //song is not yet on the device
				{
					//remove it from the list
					songs.erase(song);
					nbTrackToAdd--;
					//size of the song
					struct stat results;
					if (stat((*songToDel).filename, &results) == 0)
						addTrackTotalByte -= results.st_size;
					databaseDirty = true;
					return (2);
				}
				if ((*song).statusOfSong == ON_DEVICE)
				{
					(*song).statusOfSong = REMOVE_FROM_DEVICE;
					nbTrackToDel++;
					//size of the song
					struct stat results;
					if (stat((*songToDel).filename, &results) == 0)
						delTrackTotalByte += results.st_size;
					databaseDirty = true;
					return (1);
				}
			}
		}
	}
	return (0);
}

int SonyDb::getNbPlaylist()
{
	return(this->playlist.size());
}

bool SonyDb::addPlaylist(Playlist *p)
{
	if (this->copying)
		playlist_temporary.push_back(*p);
	else
		playlist.push_back(*p);
	return (true);
}

bool SonyDb::addSong(Song *songToAdd)
{
	//check how much space is needed
	struct stat results;
	if (stat((*songToAdd).filename, &results) == 0)
		addTrackTotalByte += results.st_size;

	if (this->copying)
	{
		//add the song to the temporary list
		songToAdd->sonyDbOrder = 0;
		songs_temporary.push_back(*songToAdd);
		nbTrackToAdd++;
		databaseDirty = true;
	}
	else
	{
		//check if song is already present on device
		for (vector<Song>::iterator song = songs.begin(); song != songs.end(); song++)
		{
			//song is the same
			if ((STRCMP2_NULLOK((*song).album, (songToAdd)->album) == 0) &&
					(STRCMP2_NULLOK((*song).artist, (songToAdd)->artist) == 0) &&
					(STRCMP2_NULLOK((*song).title, (songToAdd)->title) == 0))
			{
				if ((*song).statusOfSong != ADD_TO_DEVICE)
					(*song).statusOfSong = ON_DEVICE;
				free(songToAdd->filename);
				songToAdd->filename = strdup((*song).filename);
				return (false);
			}
		}

		//add the song
		nbTrackToAdd++;
		songToAdd->sonyDbOrder = 0; //changed when copied to the device in writeTracks
		songs.push_back(*songToAdd);
		databaseDirty = true;
	}

	return (true);
}

bool SonyDb::addSongCopy(const Song &source)
{
	Song copy = source;
	copy.album = strdup(source.album ? source.album : "");
	copy.artist = strdup(source.artist ? source.artist : "");
	copy.title = strdup(source.title ? source.title : "");
	copy.genre = strdup(source.genre ? source.genre : "");
	copy.filename = strdup(source.filename ? source.filename : "");
	copy.wAlbum = 0;
	copy.wArtist = 0;
	copy.wTitle = 0;
	copy.wGenre = 0;
	if (addSong(&copy))
		return true;
	free(copy.album);
	free(copy.artist);
	free(copy.title);
	free(copy.genre);
	free(copy.filename);
	return false;
}

bool SonyDb::removeSong(int order, const char *filename)
{
	for (vector<Song>::iterator song = songs.begin(); song != songs.end(); ++song)
	{
		const bool match = order > 0 ? song->sonyDbOrder == order
			: (filename && song->filename && strcmp(song->filename, filename) == 0);
		if (!match)
			continue;

		struct stat results;
		if (song->statusOfSong == ADD_TO_DEVICE)
		{
			if (stat(song->filename, &results) == 0)
				addTrackTotalByte -= results.st_size;
			free(song->album);
			free(song->artist);
			free(song->title);
			free(song->genre);
			free(song->filename);
			songs.erase(song);
			nbTrackToAdd--;
			databaseDirty = true;
			return true;
		}
		if (song->statusOfSong == ON_DEVICE)
		{
			song->statusOfSong = REMOVE_FROM_DEVICE;
			if (stat(song->filename, &results) == 0)
				delTrackTotalByte += results.st_size;
			nbTrackToDel++;
			databaseDirty = true;
			return true;
		}
		return false;
	}
	return false;
}

bool SonyDb::updateSong(int order, const char *filename, const Song &values)
{
	for (vector<Song>::iterator song = songs.begin(); song != songs.end(); ++song)
	{
		const bool match = order > 0 ? song->sonyDbOrder == order
			: (filename && song->filename && strcmp(song->filename, filename) == 0);
		if (!match || song->statusOfSong == REMOVE_FROM_DEVICE || song->statusOfSong == EMPTYTRACK)
			continue;

		free(song->album);
		free(song->artist);
		free(song->title);
		free(song->genre);
		song->album = strdup(values.album ? values.album : "");
		song->artist = strdup(values.artist ? values.artist : "");
		song->title = strdup(values.title ? values.title : "");
		song->genre = strdup(values.genre ? values.genre : "");
		song->track_nr = values.track_nr;
		song->songlen = values.songlen;
		song->year = values.year;
		databaseDirty = true;
		return true;
	}
	return false;
}

bool SonyDb::hasPendingChanges() const
{
	return databaseDirty;
}




/*****************************************************************************************/
/** read/write functions (common header) */

bool SonyDb::getHeader(sonyFileHeader *header, FILE *f)
{
	//read header file
	if ( fread(header, sizeof(sonyFileHeader), 1, f ) != 1)
	{
		fprintf(fp, "error could not read file header (fileheader)\n");
		fflush(fp);
		return false;
	}
	/*  else
		{
		fprintf(fp, "header\n: magic:%08x, cte: %08x ,count:%08x \n", header->magic, header->cte, header->count);

		}
		*/

	return true;
}

bool SonyDb::getObjectPointer(sonyObjectPointer *Opointer, FILE *f)
{
	//read object pointer
	if ( fread(Opointer, sizeof(sonyObjectPointer), 1, f ) != 1 )
	{
		fprintf(fp, "error could not read file header (filePointer)\n");
		fflush(fp);
		return false;
	}
	else
	{
		//fprintf(fp, "objectPointer: magic:%x length:%x, offset:%x\n", Opointer->magic, Opointer->length, Opointer->offset);
		//get values from big endian
		Opointer->length = UINT32_SWAP_BE_LE(Opointer->length);
		Opointer->offset = UINT32_SWAP_BE_LE(Opointer->offset);
		//fprintf(fp, "sizeof: %i, objectPointer\n: magic:%s length:%08x, offset:%08x\n",sizeof(sonyObjectPointer), Opointer->magic, Opointer->length, Opointer->offset);
	}
	return true;
}

bool SonyDb::getObject(sonyObject *obj, FILE *f)
{
	//read object
	if ( fread(obj, sizeof(sonyObject), 1, f ) !=1 )
	{
		fprintf(fp, "error could not read file header (object)\n");
		fflush(fp);
		return false;
	}
	else
	{
		//fprintf(fp, "object: magic:%x size :%x, count:%x \n", obj->magic, obj->size, obj->count);
		//get values from big endian
		obj->size = UINT16_SWAP_BE_LE(obj->size);
		obj->count = UINT16_SWAP_BE_LE(obj->count);
		//fprintf(fp, "sizeof: %i, object\n: magic:%s size :%08x, count:%08x \n", sizeof(sonyObject), obj->magic, obj->size, obj->count);
	}   
	return true;
}

// write a file header
bool SonyDb::writeHeader(sonyFileHeader *h, FILE *f)
{
	return(writeHeader(h, f, 1));
}

// write a file header specifying the object pointer count
bool SonyDb::writeHeader(sonyFileHeader *h, FILE *f, int count)
{
	h->cte[0] = 0x01;
	h->cte[1] = 0x01;
	h->cte[2] = 0x00;
	h->cte[3] = 0x00;
	h->count = count;
	for (int i = 0; i < 7; i++)
		h->padding[i]= 0x00;

	if (fwrite(h, sizeof(sonyFileHeader), 1, f) == 1)
		return (true);
	else
		return (false);
}

// write an object pointer header
bool SonyDb::writeObjectPointer(sonyObjectPointer *p, FILE *f)
{
	p->padding = 0x00000000;

	if (fwrite(p, sizeof(sonyFileHeader), 1, f) == 1)
		return (true);
	else
		return (false);
	return (true);
}

//internal write an object header
bool SonyDb::writeObject(sonyObject *obj, FILE *f)
{
	if (fwrite(obj, sizeof(sonyFileHeader), 1, f) == 1)
		return (true);
	else
		return (false);
}


/*****************************************************************************************/

//internal write a track header
bool SonyDb::writeTrackHeader(sonyTrack *t, FILE *f)
{
	if (fwrite(t, sizeof(sonyTrack), 1, f) != 1)
	{
		fprintf(fp, "error could not write file header (object)\n");
		fflush(fp);
		return false;
	}
	return (true);
}

//internal write a track tag
bool SonyDb::writeTrackTag(sonyTrackTag *tt, char *input, FILE *f)
{  
	//init
	int size = TAGSIZE;
	utf16char *tagRecord;

	// The fixed record payload is measured in bytes, while the converter's
	// capacity is measured in UTF-16 code units.
	tagRecord = ansi_to_utf16(input, (size - 6) / sizeof(utf16char), true);

	//write tracktag (tagtype + encoding)
	if (fwrite(tt, sizeof(sonyTrackTag), 1, f) != 1) return (false);

	//write tag (data itself)
	if (fwrite(tagRecord, (size - 6), 1, f) != 1) return (false);
	free(tagRecord);

	return (true);
}

void SonyDb::setTable(char *TableFileName, int type)
{
	if (type == ENCODING_USE_KEY)
		setDeviceKeyFile(TableFileName);
	else if (TableFileName && *TableFileName)
	{
		if (decodeTableFilename) free(decodeTableFilename);
		decodeTableFilename = strdup(TableFileName);
		codeType = type;
	}
}

bool SonyDb::setDeviceKeyFile(const char *fileName)
{
	if (!fileName || !*fileName) return false;
	FILE *file = fopen(fileName, "rb");
	if (!file) return false;
	std::uint8_t bytes[4];
	const bool valid = fseek(file, 0x0a, SEEK_SET) == 0 &&
		fread(bytes, 1, sizeof(bytes), file) == sizeof(bytes);
	fclose(file);
	if (!valid) return false;
	DvId = (static_cast<std::uint32_t>(bytes[0]) << 24) |
		(static_cast<std::uint32_t>(bytes[1]) << 16) |
		(static_cast<std::uint32_t>(bytes[2]) << 8) | bytes[3];
	if (decodeTableFilename) free(decodeTableFilename);
	decodeTableFilename = strdup(fileName);
	codeType = ENCODING_USE_KEY;
	return true;
}

void SonyDb::clearDeviceKey()
{
	if (decodeTableFilename) free(decodeTableFilename);
	decodeTableFilename = 0;
	DvId = 0;
	codeType = ENCODING_USE_NONE;
}

bool SonyDb::isDeviceKeyConfigured() const { return codeType == ENCODING_USE_KEY; }
bool SonyDb::requiresDeviceKey() const { return deviceKeyRequired; }

std::string SonyDb::findDeviceKeyFile() const
{
	if (!driveLetter) return {};
	static const char *relativePaths[] = {
		"DvID.dat", "DvID.DAT", "OMGAUDIO/DvID.dat", "OMGAUDIO/DvID.DAT",
		"omgaudio/DvID.dat", "omgaudio/DvID.DAT", "MP3FM/DvID.dat", "MP3FM/DvID.DAT",
		"mp3fm/DvID.dat", "mp3fm/DvID.DAT", "JSYMPHONIC/DvID.dat", "JSYMPHONIC/DvID.DAT"
	};
	for (const char *relative : relativePaths)
	{
		filesystem::path candidate = filesystem::path(driveLetter) / relative;
		error_code error;
		if (filesystem::is_regular_file(candidate, error) && !error)
			return candidate.string();
	}
	return {};
}

#ifndef _WIN32
static bool run_scsi_command(int device, std::uint8_t *command, unsigned commandLength,
	void *data, unsigned dataLength, int direction)
{
	std::uint8_t sense[64]{};
	sg_io_hdr_t request{};
	request.interface_id = 'S';
	request.dxfer_direction = direction;
	request.cmd_len = static_cast<unsigned char>(commandLength);
	request.mx_sb_len = sizeof(sense);
	request.dxfer_len = dataLength;
	request.dxferp = data;
	request.cmdp = command;
	request.sbp = sense;
	request.timeout = 5000;
	return ioctl(device, SG_IO, &request) == 0 && request.status == 0 &&
		request.host_status == 0 && request.driver_status == 0 && request.resid == 0;
}

static std::string block_device_for_mount(const char *mountPoint)
{
	if (!mountPoint || !*mountPoint) return {};
	std::error_code error;
	const std::filesystem::path wanted = std::filesystem::weakly_canonical(mountPoint, error);
	if (error) return {};
	FILE *mounts = setmntent("/proc/self/mounts", "r");
	if (!mounts) return {};
	std::string result;
	struct mntent *entry;
	while ((entry = getmntent(mounts)) != NULL)
	{
		error.clear();
		const std::filesystem::path mounted = std::filesystem::weakly_canonical(entry->mnt_dir, error);
		if (error || mounted != wanted) continue;
		char resolved[PATH_MAX];
		if (realpath(entry->mnt_fsname, resolved) && strncmp(resolved, "/dev/", 5) == 0)
			result = resolved;
		break;
	}
	endmntent(mounts);
	return result;
}
#endif

bool SonyDb::provisionDeviceKeyFromPlayer()
{
	const std::string existing = findDeviceKeyFile();
	if (!existing.empty()) return setDeviceKeyFile(existing.c_str());
#ifdef _WIN32
	return false;
#else
	if (!driveLetter || !deviceKeyRequired) return false;
	const std::string blockDevice = block_device_for_mount(driveLetter);
	if (blockDevice.empty()) return false;
	const int device = open(blockDevice.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK);
	if (device < 0) return false;

	// Captured from Sony MP3 File Manager talking to an NW-E405. The first
	// command selects the 18-byte device-ID record; the second reads it. Both
	// commands are non-destructive and carry no media payload.
	std::uint8_t selectCommand[12] = {
		0xa3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xbc, 0x00, 0x14, 0x30, 0x00
	};
	std::uint8_t selectData[20] = {0x00, 0x12};
	std::uint8_t readCommand[12] = {
		0xa4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xbc, 0x00, 0x12, 0x3f, 0x00
	};
	std::uint8_t response[18]{};
	const bool queried = run_scsi_command(device, selectCommand, sizeof(selectCommand),
		selectData, sizeof(selectData), SG_DXFER_TO_DEV) &&
		run_scsi_command(device, readCommand, sizeof(readCommand), response,
			sizeof(response), SG_DXFER_FROM_DEV);
	close(device);
	if (!queried || response[0] != 0x00 || response[1] != 0x10)
		return false;
	bool nonzero = false;
	for (std::size_t index = 2; index < sizeof(response); ++index)
		nonzero = nonzero || response[index] != 0;
	if (!nonzero) return false;

	std::error_code error;
	const std::filesystem::path directory = std::filesystem::path(driveLetter) / "MP3FM";
	std::filesystem::create_directories(directory, error);
	if (error) return false;
	const std::filesystem::path destination = directory / "DvID.dat";
	const std::filesystem::path temporary = directory / ".DvID.dat.sonydb-tmp";
	FILE *output = fopen(temporary.c_str(), "wb");
	if (!output) return false;
	bool written = fwrite(response + 2, 1, 16, output) == 16;
	written = fflush(output) == 0 && written;
	written = fsync(fileno(output)) == 0 && written;
	written = fclose(output) == 0 && written;
	if (!written)
	{
		remove(temporary.c_str());
		return false;
	}
	std::filesystem::rename(temporary, destination, error);
	if (error)
	{
		remove(temporary.c_str());
		return false;
	}
	return setDeviceKeyFile(destination.c_str());
#endif
}

int SonyDb::getUnprotectedMp3Count() const
{
	int count = 0;
	for (const Song &song : songs)
		if (song.statusOfSong == ON_DEVICE && (song.encoding >> 24) == 0x03 &&
			song.protection == SONY_PROTECTION_NONE)
			++count;
	return count;
}

bool SonyDb::repairUnprotectedMp3Tracks()
{
	if (copying || !deviceKeyRequired || codeType != ENCODING_USE_KEY || hasPendingChanges())
		return false;
	bool allRepaired = true;
	bool repairedAny = false;
	for (Song &song : songs)
	{
		if (song.statusOfSong != ON_DEVICE || (song.encoding >> 24) != 0x03 ||
			song.protection != SONY_PROTECTION_NONE)
			continue;
		if (!encrypt_unprotected_oma(song.filename, song.sonyDbOrder, DvId))
		{
			allRepaired = false;
			continue;
		}
		song.protection = SONY_PROTECTION_ENCRYPTED_MP3;
		memset(song.cidRecord, 0, sizeof(song.cidRecord));
		song.hasCidRecord = true;
		repairedAny = true;
	}
	if (!repairedAny) return allRepaired;
	databaseDirty = true;
	return writeTracks() && allRepaired;
}



//ported from gym
bool SonyDb::loadCodeTable(int id)
{
	FILE *t;
	std::uint8_t *header1 = (std::uint8_t*)malloc(sizeof(std::uint8_t) * 134); //deserialize manually ...
	std::uint8_t *header2 = (std::uint8_t*)malloc(sizeof(std::uint8_t) * 10);
	std::uint8_t *tmp = (std::uint8_t*)malloc(sizeof(std::uint8_t) * 256);

	t = fopen(decodeTableFilename, "rb");
	// place cursor in place...
	fseek(t, 0x4B0 * id, SEEK_SET);
	fread(header1, sizeof(std::uint8_t), 134, t);

	for (int i = 0; i < 4; i++)
	{
		fread(tmp, sizeof(std::uint8_t), 256, t);
		for (int j = 0; j < 256; j++)
			codeTable[(i*256) + j] = tmp[j];
		fread(header2, sizeof(std::uint8_t), 10, t);
	}

	free(header1);
	free(header2);
	free(tmp);
	fclose(t);
	return true;
}

//add mp3 file to device
bool SonyDb::addOMA(Song *s, int destination)
{
	std::uint32_t    key = 0xFFFFFFFF;
	bool      isVBR = false;

	this->copyPercent = 0;
	if (s->statusOfSong != ADD_TO_DEVICE)
		return false;
	s->protection = codeType == ENCODING_USE_NONE ? SONY_PROTECTION_NONE :
		SONY_PROTECTION_ENCRYPTED_MP3;
	memset(s->cidRecord, 0, sizeof(s->cidRecord));
	s->hasCidRecord = true;

	char *filename = GetOMAFilename(destination);

	fprintf(fp, "sending %s to %s\n", s->filename, filename);
	fflush(fp);

	//createDirectory if necessary
	createDir(destination);

	//load decode table for this track
	if (this->codeType == ENCODING_USE_TABLE)
		this->loadCodeTable(destination - 1);

	//or use key
	if (this->codeType == ENCODING_USE_KEY)
		key = ( 0x2465 + destination * 0x5296E435 ) ^ this->DvId;


	FILE *fout = fopen(filename, "wb");
	if (fout == NULL)
	{
		fprintf(fp, "error can't open file %s\n", filename);
		fflush(fp);
		return false;
	}

	FILE *fin = fopen(s->filename, "rb");
	float finSize = 0;
	if (fin == NULL)
	{
		fprintf(fp, "error can't open file %s\n", s->filename);
		fflush(fp);
		fclose(fout);
		return false;
	}

	//get input file size
	struct stat results;
	if (stat(s->filename, &results) == 0)
		finSize = results.st_size;

	std::uint8_t	    *header = (std::uint8_t*)malloc(sizeof(std::uint8_t) * 11);
	utf16char	    *tagRecord;
	char	    *tmpTag = (char*)malloc(sizeof(char) * 256);
	int	    tagLength;
	int	    headerLength = 0;
	long	    startPoint = 0;
	int	    nbFrames = 0; // estimated number of frames

	memset(header, 0, 11);
	if (fread(header, 1, 10, fin) != 10)
	{
		free(header);
		free(tmpTag);
		fclose(fout);
		fclose(fin);
		remove(filename);
		free(filename);
		return false;
	}
	if (STRNCMP_NULLOK((char*)header, "ID3", 3) == 0)
	{
		//skip Idv2 tags
		startPoint = ((header[6] << 21) + (header[7] << 14) + (header[8] << 7) + header[9]) + 10;

	}
	else
	{
		//no idv2 tag found
		startPoint = 0;
	}

	//go to the first frame
	fseek(fin, startPoint, SEEK_SET);

	memset(header, 0, 11);
	//skip zeros
	while (header[0] == 0)
	{
		if(fread(header, sizeof(std::uint8_t), 1, fin) != 1)
		{
			free(header);
			free(tmpTag);
			fclose(fout);
			fclose(fin);
			remove(filename);
			free(filename);
			return false;
		}
	}

	if (header[0] == 0xFF)
	{
		if (fread(header, sizeof(std::uint8_t), 3, fin) != 3)
		{
			fclose(fout);
			fclose(fin);
			return false;
		}

		//get mpeg type, layer type and bitrate
		if ((header[0] & 0xE0) == 0xE0) //we found the first frame
		{
			// OMGAUDIO stores a complete four-byte codec description.  The old
			// code retained only the third byte, so the database did not identify
			// transferred files as MP3 and affected players reported CANNOT PLAY.
			const std::uint8_t codecParameters =
				((header[0] & 0x18) << 3) | ((header[0] & 0x06) << 3) |
				((header[1] & 0xF0) >> 4);
			const std::uint8_t codecMode =
				((header[1] & 0x0C) << 4) | ((header[2] & 0xC0) >> 2) |
				((header[2] & 0x03) << 2);
			s->encoding = (0x03U << 24) | (0x80U << 16) |
				(static_cast<std::uint32_t>(codecParameters) << 8) | codecMode;

			// 00 - MPEG Version 2.5 (unofficial extension of MPEG 2)
			// 01 - reserved
			// 10 - MPEG Version 2 (ISO/IEC 13818-3)
			// 11 - MPEG Version 1 (ISO/IEC 11172-3) 
			std::uint8_t mpegVersion = (header[0] & 0x18) >> 3;

			//00 - reserved
			//01 - Layer III
			//10 - Layer II
			//11 - Layer I
			std::uint8_t layerVersion = (header[0] & 0x06) >> 1;

			std::uint8_t samplingRateIndex = (header[1] & 0xC) >> 2;

			if (mpegVersion == 1 || layerVersion != 1 ||
				(header[1] & 0xF0) == 0 || (header[1] & 0xF0) == 0xF0 ||
				samplingRateIndex == 3 ||
				((mpegVersion * 3) + samplingRateIndex >= 12) ||
				((mpegVersion * 4) + layerVersion >= 16))
			{
				//header is invalid
				nbFrames = 0;
			}
			else
			{
				int  SAMPLING_RATES[] = {11025, 12000, 8000, 0, 0, 0, 22050, 24000, 16000, 44100, 48000, 32000};
				int  SAMPLE_PER_FRAME[] = {0,576,1152,384,0,0,0,0,0,576,1152,384,0,1152,1152,384};

				//sample per frame 0=reserved
				//          MPG2.5 res        MPG2   MPG1 
				//reserved  0      0          0      0
				//Layer III 576    0          576    1152 	
				//Layer II  1152   0          1152   1152 	
				//Layer I   384    0          384    384 

				int samplingRate = SAMPLING_RATES[(mpegVersion * 3) + samplingRateIndex];
				int samplePerFrame = SAMPLE_PER_FRAME[(mpegVersion * 4) + layerVersion];
				nbFrames = (s->songlen * samplingRate) / samplePerFrame;
			}

			//skip the the frame header
			fseek(fin, sizeof(std::uint8_t) * 32, SEEK_CUR);

			//check if first is "XING" for VBR files
			memset(header, 0, 11);
			if (fread(header, sizeof(std::uint8_t), 4, fin) != 4)
			{
				fclose(fout);
				fclose(fin);
				return false;
			}

			if (STRNCMP_NULLOK((char*)header, "XING", 4) == 0)
			{
				isVBR = true;
				s->encoding |= (0x10U << 16);
			}
		}
		else
		{
			fprintf(fp, "File format invalid, could not find first frame%s\n", s->filename);
			fflush(fp);
			free(header);
			free(tmpTag);
			fclose(fout);
			fclose(fin);
			remove(filename);
			free(filename);
			return false;
		}
	}
	else
	{
		fprintf(fp, "File format invalid, could not find first frame%s\n", s->filename);
		fflush(fp);
		free(header);
		free(tmpTag);
		fclose(fout);
		fclose(fin);
		remove(filename);
		free(filename);
		return false;
	}



	//go back to the start of mp3 data
	fseek(fin, startPoint, SEEK_SET);

	//write the OMA headers :
	//format is :
	//"ea3"0x03 (4bytes), sizeOfTotaltags (6bytes)
	//TIT2(4bytes) sizeOfTag(4bytes) 2flags (2bytes) 0x02(=utf16be format?) titleOftheSong
	//...
	//not all size are coded in synchsafe format


	//idv2 header
	memset(header, 0, 11);
	header[0] = 'e';
	header[1] = 'a';
	header[2] = '3';
	header[3] = 0x03;
	//...zeros
	header[8] = 0x1f;
	header[9] = 0x76; //size of tag header in Synchsafe format (=4096 bytes - 10 of header)
	if (fwrite(header, sizeof(std::uint8_t), 10, fout) != 10)
	{
		fclose(fout);
		fclose(fin);
		return false;
	}

	//title tag
	memset(header, 0, 11);
	memcpy(header, "TIT2", 4);
	tagLength = static_cast<int>(utf8_to_utf16_string(s->title).size());
	tagRecord = ansi_to_utf16(s->title, tagLength + 1, true);
	tagLength = (tagLength * 2) + 1;
	header[4] = NOT_SYNCHSAFE_B1(tagLength);//size of the title
	header[5] = NOT_SYNCHSAFE_B2(tagLength);
	header[6] = NOT_SYNCHSAFE_B3(tagLength);
	header[7] = NOT_SYNCHSAFE_B4(tagLength);
	header[8] = 0;   //flag 1
	header[9] = 0;  //flag 2
	header[10] = 0x02;  //
	tagLength = (tagLength - 1 )/ 2;
	if (fwrite(header, sizeof(std::uint8_t), 11, fout) != 11)
	{
		fclose(fout);
		fclose(fin);
		return false;
	}
	headerLength += (11 * sizeof(std::uint8_t));
	if (fwrite(tagRecord, sizeof(utf16char), tagLength, fout) != tagLength)
	{
		fclose(fout);
		fclose(fin);
		return false;
	}
	headerLength += (tagLength * sizeof(utf16char));
	free(tagRecord);


	//artist tag
	memset(header, 0, 11);
	memcpy(header, "TPE1", 4);
	tagLength = static_cast<int>(utf8_to_utf16_string(s->artist).size());
	tagRecord = ansi_to_utf16(s->artist, tagLength + 1, true);
	tagLength = (tagLength * 2) + 1;
	header[4] = NOT_SYNCHSAFE_B1(tagLength);//size of the title
	header[5] = NOT_SYNCHSAFE_B2(tagLength);
	header[6] = NOT_SYNCHSAFE_B3(tagLength);
	header[7] = NOT_SYNCHSAFE_B4(tagLength);
	header[8] = 0;   //flag 1
	header[9] = 0;  //flag 2
	header[10] = 0x02;  //
	tagLength = (tagLength - 1 )/ 2;
	if (fwrite(header, sizeof(std::uint8_t), 11, fout) != 11)
	{
		fclose(fout);
		fclose(fin);
		return false;
	}
	headerLength += (11 * sizeof(std::uint8_t));
	if (fwrite(tagRecord, sizeof(utf16char), tagLength, fout) != tagLength)
	{
		fclose(fout);
		fclose(fin);
		return false;
	}
	headerLength += (tagLength * sizeof(utf16char));
	free(tagRecord);

	//album tag
	memset(header, 0, 11);
	memcpy(header, "TALB", 4);
	tagLength = static_cast<int>(utf8_to_utf16_string(s->album).size());
	tagRecord = ansi_to_utf16(s->album, tagLength + 1, true);
	tagLength = (tagLength * 2) + 1;
	header[4] = NOT_SYNCHSAFE_B1(tagLength);//size of the title
	header[5] = NOT_SYNCHSAFE_B2(tagLength);
	header[6] = NOT_SYNCHSAFE_B3(tagLength);
	header[7] = NOT_SYNCHSAFE_B4(tagLength);
	header[8] = 0;  //flag 1
	header[9] = 0;  //flag 2
	header[10] = 0x02;  //
	tagLength = (tagLength - 1 )/ 2;
	if (fwrite(header, sizeof(std::uint8_t), 11, fout) != 11)
	{
		fclose(fout);
		fclose(fin);
		return false;
	}
	headerLength += (11 * sizeof(std::uint8_t));
	if (fwrite(tagRecord, sizeof(utf16char), tagLength, fout) != tagLength)
	{
		fclose(fout);
		fclose(fin);
		return false;
	}
	headerLength += (tagLength * sizeof(utf16char));
	free(tagRecord);

	//genre tag
	memset(header, 0, 11);
	memcpy(header, "TCON", 4);
	tagLength = static_cast<int>(utf8_to_utf16_string(s->genre).size());
	tagRecord = ansi_to_utf16(s->genre, tagLength + 1, true);
	tagLength = (tagLength * 2) + 1;
	header[4] = NOT_SYNCHSAFE_B1(tagLength);//size of the title
	header[5] = NOT_SYNCHSAFE_B2(tagLength);
	header[6] = NOT_SYNCHSAFE_B3(tagLength);
	header[7] = NOT_SYNCHSAFE_B4(tagLength);
	header[8] = 0;   //flag 1
	header[9] = 0;  //flag 2
	header[10] = 0x02;  //
	tagLength = (tagLength - 1 )/ 2;
	if (fwrite(header, sizeof(std::uint8_t), 11, fout) != 11)
	{
		fclose(fout);
		fclose(fin);
		return false;
	}
	headerLength += (11 * sizeof(std::uint8_t));
	if (fwrite(tagRecord, sizeof(utf16char), tagLength, fout) != tagLength)
	{
		fclose(fout);
		fclose(fin);
		return false;
	}
	headerLength += (tagLength * sizeof(utf16char));
	free(tagRecord);

	//track number tag //space are suppose to be replaced by 0x00 not 0x20
	sprintf(tmpTag, "OMG_TRACK %02i", s->track_nr);
	memset(header, 0, 11);
	memcpy(header, "TXXX", 4);
	tagLength = strlen(tmpTag);
	tagRecord = ansi_to_utf16(tmpTag, tagLength + 1, true);
	tagLength = (tagLength * 2) + 1;
	header[4] = NOT_SYNCHSAFE_B1(tagLength);//size of the title
	header[5] = NOT_SYNCHSAFE_B2(tagLength);
	header[6] = NOT_SYNCHSAFE_B3(tagLength);
	header[7] = NOT_SYNCHSAFE_B4(tagLength);
	header[8] = 0;   //flag 1
	header[9] = 0;  //flag 2
	header[10] = 0x02;  //
	tagLength = (tagLength - 1 )/ 2;
	if (fwrite(header, sizeof(std::uint8_t), 11, fout) != 11)
	{
		fclose(fout);
		fclose(fin);
		return false;
	}
	headerLength += (11 * sizeof(std::uint8_t));
	if (fwrite(tagRecord, sizeof(utf16char), tagLength, fout) != tagLength)
	{
		fclose(fout);
		fclose(fin);
		return false;
	}
	headerLength += (tagLength * sizeof(utf16char));
	free(tagRecord);

	//fixme date of transfert? tag //space are suppose to be replaced by 0x00 not 0x20
	sprintf(tmpTag, "OMG_TRLDA 2001/01/01 00:00:00");
	memset(header, 0, 11);
	memcpy(header, "TXXX", 4);
	tagLength = strlen(tmpTag);
	tagRecord = ansi_to_utf16(tmpTag, tagLength + 1, true);
	tagLength = (tagLength * 2) + 1;
	tagLength = (strlen(tmpTag) + 1) * 2;
	header[4] = NOT_SYNCHSAFE_B1(tagLength);//size of the title
	header[5] = NOT_SYNCHSAFE_B2(tagLength);
	header[6] = NOT_SYNCHSAFE_B3(tagLength);
	header[7] = NOT_SYNCHSAFE_B4(tagLength);
	header[8] = 0;   //flag 1
	header[9] = 0;  //flag 2
	header[10] = 0x02;  //
	tagLength = (tagLength - 1 )/ 2;
	if (fwrite(header, sizeof(std::uint8_t), 11, fout) != 11)
	{
		fclose(fout);
		fclose(fin);
		return false;
	}
	headerLength += (11 * sizeof(std::uint8_t));
	if (fwrite(tagRecord, sizeof(utf16char), tagLength, fout) != tagLength)
	{
		fclose(fout);
		fclose(fin);
		return false;
	}
	headerLength += (tagLength * sizeof(utf16char));
	free(tagRecord);

	//track length tag
	sprintf(tmpTag, "%i", s->songlen * 1000);
	memset(header, 0, 11);
	memcpy(header, "TLEN", 4);
	tagLength = strlen(tmpTag);
	tagRecord = ansi_to_utf16(tmpTag, tagLength + 1, true);
	tagLength = (tagLength * 2) + 1;
	header[4] = NOT_SYNCHSAFE_B1(tagLength);//size of the title
	header[5] = NOT_SYNCHSAFE_B2(tagLength);
	header[6] = NOT_SYNCHSAFE_B3(tagLength);
	header[7] = NOT_SYNCHSAFE_B4(tagLength);
	header[8] = 0;   //flag 1
	header[9] = 0;  //flag 2
	header[10] = 0x02;  //
	tagLength = (tagLength - 1 )/ 2;
	if (fwrite(header, sizeof(std::uint8_t), 11, fout) != 11)
	{
		fclose(fout);
		fclose(fin);
		return false;
	}
	headerLength += (11 * sizeof(std::uint8_t));
	if (fwrite(tagRecord, sizeof(utf16char), tagLength, fout) != tagLength)
	{
		fclose(fout);
		fclose(fin);
		return false;
	}
	headerLength += (tagLength * sizeof(utf16char));
	free(tagRecord);

	memset(header, 0, 11);
	//fill the rest with 0
	while (headerLength < 4086)
	{
		headerLength += sizeof(std::uint8_t);
		if (fwrite(header, sizeof(std::uint8_t), 1, fout) != 1)
		{
			fclose(fout);
			fclose(fin);
			return false;
		}
	}

	//write second header fixme (some stuff are missing here... important?)
	std::uint8_t *header2 = (std::uint8_t*)malloc(sizeof(std::uint8_t) * 16);
	headerLength = 0;
	memset(header2, 0, 16);
	//first line
	memcpy(header2, "EA3", 3);
	header2[3] = 0x02;
	header2[4] = 0;   //size of 2nd header
	header2[5] = 0x60;//size of 2nd header
	header2[6] = static_cast<std::uint8_t>(s->protection >> 8);
	header2[7] = static_cast<std::uint8_t>(s->protection & 0xff);
	if (fwrite(header2, sizeof(std::uint8_t), 16, fout) != 16)
	{
		fclose(fout);
		fclose(fin);
		return false;
	}
	headerLength += (16 * sizeof(std::uint8_t));

	//second line
	memset(header2, 0, 16);
	// These 24 bytes are reserved for LSI DRM. Encrypted MP3 uses zeroes.
	if (fwrite(header2, sizeof(std::uint8_t), 16, fout) != 16)
	{
		fclose(fout);
		fclose(fin);
		return false;
	}
	headerLength += (16 * sizeof(std::uint8_t));

	//third line
	memset(header2, 0, 16); 

	header2[0] = static_cast<std::uint8_t>((s->encoding >> 24) & 0xff);// 3 = MP3
	header2[1] = static_cast<std::uint8_t>((s->encoding >> 16) & 0xff);
	header2[2] = static_cast<std::uint8_t>((s->encoding >> 8) & 0xff);
	header2[3] = static_cast<std::uint8_t>(s->encoding & 0xff);

	//tracklength
	std::uint32_t trackLengh = s->songlen * 1000; 
	header2[4] = (std::uint8_t) (((trackLengh) & (std::uint32_t) 0xff000000U) >> 24);
	header2[5] = (std::uint8_t) (((trackLengh) & (std::uint32_t) 0x00ff0000U) >> 16);
	header2[6] = (std::uint8_t) (((trackLengh) & (std::uint32_t) 0x0000ff00U) >>  8);
	header2[7] = (std::uint8_t) ((trackLengh) & (std::uint32_t)  0x000000ffU);

	//number of frames
	header2[8] = (std::uint8_t) (((nbFrames) & (std::uint32_t) 0xff000000U) >> 24);
	header2[9] = (std::uint8_t) (((nbFrames) & (std::uint32_t) 0x00ff0000U) >>  16);
	header2[10] = (std::uint8_t) (((nbFrames) & (std::uint32_t) 0x0000ff00U) >>  8);
	header2[11] = (std::uint8_t) ((nbFrames) & (std::uint32_t)  0x000000ffU); 

	//padding
	header2[12] = 0x00;
	header2[13] = 0x00;
	header2[14] = 0x00;
	header2[15] = 0x00;
	if (fwrite(header2, sizeof(std::uint8_t), 16, fout) != 16)
	{
		fclose(fout);
		fclose(fin);
		return false;
	}
	headerLength += (16 * sizeof(std::uint8_t));

	//padding
	memset(header2, 0, 16);
	if (fwrite(header2, sizeof(std::uint8_t), 16, fout) != 16)
	{
		fclose(fout);
		fclose(fin);
		return false;
	}
	if (fwrite(header2, sizeof(std::uint8_t), 16, fout) != 16)
	{
		fclose(fout);
		fclose(fin);
		return false;
	}
	if (fwrite(header2, sizeof(std::uint8_t), 16, fout) != 16)
	{
		fclose(fout);
		fclose(fin);
		return false;
	}

	free(header2);
	free(header);
	free(tmpTag);


	int   BLOCK_SIZE = 32767;
	int   blockNumber = 1;
	std::uint8_t *inputData = (std::uint8_t *)malloc(sizeof(std::uint8_t) * BLOCK_SIZE);
	std::uint8_t *outputData = (std::uint8_t *)malloc(sizeof(std::uint8_t) * BLOCK_SIZE);
	int   nbRead;
	long  position = 0;

	// input file -> decode -> output file
	while ((nbRead = fread(inputData, 1, BLOCK_SIZE, fin)) != 0)
	{ 
		for (int i = 0; i < nbRead; i++)
		{	  
			if (this->codeType == ENCODING_USE_NONE)//just copy without decoding
				outputData[i] = inputData[i]; 
			if (this->codeType == ENCODING_USE_TABLE) //keyEncodeTable.dat
				outputData[i] = codeTable[((position % 4) * 256) + inputData[i]];
			if (this->codeType == ENCODING_USE_KEY) //DvId.dat
			{
				if ((position % 4) == 0) outputData[i] = ((inputData[i]) ^ ((key & 0xFF000000) >> 24));
				if ((position % 4) == 1) outputData[i] = ((inputData[i]) ^ ((key & 0x00FF0000) >> 16));
				if ((position % 4) == 2) outputData[i] = ((inputData[i]) ^ ((key & 0x0000FF00) >> 8));
				if ((position % 4) == 3) outputData[i] = ((inputData[i]) ^ (key & 0x000000FF));
			}
			position++;
		}
		if (fwrite(outputData, sizeof(std::uint8_t), nbRead, fout) != nbRead)
		{
			fclose(fout);
			fclose(fin);
			return false;
		}
		blockNumber++;
		totalByteLeftToWrite -= BLOCK_SIZE;

		if ((finSize > 0) && (BLOCK_SIZE > 0))
			this->copyPercent += 100 / (finSize / BLOCK_SIZE);
	}
	this->copyPercent = 100;
	//s->statusOfSong = ON_DEVICE;

	free(inputData);
	free(outputData);

	fclose(fout);
	fclose(fin);
	return (true);
}

//returns the number of tracks to be added/deleted/kept
int  SonyDb::getNumberOfTracks()
{
	return (songs.size());
}

//returns the copy progress for the current file
int  SonyDb::getCopyPercent()
{
	return (int)(this->copyPercent);
}

static string sanitizeExportName(const char *value)
{
	string result = value ? value : "";
	for (string::iterator ch = result.begin(); ch != result.end(); ++ch)
	{
		if (*ch == '/' || *ch == '\\' || static_cast<unsigned char>(*ch) < 0x20)
			*ch = '_';
	}
	return result;
}

static string exportDirectoryName(const char *value, const char *fallback)
{
	string result = sanitizeExportName(value);
	if (result.empty() || result == "." || result == "..")
		return fallback;
	return result;
}

static string exportArtistFor(const Song *song, const vector<Song> &songs)
{
	const string artist = exportDirectoryName(song->artist, "Unknown Artist");
	if (!song->album || !*song->album)
		return artist;
	for (vector<Song>::const_iterator other = songs.begin(); other != songs.end(); ++other)
	{
		if (other->statusOfSong != ON_DEVICE ||
			STRCMP2_NULLOK(other->album, song->album) != 0)
			continue;
		if (STRCMP2_NULLOK(exportDirectoryName(other->artist, "Unknown Artist").c_str(),
			artist.c_str()) != 0)
			return "Various Artists";
	}
	return artist;
}

static string exportPathFor(const Song *song, const vector<Song> &songs,
	const char *destination)
{
	if (!song || !destination || !*destination)
		return "";
	const filesystem::path directory = filesystem::path(destination)
		/ exportArtistFor(song, songs)
		/ exportDirectoryName(song->album, "Unknown Album");
	char track[32];
	snprintf(track, sizeof(track), "%02i", song->track_nr);
	const string title = exportDirectoryName(song->title, "Unknown Title");
	return (directory / (string(track) + " - " + title + ".mp3")).string();
}

string SonyDb::exportPathForSong(int order, const char *destination) const
{
	for (vector<Song>::const_iterator song = songs.begin(); song != songs.end(); ++song)
	{
		if (song->sonyDbOrder == order && song->statusOfSong == ON_DEVICE)
			return exportPathFor(&(*song), songs, destination);
	}
	return "";
}

int SonyDb::exportSong(int order, const char *destination, bool overwrite,
	string *outputPath)
{
	const string path = exportPathForSong(order, destination);
	if (outputPath)
		*outputPath = path;
	if (path.empty())
		return EXPORT_NOT_FOUND;
	return exportSongToFile(order, path.c_str(), overwrite);
}

int SonyDb::exportSongToFile(int order, const char *outputFile, bool overwrite)
{
	if (!outputFile || !*outputFile)
		return EXPORT_FAILED;
	for (vector<Song>::iterator song = songs.begin(); song != songs.end(); ++song)
	{
		if (song->sonyDbOrder != order || song->statusOfSong != ON_DEVICE)
			continue;
		const string path = outputFile;
		struct stat existing;
		if (!overwrite && stat(path.c_str(), &existing) == 0)
			return EXPORT_ALREADY_EXISTS;
		error_code directoryError;
		filesystem::create_directories(filesystem::path(path).parent_path(), directoryError);
		if (directoryError)
			return EXPORT_FAILED;
		if (!getOMAToFile(&(*song), path.c_str()))
			return EXPORT_FAILED;
		return EXPORT_OK;
	}
	return EXPORT_NOT_FOUND;
}

//some code is from GYM
bool SonyDb::getOMA(Song *s, char *destination)
{
	if (!s || !destination)
		return false;
	const string filename = exportPathFor(s, songs, destination);
	return getOMAToFile(s, filename.c_str());
}

bool SonyDb::getOMAToFile(Song *s, const char *outputFile)
{
	if (!s || !outputFile || !*outputFile || s->statusOfSong == ADD_TO_DEVICE)
		return (false);

	const string filename = outputFile;
	const string partialFilename = filename + ".sonydb-part";
	std::uint8_t	    *header = (std::uint8_t*)malloc(sizeof(std::uint8_t) * 11);
	char	    *tmpTag = (char*)malloc(sizeof(char) * 256);
	int	    tagLength;
	int	    headerLength = 0;
	std::uint32_t	    key = 0xFFFFFFFF;

	//open the file
	fprintf(fp, "getting file from %s to %s\n", s->filename, filename.c_str());
	fflush(fp);

	FILE *fin = fopen(s->filename, "rb");
	if (fin == NULL)
	{
		fprintf(fp, "error can't open file %s\n", s->filename);fflush(fp);
		return false;
	}

	// Locate the audio from the size stored in the ea3 metadata header.  Sony
	// software and different Walkman generations use different metadata block
	// sizes, so the old fixed 0xC60 offset truncated or corrupted some exports.
	std::uint8_t omaMetadataHeader[10];
	std::uint8_t omaFormatHeader[96];
	if (fread(omaMetadataHeader, 1, sizeof(omaMetadataHeader), fin) != sizeof(omaMetadataHeader) ||
		memcmp(omaMetadataHeader, "ea3", 3) != 0)
	{
		fprintf(fp, "error invalid OMA metadata header: %s\n", s->filename);fflush(fp);
		fclose(fin);
		return false;
	}
	const long metadataSize =
		((omaMetadataHeader[6] & 0x7f) << 21) |
		((omaMetadataHeader[7] & 0x7f) << 14) |
		((omaMetadataHeader[8] & 0x7f) << 7) |
		(omaMetadataHeader[9] & 0x7f);
	const long formatOffset = 10 + metadataSize;
	if (metadataSize < 0 || fseek(fin, formatOffset, SEEK_SET) != 0 ||
		fread(omaFormatHeader, 1, sizeof(omaFormatHeader), fin) != sizeof(omaFormatHeader) ||
		memcmp(omaFormatHeader, "EA3", 3) != 0 || omaFormatHeader[32] != 0x03)
	{
		fprintf(fp, "error OMA does not contain MP3 audio: %s\n", s->filename);fflush(fp);
		fclose(fin);
		return false;
	}
	const std::uint16_t encryption =
		(static_cast<std::uint16_t>(omaFormatHeader[6]) << 8) | omaFormatHeader[7];
	if ((encryption == SONY_PROTECTION_ENCRYPTED_MP3 && this->codeType == ENCODING_USE_NONE) ||
		(encryption != SONY_PROTECTION_NONE && encryption != SONY_PROTECTION_ENCRYPTED_MP3))
	{
		fprintf(fp, "error unsupported OMA encryption 0x%04x: %s\n", encryption, s->filename);fflush(fp);
		fclose(fin);
		return false;
	}
	const long audioOffset = formatOffset + static_cast<long>(sizeof(omaFormatHeader));
	if (fseek(fin, audioOffset, SEEK_SET) != 0)
	{
		fclose(fin);
		return false;
	}

	FILE *fout = fopen(partialFilename.c_str(), "wb");
	if (fout == NULL)
	{
		fprintf(fp, "error can't open file %s\n", partialFilename.c_str());fflush(fp);
		fclose(fin);
		return false;
	}

	//load decode table for this track
	if (encryption == SONY_PROTECTION_ENCRYPTED_MP3 && this->codeType == ENCODING_USE_TABLE)
		this->loadCodeTable(s->sonyDbOrder - 1);

	//or use key
	if (encryption == SONY_PROTECTION_ENCRYPTED_MP3 && this->codeType == ENCODING_USE_KEY)
		key = ( 0x2465 + s->sonyDbOrder * 0x5296E435 ) ^ this->DvId;

	//reserve 10bytes for the header
	memset(header, 0, 11);
	fwrite(header, sizeof(std::uint8_t), 10, fout);

	//title tag
	memset(header, 0, 11);
	memcpy(header, "TIT2", 4);
	tagLength = strlen(s->title) + 1;
	header[4] = SYNCHSAFE_B1(tagLength);//size of the title
	header[5] = SYNCHSAFE_B2(tagLength);
	header[6] = SYNCHSAFE_B3(tagLength);
	header[7] = SYNCHSAFE_B4(tagLength);
	fwrite(header, sizeof(std::uint8_t), 11, fout);
	headerLength += (11 * sizeof(std::uint8_t));
	fwrite(s->title, sizeof(std::uint8_t), (tagLength - 1), fout);
	headerLength += ((tagLength - 1) * sizeof(std::uint8_t));


	//artist tag
	memset(header, 0, 11);
	memcpy(header, "TPE1", 4);
	tagLength = strlen(s->artist) + 1;
	header[4] = SYNCHSAFE_B1(tagLength);//size of the title
	header[5] = SYNCHSAFE_B2(tagLength);
	header[6] = SYNCHSAFE_B3(tagLength);
	header[7] = SYNCHSAFE_B4(tagLength);
	fwrite(header, sizeof(std::uint8_t), 11, fout);
	headerLength += (11 * sizeof(std::uint8_t));
	fwrite(s->artist, sizeof(std::uint8_t), (tagLength - 1), fout);
	headerLength += ((tagLength - 1) * sizeof(std::uint8_t));


	//album tag
	memset(header, 0, 11);
	memcpy(header, "TALB", 4);
	tagLength = strlen(s->album) + 1;
	header[4] = SYNCHSAFE_B1(tagLength);//size of the title
	header[5] = SYNCHSAFE_B2(tagLength);
	header[6] = SYNCHSAFE_B3(tagLength);
	header[7] = SYNCHSAFE_B4(tagLength);
	fwrite(header, sizeof(std::uint8_t), 11, fout);
	headerLength += (11 * sizeof(std::uint8_t));
	fwrite(s->album, sizeof(std::uint8_t), (tagLength - 1), fout);
	headerLength += ((tagLength - 1) * sizeof(std::uint8_t));

	//track number tag
	sprintf(tmpTag, "%02i", s->track_nr);
	memset(header, 0, 11);
	memcpy(header, "TRCK", 4);
	tagLength = strlen(tmpTag) + 1;
	header[4] = SYNCHSAFE_B1(tagLength);//size of the title
	header[5] = SYNCHSAFE_B2(tagLength);
	header[6] = SYNCHSAFE_B3(tagLength);
	header[7] = SYNCHSAFE_B4(tagLength);
	fwrite(header, sizeof(std::uint8_t), 11, fout);
	headerLength += (11 * sizeof(std::uint8_t));
	fwrite(tmpTag, sizeof(std::uint8_t), (tagLength - 1), fout);
	headerLength += ((tagLength - 1) * sizeof(std::uint8_t));

	//year tag
	if (s->year > 0)
	{
		sprintf(tmpTag, "%i", s->year);
		memset(header, 0, 11);
		memcpy(header, "TYER", 4);
		tagLength = strlen(tmpTag) + 1;
		header[4] = SYNCHSAFE_B1(tagLength);//size of the title
		header[5] = SYNCHSAFE_B2(tagLength);
		header[6] = SYNCHSAFE_B3(tagLength);
		header[7] = SYNCHSAFE_B4(tagLength);
		fwrite(header, sizeof(std::uint8_t), 11, fout);
		headerLength += (11 * sizeof(std::uint8_t));
		fwrite(tmpTag, sizeof(std::uint8_t), (tagLength - 1), fout);
		headerLength += ((tagLength - 1) * sizeof(std::uint8_t));
	}

	//genre tag
	memset(header, 0, 11);
	memcpy(header, "TCON", 4);
	tagLength = strlen(s->genre) + 1;
	header[4] = SYNCHSAFE_B1(tagLength);//size of the title
	header[5] = SYNCHSAFE_B2(tagLength);
	header[6] = SYNCHSAFE_B3(tagLength);
	header[7] = SYNCHSAFE_B4(tagLength);
	fwrite(header, sizeof(std::uint8_t), 11, fout);
	headerLength += (11 * sizeof(std::uint8_t));
	fwrite(s->genre, sizeof(std::uint8_t), (tagLength - 1), fout);
	headerLength += ((tagLength - 1) * sizeof(std::uint8_t));

	memset(header, 0, 11);
	//fill the rest with 0
	while ((headerLength %16) != 0)
	{
		headerLength += sizeof(std::uint8_t);
		fwrite(header, sizeof(std::uint8_t), 1, fout);
	}
	//return to the start of the file to write the header;
	fseek(fout, 0, SEEK_SET);
	memset(header, 0, 11);
	header[0] = 'I';
	header[1] = 'D';
	header[2] = '3';
	header[3] = 0x03;
	//...zeros
	header[6] = SYNCHSAFE_B1(headerLength);
	header[7] = SYNCHSAFE_B2(headerLength);
	header[8] = SYNCHSAFE_B3(headerLength);
	header[9] = SYNCHSAFE_B4(headerLength);
	fwrite(header, sizeof(std::uint8_t), 10, fout);

	//return after the header
	fseek(fout, headerLength, SEEK_CUR);

	free(header);
	free(tmpTag);

	// The input is already positioned immediately before the OMA audio payload.

	int   BLOCK_SIZE = 32767;
	int   blockNumber = 1;
	std::uint8_t *inputData = (std::uint8_t *)malloc(sizeof(std::uint8_t) * BLOCK_SIZE);
	std::uint8_t *outputData = (std::uint8_t *)malloc(sizeof(std::uint8_t) * BLOCK_SIZE);
	int   nbRead;
	long  position = 0;

	// input file -> decode -> output file
	while ((nbRead = fread(inputData, 1, BLOCK_SIZE, fin)) != 0)
	{ 
		for (int i = 0; i < nbRead; i++)
		{
			if (encryption == SONY_PROTECTION_NONE) //just copy without decoding
				outputData[i] = inputData[i];
			if (encryption == SONY_PROTECTION_ENCRYPTED_MP3 && this->codeType == ENCODING_USE_TABLE) //decodeKeys.dat
				outputData[i] = codeTable[((position % 4) * 256) + inputData[i]];
			if (encryption == SONY_PROTECTION_ENCRYPTED_MP3 && this->codeType == ENCODING_USE_KEY) //DvId.dat
			{
				if ((position % 4) == 0) outputData[i] = ((inputData[i]) ^ ((key & 0xFF000000) >> 24));
				if ((position % 4) == 1) outputData[i] = ((inputData[i]) ^ ((key & 0x00FF0000) >> 16));
				if ((position % 4) == 2) outputData[i] = ((inputData[i]) ^ ((key & 0x0000FF00) >> 8));
				if ((position % 4) == 3) outputData[i] = ((inputData[i]) ^ (key & 0x000000FF));
			}
			position++;
		}
		fwrite(outputData, sizeof(std::uint8_t), nbRead, fout);
		blockNumber++;
	}

	free(inputData);
	free(outputData);

	fclose(fout);
	fclose(fin);
	if (rename(partialFilename.c_str(), filename.c_str()) != 0)
	{
		remove(partialFilename.c_str());
		return false;
	}
	//sprintf(tmp, "del %s\n", filename);
	//system(tmp);
	return (true);
}

//delete OMA file from device
void SonyDb::deleteOMA(char *filename)
{
#ifdef _WIN32
	DeleteFile(filename);
#else
	unlink(filename);
#endif
}


void SonyDb::createDir(int value)
{
	char *dirName = (char*)calloc(sizeof(char), 256);

	sprintf(dirName, "%s/OMGAUDIO/10F%02x", getDriveLetter(), value >> 8);
#ifdef _WIN32
	CreateDirectory(dirName, NULL);
#else
#ifdef __MINGW32__
	mkdir(dirName);
#else
	mkdir(dirName, 0755);
#endif
#endif
	free(dirName);
}

bool SonyDb::writeTracks()
{
	//already copying
	if (this->copying)
		return false;
	// Generation 3 Network Walkman models reject clear MP3 payloads with MG
	// ERROR. Refuse the operation before deleting or rewriting anything.
	if (nbTrackToAdd > 0 && deviceKeyRequired && codeType == ENCODING_USE_NONE)
	{
		fprintf(fp, "error device-specific DvID key is required for MP3 transfer\n");
		fflush(fp);
		return false;
	}

	//nothing to do
	//if ((nbTrackToAdd <= 0) && (nbTrackToDel <= 0) && (getNbPlaylist() <= 0))

	this->copying = true;

	//concider all files are copied for size problems
	usedSpaceDisk = usedSpaceDisk + addTrackTotalByte - delTrackTotalByte;
	totalByteLeftToWrite = addTrackTotalByte;
	addTrackTotalByte = 0;
	delTrackTotalByte = 0;
	nbTrackToAdd = 0;
	nbTrackToDel = 0;


	vector<Song *> songlist;
	vector<Song *> addList;
	vector<Song *>::iterator iteAddList;
	bool res = false;
	bool copyFailed = false;

	//create list of add & del
	for (vector<Song>::iterator i = songs.begin(); i != songs.end(); i++)
	{
		if ((*i).statusOfSong == ADD_TO_DEVICE)
			addList.push_back(&(*i));
		//create directory if necessary
	}

	iteAddList = addList.begin();
	int nbElementsLeftToAdd = addList.size();

	this->copyIndex = 0;
	//create ordered list
	int test = songs.size();
	for (vector<Song>::iterator j = songs.begin(); j != songs.end(); j++)
	{ 
		//skip
		if ((*j).statusOfSong == ADD_TO_DEVICE)
			continue;

		//keep
		if ((*j).statusOfSong == ON_DEVICE)
		{
			this->copyIndex++; //progress bar index
			songlist.push_back(&(*j));
			continue;
		}

		//replace
		if (((*j).statusOfSong == REMOVE_FROM_DEVICE) || ((*j).statusOfSong == EMPTYTRACK))
		{
			if ((*j).statusOfSong == REMOVE_FROM_DEVICE)
			{
				this->copyIndex++; //progress bar index
				deleteOMA((*j).filename);
			}

			//add front of addList change filenumber and filename
			if (nbElementsLeftToAdd > 0)
			{
				this->copyIndex++; //progress bar index
				if (addOMA((*iteAddList), (*j).sonyDbOrder))//replace oma file by new mp3
				{ 
					if ((*iteAddList)->filename) free((*iteAddList)->filename);
					(*iteAddList)->filename = strdup((*j).filename);
					(*iteAddList)->sonyDbOrder = (*j).sonyDbOrder;
					(*j).statusOfSong = -1;
					//(*iteAddList)->statusOfSong = ON_DEVICE; //removed otherwise get added twice
					songlist.push_back((*iteAddList));
					iteAddList++;
					nbElementsLeftToAdd--;
				}
				else
				{
					fprintf(fp,"Could not write file : %s\n", (*iteAddList)->filename);
					fflush(fp);
					copyFailed = true;
					iteAddList++;
					nbElementsLeftToAdd--;
				}
			}
			else
			{
				if ((*j).statusOfSong == EMPTYTRACK)
				{
					//keep the empty track for now
					songlist.push_back(&(*j));
				}
				else
				{
					//nothing else to add, so add a blank element instead of deletion
					//ex : tracks 1 2 3 4 5 -> deletion of 2 and 5 becomes -> 1 'blank' 3 4
					//otherwise we would have to reencode 3 and 4 with the correct bit mask... 
					free((*j).album);
					free((*j).artist);
					free((*j).genre);
					free((*j).title);
					free((*j).filename);

					(*j).album = strdup("");
					(*j).artist = strdup("");
					(*j).genre = strdup("");
					(*j).title = strdup("");
					(*j).filename = strdup("");
					(*j).statusOfSong = EMPTYTRACK;
					(*j).songlen = 0;
					(*j).track_nr = 0;
					(*j).year = 0;
					(*j).encoding = 0;
					(*j).protection = SONY_PROTECTION_NONE;
					memset((*j).cidRecord, 0, sizeof((*j).cidRecord));
					(*j).hasCidRecord = true;
					songlist.push_back(&(*j));
				}
			}
		}
	}

	//add the rest of the add list at the end incrementing filenumber and filename
	while (nbElementsLeftToAdd > 0)
	{
		this->copyIndex++; //progress bar index
		if (addOMA((*iteAddList), lastTrackIndex)) //send the mp3
		{
			(*iteAddList)->filename = GetOMAFilename(lastTrackIndex);
			(*iteAddList)->sonyDbOrder = lastTrackIndex;
			(*iteAddList)->statusOfSong = ON_DEVICE;
			songlist.push_back((*iteAddList));
			lastTrackIndex++;
			nbElementsLeftToAdd--;
			iteAddList++;
		}
		else
		{
			fprintf(fp,"Could not write file : %s\n", (*iteAddList)->filename);
			fflush(fp);
			copyFailed = true;
			nbElementsLeftToAdd--;
			iteAddList++;
		}
	}
	int test2 = songlist.size();

	//remove empty tracks at the end of the list
	while ((songlist.size() > 0) && (songlist.back()->statusOfSong == EMPTYTRACK))
	{
		songlist.pop_back();
		if (this->lastTrackIndex -1 > 0)
			this->lastTrackIndex--;
	}

	//write the new database to the device
	const bool databaseWritten = writeDatabase(songlist);
	res = databaseWritten && !copyFailed;
#if defined(__linux__)
	// Do not report completion while the kernel still has pending writes for a
	// removable Walkman.  This avoids a successful-looking transfer followed by
	// FAT corruption when the cable is disconnected immediately afterwards.
	if (res)
	{
		const int deviceFd = open(getDriveLetter(), O_RDONLY | O_DIRECTORY);
		if (deviceFd < 0 || syncfs(deviceFd) != 0)
			res = false;
		if (deviceFd >= 0)
			close(deviceFd);
	}
#endif
	if (databaseWritten)
	{
		fprintf(fp,"Writing database files: OK\n");
		if (!copyFailed)
			databaseDirty = false;
	}
	else
		fprintf(fp,"Writing database files: FAILED\n");
	fflush(fp);

	songlist.clear();
	addList.clear();

	this->copying = false;
	return (res);
}


//write the database to the device (overwrite existing db)
bool SonyDb::writeDatabase(vector<Song *> songsToSend)
{
	vector<Song *>     listArtist;
	vector<Song *>     listAlbum;
	vector<Song *>     listGenre;

	//create the lists
	bool alreadyAddedArtist = false;
	bool alreadyAddedAlbum = false;
	bool alreadyAddedGenre = false;

	//for each song
	int index = 1;
	for (vector<Song*>::iterator song = songsToSend.begin(); song != songsToSend.end(); song++)
	{
		alreadyAddedArtist = false;
		alreadyAddedAlbum = false;
		alreadyAddedGenre = false;

		//search in artist list
		for (vector<Song*>::iterator artist = listArtist.begin(); artist != listArtist.end(); artist++)
		{
			if (STRCMP2_NULLOK((*song)->artist, (*artist)->artist) == 0)
			{
				alreadyAddedArtist = true;
				break;
			}
		}

		//search in album list
		for (vector<Song*>::iterator album = listAlbum.begin(); album != listAlbum.end(); album++)
		{
			if (STRCMP2_NULLOK((*song)->album, (*album)->album) == 0)
			{
				alreadyAddedAlbum = true;
				break;
			}
		}	

		//search in the genre list
		for (vector<Song*>::iterator genre = listGenre.begin(); genre != listGenre.end(); genre++)
		{
			if (STRCMP2_NULLOK((*song)->genre, (*genre)->genre) == 0)
			{
				alreadyAddedGenre = true;
				break;
			}
		}

		//add it if not already in the list 
		if (!alreadyAddedArtist) listArtist.push_back((*song));
		if (!alreadyAddedGenre) listGenre.push_back((*song));
		if (!alreadyAddedAlbum) listAlbum.push_back((*song));
	}

	//resort the lists alphabetically
	sort(listArtist.begin(), listArtist.end(), sortByArtistName);
	sort(listGenre.begin(), listGenre.end(), sortByGenreName);
	sort(listAlbum.begin(), listAlbum.end(), sortByAlbumName);

	//to sort our songs by track number (if same album)
	bool first;
	int firstIndex = -1;
	int endIndex = -1;

	//now that we have listArtist listAlbum and listGenre
	//resort our songs
	vector<Song *> songsSortedByAlbum;
	for (vector<Song *>::iterator album = listAlbum.begin(); album != listAlbum.end(); album++)
	{
		first = true;
		for (vector<Song *>::iterator song = songsToSend.begin(); song != songsToSend.end(); song++)
		{
			if (STRCMP2_NULLOK((*song)->album, (*album)->album) == 0)
			{
				songsSortedByAlbum.push_back((*song));

				//for sorting by track number
				if (first)
				{
					first = false;
					firstIndex = endIndex + 1;
					endIndex = firstIndex;
				}
				else
				{
					endIndex++;
				}
			}
		}

		//sort by track number
		if (endIndex > firstIndex) 
			sort(songsSortedByAlbum.begin() + firstIndex, songsSortedByAlbum.begin() + endIndex + 1, sortByTrackNumber);
	}

	firstIndex = -1;
	endIndex = -1;
	vector<Song *> songsSortedByArtist;
	for (vector<Song *>::iterator artist = listArtist.begin(); artist != listArtist.end(); artist++)
	{
		first = true;
		for (vector<Song *>::iterator song = songsToSend.begin(); song != songsToSend.end(); song++)
		{

			if (STRCMP2_NULLOK((*song)->artist, (*artist)->artist) == 0)
			{
				songsSortedByArtist.push_back((*song));

				//for sorting by track number
				if (first)
				{
					first = false;
					firstIndex = endIndex + 1;
					endIndex = firstIndex;
				}
				else
				{
					endIndex++;
				}
			}
		}

		//sort by track number
		if (endIndex > firstIndex)
			sort(songsSortedByArtist.begin() + firstIndex, songsSortedByArtist.begin() + endIndex + 1, sortByTrackNumber);
	}

	firstIndex = -1;
	endIndex = -1;
	vector<Song *> songsSortedByGenre;
	for (vector<Song *>::iterator genre = listGenre.begin(); genre != listGenre.end(); genre++)
	{
		first = true;
		for (vector<Song *>::iterator song = songsToSend.begin(); song != songsToSend.end(); song++)
		{
			if (STRCMP2_NULLOK((*song)->genre, (*genre)->genre) == 0)
			{
				songsSortedByGenre.push_back((*song));

				//for sorting by track number
				if (first)
				{
					first = false;
					firstIndex = endIndex + 1;
					endIndex = firstIndex;
				}
				else
				{
					endIndex++;
				}
			}
		}

		//sort by track number
		if (endIndex > firstIndex)
			sort(songsSortedByGenre.begin() + firstIndex, songsSortedByGenre.begin() + endIndex + 1, sortByTrackNumber);
	}

	//write the files
	if (!(write_00GTRLST())) return false;

	if (!(write_01TREEXX(songsSortedByAlbum, listAlbum, 1))) return false;
	if (!(write_01TREEXX(songsSortedByArtist, listArtist, 2))) return false;
	if (!(write_01TREEXX(songsSortedByAlbum, listAlbum, 3))) return false;
	if (!(write_01TREEXX(songsSortedByGenre, listGenre, 4))) return false;

	// danger
	if (!(write_02TREINF(songsToSend))) return false;

	if (!(write_03GINFXX(listAlbum, 1))) return false;
	if (!(write_03GINFXX(listArtist, 2))) return false;
	if (!(write_03GINFXX(listAlbum, 3))) return false;
	if (!(write_03GINFXX(listGenre, 4))) return false;

	if (!(write_04CNTINF(songsToSend))) return false;

	if (!(write_05CIDLST(songsToSend))) return false;

	//write_TrackNumber(songsToSend);

	//sort & create the playlists
	vector<Song *>     listPlaylist;
	vector<Song *>	   songsInPlaylist;

	int nbSongs;

	//sort the playlist by alphabetical order
	sort(playlist.begin(), playlist.end(), sortPlaylist);

	//scan all playlist
	for (vector<Playlist>::iterator pl = playlist.begin(); pl != playlist.end(); pl++)
	{
		//all songs in playlist
		nbSongs = 0;
		for (vector<Song *>::iterator songInPl = (*pl).songs.begin(); songInPl != (*pl).songs.end(); songInPl++)
		{
			//all songs on device (check if present and get the file number)
			for (vector<Song *>::iterator tmpSong = songsToSend.begin(); tmpSong != songsToSend.end(); tmpSong++)
			{
				if ((*tmpSong)->statusOfSong != EMPTYTRACK)
				{
					if ((STRCMP2_NULLOK((*tmpSong)->album, (*songInPl)->album) == 0) &&
							(STRCMP2_NULLOK((*tmpSong)->artist, (*songInPl)->artist) == 0) &&
							(STRCMP2_NULLOK((*tmpSong)->title, (*songInPl)->title) == 0))
					{
						(*songInPl)->sonyDbOrder = (*tmpSong)->sonyDbOrder;
						(*songInPl)->statusOfSong = (*tmpSong)->statusOfSong;
						songsInPlaylist.push_back((*songInPl));
						nbSongs++;
						break;
					}
				}
			}
		}

		//playlist is not empty
		if (nbSongs > 0)
		{
			//add the playlist
			Song *s = new Song();
			s->album = (*pl).name;
			s->artist = strdup(" ");
			s->genre = strdup(" ");
			s->title = strdup(" ");
			s->filename = strdup(" ");
			s->track_nr = nbSongs; //for writing the file later
			listPlaylist.push_back(s);
		}
		else
		{
			fprintf(fp, "Ignoring Playlist %s because all tracks are missing\n", (*pl).name);
			fflush(fp);
		}
	}

	if (listPlaylist.size() > 0)
	{
		//write the playlists 
		write_01TREEXX(songsInPlaylist , listPlaylist, 22); //write_01TREE22
		write_03GINFXX(listPlaylist ,22); //write_03GINF22
	}
	else
	{
		write_01TREE22();
		write_03GINF22();
	}
	while (listPlaylist.size() > 0)
	{
		Song *s = listPlaylist.back();
		listPlaylist.pop_back();
		free(s->album);
		free(s->artist);
		free(s->genre);
		free(s->title);
		free(s->filename);
		free(s);
	}

	songsInPlaylist.clear();

	listArtist.clear();
	listAlbum.clear();
	listGenre.clear();

	return (true);
}

//read the track number directly from the omg header
int SonyDb::getTrackNumber(char *filename)
{
	FILE *fin = fopen(filename, "rb");
	if (fin == NULL)
	{
		fprintf(fp, "error can't open file %s\n", filename);
		fflush(fp);
		return (-1);
	}
	//fprintf(fp, "1 Opening file %s ok\n", filename);fflush(fp);
	std::uint8_t     tmpTag[512];
	std::uint32_t    tagLength = 0;

	//read the id3 header
	if (fread(&tmpTag, sizeof(std::uint8_t), 10, fin) != 10)
	{
		fprintf(fp, "error can't read file header : %s\n", filename);
		fflush(fp);
		fclose(fin);
		return (-1);
	}
	//fprintf(fp, "2 Reading of the id3 header ok\n");fflush(fp);
	memset(tmpTag, 0, 512);
	for (int i = 0; i < 5; i++)
	{
		//fprintf(fp, "Reading tag %i: ", i);fflush(fp);
		if (fread(&tmpTag, sizeof(std::uint8_t), 11, fin) != 11)
		{
			fprintf(fp, "error can't read file tag %i : %s\n", i, filename);
			fflush(fp);
			fclose(fin);
			return (-1);
		}

		if ((tmpTag[4] == 0) && (tmpTag[5] == 0) && (tmpTag[6] == 0) && (tmpTag[7] == 0))
		{
			//tag not found
			fprintf(fp, "Tag not present: %s\n", filename);
			fflush(fp);
			fclose(fin);
			return (-1);
		}
		else
		{
			//not synch safe!?
			//tagLength = ((tmpTag[4] << 21) + (tmpTag[5] << 14) + (tmpTag[6] << 7) + tmpTag[7]) - 1;
			tagLength = ((tmpTag[4] << 24) + (tmpTag[5] << 16) + (tmpTag[6] << 8) + tmpTag[7]) - 1;

			if ((STRNCMP_NULLOK((char*)tmpTag, "TXXX", 4) == 0))
			{
				memset(tmpTag, 0, 512);
				if (fread(&tmpTag, sizeof(std::uint8_t), tagLength, fin) != tagLength)
				{
					fprintf(fp, "error can't read file 4 : %s\n", filename);
					fflush(fp);
					fclose(fin);
					return (-1);
				}
				tmpTag[19] = 20;//quick fix : replace '*' by a space in "OMG_TRACK*XXXX"
				char *res1 = utf16_to_ansi((std::uint16_t*)tmpTag, tagLength, true);
				//fprintf(fp, "TRACK NUMBER : >%s<\n", res1);fflush(fp);
				char *res2 = res1 + 10; //remove OMG_TRACK 
				int res3 = atoi(res2);
				free(res1);
				fclose(fin);
				return (res3);
			}
			else
			{
				//fprintf(fp, "Skipping Tag \n");fflush(fp);
				if (fseek(fin, tagLength, SEEK_CUR) != 0)
				{
					fprintf(fp, "error can't seek in file : %s\n", filename);
					fflush(fp);
					fclose(fin);
					return (-1);
				}
			}	  
		}
	}
	fclose(fin);
	return (-1);
}

//read all playlist on the device
int  SonyDb::readAllPlaylist()
{
	freeAllPlaylist();

	FILE *f;
	Song *s;

	char fileName[512];    
	//open the file
	sprintf(fileName, "%s/OMGAUDIO/03GINF22.DAT", getDriveLetter());
	f = fopen(fileName, "rb");
	if (f == NULL)
	{
		fprintf(fp, "error can't open file 03GINF22.DAT\n");fflush(fp);
		return 0;
	}

	//read the headers
	sonyFileHeader header;
	sonyObjectPointer Opointer;
	sonyObject obj;

	if (!getHeader(&header, f))
	{
		fclose(f);
		return false;
	}
	if (!getObjectPointer(&Opointer, f))
	{
		fclose(f);
		return false;
	}
	if (!getObject(&obj, f))
	{
		fclose(f);
		return false;
	}

	vector<Song*> listOfPlaylist;

	for (int index = 1; index <= obj.count; index++)
	{
		s = new Song();

		if (getTrack(f, s) )
			listOfPlaylist.push_back(s);
	}
	fclose(f);

	int plIndex = 1;
	for (vector<Song *>::iterator song = listOfPlaylist.begin(); song != listOfPlaylist.end(); song++)
	{
		Playlist *p = new Playlist();
		p->name = strdup((*song)->title);
		p->index = plIndex++;
		addPlaylist(p);
	}

	//sort the playlist by index
	sort(playlist.begin(), playlist.end(), sortByPlaylistIndex);

	while (listOfPlaylist.size() > 0)
	{
		s = listOfPlaylist.back();
		if (s->title) free(s->title);
		free(s);
		listOfPlaylist.pop_back();
	}

	//open the file
	sprintf(fileName, "%s/OMGAUDIO/01TREE22.DAT", getDriveLetter());
	f = fopen(fileName, "rb");
	if (f == NULL)
	{
		fprintf(fp, "error can't open file 01TREE22.DAT\n");fflush(fp);
		return 0;
	}

	//read the headers
	sonyFileHeader header2;
	sonyObjectPointer Opointer2;
	sonyObjectPointer Opointer3;
	sonyObject obj2;
	sonyObject obj3;

	if (!getHeader(&header2, f))
	{
		fclose(f);
		return false;
	}
	if (!getObjectPointer(&Opointer2, f))
	{
		fclose(f);
		return false;
	}
	if (!getObjectPointer(&Opointer3, f))
	{
		fclose(f);
		return false;
	}
	if (!getObject(&obj2, f))
	{
		fclose(f);
		return false;
	}

	std::uint16_t index1;
	std::uint16_t index2;
	std::uint16_t cte1;
	std::uint16_t cte2;

	vector<std::uint16_t> p1;
	vector<std::uint16_t> p2;

	for (int i = 0; i < obj2.count; i++)
	{
		fread(&index1, sizeof(std::uint16_t), 1, f);
		fread(&cte1, sizeof(std::uint16_t), 1, f);
		fread(&index2, sizeof(std::uint16_t), 1, f);
		fread(&cte2, sizeof(std::uint16_t), 1, f);
		index1 = UINT16_SWAP_BE_LE(index1);
		index2 = UINT16_SWAP_BE_LE(index2);
		p1.push_back(index1);
		p2.push_back(index2);
	}

	fseek(f, Opointer3.offset, SEEK_SET);
	if (!getObject(&obj3, f))
	{
		fclose(f);
		return false;
	}

	vector<Playlist>::iterator pla = playlist.begin();

	//for each track
	int nbTrack = 0;
	int catIndex = 0;
	for (int j = 0; j < obj3.count; j++)
	{
		fread(&cte1, sizeof(std::uint16_t), 1, f);
		cte1 = UINT16_SWAP_BE_LE(cte1);

		if (nbTrack == 0)
		{
			//find the next playlist
			for (vector<Playlist>::iterator tmp = playlist.begin(); tmp != playlist.end(); tmp++)	
			{
				int index = (int)(p1.front()+catIndex);
				if ((*tmp).index == index)
				{
					pla = tmp; //found the playlist
					if ((p2.begin()+(catIndex + 1) >= p2.end()))
						nbTrack = -1;
					else
						nbTrack = p2.at(catIndex + 1) - p2.at(catIndex); //number of track to read for this playlist
					//if nbTrack is negative it will take all the rest of the tracks for this playlist
					catIndex++;
					break;
				}
			}
		}	
		nbTrack--;
		for (vector<Song>::iterator so = songs.begin(); so != songs.end(); so++)
		{
			if ((*so).sonyDbOrder == cte1)
				(*pla).songs.push_back(&(*so));
		}
	}  
	fclose(f);

	while (playlist_temporary.size() > 0)
	{
		Playlist p = playlist_temporary.back();
		addPlaylist(&p);
		playlist_temporary.pop_back();
	}

	return (playlist.size());
}

//read all tracks from the device to a song vector
int SonyDb::readAllTracks()
{
	updateDiskSpaceInfo();

	if (this->copying)
	{
		return (songs_temporary.size());
	}
	else
	{
		freeAllTracks();
		nbTrackToAdd = 0;
		nbTrackToDel = 0;
		addTrackTotalByte = 0;
		delTrackTotalByte = 0;
		databaseDirty = false;
		deviceKeyRequired = false;

		FILE *f, *f2;
		Song *s;
		bool trackNumberAvailable = false;

		char tmp[512];
		//open the file
		if (!getDriveLetter())
		{
			fprintf(fp, "error can't open file 04CNTINF.DAT\n");fflush(fp);
			return 0;
		}
		// OpenMG generation 3 databases carry this DRM control file even when the
		// library is empty or a previous application damaged per-track flags.
		sprintf(tmp, "%s/OMGAUDIO/00010021.DAT", getDriveLetter());
		struct stat drmControlFile;
		if (stat(tmp, &drmControlFile) == 0 && S_ISREG(drmControlFile.st_mode))
			deviceKeyRequired = true;
		sprintf(tmp, "%s/OMGAUDIO/04CNTINF.DAT", getDriveLetter());
		f = fopen(tmp, "rb");
		if (f == NULL)
		{
			fprintf(fp, "error can't open file 04CNTINF.DAT\n");fflush(fp);
			return 0;
		}

		//try to open TRACKS.DAT
		sprintf(tmp, "%s/OMGAUDIO/TRACKS.DAT", getDriveLetter());
		f2 = fopen(tmp, "rb");
		if (f2 == NULL)
		{
			trackNumberAvailable = false;
		}
		else
		{
			trackNumberAvailable = true;
		}


		//read the headers
		sonyFileHeader header;
		sonyObjectPointer Opointer;
		sonyObject obj;

		lastTrackIndex = 1;

		if (!getHeader(&header, f))
		{
			this->trackListLoaded = true;
			fclose(f);
			return false;
		}
		if (!getObjectPointer(&Opointer, f))
		{
			this->trackListLoaded = true;
			fclose(f);
			return false;
		}
		if (!getObject(&obj, f))
		{
			this->trackListLoaded = true;
			fclose(f);
			return false;
		}

		char *oldAlbum = NULL;
		int  tracknum = 1;

		for (int index = 1; index <= obj.count; index++)
		{
			s = (Song *)calloc(1, sizeof(Song));

			if (getTrack(f, s))
			{
				//dummy track number in case we can't read it from the tags
				if (STRCMP2_NULLOK(s->album, oldAlbum) == 0)
				{
					tracknum++;
				}
				else
				{
					if (STRCMP2_NULLOK(s->album, "") != 0)
					{
						oldAlbum = s->album;
						tracknum = 1;
					}
				}

				if (!(s->album)) s->album = strdup("");
				if (!(s->artist)) s->artist = strdup("");
				if (!(s->genre)) s->genre = strdup("");
				if (!(s->title)) s->title = strdup("");

				if ((STRCMP2_NULLOK(s->album, "") == 0) && 
						(STRCMP2_NULLOK(s->artist, "") == 0) &&
						(STRCMP2_NULLOK(s->genre, "") == 0) &&
						(STRCMP2_NULLOK(s->title, "") == 0))
				{
					//Empty slot
					s->filename = strdup("");
					s->statusOfSong = EMPTYTRACK;
				}
				else
				{    
					//adding additionnal info
					s->filename = GetOMAFilename(index);
					const std::uint16_t fileProtection = oma_protection(s->filename);
					struct stat omaFile;
					if (stat(s->filename, &omaFile) == 0 && S_ISREG(omaFile.st_mode))
						s->protection = fileProtection;
					if (s->protection == SONY_PROTECTION_LSI_DRM ||
						s->protection == SONY_PROTECTION_ENCRYPTED_MP3)
						deviceKeyRequired = true;
					//fprintf(fp, "file number %s\n", s->filename);fflush(fp);
					if (trackNumberAvailable)
					{
						fread(&tmp, 1, 9, f2);
						s->track_nr = atoi(tmp);
					}
					else
					{
						int tmp = getTrackNumber(s->filename);
						if (tmp != -1)
							s->track_nr = tmp;
						else
							s->track_nr = tracknum;
					}
					s->year = 0;
					s->statusOfSong = ON_DEVICE;
				}
				s->sonyDbOrder = index;
				lastTrackIndex++;
				songs.push_back(*s);
				free(s);
			}
			else
				free(s);
		}
		fclose(f);
		if (trackNumberAvailable)
			fclose(f2);

		// Keep the complete per-title CIDL record. Existing LSI DRM tracks need
		// their original bytes; non-LSI tracks and empty slots normally contain zeros.
		sprintf(tmp, "%s/OMGAUDIO/05CIDLST.DAT", getDriveLetter());
		FILE *cid = fopen(tmp, "rb");
		if (cid)
		{
			sonyFileHeader cidHeader;
			sonyObjectPointer cidPointer;
			sonyObject cidObject;
			if (getHeader(&cidHeader, cid) && getObjectPointer(&cidPointer, cid) &&
				getObject(&cidObject, cid) && cidObject.size >= 48)
			{
				vector<std::uint8_t> record(cidObject.size);
				for (size_t index = 0; index < songs.size() && index < cidObject.count; ++index)
				{
					if (fread(record.data(), 1, record.size(), cid) != record.size()) break;
					memcpy(songs[index].cidRecord, record.data(), sizeof(songs[index].cidRecord));
					songs[index].hasCidRecord = true;
				}
			}
			fclose(cid);
		}

		while (songs_temporary.size() > 0)
		{
			Song s2 = songs_temporary.back();
			addSong(&s2);
			songs_temporary.pop_back();
		}

		this->trackListLoaded = true;
		//fprintf(fp, "All Done\n");fflush(fp);
		return (songs.size());
	}
}


bool SonyDb::getTrack(FILE *f, Song *output)
{
	sonyTrack t;

	//get the track info
	if (fread(&t, sizeof(sonyTrack), 1, f) != 1)
	{
		fprintf(fp, "error could not read file header\n");fflush(fp);
		return false;
	}
	else
	{
		if (t.nbTagRecords == 0) //last track reached
			return false;

		//get values from big endian
		t.trackEncoding = UINT32_SWAP_BE_LE(t.trackEncoding);
		t.trackLength = UINT32_SWAP_BE_LE(t.trackLength);
		t.nbTagRecords = UINT16_SWAP_BE_LE(t.nbTagRecords);
		t.sizeTagRecords = UINT16_SWAP_BE_LE(t.sizeTagRecords);
		//fprintf(fp, "\nTrack :\n, length : %i, nbFrames : %i , nbTags : %i, tagSize : %i\n", t.trackLength, t.trackEncoding, t.nbTagRecords, t.sizeTagRecords);
		//fflush(fp);
	}   

	output->encoding = t.trackEncoding;
	output->protection = static_cast<std::uint16_t>((t.fileType[2] << 8) | t.fileType[3]);


	//length is in ms
	output->songlen = t.trackLength / 1000;

	//read the tags from the 04CNTINF.DAT file
	char tagType[4];
	char encoding[2];
	utf16char *tagRecord = (utf16char*) malloc(sizeof(utf16char) * t.sizeTagRecords);

	for (int i = 1; i <= t.nbTagRecords; i++)
	{  
		//read type
		if (fread(tagType, 4, 1, f) != 1)
			return false;

		//read encoding
		if (fread(encoding, 2, 1, f)!= 1)
			return false;

		//read the tag
		if (fread(tagRecord, t.sizeTagRecords - 6, 1, f) != 1)
			return (false);

		if (STRNCMP_NULLOK(tagType, "TIT2", 4) == 0)
		{
			//utf16 version
			output->wTitle = tagRecord;

			//ansi version
			output->title = utf16_to_ansi((utf16char*)tagRecord, t.sizeTagRecords, true);
			//fprintf(fp, "Read title : %s\n", output->title);fflush(fp);
			continue;
		}

		if (STRNCMP_NULLOK(tagType, "TPE1", 4) == 0)
		{
			//utf16 version
			output->wArtist = tagRecord;

			//ansi version
			output->artist = utf16_to_ansi((utf16char*)tagRecord, t.sizeTagRecords, true);
			continue;
		}

		if (STRNCMP_NULLOK(tagType, "TALB", 4) == 0)
		{
			//utf16 version
			output->wAlbum = tagRecord;

			//ansi version
			output->album = utf16_to_ansi((utf16char*)tagRecord, t.sizeTagRecords, true);
			continue;
		}

		if (STRNCMP_NULLOK(tagType, "TCON", 4) == 0)
		{
			//utf16 version
			output->wGenre = tagRecord;

			//ansi version
			output->genre = utf16_to_ansi((utf16char*)tagRecord, t.sizeTagRecords, true);
			continue;
		}
	}
	return (true);
}


/*****************************************************************************************/
/** misc functions : detect devices etc...*/

//free disk space info
static void commaValue(__int64 val0, char *dest)
{
	int val = (int)val0;
	if ((val>=1024) || (val0 <= -1024))
	{
		sprintf(dest,"%d.%02d GB",val/1024,(val%1024)/10);
	} else
		sprintf(dest,"%d MB",val);
}

//free disk space info
void SonyDb::updateDiskSpaceInfo()
{
#ifdef _WIN32
	char drive[2];

	strcpy(drive, this->getDriveLetter());

	ULARGE_INTEGER free={0,};
	ULARGE_INTEGER total={0,};
	ULARGE_INTEGER freeb={0,};
	GetDiskFreeSpaceEx(drive, &free, &total, &freeb);
	usedSpaceDisk = total.QuadPart - freeb.QuadPart;
	freeSpaceDisk = freeb.QuadPart;
	totalDiskSpaceValue = total.QuadPart;

	unsigned int totalmb = (unsigned int)((total.QuadPart)/(1024*1024));
	commaValue(totalmb, totalDiskSpace);
#else
	struct statvfs diskInfo;
	if (this->getDriveLetter() && statvfs(this->getDriveLetter(), &diskInfo) == 0)
	{
		totalDiskSpaceValue = static_cast<__int64>(diskInfo.f_blocks) * diskInfo.f_frsize;
		freeSpaceDisk = static_cast<__int64>(diskInfo.f_bavail) * diskInfo.f_frsize;
		usedSpaceDisk = totalDiskSpaceValue -
			(static_cast<__int64>(diskInfo.f_bfree) * diskInfo.f_frsize);
		commaValue(totalDiskSpaceValue / (1024 * 1024), totalDiskSpace);
	}
	else
	{
		totalDiskSpaceValue = 0;
		freeSpaceDisk = 0;
		usedSpaceDisk = 0;
		strcpy(totalDiskSpace, "Unknown");
	}
#endif
}


char *SonyDb::getFreeDiskSpaceAfterApply()
{
	__int64 tmp = freeSpaceDisk - addTrackTotalByte + delTrackTotalByte;
	commaValue((int)(tmp/(1024*1024)), freeDiskSpaceAfterApply);
	return (this->freeDiskSpaceAfterApply);
}


char *SonyDb::getTotalDiskSpace()
{
	return (this->totalDiskSpace);
}


char *SonyDb::getTotalUsedSpaceAfterApply()
{
	__int64 tmp = usedSpaceDisk + addTrackTotalByte - delTrackTotalByte;
	commaValue((int)(tmp/(1024*1024)), totalUsedSpaceAfterApply);
	return (this->totalUsedSpaceAfterApply);
}

char *SonyDb::getSizeTrackToAdd()
{
	commaValue((int)(addTrackTotalByte/(1024*1024)), addTrackTotalByteString);
	return (addTrackTotalByteString);
}

char *SonyDb::getSizeTrackToDel()
{
	commaValue((int)(delTrackTotalByte/(1024*1024)), delTrackTotalByteString);
	return (delTrackTotalByteString);
}

//return the disk space missing to apply the modifications
char *SonyDb::getNeededSpace()
{
	updateDiskSpaceInfo();
	int res;
	__int64 totalSpaceUsed;

	totalSpaceUsed = usedSpaceDisk + addTrackTotalByte + DATABASE_HEADER_SIZE - delTrackTotalByte;
	res = (int)((totalSpaceUsed - totalDiskSpaceValue) / (1024 * 1024));
	commaValue(res, neededSpace);
	return (neededSpace);
}

//return a positive value if there is not enough space
int  SonyDb::getNeededSpaceValue()
{
	updateDiskSpaceInfo();
	int res;
	__int64 totalSpaceUsed;

	totalSpaceUsed = usedSpaceDisk + addTrackTotalByte + DATABASE_HEADER_SIZE - delTrackTotalByte;
	res = (int)((totalSpaceUsed - totalDiskSpaceValue) / (1024 * 1024));

	return (res);
}


//return the devices letter
char* SonyDb::getDriveLetter()
{
	return (this->driveLetter);
}

//return the device name
char *SonyDb::getDeviceName()
{
	return (this->deviceName);
}

//is the device currently copying or downloading a file?
bool SonyDb::isCopying()
{
	return (this->copying);
}

//check if the device is still present
bool SonyDb::isPresent()
{
	if (this->driveLetter != 0)
		return (detectPlayer(this->driveLetter));
	else
		return false;
}

//find the first device available from A to Z
bool SonyDb::detectPlayer()
{    
#ifdef _WIN32
	ULONG uDriveMask = _getdrives(); //get all valid drives
	char letter[3];
	string path = "/OMGAUDIO/04CNTINF.DAT";
	FILE *stream;
	strcpy(letter, "A:");

	if (uDriveMask == 0)
	{
		fprintf(fp, "_getdrives() failed with failure code: %d\n", GetLastError());
		fflush(fp);
	}
	else
	{ 
		//no popup for insert cd in cd drives
		SetErrorMode(SEM_FAILCRITICALERRORS);
		while (uDriveMask)
		{
			//assume A & B are floppy drive (avoid annoying sounds :-) )
			if ((letter[0] != 'A') && (letter[0] != 'B') && (uDriveMask & 1))
			{
				if( (stream  = fopen( (letter + path).c_str(), "r" )) != NULL )
				{
					fclose(stream);
					this->driveLetter = strdup(letter);
					sprintf(deviceName, "%sSony Walkman", getDriveLetter());
					SetErrorMode(0); //restore error mode to normal
					return (true);
				}
			}
			letter[0]++;
			uDriveMask >>= 1;
		}
	}

	SetErrorMode(0); //restore error mode to normal
#else
	// Modern desktop Linux normally mounts removable media below
	// /media/$USER or /run/media/$USER. Keep the historical paths as
	// fallbacks, and inspect one directory level below each mount root.
	namespace fs = std::filesystem;
	std::vector<fs::path> candidates;
	candidates.push_back("/media/usbdisk");
	candidates.push_back("/media/usbdisk1");
	candidates.push_back("/media/WALKMAN");
	const char *user = getenv("USER");
	if (user && *user)
	{
		candidates.push_back(fs::path("/media") / user);
		candidates.push_back(fs::path("/run/media") / user);
	}
	candidates.push_back("/mnt");

	std::vector<fs::path> expanded = candidates;
	for (std::vector<fs::path>::const_iterator root = candidates.begin();
		 root != candidates.end(); ++root)
	{
		std::error_code error;
		if (!fs::is_directory(*root, error))
			continue;
		for (fs::directory_iterator entry(*root, error), end;
			 !error && entry != end; entry.increment(error))
		{
			if (entry->is_directory(error))
				expanded.push_back(entry->path());
		}
	}

	for (std::vector<fs::path>::const_iterator candidate = expanded.begin();
		 candidate != expanded.end(); ++candidate)
	{
		std::string mountPath = candidate->string();
		if (detectPlayer(const_cast<char *>(mountPath.c_str())))
			return (true);
	}
#endif
	return (false);
}

//search for a device starting at the letter specified order is A to Z
bool SonyDb::detectPlayer(char* letter)
{
	FILE *stream;
#ifdef _WIN32
	ULONG uDriveMask = _getdrives(); //get all valid drives
	string path = "/OMGAUDIO/04CNTINF.DAT";
	char drive[3] = {0};
	strncpy(drive, letter, 2);

	if (uDriveMask == 0)
	{
		fprintf(fp, "_getdrives() failed with failure code: %d\n", GetLastError());
		fflush(fp);
	}
	else
	{ 
		//no popup for insert cd in cd drives
		SetErrorMode(SEM_FAILCRITICALERRORS);
		for (int i = 0; i < (drive[0] - 'A'); i++)
		{
			uDriveMask >>= 1;
		}

		while (uDriveMask)
		{
			//assume A & B are floppy drive (avoid annoying sounds :-) )
			if ((drive[0] != 'A') && (drive[0] != 'B') && (uDriveMask & 1))
			{
				if( (stream  = fopen( (drive + path).c_str(), "r" )) != NULL )
				{
					fclose(stream);
					this->driveLetter = strdup(drive);
					sprintf(deviceName, "%s Sony Walkman", getDriveLetter());
					SetErrorMode(0); //restore error mode to normal
					return (true);
				}
			}
			drive[0]++;
			uDriveMask >>= 1;
		}
	}

	SetErrorMode(0); //restore error mode to normal
#else
	// letter can point at driveLetter (via isPresent), so retain a copy before
	// replacing the stored path.
	string detectedDrive = letter;
	string path = detectedDrive;
	path += "/OMGAUDIO/04CNTINF.DAT";
	if((stream = fopen(path.c_str(), "r")) != NULL)
	{
		fclose(stream);
		if (this->driveLetter)
			free(this->driveLetter);
		this->driveLetter = strdup(detectedDrive.c_str());
		snprintf(deviceName, 255, "%s Sony Walkman", getDriveLetter());
		return (true);
	}
#endif
	return (false);
}

bool SonyDb::detectPlayerStorage(char *drive)
{
	if (!drive || !*drive)
		return false;
#ifdef _WIN32
	return detectPlayer(drive);
#else
	std::error_code error;
	if (!std::filesystem::is_directory(drive, error) || access(drive, W_OK) != 0)
		return false;
	if (this->driveLetter)
		free(this->driveLetter);
	this->driveLetter = strdup(drive);
	snprintf(deviceName, 255, "%s Sony Walkman", drive);
	updateDiskSpaceInfo();
	return true;
#endif
}

bool SonyDb::detectPlayerStorage()
{
	if (detectPlayer())
		return true;
#ifdef _WIN32
	return false;
#else
	// Resolve Sony's stable /dev/disk/by-id links and match them against the
	// source device recorded for each currently mounted filesystem.
	std::set<std::string> sonyDevices;
	glob_t matches;
	memset(&matches, 0, sizeof(matches));
	if (glob("/dev/disk/by-id/usb-Sony_*Walkman*", 0, NULL, &matches) == 0)
	{
		for (size_t i = 0; i < matches.gl_pathc; ++i)
		{
			char resolved[PATH_MAX];
			if (realpath(matches.gl_pathv[i], resolved))
				sonyDevices.insert(resolved);
		}
	}
	globfree(&matches);
	if (sonyDevices.empty())
		return false;

	FILE *mounts = setmntent("/proc/self/mounts", "r");
	if (!mounts)
		return false;
	bool found = false;
	struct mntent *entry;
	while (!found && (entry = getmntent(mounts)) != NULL)
	{
		char resolved[PATH_MAX];
		if (realpath(entry->mnt_fsname, resolved) && sonyDevices.count(resolved))
			found = detectPlayerStorage(entry->mnt_dir);
	}
	endmntent(mounts);
	return found;
#endif
}

bool SonyDb::initializePlayer()
{
	if (!getDriveLetter())
		return false;
#ifdef _WIN32
	char directory[512];
	snprintf(directory, sizeof(directory), "%s/OMGAUDIO", getDriveLetter());
	if (!CreateDirectory(directory, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
		return false;
#else
	std::error_code error;
	const std::filesystem::path directory =
		std::filesystem::path(getDriveLetter()) / "OMGAUDIO";
	if (!std::filesystem::create_directory(directory, error) && error)
		return false;
#endif
	databaseDirty = true;
	return writeTracks();
}

//get real file name from id number
char *SonyDb::GetOMAFilename(int id)
{
	char *buffer = (char*)calloc(sizeof(char), 256);
	sprintf(buffer, "%s/OMGAUDIO/10F%02x/1%07x.OMA", getDriveLetter(), id >> 8, id);
	return (buffer);
}




/*  file writers  */
//this one is special...
bool SonyDb::write_00GTRLST()
{
	//write the 00GTRLST.DAT file
	FILE *f;
	char filename[512];

	//open the file
	if (!getDriveLetter())
	{
		fprintf(fp, "error can't open file 04CNTINF.DAT\n");
		fflush(fp);
		return 0;
	}
	sprintf(filename, "%s/OMGAUDIO/00GTRLST.DAT", getDriveLetter());
	f = fopen(filename, "wb");
	if (f == NULL)
	{
		fprintf(fp, "error can't open file %s\n", filename);
		fflush(fp);
		return false;
	}

	//write the headers
	sonyFileHeader header;
	sonyObjectPointer Opointer;
	sonyObject obj;

	//file header
	header.magic[0] = 'G';
	header.magic[1] = 'T';
	header.magic[2] = 'L';
	header.magic[3] = 'T';
	writeHeader(&header, f, 2);

	//object pointer 1
	Opointer.magic[0] = 'S';
	Opointer.magic[1] = 'Y';
	Opointer.magic[2] = 'S';
	Opointer.magic[3] = 'B';
	Opointer.offset = UINT32_SWAP_BE_LE(0X0030);
	Opointer.length = UINT32_SWAP_BE_LE(0x0070);
	writeObjectPointer(&Opointer, f);

	//object pointer 2
	Opointer.magic[0] = 'G';
	Opointer.magic[1] = 'T';
	Opointer.magic[2] = 'L';
	Opointer.magic[3] = 'B';
	Opointer.offset = UINT32_SWAP_BE_LE(0X00A0);
	Opointer.length = UINT32_SWAP_BE_LE(0x0AB0);
	writeObjectPointer(&Opointer, f);

	std::uint8_t t[16];
	for (int i = 0; i < 16; i++)
		t[i] = 0;

	//write the SYSB object header
	obj.magic[0] = 'S';
	obj.magic[1] = 'Y';
	obj.magic[2] = 'S';
	obj.magic[3] = 'B';
	obj.count = UINT16_SWAP_BE_LE(1);
	obj.size = UINT16_SWAP_BE_LE(80);
	obj.padding[0] = 0x00D0;
	obj.padding[1] = 0;
	writeObject(&obj, f);

	fwrite(&t, sizeof(std::uint8_t), 16, f);
	fwrite(&t, sizeof(std::uint8_t), 16, f);
	fwrite(&t, sizeof(std::uint8_t), 16, f);
	fwrite(&t, sizeof(std::uint8_t), 16, f);
	fwrite(&t, sizeof(std::uint8_t), 16, f);
	fwrite(&t, sizeof(std::uint8_t), 16, f);

	//write the GTLB object header
	obj.magic[0] = 'G';
	obj.magic[1] = 'T';
	obj.magic[2] = 'L';
	obj.magic[3] = 'B';
	obj.count = UINT16_SWAP_BE_LE(34);
	obj.size = UINT16_SWAP_BE_LE(80);
	obj.padding[0] = UINT32_SWAP_BE_LE(0x0005);
	obj.padding[1] = 0x0003;
	writeObject(&obj, f);



	//first block
	t[1] = 0x01;
	t[3] = 0x01;
	fwrite(&t, sizeof(std::uint8_t), 16, f);
	t[3] = 0;
	fwrite(&t, sizeof(std::uint8_t), 16, f);
	t[1] = 0;
	fwrite(&t, sizeof(std::uint8_t), 16, f);
	fwrite(&t, sizeof(std::uint8_t), 16, f);
	fwrite(&t, sizeof(std::uint8_t), 16, f);

	//2nd
	t[1] = 0x02;
	t[3] = 0x03;
	fwrite(&t, sizeof(std::uint8_t), 16, f);
	t[1] = 0x01;
	t[3] = 0;
	t[4] = 'T';
	t[5] = 'P';
	t[6] = 'E';
	t[7] = '1';
	fwrite(&t, sizeof(std::uint8_t), 16, f);
	t[1] = 0;
	t[4] = 0;
	t[5] = 0;
	t[6] = 0;
	t[7] = 0;
	fwrite(&t, sizeof(std::uint8_t), 16, f);
	fwrite(&t, sizeof(std::uint8_t), 16, f);
	fwrite(&t, sizeof(std::uint8_t), 16, f);

	//3rd
	t[1] = 0x03;
	t[3] = 0x03;
	fwrite(&t, sizeof(std::uint8_t), 16, f);
	t[1] = 0x01;
	t[3] = 0;
	t[4] = 'T';
	t[5] = 'A';
	t[6] = 'L';
	t[7] = 'B';
	fwrite(&t, sizeof(std::uint8_t), 16, f);
	t[1] = 0;
	t[4] = 0;
	t[5] = 0;
	t[6] = 0;
	t[7] = 0;
	fwrite(&t, sizeof(std::uint8_t), 16, f);
	fwrite(&t, sizeof(std::uint8_t), 16, f);
	fwrite(&t, sizeof(std::uint8_t), 16, f);


	//4th
	t[1] = 0x04;
	t[3] = 0x03;
	fwrite(&t, sizeof(std::uint8_t), 16, f);
	t[1] = 0x01;
	t[3] = 0;
	t[4] = 'T';
	t[5] = 'C';
	t[6] = 'O';
	t[7] = 'N';
	fwrite(&t, sizeof(std::uint8_t), 16, f);
	t[1] = 0;
	t[4] = 0;
	t[5] = 0;
	t[6] = 0;
	t[7] = 0;
	fwrite(&t, sizeof(std::uint8_t), 16, f);
	fwrite(&t, sizeof(std::uint8_t), 16, f);
	fwrite(&t, sizeof(std::uint8_t), 16, f);

	//5th
	t[1] = 0x22;
	t[3] = 0x02;
	fwrite(&t, sizeof(std::uint8_t), 16, f);
	t[1] = 0;
	t[3] = 0;
	fwrite(&t, sizeof(std::uint8_t), 16, f);
	fwrite(&t, sizeof(std::uint8_t), 16, f);
	fwrite(&t, sizeof(std::uint8_t), 16, f);
	fwrite(&t, sizeof(std::uint8_t), 16, f);

	for (int j = 5; j < 34 ; j++)
	{
		t[1] = (std::uint8_t) j;
		fwrite(&t, sizeof(std::uint8_t), 16, f);
		t[1] = 0;
		fwrite(&t, sizeof(std::uint8_t), 16, f);
		fwrite(&t, sizeof(std::uint8_t), 16, f);
		fwrite(&t, sizeof(std::uint8_t), 16, f);
		fwrite(&t, sizeof(std::uint8_t), 16, f);
	}

	fclose(f);
	return (true);
}


bool SonyDb::write_01TREEXX(vector<Song *> songs, vector<Song *> list, int type)
{
	//write the 01TREEXX.DAT file
	FILE *f;
	char filename[512];
	int nbSongs = 0;
	int nbCat = 0;

	//open the file
	sprintf(filename, "%s/OMGAUDIO/01TREE%02i.DAT", getDriveLetter(), type);
	f = fopen(filename, "wb");
	if (f == NULL)
	{
		fprintf(fp, "error can't open file %s\n", filename);
		fflush(fp);
		return false;
	}


	//number of non empty tracks
	for (vector<Song *>::iterator count = songs.begin(); count != songs.end(); count++)
	{
		if ((*count)->statusOfSong != EMPTYTRACK)
			nbSongs++;
	}

	////nbSongs = nbsongs * 2 (because of all songs)
	if (useAllTags)
		nbSongs = nbSongs * 2; //"all x"

	//number of non empty categories
	for (vector<Song *>::iterator count2 = list.begin(); count2 != list.end(); count2++)
	{
		if ((*count2)->statusOfSong != EMPTYTRACK)
			nbCat++;
	}

	//nbcat = nbcat + 1 (for the "all x" categorie)
	if (useAllTags)
		nbCat++; //"all x" categorie

	//write the headers
	sonyFileHeader header;
	sonyObjectPointer Opointer;
	sonyObject obj;

	memcpy( header.magic, "TREE", 4 );
	writeHeader(&header, f, 2);

	//GPLB object pointer
	memcpy( Opointer.magic, "GPLB", 4 );
	Opointer.offset = UINT32_SWAP_BE_LE(0X0030);
	Opointer.length = UINT32_SWAP_BE_LE(16400);// 8 * 2048 + 16 of header
	writeObjectPointer(&Opointer, f);

	//TPLB object pointer
	memcpy( Opointer.magic, "TPLB", 4 );
	Opointer.offset = UINT32_SWAP_BE_LE(0X4040);
	Opointer.length = UINT32_SWAP_BE_LE(16 + (nbSongs * 2)  + (16 - (nbSongs * 2 ) % 16)); //TPLB are 2 byte long + 16 of TPLB header
	writeObjectPointer(&Opointer, f);

	//write the GPLB object header
	memcpy( obj.magic, "GPLB", 4 );
	obj.count = UINT16_SWAP_BE_LE(nbCat);
	obj.size = UINT16_SWAP_BE_LE(8);
	obj.padding[0] = UINT32_SWAP_BE_LE(UINT16_SWAP_BE_LE(obj.count));
	obj.padding[1] = 0;	
	writeObject(&obj, f);

	//write the GPLB object
	std::uint16_t index1 = 0;
	std::uint16_t cte1 = UINT16_SWAP_BE_LE(0x0100);
	std::uint16_t index2;
	std::uint16_t cte2 = 0x0000;

	int index = 1;
	int indexTPLB = 1;
	vector<Song *>::iterator pointerTPLB = songs.begin();

	//*debug
	//for (vector<Song *>::iterator sort2 = songs.begin(); sort2 != songs.end(); sort2++)
	//{
		//fprintf(fp, "%i artist:%s, album:%s, title:%s Track:%i\n", (*sort2)->sonyDbOrder, (*sort2)->artist, (*sort2)->album ,(*sort2)->title, (*sort2)->track_nr);
		//fflush(fp);
	//}
	//fprintf(fp, "\n");
	int oldTPLB = 0;
	//debug */

	//add "all tracks" indexes first
	if (useAllTags)
	{
		index1 = UINT16_SWAP_BE_LE(index); index++; //fucking side effect :-P
		index2 = UINT16_SWAP_BE_LE(indexTPLB);
		fwrite(&index1, sizeof(std::uint16_t), 1, f);
		fwrite(&cte1, sizeof(std::uint16_t), 1, f);
		fwrite(&index2, sizeof(std::uint16_t), 1, f);
		fwrite(&cte2, sizeof(std::uint16_t), 1, f);
		indexTPLB += (nbSongs / 2);
	}

	//scan the list (by album, by artist, by genre)
	for (vector<Song *>::iterator sort = list.begin(); sort != list.end(); sort++)
	{
		//skip empty tracks
		if ((*sort)->statusOfSong == EMPTYTRACK)
		{
			while ((pointerTPLB != songs.end()) && ((*pointerTPLB)->statusOfSong == EMPTYTRACK))
			{
				pointerTPLB++;
			}
			continue;
		}

		index1 = UINT16_SWAP_BE_LE(index); index++; //fucking side effect :-P
		index2 = UINT16_SWAP_BE_LE(indexTPLB);
		fwrite(&index1, sizeof(std::uint16_t), 1, f);
		fwrite(&cte1, sizeof(std::uint16_t), 1, f);
		fwrite(&index2, sizeof(std::uint16_t), 1, f);
		fwrite(&cte2, sizeof(std::uint16_t), 1, f);

		//find the next TPLB index

		if (type == 22)
		{
			//*debug
			//fprintf(fp, " (%i tracks)\nPlaylist : %s \t, from file : %02i",indexTPLB - oldTPLB, (*sort)->album, indexTPLB);
			oldTPLB = indexTPLB;
			//debug */
			while ((pointerTPLB != songs.end()) && ( (*sort)->track_nr-- > 0))
			{
				pointerTPLB++;
				indexTPLB++;
			}
		}

		if ((type == 1) || (type == 3))
		{
			//*debug
			//fprintf(fp, " (%i tracks)\nalbum : %s \t, from file : %02i",indexTPLB - oldTPLB, (*sort)->album, indexTPLB);
			oldTPLB = indexTPLB;
			//debug */
			while ((pointerTPLB != songs.end()) && (STRCMP2_NULLOK((*pointerTPLB)->album, (*sort)->album) == 0))
			{
				pointerTPLB++;
				indexTPLB++;
			}
		}

		if (type == 2)
		{
			//*debug
			//fprintf(fp, " (%i tracks)\nartist : %s \t, from file : %02i",indexTPLB - oldTPLB, (*sort)->artist, indexTPLB);
			oldTPLB = indexTPLB;
			//debug */
			while ((pointerTPLB != songs.end()) && (STRCMP2_NULLOK((*pointerTPLB)->artist, (*sort)->artist) == 0))
			{
				pointerTPLB++;
				indexTPLB++;
			}
		}

		if (type == 4)
		{
			//*debug
			//fprintf(fp, " (%i tracks)\ngenre : %s \t, from file : %02i",indexTPLB - oldTPLB, (*sort)->genre, indexTPLB);
			oldTPLB = indexTPLB;
			//debug */
			while ((pointerTPLB != songs.end()) && (STRCMP2_NULLOK((*pointerTPLB)->genre, (*sort)->genre) == 0))
			{
				pointerTPLB++;
				indexTPLB++;
			}
		}
	}
	//fprintf(fp, "\n\n");//debug

	//fill the rest with zeros 
	index--;
	int last = (16384 - (index * 8));
	std::uint8_t cte3 = 0;
	for (int i = 0; i < last; i++)
		fwrite(&cte3, sizeof(std::uint8_t), 1, f);

	//write the TPLB object header
	memcpy( obj.magic, "TPLB", 4 );
	obj.count = UINT16_SWAP_BE_LE(nbSongs);
	obj.size = UINT16_SWAP_BE_LE(2);
	obj.padding[0] = UINT32_SWAP_BE_LE(UINT16_SWAP_BE_LE(obj.count));
	obj.padding[1] = 0;
	writeObject(&obj, f);

	index = 0;
	index1 = 0;

	//do this twice for "all x" indexes
	if (useAllTags)
	{
		for (vector<Song *>::iterator song = songs.begin(); song != songs.end(); song++)
		{
			if ((*song)->statusOfSong == EMPTYTRACK)
				continue;

			index++;
			index1 = UINT16_SWAP_BE_LE((*song)->sonyDbOrder);
			//fprintf(fp, "%i ", (*song)->sonyDbOrder);//debug
			fwrite(&index1, sizeof(std::uint16_t), 1, f);
		}
	}

	//rest of indexes
	for (vector<Song *>::iterator song = songs.begin(); song != songs.end(); song++)
	{
		if ((*song)->statusOfSong == EMPTYTRACK)
			continue;

		index++;
		index1 = UINT16_SWAP_BE_LE((*song)->sonyDbOrder);
		//fprintf(fp, "%i ", (*song)->sonyDbOrder);//debug
		fwrite(&index1, sizeof(std::uint16_t), 1, f);
	}
	//fprintf(fp, "\n\n");//debug

	//so that we have 8 byte round file
	index1 = 0;
	while (index % 8 != 0)
	{
		fwrite(&index1, sizeof(std::uint16_t), 1, f);
		index++;
	}

	fclose(f);
	return (true);
}

//empty playlist
bool SonyDb::write_01TREE22()
{
	//write the 01TREE22.DAT file
	FILE *f;
	char filename[512];

	//open the file
	sprintf(filename, "%s/OMGAUDIO/01TREE22.DAT", getDriveLetter());
	f = fopen(filename, "wb");
	if (f == NULL)
	{
		fprintf(fp, "error can't open file %s\n", filename);
		fflush(fp);
		return false;
	}

	//write the headers
	sonyFileHeader header;
	sonyObjectPointer Opointer;
	sonyObject obj;

	memcpy( header.magic, "TREE", 4 );
	writeHeader(&header, f, 2);

	//GPLB object pointer
	memcpy( Opointer.magic, "GPLB", 4 );
	Opointer.offset = UINT32_SWAP_BE_LE(0X0030);
	Opointer.length = UINT32_SWAP_BE_LE(16);
	writeObjectPointer(&Opointer, f);

	//TPLB object pointer
	memcpy( Opointer.magic, "TPLB", 4 );
	Opointer.offset = UINT32_SWAP_BE_LE(0X0040);
	Opointer.length = UINT32_SWAP_BE_LE(16);
	writeObjectPointer(&Opointer, f);

	//write the GPLB object header
	memcpy( obj.magic, "GPLB", 4 );
	obj.count = 0;
	obj.size = UINT16_SWAP_BE_LE(8);
	obj.padding[0] = UINT32_SWAP_BE_LE(UINT16_SWAP_BE_LE(obj.count));
	obj.padding[1] = 0;
	writeObject(&obj, f);

	//write the TPLB object header
	memcpy( obj.magic, "TPLB", 4 );
	obj.count = UINT16_SWAP_BE_LE(0);
	obj.size = UINT16_SWAP_BE_LE(2);
	obj.padding[0] = UINT32_SWAP_BE_LE(UINT16_SWAP_BE_LE(obj.count));
	obj.padding[1] = 0;
	writeObject(&obj, f);

	fclose(f);
	return (true);
}

bool SonyDb::write_02TREINF(vector<Song *> songs)
{
	//write the write_02TREINF.DAT file
	FILE *f;
	char filename[512];
	int nbTags = 0;

	//open the file
	sprintf(filename, "%s/OMGAUDIO/02TREINF.DAT", getDriveLetter());
	f = fopen(filename, "wb");
	if (f == NULL)
	{
		fprintf(fp, "error can't open file %s\n", filename);
		fflush(fp);
		return false;
	}


	//write the headers
	sonyFileHeader header;
	sonyObjectPointer Opointer;
	sonyObject obj;

	memcpy( header.magic, "GTIF", 4 );
	writeHeader(&header, f);

	memcpy( Opointer.magic, "GTFB", 4 );
	Opointer.offset = UINT32_SWAP_BE_LE(0x0020);
	Opointer.length = UINT32_SWAP_BE_LE(0x2410);
	writeObjectPointer(&Opointer, f);

	memcpy( obj.magic, "GTFB", 4 );
	obj.count = UINT16_SWAP_BE_LE(34);
	obj.size = UINT16_SWAP_BE_LE(0x0090);;
	obj.padding[0] = 0;
	obj.padding[1] = 0;
	writeObject(&obj, f);

	//write the tags
	int nbWriten = 0; 
	sonyTrack t;
	sonyTrackTag tt;

	//not really a sonytrack, just using the same struct
	t.fileType[0] = 0; 
	t.fileType[1] = 0;
	t.fileType[2] = 0;
	t.fileType[3] = 0;
	t.trackLength = 0;
	t.trackEncoding = 0; //?? unknown value related to the album (this is not really trackEncoding)
	memcpy( tt.tagType, "TIT2", 4 );
	t.nbTagRecords = UINT16_SWAP_BE_LE(1);
	t.sizeTagRecords = UINT16_SWAP_BE_LE(TAGSIZE);

	//encoding
	tt.tagEncoding[0] = 0x00;
	tt.tagEncoding[1] = 0x02;

	for (int i = 0; i < 64; i++)
	//for (int i = 0; i < songs.size()-1; i++)
	{
		if (i >= 4)
		{
			tt.tagType[0] = 0;
			tt.tagType[1] = 0;
			tt.tagType[2] = 0;
			tt.tagType[3] = 0;
			tt.tagEncoding[0] = 0;
			tt.tagEncoding[1] = 0;
			t.nbTagRecords = 0;
			t.sizeTagRecords = 0;
			t.trackLength = 0;
		}
		//else
			//t.trackLength = UINT32_SWAP_BE_LE(songs[i]->songlen * 1000);

		if (!(writeTrackHeader(&t, f)))
			return (false);

		if (!(writeTrackTag(&tt, "", f)))
			return (false);
	}
	fclose(f);
	return (true);
}

bool SonyDb::write_03GINFXX(vector<Song *> list, int type)
{
	//write the 03GINFXX.DAT file
	FILE *f;
	char filename[512];
	int nbTags = 0;
	int nbCat = 0;

	//open the file
	sprintf(filename, "%s/OMGAUDIO/03GINF%02i.DAT", getDriveLetter(), type);
	f = fopen(filename, "wb");
	if (f == NULL)
	{
		fprintf(fp, "error can't open file %s\n", filename);
		fflush(fp);
		return false;
	}

	if (type == 22)
		type = 1;

	//number of non empty categories
	for (vector<Song *>::iterator count2 = list.begin(); count2 != list.end(); count2++)
	{
		if ((*count2)->statusOfSong != EMPTYTRACK)
			nbCat++;
	}

	//for "all x" categorie
	if (useAllTags)
		nbCat++;

	if (type == 1)
		nbTags = 6;
	else
		nbTags = 1;

	//write the headers
	sonyFileHeader header;
	sonyObjectPointer Opointer;
	sonyObject obj;

	memcpy( header.magic, "GPIF", 4 );
	writeHeader(&header, f);

	memcpy( Opointer.magic, "GPFB", 4 );
	Opointer.length = UINT32_SWAP_BE_LE((((TAGSIZE * nbTags) + 16) * nbCat)+16);//basically the size of the next block
	obj.size = UINT16_SWAP_BE_LE((TAGSIZE * nbTags) + 16); //(128 * nbtag) + tag header (same thing here)
	Opointer.offset = 0x20000000;
	writeObjectPointer(&Opointer, f);

	memcpy( obj.magic, "GPFB", 4 );
	obj.count = UINT16_SWAP_BE_LE(nbCat);
	obj.padding[0] = 0;
	obj.padding[1] = 0;
	writeObject(&obj, f);

	//write the tags
	int nbWriten = 0; 
	sonyTrack t;
	sonyTrackTag tt;

	t.fileType[0] = 0;
	t.fileType[1] = 0;
	t.fileType[2] = 0;
	t.fileType[3] = 0;
	t.trackLength = 0;
	t.trackEncoding = 0;//UINT32_SWAP_BE_LE(); //fixme with the correct number of frames
	t.nbTagRecords = UINT16_SWAP_BE_LE(nbTags);
	t.sizeTagRecords = UINT16_SWAP_BE_LE(TAGSIZE);

	//encoding
	tt.tagEncoding[0] = 0x00;
	tt.tagEncoding[1] = 0x02;

	//push front of the list the "all x" categorie
	if (useAllTags)
	{
		Song *s = new Song();
		if (type == 1)
		{
			s->album = "All Albums";
			s->artist = "MLSONYALL";
			s->genre = "MLSONYALL";
		}
		else
		{
			s->album = "All Albums";
			s->artist = "All Artists";
			s->genre = "All Genre";	       
		}
		list.insert(list.begin(), s);
		s->statusOfSong = ADD_TO_DEVICE;
	}

	for (vector<Song *>::iterator i = list.begin(); i != list.end(); i++)
	{
		if ((*i)->statusOfSong == EMPTYTRACK)
			continue;

		if (!(writeTrackHeader(&t, f)))
			return (false);

		//list of : genre
		if (type == 4)
		{
			//artist tag
			memcpy( tt.tagType, "TIT2", 4 );
			if (!(writeTrackTag(&tt, (*i)->genre, f)))
				return (false);
		}

		//list of : albums
		if (type == 3)
		{
			//artist tag
			memcpy( tt.tagType, "TIT2", 4 );
			if (!(writeTrackTag(&tt, (*i)->album, f)))
				return (false);
		}

		//list of : artist
		if (type == 2)
		{
			//artist tag
			memcpy( tt.tagType, "TIT2", 4 );
			if (!(writeTrackTag(&tt, (*i)->artist, f)))
				return (false);
		}

		//list of: album artist genre
		if (type == 1)
		{
			//album tag
			memcpy( tt.tagType, "TIT2", 4 );
			if (!(writeTrackTag(&tt, (*i)->album, f)))
				return (false);

			//artist tag
			memcpy( tt.tagType, "TPE1", 4 );
			if (!(writeTrackTag(&tt, (*i)->artist, f)))
				return (false);

			//genre tag
			memcpy( tt.tagType, "TCON", 4 );
			if (!(writeTrackTag(&tt, (*i)->genre, f)))
				return (false);

			//tsop tag
			memcpy( tt.tagType, "TSOP", 4 );
			if (!(writeTrackTag(&tt, "", f)))
				return (false);

			//picp tag
			memset(tt.tagType, 0, sizeof(tt.tagType));
			if (!(writeTrackTag(&tt, "", f)))
				return (false);

			//pic0 tag
			memcpy( tt.tagType, "PIC0", 4 );
			if (!(writeTrackTag(&tt, "", f)))
				return (false);
		}
	}
	//Remove "all x" categorie
	if (useAllTags)
	{
		Song *s = *(list.begin());
		free(s);
		list.erase(list.begin());
	}

	fclose(f);
	return (true);
}


bool SonyDb::write_04CNTINF(vector<Song *> songsToSend)
{
	//write the 04CNTINF.DAT file
	FILE *f;
	char filename[512];
	//open the file
	sprintf(filename, "%s/OMGAUDIO/04CNTINF.DAT", getDriveLetter());
	f = fopen(filename, "wb");
	if (f == NULL)
	{
		fprintf(fp, "error can't open file 04CNTINF.DAT\n");
		fflush(fp);
		return false;
	}

	//write the headers
	sonyFileHeader header;
	sonyObjectPointer Opointer;
	sonyObject obj;

	memcpy( header.magic, "CNIF", 4 );
	writeHeader(&header, f);

	memcpy( Opointer.magic, "CNFB", 4 );
	Opointer.length = UINT32_SWAP_BE_LE((((TAGSIZE * 5) + 16) * songsToSend.size())+16);
	Opointer.offset = 0x20000000;
	writeObjectPointer(&Opointer, f);

	memcpy( obj.magic, "CNFB", 4 );
	obj.count = UINT16_SWAP_BE_LE(songsToSend.size());
	obj.size = UINT16_SWAP_BE_LE((TAGSIZE * 5) + 16); //(128 * nbtag) + tag header
	obj.padding[0] = 0;
	obj.padding[1] = 0;
	writeObject(&obj, f);

	//write the tags
	int nbWriten = 0;
	sonyTrack t;
	sonyTrackTag tt;

	t.fileType[0] = 0x00;
	t.fileType[1] = 0x00;


	t.nbTagRecords = UINT16_SWAP_BE_LE(5);
	t.sizeTagRecords = UINT16_SWAP_BE_LE(TAGSIZE);

	//encoding
	tt.tagEncoding[0] = 0x00;
	tt.tagEncoding[1] = 0x02;

	for (vector<Song *>::iterator i = songsToSend.begin(); i != songsToSend.end(); i++)
	{
		std::uint16_t protection = (*i)->protection;
		if (protection == 0) protection = SONY_PROTECTION_NONE;
		t.fileType[2] = static_cast<std::uint8_t>(protection >> 8);
		t.fileType[3] = static_cast<std::uint8_t>(protection & 0xff);
		t.trackEncoding = UINT32_SWAP_BE_LE((*i)->encoding);
		t.trackLength = UINT32_SWAP_BE_LE((*i)->songlen * 1000);


		if (!(writeTrackHeader(&t, f)))
			return (false);

		//title tag
		memcpy( tt.tagType, "TIT2", 4 );
		if (!(writeTrackTag(&tt, (*i)->title, f)))
			return (false);

		//artist tag
		memcpy( tt.tagType, "TPE1", 4 );
		if (!(writeTrackTag(&tt, (*i)->artist, f)))
			return (false);

		//album tag
		memcpy( tt.tagType, "TALB", 4 );
		if (!(writeTrackTag(&tt, (*i)->album, f)))
			return (false);

		//genre tag
		memcpy( tt.tagType, "TCON", 4 );
		if (!(writeTrackTag(&tt, (*i)->genre, f)))
			return (false);

		//tsop tag
		memcpy( tt.tagType, "TSOP", 4 );
		if (!(writeTrackTag(&tt, "", f)))
			return (false);
	}
	fclose(f);
	return (true);
}


bool SonyDb::write_05CIDLST(vector<Song *> songsToSend)
{
	FILE *f;
	char filename[512];
	int nbTags = 0;

	//open the file
	sprintf(filename, "%s/OMGAUDIO/05CIDLST.DAT", getDriveLetter());
	f = fopen(filename, "wb");
	if (f == NULL)
	{
		fprintf(fp, "error can't open file %s\n", filename);
		fflush(fp);
		return false;
	}

	//write the headers
	sonyFileHeader header;
	sonyObjectPointer Opointer;
	sonyObject obj;

	memcpy( header.magic, "CIDL", 4 );
	writeHeader(&header, f);

	memcpy( Opointer.magic, "CILB", 4 );
	Opointer.length = UINT32_SWAP_BE_LE(((32 + 16) * songsToSend.size())+16);
	Opointer.offset = UINT32_SWAP_BE_LE(0x0020);
	writeObjectPointer(&Opointer, f);

	memcpy( obj.magic, "CILB", 4 );
	obj.count = UINT16_SWAP_BE_LE(songsToSend.size());
	obj.size = UINT16_SWAP_BE_LE(32 + 16);
	obj.padding[0] = 0;
	obj.padding[1] = 0;
	writeObject(&obj, f);

	for (vector<Song *>::iterator song = songsToSend.begin(); song != songsToSend.end(); song++)
	{
		std::uint8_t emptyRecord[48]{};
		const std::uint8_t *record = (*song)->hasCidRecord ? (*song)->cidRecord : emptyRecord;
		if (fwrite(record, sizeof(emptyRecord), 1, f) != 1)
			return (false);
	}

	fclose(f);
	return (true);
}

//write empty playlist file
bool SonyDb::write_03GINF22()
{
	//write the 03GINF22.DAT file
	FILE *f;
	char filename[512];
	int nbTags = 0;

	//open the file
	sprintf(filename, "%s/OMGAUDIO/03GINF22.DAT", getDriveLetter());
	f = fopen(filename, "wb");
	if (f == NULL)
	{
		fprintf(fp, "error can't open file %s\n", filename);
		fflush(fp);
		return false;
	}

	//write the headers
	sonyFileHeader header;
	sonyObjectPointer Opointer;
	sonyObject obj;

	memcpy( header.magic, "GPIF", 4 );
	writeHeader(&header, f);

	memcpy( Opointer.magic, "GPFB", 4 );
	Opointer.length = UINT32_SWAP_BE_LE(16);
	Opointer.offset = 0x20000000;
	writeObjectPointer(&Opointer, f);

	memcpy( obj.magic, "GPFB", 4 );
	obj.count = 0;//UINT16_SWAP_BE_LE(list.size());
	obj.size = UINT16_SWAP_BE_LE(784); //wtf?
	obj.padding[0] = 0;
	obj.padding[1] = 0;
	writeObject(&obj, f);

	fclose(f);
	return (true);
}

bool SonyDb::write_TrackNumber(vector<Song *> songsToSend)
{
	FILE *f;
	char tmp[512];

	//open the file
	sprintf(tmp, "%s/OMGAUDIO/TRACKS.DAT", getDriveLetter());
	f = fopen(tmp, "wb");
	if (f == NULL)
	{
		fprintf(fp, "error can't open file %s\n", tmp);
		fflush(fp);
		return false;
	}

	for (vector<Song *>::iterator song = songsToSend.begin(); song != songsToSend.end(); song++)
	{
		if ((*song)->statusOfSong == EMPTYTRACK)
			continue;
		sprintf(tmp, "%8i\n", (*song)->track_nr);
		fwrite(tmp, 1, 9, f);
	}

	fclose(f);
	return (true);
}


/* TOOLS */
utf16char *ansi_to_utf16(const char  *str, long len, bool endian)
{
	if (len <= 0) return NULL;
	utf16char *dest = (utf16char*)calloc(static_cast<size_t>(len), sizeof(utf16char));
	if (!dest || !str) return dest;

	const std::u16string converted = utf8_to_utf16_string(str);
	size_t count = std::min(converted.size(), static_cast<size_t>(len - 1));
	if (count < converted.size() && count > 0
		&& converted[count - 1] >= 0xd800 && converted[count - 1] <= 0xdbff)
		--count;
	for (size_t i = 0; i < count; ++i) {
		utf16char value = static_cast<utf16char>(converted[i]);
		dest[i] = endian ? UINT16_SWAP_BE_LE(value) : value;
	}
	return dest;
}

char *utf16_to_ansi(const utf16char *str, long len, bool endian)
{
	if (!str || len <= 0) return NULL;
	std::u16string converted;
	converted.reserve(static_cast<size_t>(len));
	for (long i = 0; i < len; ++i) {
		utf16char value = endian ? UINT16_SWAP_BE_LE(str[i]) : str[i];
		if (value == 0) break;
		converted.push_back(static_cast<char16_t>(value));
	}
	try {
		std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> converter;
		const std::string utf8 = converter.to_bytes(converted);
		return strdup(utf8.c_str());
	} catch (const std::range_error &) {
		return strdup("");
	}
}



//#define RETIFNZ(v) if ((v)<0) return use_dir?1:-1; if ((v)>0) return use_dir?-1:1;

bool sortByIndex(Song *a, Song *b)
{
	return (a->sonyDbOrder < b->sonyDbOrder);
}

bool sortByTrackNumber(Song *a, Song *b)
{
	return (a->track_nr < b->track_nr);
}

bool sortByAlbumName(Song *a, Song *b)
{
	return (STRCMP_NULLOK(a->album, b->album) < 0);
}

bool sortByArtistName(Song *a, Song *b)
{
	return (STRCMP_NULLOK(a->artist, b->artist) < 0);
}

bool sortByTitleName(Song *a, Song *b)
{
	return (STRCMP_NULLOK(a->title, b->title) < 0);
}

bool sortByGenreName(Song *a, Song *b)
{
	return (STRCMP_NULLOK(a->genre, b->genre) < 0);
}


bool sortPlaylist(Playlist s, Playlist s2)
{
	return (STRCMP_NULLOK(s.name, s2.name) < 0);
}

bool sortByPlaylistIndex(Playlist s, Playlist s2)
{
	return (s.index < s2.index);
}
