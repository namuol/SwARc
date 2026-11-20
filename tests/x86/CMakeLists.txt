# Dev-only tests of the x86 core; nothing here ships. The lifting oracle
# tests run on the fixture below; the game projects add them on their EXEs
# too (recomp_add_lift_tests).

# x87_test runs the host CPU's own x87 (inline asm) next to the model from
# the same state and compares the results, so it can only exist on x86-64.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  add_executable(x87_test x87_test.cpp)
  target_link_libraries(x87_test PRIVATE recomp_x87)
  add_test(NAME x87 COMMAND x87_test)
  set_tests_properties(x87 PROPERTIES TIMEOUT 300)
endif()

# x87_fast_test checks the model's integer fast paths against its own
# SoftFloat paths; portable.
add_executable(x87_fast_test x87_fast_test.cpp)
target_link_libraries(x87_fast_test PRIVATE recomp_x87)
target_compile_options(x87_fast_test PRIVATE -O2 ${RECOMP_X87_FP_FLAGS})
add_test(NAME x87_fast COMMAND x87_fast_test)
set_tests_properties(x87_fast PROPERTIES TIMEOUT 300)
# The same built with each planted bug of the fast paths
# (RECOMP_X86_MUTATION, x86/x87.h), which it must catch (it exits 0 only
# then); each builds its own x87.cpp.
foreach(_m RANGE 1 17)
  add_executable(x87_fast_mutation_${_m} x87_fast_test.cpp "${RECOMP_ROOT}/src/x86/x87.cpp")
  target_include_directories(x87_fast_mutation_${_m} PRIVATE "${RECOMP_ROOT}/src")
  target_compile_features(x87_fast_mutation_${_m} PRIVATE cxx_std_20)
  target_compile_definitions(x87_fast_mutation_${_m} PRIVATE RECOMP_X86_MUTATION=${_m})
  target_compile_options(x87_fast_mutation_${_m} PRIVATE -O2 ${RECOMP_X87_FP_FLAGS})
  target_link_libraries(x87_fast_mutation_${_m} PRIVATE softfloat recomp_base)
  add_test(NAME x87_fast_mutation_${_m} COMMAND x87_fast_mutation_${_m} 200000)
endforeach()

# sse_fast_test checks the SSE model's host-arithmetic fast paths
# against its exact SoftFloat paths; portable.
add_executable(sse_fast_test sse_fast_test.cpp)
target_link_libraries(sse_fast_test PRIVATE recomp_x87)
target_compile_options(sse_fast_test PRIVATE -O2 ${RECOMP_X87_FP_FLAGS})
add_test(NAME sse_fast COMMAND sse_fast_test)
set_tests_properties(sse_fast PROPERTIES TIMEOUT 300)
# The variant for games whose code never reads MXCSR's status flags
# (RECOMP_SSE_FLAGS_UNOBSERVED, x86/sse.h), and its planted bug (111: no
# NaN check).
add_executable(sse_fast_noflags_test sse_fast_test.cpp "${RECOMP_ROOT}/src/x86/sse.cpp")
target_include_directories(sse_fast_noflags_test PRIVATE "${RECOMP_ROOT}/src")
target_compile_features(sse_fast_noflags_test PRIVATE cxx_std_20)
target_compile_definitions(sse_fast_noflags_test PRIVATE RECOMP_SSE_FLAGS_UNOBSERVED=1)
target_compile_options(sse_fast_noflags_test PRIVATE -O2 ${RECOMP_X87_FP_FLAGS})
target_link_libraries(sse_fast_noflags_test PRIVATE softfloat recomp_base)
add_test(NAME sse_fast_noflags COMMAND sse_fast_noflags_test)
set_tests_properties(sse_fast_noflags PROPERTIES TIMEOUT 300)
add_executable(sse_fast_noflags_mutation_111 sse_fast_test.cpp "${RECOMP_ROOT}/src/x86/sse.cpp")
target_include_directories(sse_fast_noflags_mutation_111 PRIVATE "${RECOMP_ROOT}/src")
target_compile_features(sse_fast_noflags_mutation_111 PRIVATE cxx_std_20)
target_compile_definitions(sse_fast_noflags_mutation_111 PRIVATE RECOMP_SSE_FLAGS_UNOBSERVED=1
                           RECOMP_X86_MUTATION=111)
target_compile_options(sse_fast_noflags_mutation_111 PRIVATE -O2 ${RECOMP_X87_FP_FLAGS})
target_link_libraries(sse_fast_noflags_mutation_111 PRIVATE softfloat recomp_base)
add_test(NAME sse_fast_noflags_mutation_111 COMMAND sse_fast_noflags_mutation_111 400000)
# The same built with each planted bug of those fast paths
# (RECOMP_X86_MUTATION 101-110, x86/sse.h), which it must catch.
foreach(_m RANGE 101 110)
  add_executable(sse_fast_mutation_${_m} sse_fast_test.cpp "${RECOMP_ROOT}/src/x86/sse.cpp")
  target_include_directories(sse_fast_mutation_${_m} PRIVATE "${RECOMP_ROOT}/src")
  target_compile_features(sse_fast_mutation_${_m} PRIVATE cxx_std_20)
  target_compile_definitions(sse_fast_mutation_${_m} PRIVATE RECOMP_X86_MUTATION=${_m})
  target_compile_options(sse_fast_mutation_${_m} PRIVATE -O2 ${RECOMP_X87_FP_FLAGS})
  target_link_libraries(sse_fast_mutation_${_m} PRIVATE softfloat recomp_base)
  add_test(NAME sse_fast_mutation_${_m} COMMAND sse_fast_mutation_${_m} 400000)
endforeach()

# sse_rsqrt_test: RSQRTPS is the correctly rounded 1/sqrt(x), checked
# exhaustively with exact arithmetic (the host oracle cannot: CPUs differ);
# portable.
add_executable(sse_rsqrt_test sse_rsqrt_test.cpp)
target_link_libraries(sse_rsqrt_test PRIVATE recomp_x87)
target_compile_options(sse_rsqrt_test PRIVATE -O2)
add_test(NAME sse_rsqrt COMMAND sse_rsqrt_test)
set_tests_properties(sse_rsqrt PROPERTIES TIMEOUT 300)

# sse_ftz_test: FTZ and DAZ against the host (x86-64 hosts only).
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  add_executable(sse_ftz_test sse_ftz_test.cpp)
  target_include_directories(sse_ftz_test PRIVATE "${RECOMP_ROOT}/src")
  target_link_libraries(sse_ftz_test PRIVATE recomp_x87)
  target_compile_options(sse_ftz_test PRIVATE -O2)
  add_test(NAME sse_ftz COMMAND sse_ftz_test)
endif()

# hle_abi_test: HLE argument order under the Win64 and System V ABIs.
add_executable(hle_abi_test hle_abi_test.cpp)
target_link_libraries(hle_abi_test PRIVATE recomp_x86)
add_test(NAME hle_abi COMMAND hle_abi_test)

# hle_dup_test: a duplicate HLE registration is fatal (its message passes).
add_executable(hle_dup_test hle_dup_test.cpp)
target_link_libraries(hle_dup_test PRIVATE recomp_x86)
add_test(NAME hle_dup COMMAND hle_dup_test)
set_tests_properties(hle_dup PROPERTIES PASS_REGULAR_EXPRESSION "test.dll!Twice is registered twice")

# runtime_thread_test: guest faults, thread exits and the watchdog (Linux).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(runtime_thread_test runtime_thread_test.cpp)
  target_include_directories(runtime_thread_test PRIVATE ${RECOMP_ROOT}/src)
  target_link_libraries(runtime_thread_test PRIVATE recomp_x86)
  target_compile_options(runtime_thread_test PRIVATE -fnon-call-exceptions)
  add_test(NAME runtime_thread COMMAND runtime_thread_test)
endif()

# fxsave_test: FXSAVE/FXRSTOR of the x87 and SSE models against the host.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  add_executable(fxsave_test fxsave_test.cpp)
  target_link_libraries(fxsave_test PRIVATE recomp_x87)
  add_test(NAME fxsave COMMAND fxsave_test)
endif()

# mmx_test: the MMX helpers (x86/mmx.h) and the x87 state MMX code leaves
# against the host. Under Rosetta 2 (x86_64 macOS build) it checks the
# values only.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  add_executable(mmx_test mmx_test.cpp)
  target_link_libraries(mmx_test PRIVATE recomp_x87)
  add_test(NAME mmx COMMAND mmx_test)
endif()

# lift_semantics_test checks the integer/flag helpers in lifted.h against
# the host CPU the same way.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  add_executable(lift_semantics_test lift_semantics_test.cpp)
  target_include_directories(lift_semantics_test PRIVATE ${RECOMP_ROOT}/src)
  target_compile_features(lift_semantics_test PRIVATE cxx_std_20)
  target_compile_options(lift_semantics_test PRIVATE -O1)
  add_test(NAME lift_semantics COMMAND lift_semantics_test)
  set_tests_properties(lift_semantics PROPERTIES TIMEOUT 300)
endif()

# lift_isa64_int, lift_isa64_sse: lift_x64_test.cpp on the forms in
# x64_isa/forms.txt (recomp_lift --forms): the x64 CPU model's extensions
# beyond what the x64 test images cover. They run natively too, so the
# host needs those extensions (BMI1, ABM, MOVBE, SSE4.2, AES, PCLMULQDQ,
# F16C, AVX), as every AMD family 16h or later and Intel Haswell or later
# has.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$" AND TARGET recomp_lift)
  foreach(_kind int sse)
    set(_forms "${CMAKE_CURRENT_BINARY_DIR}/lift_isa64_${_kind}_forms.cpp")
    add_custom_command(OUTPUT "${_forms}"
      COMMAND recomp_lift --forms "${CMAKE_CURRENT_SOURCE_DIR}/x64_isa/forms.txt" --x64
              --${_kind}-test "${_forms}"
      DEPENDS recomp_lift "${CMAKE_CURRENT_SOURCE_DIR}/x64_isa/forms.txt"
      COMMENT "Lifting the x64 extension forms for lift_isa64_${_kind}_test")
    add_executable(lift_isa64_${_kind}_test lift_x64_test.cpp "${_forms}")
    target_include_directories(lift_isa64_${_kind}_test PRIVATE ${RECOMP_ROOT}/src
                               ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(lift_isa64_${_kind}_test PRIVATE recomp_x87 recomp_base)
    target_compile_options(lift_isa64_${_kind}_test PRIVATE -O1 -fno-strict-aliasing
                           ${RECOMP_X87_FP_FLAGS})
    if(_kind STREQUAL "sse")
      target_compile_definitions(lift_isa64_${_kind}_test PRIVATE LIFT_X64_SSE)
    endif()
    add_test(NAME lift_isa64_${_kind} COMMAND lift_isa64_${_kind}_test)
    set_tests_properties(lift_isa64_${_kind} PROPERTIES TIMEOUT 1800)
  endforeach()
endif()

# lift_discovery: recomp_lift's discovery of switch tables and of code
# nothing refers to (--sweep-gaps) on fixture/discovery.S, assembled by
# clang for i686 and not linked (it needs no linker, and runs on any
# host): recomp_lift_fixture is the lifter with a reader for that object
# (fixture_reader.cpp), lift_discovery_test runs it and checks what it
# found against the fixture's expectations.
if(TARGET recomp_lift)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    set(_fix "${CMAKE_CURRENT_BINARY_DIR}/fixture")
    add_custom_command(OUTPUT "${_fix}/discovery.o"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${_fix}"
      COMMAND "${CMAKE_C_COMPILER}" --target=i686-unknown-linux-gnu -c
              "${CMAKE_CURRENT_SOURCE_DIR}/fixture/discovery.S" -o "${_fix}/discovery.o"
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/fixture/discovery.S"
      COMMENT "Assembling the x86 lifter fixture discovery.S")
    add_custom_target(lift_discovery_fixture ALL DEPENDS "${_fix}/discovery.o")
    add_executable(recomp_lift_fixture fixture_reader.cpp)
    target_link_libraries(recomp_lift_fixture PRIVATE recomp_lift_objs)
    add_executable(lift_discovery_test lift_discovery_test.cpp)
    target_compile_features(lift_discovery_test PRIVATE cxx_std_20)
    add_dependencies(lift_discovery_test recomp_lift_fixture lift_discovery_fixture)
    add_test(NAME lift_discovery
             COMMAND lift_discovery_test $<TARGET_FILE:recomp_lift_fixture> "${_fix}/discovery.o"
                     "${CMAKE_CURRENT_BINARY_DIR}/lift_discovery")
  elseif(NOT RECOMP_ALLOW_MISSING_TEST_TOOLS)
    add_test(NAME x86_discovery_prerequisites COMMAND "${CMAKE_COMMAND}" -E echo
             "lift_discovery needs clang as the C compiler (RECOMP_ALLOW_MISSING_TEST_TOOLS=ON leaves it out)")
    set_tests_properties(x86_discovery_prerequisites PROPERTIES FAIL_REGULAR_EXPRESSION .)
  endif()
endif()

# lift_fixture_int, lift_fixture_sse, lift_fixture_x87: lift_int_test.cpp,
# lift_sse_test.cpp and lift_x87_test.cpp on the forms of fixture/forms.s,
# assembled by clang for i686 and linked by LLD to flat code, which
# recomp_lift --code takes its forms from; lift_fixture_int64,
# lift_fixture_sse64 and lift_fixture_x87_64: lift_x64_test.cpp and
# lift_x87_test.cpp on those of fixture/forms64.s (x86-64; the SSE test
# needs an AVX host, as lift_isa64_sse). Without those tools
# x86_prerequisites fails, naming them, unless
# RECOMP_ALLOW_MISSING_TEST_TOOLS says this host cannot run these tests.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$" AND TARGET recomp_lift)
  find_program(RECOMP_LD_LLD ld.lld)
  set(_missing)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    list(APPEND _missing "clang as the C compiler")
  endif()
  if(NOT RECOMP_LD_LLD)
    list(APPEND _missing ld.lld)
  endif()
  if(_missing)
    string(JOIN ", " _missing ${_missing})
    message(STATUS "x86: missing ${_missing}; the lifter fixture's tests are left out")
    if(NOT RECOMP_ALLOW_MISSING_TEST_TOOLS)
      add_test(NAME x86_prerequisites COMMAND "${CMAKE_COMMAND}" -E echo
               "missing ${_missing} (RECOMP_ALLOW_MISSING_TEST_TOOLS=ON leaves their tests out)")
      set_tests_properties(x86_prerequisites PROPERTIES FAIL_REGULAR_EXPRESSION .)
    endif()
  else()
    set(_fix "${CMAKE_CURRENT_BINARY_DIR}/fixture")
    foreach(_f forms:i686 forms64:x86_64)
      string(REPLACE ":" ";" _f ${_f})
      list(GET _f 0 _name)
      list(GET _f 1 _arch)
      add_custom_command(OUTPUT "${_fix}/${_name}.bin"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_fix}"
        COMMAND "${CMAKE_C_COMPILER}" --target=${_arch}-unknown-linux-gnu -c
                "${CMAKE_CURRENT_SOURCE_DIR}/fixture/${_name}.s" -o "${_fix}/${_name}.o"
        COMMAND "${RECOMP_LD_LLD}" --oformat=binary -o "${_fix}/${_name}.bin" "${_fix}/${_name}.o"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/fixture/${_name}.s"
        COMMENT "Assembling the x86 lifter fixture ${_name}.s")
    endforeach()
    foreach(_kind x87 int sse)
      set(_forms "${_fix}/lift_${_kind}_forms.cpp")
      add_custom_command(OUTPUT "${_forms}"
        COMMAND recomp_lift --code "${_fix}/forms.bin" --${_kind}-test "${_forms}"
        DEPENDS recomp_lift "${_fix}/forms.bin"
        COMMENT "Lifting the fixture's ${_kind} instruction forms for lift_fixture_${_kind}_test")
      add_executable(lift_fixture_${_kind}_test lift_${_kind}_test.cpp "${_forms}")
      target_include_directories(lift_fixture_${_kind}_test PRIVATE ${RECOMP_ROOT}/src
                                 ${CMAKE_CURRENT_SOURCE_DIR})
      target_link_libraries(lift_fixture_${_kind}_test PRIVATE recomp_x87 recomp_base)
      target_compile_options(lift_fixture_${_kind}_test PRIVATE -O1 -fno-strict-aliasing
                             ${RECOMP_X87_FP_FLAGS})
      add_test(NAME lift_fixture_${_kind} COMMAND lift_fixture_${_kind}_test)
      set_tests_properties(lift_fixture_${_kind} PROPERTIES TIMEOUT 900)
    endforeach()
    foreach(_kind int sse)
      set(_forms "${_fix}/lift_${_kind}64_forms.cpp")
      add_custom_command(OUTPUT "${_forms}"
        COMMAND recomp_lift --code "${_fix}/forms64.bin" --x64 --${_kind}-test "${_forms}"
        DEPENDS recomp_lift "${_fix}/forms64.bin"
        COMMENT "Lifting the fixture's x64 ${_kind} instruction forms for lift_fixture_${_kind}64_test")
      add_executable(lift_fixture_${_kind}64_test lift_x64_test.cpp "${_forms}")
      target_include_directories(lift_fixture_${_kind}64_test PRIVATE ${RECOMP_ROOT}/src
                                 ${CMAKE_CURRENT_SOURCE_DIR})
      target_link_libraries(lift_fixture_${_kind}64_test PRIVATE recomp_x87 recomp_base)
      target_compile_options(lift_fixture_${_kind}64_test PRIVATE -O1 -fno-strict-aliasing
                             ${RECOMP_X87_FP_FLAGS})
      if(_kind STREQUAL "sse")
        target_compile_definitions(lift_fixture_${_kind}64_test PRIVATE LIFT_X64_SSE)
      endif()
      add_test(NAME lift_fixture_${_kind}64 COMMAND lift_fixture_${_kind}64_test)
      set_tests_properties(lift_fixture_${_kind}64 PROPERTIES TIMEOUT 900)
    endforeach()
    set(_forms "${_fix}/lift_x87_64_forms.cpp")
    add_custom_command(OUTPUT "${_forms}"
      COMMAND recomp_lift --code "${_fix}/forms64.bin" --x64 --x87-test "${_forms}"
      DEPENDS recomp_lift "${_fix}/forms64.bin"
      COMMENT "Lifting the fixture's x64 x87 instruction forms for lift_fixture_x87_64_test")
    add_executable(lift_fixture_x87_64_test lift_x87_test.cpp "${_forms}")
    target_include_directories(lift_fixture_x87_64_test PRIVATE ${RECOMP_ROOT}/src
                               ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(lift_fixture_x87_64_test PRIVATE recomp_x87 recomp_base)
    target_compile_options(lift_fixture_x87_64_test PRIVATE -O1 -fno-strict-aliasing
                           ${RECOMP_X87_FP_FLAGS})
    target_compile_definitions(lift_fixture_x87_64_test PRIVATE LIFT_X87_X64)
    add_test(NAME lift_fixture_x87_64 COMMAND lift_fixture_x87_64_test)
    set_tests_properties(lift_fixture_x87_64 PROPERTIES TIMEOUT 900)
  endif()
endif()
