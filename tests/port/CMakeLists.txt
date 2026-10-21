cmake_minimum_required(VERSION 3.20)
project(NXboxPortTests LANGUAGES CXX)

enable_testing()
add_executable(demand-commit-tests demand_commit.cpp)
target_compile_features(demand-commit-tests PRIVATE cxx_std_20)
target_include_directories(demand-commit-tests PRIVATE ../../src)
if(MSVC)
  target_compile_options(demand-commit-tests PRIVATE /W4 /WX)
else()
  target_compile_options(demand-commit-tests PRIVATE -Wall -Wextra -Werror
                                                     -pedantic)
endif()
add_test(NAME demand-commit-policy COMMAND demand-commit-tests)

add_executable(scratch-buffer-tests scratch_buffer.cpp)
target_compile_features(scratch-buffer-tests PRIVATE cxx_std_20)
target_include_directories(scratch-buffer-tests PRIVATE ../../src)
add_test(NAME scratch-buffer-alloc-failure COMMAND scratch-buffer-tests)

if(WIN32)
  add_executable(sparse-memory-tests sparse_memory.cpp ../../src/common/sparse_memory.cpp)
  target_compile_features(sparse-memory-tests PRIVATE cxx_std_20)
  target_include_directories(sparse-memory-tests PRIVATE ../../src)
  target_compile_definitions(sparse-memory-tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  add_test(NAME sparse-memory-integration COMMAND sparse-memory-tests)
  set_tests_properties(sparse-memory-integration PROPERTIES TIMEOUT 30)
endif()

add_executable(update-version-tests update_version.cpp)
target_compile_features(update-version-tests PRIVATE cxx_std_20)
target_include_directories(update-version-tests PRIVATE ../../src)
add_test(NAME update-version-policy COMMAND update-version-tests)

add_executable(protocol-uri-tests protocol_uri.cpp)
target_compile_features(protocol-uri-tests PRIVATE cxx_std_20)
target_include_directories(protocol-uri-tests PRIVATE ../../src)
add_test(NAME protocol-uri-parse COMMAND protocol-uri-tests)

find_package(ZLIB)
if(ZLIB_FOUND)
  add_executable(cheats-parse-tests cheats_parse.cpp)
  target_compile_features(cheats-parse-tests PRIVATE cxx_std_20)
  target_include_directories(cheats-parse-tests PRIVATE ../../src)
  target_link_libraries(cheats-parse-tests PRIVATE ZLIB::ZLIB)
  add_test(NAME cheats-parse COMMAND cheats-parse-tests)
endif()

add_executable(pfs0-extent-tests pfs0_extent.cpp)
target_compile_features(pfs0-extent-tests PRIVATE cxx_std_20)
target_include_directories(pfs0-extent-tests PRIVATE ../../src)
add_test(NAME pfs0-extent-truncation COMMAND pfs0-extent-tests)

add_executable(input-state-tests input_state.cpp)
target_compile_features(input-state-tests PRIVATE cxx_std_17)
target_include_directories(input-state-tests PRIVATE ../../src)
add_test(NAME input-state-transitions COMMAND input-state-tests)

option(NXBOX_BUILD_HLSL_HARNESS "Build the offline Maxwell/HLSL host tool" OFF)
if(NXBOX_BUILD_HLSL_HARNESS)
  add_subdirectory(../../tools/hlsl-harness hlsl-harness)
endif()

add_executable(hlsl-helper-tests hlsl_helpers.cpp)
target_compile_features(hlsl-helper-tests PRIVATE cxx_std_20)
target_include_directories(hlsl-helper-tests PRIVATE ../../src)
add_test(NAME hlsl-root-binding-helpers COMMAND hlsl-helper-tests)

# IR fixtures require Eden's existing fmt/Boost headers, but no GPU or renderer.
find_package(fmt CONFIG QUIET)
find_path(HLSL_BOOST_INCLUDE_DIR boost/container/small_vector.hpp)
if(fmt_FOUND AND HLSL_BOOST_INCLUDE_DIR)
  include(../../src/shader_recompiler/backend/hlsl/CMakeLists.txt)
  add_executable(hlsl-ir-tests hlsl_ir.cpp ${HLSL_BACKEND_SOURCES}
    ../../src/shader_recompiler/frontend/ir/basic_block.cpp
    ../../src/shader_recompiler/frontend/ir/value.cpp
    ../../src/shader_recompiler/frontend/ir/microinstruction.cpp
    ../../src/shader_recompiler/frontend/ir/type.cpp
    ../../src/shader_recompiler/frontend/ir/opcodes.cpp
    ../../src/shader_recompiler/frontend/ir/patch.cpp)
  target_compile_features(hlsl-ir-tests PRIVATE cxx_std_20)
  target_include_directories(hlsl-ir-tests PRIVATE ../../src)
  target_include_directories(hlsl-ir-tests SYSTEM PRIVATE ${HLSL_BOOST_INCLUDE_DIR})
  target_link_libraries(hlsl-ir-tests PRIVATE fmt::fmt)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(hlsl-ir-tests PRIVATE -fbracket-depth=1024)
  endif()
  add_test(NAME hlsl-ir-emission COMMAND hlsl-ir-tests)
  set_tests_properties(hlsl-ir-emission PROPERTIES FIXTURES_SETUP hlsl-emission)
endif()

find_program(HLSL_DXC_EXECUTABLE dxc)
# Exercise the same embedded FidelityFX HLSL entry points as the runtime compiler.
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND AND HLSL_DXC_EXECUTABLE)
  add_test(NAME d3d12-fsr-dxc
    COMMAND ${CMAKE_COMMAND} -E env "DXC=${HLSL_DXC_EXECUTABLE}"
      ${Python3_EXECUTABLE} -B -m unittest discover
      -s ${CMAKE_CURRENT_SOURCE_DIR} -p test_d3d12_fsr.py -k test_hlsl_dxc)
  add_test(NAME d3d12-temporal-frame-generation-dxc
    COMMAND ${CMAKE_COMMAND} -E env "DXC=${HLSL_DXC_EXECUTABLE}"
      ${Python3_EXECUTABLE} -B -m unittest discover
      -s ${CMAKE_CURRENT_SOURCE_DIR} -p test_p59_upscaling.py -k test_shader_entries)
endif()
if(TARGET hlsl-ir-tests AND HLSL_DXC_EXECUTABLE)
  foreach(fixture vertex fragment compute waves resources arithmetic msaa msaa_array msaa_depth msaa_array_depth clip intsample intsample_array global global_windows)
    if(fixture STREQUAL "vertex" OR fixture STREQUAL "clip")
      set(hlsl_target vs_6_0)
    elseif(fixture STREQUAL "global")
      # Global int64 atomics need SM 6.6.
      set(hlsl_target cs_6_6)
    elseif(fixture STREQUAL "fragment" OR fixture STREQUAL "resources" OR fixture MATCHES "^msaa" OR
           fixture MATCHES "^intsample")
      set(hlsl_target ps_6_0)
    else()
      set(hlsl_target cs_6_0)
    endif()
    add_test(NAME hlsl-dxc-${fixture}
      COMMAND ${HLSL_DXC_EXECUTABLE} -T ${hlsl_target} -E main
        ${CMAKE_CURRENT_BINARY_DIR}/hlsl_${fixture}.hlsl
        -Fo ${CMAKE_CURRENT_BINARY_DIR}/hlsl_${fixture}.dxil)
    set_tests_properties(hlsl-dxc-${fixture} PROPERTIES FIXTURES_REQUIRED hlsl-emission)
  endforeach()
endif()

add_executable(ftp-protocol-tests ftp_protocol.cpp)
target_compile_features(ftp-protocol-tests PRIVATE cxx_std_20)
target_include_directories(ftp-protocol-tests PRIVATE ../../src)
add_test(NAME ftp-protocol COMMAND ftp-protocol-tests)

add_executable(library-transfer-tests library_transfer.cpp)
target_compile_features(library-transfer-tests PRIVATE cxx_std_20)
target_include_directories(library-transfer-tests PRIVATE ../../src)
add_test(NAME library-transfer COMMAND library-transfer-tests)
