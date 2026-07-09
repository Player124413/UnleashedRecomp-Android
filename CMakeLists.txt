cmake_minimum_required (VERSION 3.20)

option(UNLEASHED_RECOMP_ANDROID "Build for Android. Does not use vcpkg; expects prebuilt host tool paths via UNLEASHED_RECOMP_HOST_*." OFF)

if(NOT UNLEASHED_RECOMP_ANDROID
   AND NOT UNLEASHED_RECOMP_HOST_TOOLS_ONLY
   AND NOT DEFINED ENV{VCPKG_ROOT})
    message(FATAL_ERROR "VCPKG_ROOT is not defined!")
endif()

set(UNLEASHED_RECOMP_THIRDPARTY_ROOT ${CMAKE_SOURCE_DIR}/thirdparty)
set(UNLEASHED_RECOMP_TOOLS_ROOT ${CMAKE_SOURCE_DIR}/tools)
set(CMAKE_CXX_STANDARD 20)
set(BUILD_SHARED_LIBS OFF)

option(UNLEASHED_RECOMP_HOST_TOOLS_ONLY "Only build the host code-generation tools (XenonRecomp, XenosRecomp, file_to_c, x_decompress), skipping the game executable and its platform-specific dependencies. Used to produce host binaries for cross-compiling to another target platform." OFF)

# When cross-compiling (e.g. to Android), these point at prebuilt host executables
# produced by a UNLEASHED_RECOMP_HOST_TOOLS_ONLY pass, since the in-tree tool targets
# would otherwise be built for the (unrunnable, on the host) target architecture.
set(UNLEASHED_RECOMP_HOST_XENON_RECOMP "" CACHE FILEPATH "Path to a prebuilt host XenonRecomp executable")
set(UNLEASHED_RECOMP_HOST_XENOS_RECOMP "" CACHE FILEPATH "Path to a prebuilt host XenosRecomp executable")
set(UNLEASHED_RECOMP_HOST_FILE_TO_C "" CACHE FILEPATH "Path to a prebuilt host file_to_c executable")
set(UNLEASHED_RECOMP_HOST_X_DECOMPRESS "" CACHE FILEPATH "Path to a prebuilt host x_decompress executable")

# Enable Hot Reload for MSVC compilers if supported.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project("UnleashedRecomp-ALL")

if (APPLE)
    enable_language(OBJC OBJCXX)
endif()

if (CMAKE_OSX_ARCHITECTURES)
    set(UNLEASHED_RECOMP_ARCHITECTURE ${CMAKE_OSX_ARCHITECTURES})
elseif(CMAKE_SYSTEM_PROCESSOR)
    set(UNLEASHED_RECOMP_ARCHITECTURE ${CMAKE_SYSTEM_PROCESSOR})
else()
    set(UNLEASHED_RECOMP_ARCHITECTURE ${CMAKE_HOST_SYSTEM_PROCESSOR})
endif()
string(TOLOWER "${UNLEASHED_RECOMP_ARCHITECTURE}" UNLEASHED_RECOMP_ARCHITECTURE)
message(STATUS "Detected architecture: ${UNLEASHED_RECOMP_ARCHITECTURE}")

if (UNLEASHED_RECOMP_ARCHITECTURE STREQUAL "x86_64" OR UNLEASHED_RECOMP_ARCHITECTURE STREQUAL "amd64")
    # Target Sandy Bridge for all projects
    add_compile_options(
        -march=sandybridge
    )
endif()

if (UNLEASHED_RECOMP_HOST_XENON_RECOMP AND UNLEASHED_RECOMP_HOST_XENOS_RECOMP AND UNLEASHED_RECOMP_HOST_FILE_TO_C AND UNLEASHED_RECOMP_HOST_X_DECOMPRESS)
    set(UNLEASHED_RECOMP_USE_PREBUILT_HOST_TOOLS ON)
else()
    set(UNLEASHED_RECOMP_USE_PREBUILT_HOST_TOOLS OFF)
endif()

if (UNLEASHED_RECOMP_HOST_TOOLS_ONLY)
    add_subdirectory(${UNLEASHED_RECOMP_TOOLS_ROOT})
else()
    add_subdirectory(${UNLEASHED_RECOMP_THIRDPARTY_ROOT})

    # tools/ also provides shared thirdparty libraries (fmt, zstd, xxHash) that
    # UnleashedRecomp links against directly, so it's always added even when the
    # recompiler executables themselves are swapped out for prebuilt host binaries below.
    add_subdirectory(${UNLEASHED_RECOMP_TOOLS_ROOT})

    # Include sub-projects.
    add_subdirectory("UnleashedRecompLib")
    add_subdirectory("UnleashedRecomp")
endif()
