# libopticc: model, predictors, policies, bounds (shared by simulator and tool)
add_library(opticc STATIC
  src/model.cpp
  src/predictor.cpp
  src/sched.cpp
  src/bounds.cpp
  src/speed.cpp)
target_include_directories(opticc PUBLIC include)
target_link_libraries(opticc PUBLIC nlohmann_json PRIVATE opticc_warnings)

# opticc_core: the above plus wire protocol, transport, argument scanning and
# POSIX helpers (needs zstd). Kept separate so the simulator stays dependency-free.
find_package(zstd CONFIG QUIET HINTS /opt/homebrew/opt/zstd /usr/local/opt/zstd)
if(zstd_FOUND)
  if(TARGET zstd::libzstd_static)
    set(OPTICC_ZSTD zstd::libzstd_static)
  else()
    set(OPTICC_ZSTD zstd::libzstd_shared)
  endif()
else()
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(ZSTD IMPORTED_TARGET libzstd)
  endif()
  if(ZSTD_FOUND)
    set(OPTICC_ZSTD PkgConfig::ZSTD)
  else()
    include(FetchContent)
    FetchContent_Declare(zstd URL https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-1.5.6.tar.gz
                         SOURCE_SUBDIR build/cmake)
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(zstd)
    set(OPTICC_ZSTD libzstd_static)
  endif()
endif()

# libsodium for the Noise channel: pkg-config, else a plain search.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  set(ENV{PKG_CONFIG_PATH} "$ENV{PKG_CONFIG_PATH}:/opt/homebrew/opt/libsodium/lib/pkgconfig:/usr/local/opt/libsodium/lib/pkgconfig")
  pkg_check_modules(SODIUM IMPORTED_TARGET libsodium)
endif()
if(SODIUM_FOUND)
  set(OPTICC_SODIUM PkgConfig::SODIUM)
else()
  find_path(SODIUM_INCLUDE_DIR sodium.h HINTS /opt/homebrew/opt/libsodium/include /usr/local/opt/libsodium/include)
  find_library(SODIUM_LIBRARY NAMES sodium HINTS /opt/homebrew/opt/libsodium/lib /usr/local/opt/libsodium/lib)
  if(NOT SODIUM_INCLUDE_DIR OR NOT SODIUM_LIBRARY)
    message(FATAL_ERROR "libsodium not found: install it (brew install libsodium / apt install libsodium-dev)")
  endif()
  add_library(opticc_sodium INTERFACE)
  target_include_directories(opticc_sodium SYSTEM INTERFACE ${SODIUM_INCLUDE_DIR})
  target_link_libraries(opticc_sodium INTERFACE ${SODIUM_LIBRARY})
  set(OPTICC_SODIUM opticc_sodium)
endif()

add_library(opticc_core STATIC
  src/protocol.cpp
  src/transport.cpp
  src/noise.cpp
  src/argscan.cpp
  src/closure.cpp
  src/servescope.cpp
  src/incscan.cpp
  src/cachekey.cpp
  src/direct.cpp
  src/probe.cpp
  src/rdirect.cpp
  src/sysinc.cpp
  src/util.cpp)
target_include_directories(opticc_core PUBLIC include)
target_link_libraries(opticc_core PUBLIC opticc ${OPTICC_ZSTD} ${OPTICC_SODIUM} PRIVATE opticc_warnings)
find_package(Threads REQUIRED)
target_link_libraries(opticc_core PUBLIC Threads::Threads)
