# Unit tests for pure logic. They link only libobs (obs_data / util), never
# call obs_startup, and use no frontend API or Qt.
set(SMULTI_DEPS_DIR "${CMAKE_SOURCE_DIR}/.deps")

find_library(SMULTI_OBS_LIB obs PATHS "${SMULTI_DEPS_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
find_file(SMULTI_OBS_DLL obs.dll PATHS "${SMULTI_DEPS_DIR}/bin/64bit" NO_DEFAULT_PATH REQUIRED)

# Test case files. Later tasks append their new file here only; the executable
# and the per-case ctest registration below both consume this list.
set(SMULTI_TEST_SOURCES
    test_endpoint_roundtrip.cpp
    test_endpoint_defaults.cpp
    test_linked_start.cpp
    test_endpoint_validation.cpp
    test_linked_start_duplicates.cpp
    test_canvas_endpoint.cpp
    test_canvas_resolve.cpp
    test_canvas_owned.cpp
    test_layout_math.cpp
    test_scene_link.cpp
    test_canvas_scale_ref.cpp
    test_vertical_record.cpp
    test_streamlabs_tiktok.cpp
)

add_executable(
  streammulticast_tests
  main.cpp
  ${SMULTI_TEST_SOURCES}
  ${CMAKE_SOURCE_DIR}/src/core/Endpoint.cpp
  ${CMAKE_SOURCE_DIR}/src/core/LinkedStart.cpp
  ${CMAKE_SOURCE_DIR}/src/core/EndpointValidation.cpp
  ${CMAKE_SOURCE_DIR}/src/core/CanvasMigration.cpp
  ${CMAKE_SOURCE_DIR}/src/core/CanvasResolve.cpp
  ${CMAKE_SOURCE_DIR}/src/core/OwnedCanvasPolicy.cpp
  ${CMAKE_SOURCE_DIR}/src/core/LayoutMath.cpp
  ${CMAKE_SOURCE_DIR}/src/core/SceneLink.cpp
  ${CMAKE_SOURCE_DIR}/src/core/CanvasScaleRef.cpp
  ${CMAKE_SOURCE_DIR}/src/core/VerticalRecord.cpp
  ${CMAKE_SOURCE_DIR}/src/core/StreamlabsTikTok.cpp
)
target_compile_features(streammulticast_tests PRIVATE cxx_std_17)
target_include_directories(
  streammulticast_tests
  PRIVATE "${CMAKE_SOURCE_DIR}/src" "${SMULTI_DEPS_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(streammulticast_tests PRIVATE "${SMULTI_OBS_LIB}")
# Fake-token fixtures only; tests never read a real generator config.
target_compile_definitions(streammulticast_tests PRIVATE SMULTI_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

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

# One ctest test per SMULTI_TEST case so `ctest -R canvas_` selects cases.
# Case names are parsed from the sources at configure time; touching a test
# source re-runs configure, so a new case is registered automatically.
set(SMULTI_ENV "PATH=path_list_prepend:${SMULTI_OBS_DLL_DIR};PATH=path_list_prepend:${SMULTI_OBSDEPS_BIN}")
foreach(SMULTI_SRC IN LISTS SMULTI_TEST_SOURCES)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${SMULTI_SRC}")
  file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/${SMULTI_SRC}" SMULTI_CASE_LINES REGEX "^SMULTI_TEST\\([a-z0-9_]+\\)")
  foreach(SMULTI_LINE IN LISTS SMULTI_CASE_LINES)
    string(REGEX REPLACE "^SMULTI_TEST\\(([a-z0-9_]+)\\).*$" "\\1" SMULTI_CASE "${SMULTI_LINE}")
    add_test(NAME ${SMULTI_CASE} COMMAND streammulticast_tests --run ${SMULTI_CASE})
    set_tests_properties(${SMULTI_CASE} PROPERTIES ENVIRONMENT_MODIFICATION "${SMULTI_ENV}")
  endforeach()
endforeach()

# Harness self-check: an unknown case name must be reported, never pass silently.
add_test(NAME harness_unknown_case_fails COMMAND streammulticast_tests --run no_such_case)
set_tests_properties(
  harness_unknown_case_fails
  PROPERTIES ENVIRONMENT_MODIFICATION "${SMULTI_ENV}" PASS_REGULAR_EXPRESSION "unknown case: no_such_case"
)
