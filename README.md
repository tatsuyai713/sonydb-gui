# SonyDb GUI

SonyDb GUI is a C++/Qt 6 application for importing audio CDs, managing a local
audio library, and transferring music between Ubuntu and legacy Sony Walkman
players. It uses the
OpenMG/OMA database support from `mattn/sonydb` and does not use Python.

Audio CDs are read directly from the Linux optical-drive interface and encoded
as MP3, FLAC, or Ogg Vorbis by FFmpeg. Album and track metadata comes from
MusicBrainz, one of the world's largest open music metadata databases, using CD
table-of-contents matching without an API key.

## Source layout

```text
.
├── gui_main.cpp       # Qt GUI for SonyDb
├── cd_importer.*      # Linux CD-DA, MusicBrainz, and audio import service
├── local_audio.*      # Local format detection and metadata probing
├── encoded_audio_importer.* # Shared encoded-audio import path
├── library_path.h     # Unicode-normalized library paths
├── id3_metadata.*     # Locale-independent ID3 Unicode decoder
├── scripts/           # Debian package and AppImage build helpers
├── CMakeLists.txt     # Top-level project, including the GUI
└── sonydb/            # Mattn's sonydb transfer engine and CLI
    ├── sonydb.cpp
    ├── sonydb.h
    └── frontend.cpp
```

The `sonydb/` directory does not contain GUI-specific code. It contains the
original engine with only the changes required for Ubuntu support and a safe
public API.

> **Important:** This application targets legacy Walkman players that contain
> `OMGAUDIO/04CNTINF.DAT` at the device root. Later NWZ mass-storage and MTP
> models should use regular file copying or an MTP application. Back up
> important device data before transferring tracks to real hardware.

## Building on Ubuntu

Install the required packages on Ubuntu 24.04 or later:

```sh
sudo apt update
sudo apt install build-essential cmake qt6-base-dev libid3-3.8.3-dev ffmpeg
cmake --preset default
cmake --build --preset default -j
ctest --test-dir build/default --output-on-failure
```

Run the application:

```sh
./build/default/sonydb-gui
```

To build and install a release version:

```sh
cmake --preset release
cmake --build --preset release -j
sudo cmake --install build/release
```

To produce an installable Debian package or standalone AppImage:

```sh
./scripts/build-deb.sh
./scripts/build-appimage.sh
```

Package output is written under `dist/`. The Debian package declares its
runtime dependencies so APT can resolve them during installation.

## Usage

1. Use **Music Folder** in the top toolbar to select the PC music library and CD
   import destination.
2. Insert an audio CD and click **Refresh CD**. The application obtains album,
   track, date, album-artist, and per-track artist metadata from MusicBrainz.
   Select the correct release when more than one edition matches the disc.
3. Open **Settings**, then **Audio Encoding Settings**, and choose MP3, FLAC, or
   Ogg Vorbis. MP3 supports CBR (128–320 kbps), VBR (V0–V5), channel mode, and
   encoder quality. FLAC supports compression levels 0–12, and Ogg Vorbis
   supports quality levels 0–10. The settings are remembered.
4. Select tracks in the **Audio CD** pane and click the right-arrow **Import**
   button. Files are written as
   `Album Artist/Album/Track Number - Title.extension`, with format-native metadata. Compilation
   albums keep each track artist and normally use `Various Artists` as the album
   folder.
5. Connect the Walkman over USB and mount it in Ubuntu.
6. Select **Auto Detect**. If the device is not found, use **Select Walkman** to
   choose its mount point manually.
   Early OpenMG models such as the NW-E405 also require their device-specific
   `DvID.dat`; select it under **Settings > Walkman Security** when requested.
7. Select supported audio files or folders in the PC tree and click the center
   right arrow to transfer them to the Walkman. Compatible MP3 files are copied
   without re-encoding; other formats are temporarily converted using the
   Walkman MP3 settings.
8. Use **By Album**, **By Artist**, **By Genre**, or **Ungrouped** to change the
   device list view.
9. To restore music from the Walkman, select a destination folder on the PC,
   select tracks on the right, and click the center left arrow. Tracks are saved
   using the common `Artist/Album/Track Number - Title.mp3` layout.
10. Compilation albums containing multiple track artists are placed under
   `Various Artists/Album/` while each track keeps its own artist metadata.
11. If files with the same names already exist, choose **Overwrite**,
   **Skip Duplicates**, or **Cancel**.
12. To edit track information or remove tracks from the device, select tracks on
   the right and use the corresponding toolbar action.

Every track pane also has a context menu. Right-click selected CD tracks to
import them. On the PC pane, selected MP3 files can display their tags, audio
format, duration, size, and paths or be transferred to the Walkman. Selected
Walkman tracks can be restored, edited, or deleted; right-clicking a group
heading also offers restore-all and delete-all actions. Right-clicking within an existing
multi-selection preserves that selection, so batch operations work from the
context menus as well as from the arrow buttons.

Column headers in the Audio CD, This PC, and Sony Walkman panes are resizable by
dragging their separators. Columns can also be reordered by dragging a header,
and each pane remembers its header layout for the next launch.

## Transfer quality and text encoding

Transferring a compatible MP3 to the Walkman does not re-encode its audio. SonyDb wraps
the original MP3 frames in the device-required OMA container, so the bitrate,
VBR/CBR mode, sample rate, and audible quality remain unchanged. For CD imports,
the quality selected under **MP3 Settings** therefore becomes the Walkman copy's
quality as well. FLAC, Ogg Vorbis, Opus, M4A/AAC, WAV, and WMA files remain
unchanged in the PC library and are converted to a temporary Walkman-compatible
MP3 only during transfer. CBR 192 or 256 kbps is a practical compatibility-oriented
choice; CBR 320 kbps or VBR V0 provides the highest quality offered by the
application at a larger file size.

The GUI and SonyDb core exchange metadata as UTF-8, and the core writes the
Walkman database and OMA text fields as UTF-16BE. This conversion is explicit
and does not depend on the Ubuntu process locale, so Japanese and other Unicode
artist, album, title, and genre text is preserved. The ID3 reader also handles
id3lib's byte-ordered UTF-16 buffers and stops at the real string terminator
instead of treating the field capacity as its text length. Tracks written by an older
build with already-corrupted metadata must be removed and transferred again;
the lost text cannot be reconstructed from the corrupted device database.

PC library path components are normalized with Unicode NFKC plus consistent
apostrophes, quotation marks, dashes, wave marks, and whitespace. Imports reuse
equivalent existing artist and album directories. Existing folders can be
normalized without overwriting colliding files:

```sh
./build/default/sonydb-normalize-library "$HOME/Music"
```

CD metadata lookup requires an internet connection. CD extraction also requires
read permission for the optical drive (normally `/dev/sr0`); Ubuntu desktop
sessions usually grant this automatically. If access is denied, add the user to
the `cdrom` group and sign in again:

```sh
sudo usermod -aG cdrom "$USER"
```

CD audio read through Linux is 16-bit PCM in the host byte order. SonyDb selects
the matching FFmpeg input format (`s16le` on standard Ubuntu x86-64 systems).
Builds made before this correction treated the data as big-endian and could
produce full-scale white noise. MP3 files created by such a build must be
deleted and imported from the CD again; lossy encoding makes the damaged files
unrecoverable.

Transfers, edits, and removals are applied to the device only after
confirmation. Do not disconnect the USB cable while an operation is running.
Use Ubuntu's eject or unmount action before physically disconnecting the
Walkman.
Selecting an artist or album folder on the PC recursively transfers supported
audio files below it. The physical files on the Walkman use the device-required
`OMGAUDIO` layout, while artist and album classifications are preserved as
track metadata in the SonyDb database.

If an empty device has no `OMGAUDIO` database, the application detects Sony
Network Walkman storage and asks for explicit confirmation before creating an
empty database. Existing files are not removed.

Before a transfer, `OMGAUDIO/*.DAT` is automatically backed up under
`~/.local/share/SonyDb/SonyDb GUI/backups/`. The original command-line program
is still built as `./build/default/sonydb/sonydb-cli`. To set the mount point
explicitly, use `SONYDB_PLAYERPATH=/media/$USER/WALKMAN`.

## OpenMG device keys and MG ERROR

Some early Network Walkman models require MP3 payload encryption using a key
derived from that physical device's `DvID.dat`. SonyDb detects these databases,
searches common device and MP3 File Manager locations for the key, preserves
existing protection records, and blocks a protected transfer before changing
the device when no valid key is available.

If `DvID.dat` is absent on Linux,
SonyDb asks the connected player for its device-ID record through the same
non-destructive vendor SCSI query used by Sony MP3 File Manager, validates the
16-byte response, and stores it as `MP3FM/DvID.dat`.

If an older SonyDb build created unprotected tracks that report **MG ERROR**,
load the correct key under **Settings > Walkman Security** and use **Repair
Existing MG ERROR Tracks**. A database backup is created before repair.

`DvID.dat` is unique to the physical Walkman and cannot safely be invented or
copied from another unit. For a supported connected player such as the NW-E405,
SonyDb retrieves it directly from the hardware. Manual selection in
**Settings > Walkman Security** remains available when the raw device cannot be
opened or a player does not implement this query. Files transferred by an older
SonyDb build that already show **MG ERROR** were written without this encryption
and must be removed and transferred again after the correct key is loaded.

## Format limitations

MP3 tracks transferred by SonyDb GUI can be restored as playable MP3 files.
Legacy ATRAC/OpenMG tracks protected by SonicStage cannot be exported as MP3
without the original SonicStage encryption keys. The application rejects these
tracks instead of writing a corrupt file with an `.mp3` extension.
