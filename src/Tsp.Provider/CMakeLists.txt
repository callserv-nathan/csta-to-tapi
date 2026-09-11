cmake_minimum_required(VERSION 3.25)

project(ZultysNCallTsp VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "A TSPI provider can only be built on Windows.")
endif()

if(NOT MSVC)
  message(FATAL_ERROR "This provider is built and validated for MSVC plus the Windows SDK.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Build the TSP as x64. It is loaded by the 64-bit TAPISRV process.")
endif()

add_library(ZultysNCallTsp SHARED
  ZultysNCallTsp.def
  src/BridgeProtocol.cpp
  src/Exports.cpp
  src/ProviderRuntime.cpp
)

target_compile_features(ZultysNCallTsp PRIVATE cxx_std_20)
target_compile_definitions(ZultysNCallTsp PRIVATE
  UNICODE
  _UNICODE
  TAPI_CURRENT_VERSION=0x00020002
  WIN32_LEAN_AND_MEAN
  NOMINMAX
)
target_include_directories(ZultysNCallTsp PRIVATE include)
target_compile_options(ZultysNCallTsp PRIVATE /W4 /permissive- /EHsc)
target_link_options(ZultysNCallTsp PRIVATE /INCREMENTAL:NO)

# A TSP is a DLL with a .tsp extension, not an ordinary application DLL.
set_target_properties(ZultysNCallTsp PROPERTIES
  OUTPUT_NAME "ZultysNCallTsp"
  PREFIX ""
  SUFFIX ".tsp"
)
