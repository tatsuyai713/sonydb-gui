cmake_minimum_required(VERSION 3.21)

project(SonyDbCore LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_path(ID3_INCLUDE_DIR NAMES id3.h REQUIRED)
find_library(ID3_LIBRARY NAMES id3 id3-3.8 REQUIRED)

add_library(sonydb_core STATIC sonydb.cpp sonydb.h)
target_include_directories(sonydb_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

option(SONYDB_BUILD_CLI "Build the original command-line application" ON)

if(SONYDB_BUILD_CLI)
  add_executable(sonydb-cli frontend.cpp)
  target_include_directories(sonydb-cli PRIVATE ${ID3_INCLUDE_DIR})
  target_link_libraries(sonydb-cli PRIVATE sonydb_core ${ID3_LIBRARY})
  install(TARGETS sonydb-cli RUNTIME DESTINATION bin)
endif()

if(BUILD_TESTING)
  add_executable(sonydb-core-test tests/core_test.cpp)
  target_include_directories(sonydb-core-test PRIVATE ${ID3_INCLUDE_DIR})
  target_link_libraries(sonydb-core-test PRIVATE sonydb_core ${ID3_LIBRARY})
  add_test(NAME sonydb-core-api COMMAND sonydb-core-test)
endif()
