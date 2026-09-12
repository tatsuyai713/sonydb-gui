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

  add_executable(sonydb-gui gui_main.cpp cd_importer.cpp cd_importer.h
                 id3_metadata.cpp id3_metadata.h)
  target_include_directories(sonydb-gui PRIVATE ${ID3_INCLUDE_DIR})
  target_link_libraries(sonydb-gui PRIVATE sonydb_core Qt6::Widgets Qt6::Network ${ID3_LIBRARY})

  if(BUILD_TESTING)
    add_executable(cd-importer-test cd_importer_test.cpp cd_importer.cpp cd_importer.h)
    target_link_libraries(cd-importer-test PRIVATE Qt6::Network)
    add_test(NAME cd-importer-metadata COMMAND cd-importer-test)
    add_executable(id3-metadata-test id3_metadata_test.cpp id3_metadata.cpp id3_metadata.h)
    target_include_directories(id3-metadata-test PRIVATE ${ID3_INCLUDE_DIR} sonydb)
    target_link_libraries(id3-metadata-test PRIVATE sonydb_core Qt6::Core ${ID3_LIBRARY})
    add_test(NAME id3-unicode-metadata COMMAND id3-metadata-test)
  endif()

  install(TARGETS sonydb-gui RUNTIME DESTINATION bin)
  install(FILES sonydb-gui.desktop
          DESTINATION share/applications)
endif()
