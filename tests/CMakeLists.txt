# Unit tests for pure logic. They link only libobs (obs_data / util), never
# call obs_startup, and use no frontend API or Qt.
set(SMULTI_DEPS_DIR "${CMAKE_SOURCE_DIR}/.deps")

find_library(SMULTI_OBS_LIB obs PATHS "${SMULTI_DEPS_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
find_file(SMULTI_OBS_DLL obs.dll PATHS "${SMULTI_DEPS_DIR}/bin/64bit" NO_DEFAULT_PATH REQUIRED)

add_executable(
  streammulticast_tests
  main.cpp
  test_endpoint_roundtrip.cpp
  test_endpoint_defaults.cpp
  ${CMAKE_SOURCE_DIR}/src/core/Endpoint.cpp
)
target_compile_features(streammulticast_tests PRIVATE cxx_std_17)
target_include_directories(
  streammulticast_tests
  PRIVATE "${CMAKE_SOURCE_DIR}/src" "${SMULTI_DEPS_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(streammulticast_tests PRIVATE "${SMULTI_OBS_LIB}")

# obs.dll needs FFmpeg/zlib from obs-deps at load time. Resolve both through
# PATH for ctest instead of copying a growing DLL set next to the executable.
get_filename_component(SMULTI_OBS_DLL_DIR "${SMULTI_OBS_DLL}" DIRECTORY)
set(SMULTI_OBSDEPS_BIN "${SMULTI_DEPS_DIR}/obs-deps-2025-07-11-x64/bin")

add_test(NAME streammulticast_tests COMMAND streammulticast_tests)
set_tests_properties(
  streammulticast_tests
  PROPERTIES
    ENVIRONMENT_MODIFICATION
      "PATH=path_list_prepend:${SMULTI_OBS_DLL_DIR};PATH=path_list_prepend:${SMULTI_OBSDEPS_BIN}"
)
