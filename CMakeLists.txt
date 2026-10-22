cmake_minimum_required(VERSION 3.20)
project(marketpipe_hft_demo VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# vcpkg is the supported dependency provider on Windows and CI.  An existing
# system Boost/OpenSSL installation is used when vcpkg is absent.
if(DEFINED ENV{VCPKG_ROOT})
  list(PREPEND CMAKE_PREFIX_PATH "$ENV{VCPKG_ROOT}/installed/x64-windows")
endif()

find_package(OpenSSL CONFIG QUIET)
if(NOT OpenSSL_FOUND)
  find_package(OpenSSL REQUIRED)
endif()
find_package(Threads REQUIRED)

find_package(Boost 1.74 QUIET CONFIG)
if(NOT Boost_FOUND)
  find_package(Boost 1.74 QUIET)
endif()
if(Boost_FOUND)
  if(TARGET Boost::headers)
    set(MARKETPIPE_BOOST_TARGET Boost::headers)
  elseif(TARGET Boost::boost)
    set(MARKETPIPE_BOOST_TARGET Boost::boost)
  else()
    set(MARKETPIPE_BOOST_TARGET "")
  endif()
else()
  find_path(BOOST_INCLUDE_DIR boost/beast.hpp
    HINTS "$ENV{VCPKG_ROOT}/installed/x64-windows/include" "D:/Dev/vcpkg/installed/x64-windows/include")
  if(NOT BOOST_INCLUDE_DIR)
    message(FATAL_ERROR "Boost.Beast headers not found; set VCPKG_ROOT or install Boost")
  endif()
  add_library(marketpipe_boost_headers INTERFACE)
  target_include_directories(marketpipe_boost_headers INTERFACE ${BOOST_INCLUDE_DIR})
  set(MARKETPIPE_BOOST_TARGET marketpipe_boost_headers)
endif()

add_library(marketpipe_core STATIC
  cpp/src/json.cpp
  cpp/src/core.cpp
  cpp/src/runtime.cpp
  cpp/src/capture.cpp
  cpp/src/report.cpp
  cpp/src/benchmark.cpp
)
target_include_directories(marketpipe_core PUBLIC cpp/include)
target_link_libraries(marketpipe_core PUBLIC OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
if(MARKETPIPE_BOOST_TARGET)
  target_link_libraries(marketpipe_core PUBLIC ${MARKETPIPE_BOOST_TARGET})
endif()
target_compile_definitions(marketpipe_core PUBLIC MARKETPIPE_VERSION="${PROJECT_VERSION}")

if(MSVC)
  target_compile_options(marketpipe_core PUBLIC /W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus /bigobj)
  target_compile_definitions(marketpipe_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
  target_link_libraries(marketpipe_core PUBLIC crypt32 psapi)
else()
  target_compile_options(marketpipe_core PUBLIC -Wall -Wextra -Wpedantic)
endif()

add_executable(marketpipe cpp/src/main.cpp)
target_link_libraries(marketpipe PRIVATE marketpipe_core)

if(WIN32)
  set(MARKETPIPE_OPENSSL_DLLS "")
  foreach(openssl_target OpenSSL::SSL OpenSSL::Crypto)
    if(TARGET ${openssl_target})
      get_target_property(MARKETPIPE_IMPLIB ${openssl_target} IMPORTED_IMPLIB)
      if(MARKETPIPE_IMPLIB)
        get_filename_component(MARKETPIPE_LIBDIR "${MARKETPIPE_IMPLIB}" DIRECTORY)
        get_filename_component(MARKETPIPE_BINDIR "${MARKETPIPE_LIBDIR}/../bin" ABSOLUTE)
        file(GLOB MARKETPIPE_TARGET_DLLS "${MARKETPIPE_BINDIR}/libssl*.dll"
             "${MARKETPIPE_BINDIR}/libcrypto*.dll")
        list(APPEND MARKETPIPE_OPENSSL_DLLS ${MARKETPIPE_TARGET_DLLS})
      endif()
    endif()
  endforeach()
  if(DEFINED ENV{VCPKG_ROOT})
    file(GLOB MARKETPIPE_VCPKG_DLLS
      "$ENV{VCPKG_ROOT}/installed/x64-windows/bin/libssl*.dll"
      "$ENV{VCPKG_ROOT}/installed/x64-windows/bin/libcrypto*.dll")
    list(APPEND MARKETPIPE_OPENSSL_DLLS ${MARKETPIPE_VCPKG_DLLS})
  endif()
  # Derive the DLL directory from the headers that were actually found, so a
  # Visual Studio-provided VCPKG_ROOT cannot hide the real installation.
  if(OpenSSL_INCLUDE_DIR)
    get_filename_component(MARKETPIPE_OPENSSL_PREFIX "${OpenSSL_INCLUDE_DIR}/.." ABSOLUTE)
    file(GLOB MARKETPIPE_OPENSSL_PREFIX_DLLS
      "${MARKETPIPE_OPENSSL_PREFIX}/bin/libssl*.dll"
      "${MARKETPIPE_OPENSSL_PREFIX}/bin/libcrypto*.dll")
    list(APPEND MARKETPIPE_OPENSSL_DLLS ${MARKETPIPE_OPENSSL_PREFIX_DLLS})
  endif()
  if(OpenSSL_DIR)
    get_filename_component(MARKETPIPE_OPENSSL_CONFIG_PREFIX "${OpenSSL_DIR}/../.." ABSOLUTE)
    file(GLOB MARKETPIPE_OPENSSL_CONFIG_DLLS
      "${MARKETPIPE_OPENSSL_CONFIG_PREFIX}/bin/libssl*.dll"
      "${MARKETPIPE_OPENSSL_CONFIG_PREFIX}/bin/libcrypto*.dll")
    list(APPEND MARKETPIPE_OPENSSL_DLLS ${MARKETPIPE_OPENSSL_CONFIG_DLLS})
  endif()
  list(REMOVE_DUPLICATES MARKETPIPE_OPENSSL_DLLS)
  if(MARKETPIPE_OPENSSL_DLLS)
    add_custom_target(marketpipe_copy_openssl ALL
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${MARKETPIPE_OPENSSL_DLLS} ${CMAKE_BINARY_DIR}
      COMMENT "Copying OpenSSL runtime DLLs next to the executables")
  endif()
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(marketpipe_unit_tests cpp/tests/test_unit.cpp)
  target_link_libraries(marketpipe_unit_tests PRIVATE marketpipe_core)
  add_executable(marketpipe_integration_tests cpp/tests/test_integration.cpp)
  target_link_libraries(marketpipe_integration_tests PRIVATE marketpipe_core)

  if(TARGET marketpipe_copy_openssl)
    add_dependencies(marketpipe marketpipe_copy_openssl)
    add_dependencies(marketpipe_unit_tests marketpipe_copy_openssl)
    add_dependencies(marketpipe_integration_tests marketpipe_copy_openssl)
  endif()

  add_test(NAME marketpipe_help COMMAND marketpipe --help)
  add_test(NAME marketpipe_unit COMMAND marketpipe_unit_tests)
  add_test(NAME marketpipe_integration COMMAND marketpipe_integration_tests)
  add_test(NAME marketpipe_benchmark_100k
    COMMAND marketpipe benchmark --events 100000 --mode all
            --output-dir ${CMAKE_BINARY_DIR}/benchmark-data)
  set_tests_properties(marketpipe_integration PROPERTIES TIMEOUT 180)
  set_tests_properties(marketpipe_benchmark_100k PROPERTIES TIMEOUT 300)
endif()

install(TARGETS marketpipe RUNTIME DESTINATION bin)
