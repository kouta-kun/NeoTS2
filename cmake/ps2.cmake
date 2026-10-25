# Toolchain file for the PlayStation 2's Emotion Engine, from the ps2dev toolchain ($PS2DEV and $PS2SDK must be set):
#
#   cmake -S . -B build-ps2 -DCMAKE_TOOLCHAIN_FILE=cmake/ps2.cmake -DNEOTS2_CORE_ONLY=ON
#
# See docs/ps2-port-plan.md for what the toolchain's standard library lacks.
include($ENV{PS2SDK}/ps2dev.cmake)

# GCC 15 crashes on <format>'s 128-bit integer path for this target (an internal error in std::__countl_zero<unsigned
# __int128>); without the type, the header works for integers and strings.
set(CMAKE_CXX_FLAGS_INIT "${CMAKE_CXX_FLAGS_INIT} -U__SIZEOF_INT128__")

# The standard library has no floating-point formatting for std::format either; this header supplies the little the
# engine uses, in every source file (it does nothing below C++20, so CMake's own test of the compiler is not affected).
get_filename_component(NEOTS2_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(CMAKE_CXX_FLAGS_INIT "${CMAKE_CXX_FLAGS_INIT} -include ${NEOTS2_ROOT}/src/platform/ps2/float_format.hpp")
