cmake_minimum_required(VERSION 3.25)

project(ZultysNCallProviderRegistration VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "TAPI provider registration can only be built on Windows.")
endif()

if(NOT MSVC)
  message(FATAL_ERROR "The registration helper is built and validated with MSVC plus the Windows SDK.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Build the provider registration helper as x64.")
endif()

add_executable(ZultysNCallProviderReg ProviderRegistration.cpp)
target_compile_features(ZultysNCallProviderReg PRIVATE cxx_std_20)
target_compile_definitions(ZultysNCallProviderReg PRIVATE
  UNICODE
  _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
)
target_compile_options(ZultysNCallProviderReg PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(ZultysNCallProviderReg PRIVATE tapi32 advapi32)

