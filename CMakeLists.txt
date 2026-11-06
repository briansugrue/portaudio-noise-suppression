cmake_minimum_required(VERSION 3.15)

# Statically link the MSVC runtime so the .exe doesn't need the VC++ Redistributable.
# (Must be set before project(); ignored on non-MSVC compilers.)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(portaudio_noise_suppression C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# PortAudio v19.7.0 declares a very old minimum CMake version, which new CMake
# releases (4.x) reject. This lets it configure anyway.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# PortAudio
set(PA_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)  # static library
set(PA_BUILD_TESTS       OFF CACHE BOOL "" FORCE)
set(PA_BUILD_EXAMPLES    OFF CACHE BOOL "" FORCE)
set(PA_USE_ASIO          OFF CACHE BOOL "" FORCE)  # needs the Steinberg SDK; not required

FetchContent_Declare(
  portaudio
  GIT_REPOSITORY https://github.com/PortAudio/portaudio.git
  GIT_TAG        v19.7.0
)

# libsndfile (WAV only, no external codec libraries)
set(BUILD_SHARED_LIBS      OFF CACHE BOOL "" FORCE)
set(BUILD_PROGRAMS         OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES         OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING          OFF CACHE BOOL "" FORCE)
set(ENABLE_EXTERNAL_LIBS   OFF CACHE BOOL "" FORCE)  # skips FLAC/Ogg/Vorbis/Opus; WAV still works
set(ENABLE_MPEG            OFF CACHE BOOL "" FORCE)
set(ENABLE_PACKAGE_CONFIG  OFF CACHE BOOL "" FORCE)
set(INSTALL_PKGCONFIG_MODULE OFF CACHE BOOL "" FORCE)

# KissFFT (float build, static)
set(KISSFFT_DATATYPE float CACHE STRING "" FORCE)
set(KISSFFT_STATIC   ON    CACHE BOOL   "" FORCE)
set(KISSFFT_TEST     OFF   CACHE BOOL   "" FORCE)
set(KISSFFT_TOOLS    OFF   CACHE BOOL   "" FORCE)

FetchContent_Declare(
  kissfft
  GIT_REPOSITORY https://github.com/mborgerding/kissfft.git
  GIT_TAG        131.1.0
)

FetchContent_Declare(
  libsndfile
  GIT_REPOSITORY https://github.com/libsndfile/libsndfile.git
  GIT_TAG        1.2.2
)

FetchContent_MakeAvailable(portaudio kissfft libsndfile)

# Executable (wiener_vad.h sits next to main.cpp; list it so IDEs show it)
add_executable(portaudio_noise_suppression main.cpp wiener_vad.h)

# Different PortAudio versions name the static target differently; pick whichever exists.
if(TARGET portaudio_static)
  target_link_libraries(portaudio_noise_suppression PRIVATE portaudio_static)
else()
  target_link_libraries(portaudio_noise_suppression PRIVATE portaudio)
endif()

target_link_libraries(portaudio_noise_suppression PRIVATE sndfile)

# Link KissFFT by its CMake *target* name. The output file is called
# kissfft-float, but the target is not, and linking the plain name "kissfft-float"
# is what caused LNK1104. Pick whichever target name this KissFFT version defines.
if(TARGET kissfft::kissfft)
  target_link_libraries(portaudio_noise_suppression PRIVATE kissfft::kissfft)
elseif(TARGET kissfft)
  target_link_libraries(portaudio_noise_suppression PRIVATE kissfft)
elseif(TARGET kissfft-float)
  target_link_libraries(portaudio_noise_suppression PRIVATE kissfft-float)
else()
  message(FATAL_ERROR "No KissFFT target found (tried kissfft::kissfft, kissfft, kissfft-float)")
endif()