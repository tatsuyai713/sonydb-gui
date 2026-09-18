cmake_minimum_required(VERSION 3.21)

project(SonyDbGui VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

find_path(ID3_INCLUDE_DIR NAMES id3.h REQUIRED)
find_library(ID3_LIBRARY NAMES id3 id3-3.8 REQUIRED)

# Keep Mattn's sonydb transfer engine in its own subdirectory.
add_subdirectory(sonydb)

option(SONYDB_BUILD_GUI "Build the Qt 6 desktop application" ON)

if(SONYDB_BUILD_GUI)
  find_package(Qt6 6.2 COMPONENTS Widgets Network QUIET)
  if(NOT Qt6_FOUND)
    message(FATAL_ERROR
      "Qt 6 Widgets and Network development files were not found.\n"
      "Install them on Ubuntu with:\n"
      "  sudo apt install qt6-base-dev")
  endif()

  set(CMAKE_AUTOMOC ON)
  set(CMAKE_AUTORCC ON)
  add_executable(sonydb-gui gui_main.cpp cd_importer.cpp cd_importer.h
                 encoded_audio_importer.cpp encoded_audio_importer.h
                 id3_metadata.cpp id3_metadata.h library_path.h local_audio.cpp local_audio.h
                 resources.qrc)
  target_include_directories(sonydb-gui PRIVATE ${ID3_INCLUDE_DIR})
  target_link_libraries(sonydb-gui PRIVATE sonydb_core Qt6::Widgets Qt6::Network ${ID3_LIBRARY})

  add_executable(sonydb-normalize-library normalize_library_main.cpp library_path.h)
  target_link_libraries(sonydb-normalize-library PRIVATE Qt6::Core)

  if(BUILD_TESTING)
    add_executable(cd-importer-test cd_importer_test.cpp cd_importer.cpp cd_importer.h)
    target_sources(cd-importer-test PRIVATE encoded_audio_importer.cpp encoded_audio_importer.h
                   local_audio.cpp local_audio.h)
    target_link_libraries(cd-importer-test PRIVATE Qt6::Network)
    add_test(NAME cd-importer-metadata COMMAND cd-importer-test)
    add_executable(id3-metadata-test id3_metadata_test.cpp id3_metadata.cpp id3_metadata.h)
    target_include_directories(id3-metadata-test PRIVATE ${ID3_INCLUDE_DIR} sonydb)
    target_link_libraries(id3-metadata-test PRIVATE sonydb_core Qt6::Core ${ID3_LIBRARY})
    add_test(NAME id3-unicode-metadata COMMAND id3-metadata-test)
    add_executable(local-audio-test local_audio_test.cpp local_audio.cpp local_audio.h)
    target_link_libraries(local-audio-test PRIVATE Qt6::Core Qt6::Network)
    add_test(NAME local-audio-formats COMMAND local-audio-test)
  endif()

  install(TARGETS sonydb-gui RUNTIME DESTINATION bin)
  install(TARGETS sonydb-normalize-library RUNTIME DESTINATION bin)
  install(FILES sonydb-gui.desktop
          DESTINATION share/applications)
  install(FILES logo.png
          DESTINATION share/icons/hicolor/512x512/apps
          RENAME sonydb-gui.png)
  install(FILES 70-sonydb-walkman.rules
          DESTINATION lib/udev/rules.d)
endif()

# Native Debian/Ubuntu package metadata. CPack asks dpkg-shlibdeps to derive
# shared-library dependencies from every installed executable; FFmpeg remains
# explicit because it is launched as a subprocess at runtime.
set(CPACK_PACKAGE_NAME "sonydb-gui")
set(CPACK_PACKAGE_VENDOR "SonyDb Contributors")
set(CPACK_PACKAGE_CONTACT "SonyDb Contributors <tatsuyai713@users.noreply.github.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Music manager for legacy Sony Walkman players")
set(CPACK_PACKAGE_DESCRIPTION
    "SonyDb GUI is a C++/Qt music manager for Ubuntu. It imports audio CDs, manages a local music library, and transfers compatible audio to and from legacy Sony Walkman players.")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/tatsuyai713/sonydb")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_STRIP_FILES ON)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION "sound")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "ffmpeg")
include(CPack)
