# Toolchain file for the GNU Arm Embedded toolchain (arm-none-eabi-gcc).
#
#   cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
#
# Set ARM_TOOLCHAIN_DIR (env or -D) if arm-none-eabi-gcc is not on PATH,
# e.g. -DARM_TOOLCHAIN_DIR=/opt/arm-gnu-toolchain-13.2/bin
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR cortex-m7)

if(NOT ARM_TOOLCHAIN_DIR AND DEFINED ENV{ARM_TOOLCHAIN_DIR})
    set(ARM_TOOLCHAIN_DIR "$ENV{ARM_TOOLCHAIN_DIR}")
endif()
if(ARM_TOOLCHAIN_DIR)
    set(_p "${ARM_TOOLCHAIN_DIR}/")
else()
    set(_p "")
endif()

set(CMAKE_C_COMPILER   ${_p}arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER ${_p}arm-none-eabi-g++)
set(CMAKE_ASM_COMPILER ${_p}arm-none-eabi-gcc)
set(CMAKE_OBJCOPY      ${_p}arm-none-eabi-objcopy CACHE FILEPATH "objcopy")
set(CMAKE_SIZE         ${_p}arm-none-eabi-size    CACHE FILEPATH "size")

# Compiler checks must not try to link a hosted executable.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
