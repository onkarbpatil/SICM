cmake_minimum_required (VERSION 3.0)
project (SICM)

# Use -DSICM_BUILD_HIGH_LEVEL=True to build the high-level interface.
set(SICM_BUILD_HIGH_LEVEL False CACHE BOOL "Should we build the high-level interface?")

find_package(Threads REQUIRED)
link_libraries(Threads::Threads m dl)

# Install custom CMake modules for jemalloc and libnuma.
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/Modules/")

# find jemalloc through pkg-config
find_package(PkgConfig REQUIRED)
pkg_search_module(JEMALLOC REQUIRED jemalloc)
string(REPLACE ";" " " JEMALLOC_LDFLAGS "${JEMALLOC_LDFLAGS}")
link_directories(${JEMALLOC_LIBRARY_DIRS})

# Compile and install the low- and high-level interfaces
add_subdirectory(src)

# Install the compiler wrappers if we're doing the high-level interface
if(SICM_BUILD_HIGH_LEVEL)
  add_subdirectory(bin)
endif()

# Add helper code found in the contrib directory
add_subdirectory(contrib)

# Compile some tests
enable_testing()
add_subdirectory(test)
