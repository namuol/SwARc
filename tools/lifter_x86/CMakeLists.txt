# recomp_lift: x86 -> C++ lifter (build-time only, never linked into a game).

set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zydis
  GIT_REPOSITORY https://github.com/zyantific/zydis.git
  GIT_TAG a2278f1d254e492f6a6b39f6cb5d1f5d515659dc  # v4.1.1
  GIT_SUBMODULES dependencies/zycore
  GIT_SHALLOW FALSE)
FetchContent_MakeAvailable(zydis)

# The lifter's objects, also linked with a test-only image reader into
# recomp_lift_fixture (tests/x86).
add_library(recomp_lift_objs OBJECT
  main.cpp
  program.cpp
  function.cpp
  emit.cpp
  x87test.cpp)
target_include_directories(recomp_lift_objs PUBLIC ${RECOMP_ROOT}/src
                           ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(recomp_lift_objs PUBLIC recomp_base Zydis)

add_executable(recomp_lift)
target_link_libraries(recomp_lift PRIVATE recomp_lift_objs)
