# Dev-only tests of the Win32 platform; nothing here ships.

# win32_text_test: UTF-8 <-> UTF-16 conversion (win32/win32.h);
# portable.
add_executable(win32_text_test win32_text_test.cpp)
target_include_directories(win32_text_test PRIVATE ${RECOMP_ROOT}/src ../src)
target_compile_features(win32_text_test PRIVATE cxx_std_20)
add_test(NAME win32_text COMMAND win32_text_test)

# pe_test: the PE loader's bounds checks and the x64 unwinder's epilog
# decoding (recomp_pe); portable.
add_executable(pe_test pe_test.cpp)
target_link_libraries(pe_test PRIVATE recomp_pe)
add_test(NAME pe COMMAND pe_test)

# d3dx_effect_test: compiled D3DX effects as data (win32/d3dx9_effect_format.h):
# loading, constant tables, register layout and preshaders, on effects
# built by the test; portable.
add_executable(d3dx_effect_test d3dx_effect_test.cpp ../src/win32/d3dx9_effect_format.cpp
               ${RECOMP_ROOT}/src/runtime/log.cpp)
target_include_directories(d3dx_effect_test PRIVATE ${RECOMP_ROOT}/src ../src)
target_compile_features(d3dx_effect_test PRIVATE cxx_std_20)
add_test(NAME d3dx_effect COMMAND d3dx_effect_test)
