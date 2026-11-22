# lean-bsp-pico2: one pico-sdk firmware made of this BSP's board support and one lean-bsp application.
#
# bsp_cli.py configures it (`lbsp build`); not meant to be configured by hand. It passes
#   LEAN_BSP_APP_CMAKE   the file it wrote with the application: LEAN_BSP_APP_NAME, _SOURCES,
#                        _INCLUDE_DIRS, _DEFINES and _LINKER_SCRIPT
#   LEAN_BSP_OUTPUT_DIR  where the firmware goes: the lean-bsp build directory
# and the pico-sdk settings: PICO_SDK_PATH, PICO_PLATFORM (the core), PICO_TOOLCHAIN_PATH,
# PICO_GCC_TRIPLE, and PICO_NO_PICOTOOL, since lean-hwbridge loads the ELF itself (no UF2 needed).
cmake_minimum_required(VERSION 3.13...3.31)

if(NOT LEAN_BSP_APP_CMAKE)
    message(FATAL_ERROR "LEAN_BSP_APP_CMAKE is not set: lean-bsp's `build` configures this project")
endif()
include(${LEAN_BSP_APP_CMAKE})

set(PICO_BOARD pico2 CACHE STRING "Board type")
include(pico_sdk_import.cmake)
project(lean_bsp_pico2 C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

# The application, with only its own include directories and defines, and the contract header
add_library(lean_app OBJECT ${LEAN_BSP_APP_SOURCES})
target_include_directories(lean_app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/include ${LEAN_BSP_APP_INCLUDE_DIRS})
target_compile_definitions(lean_app PRIVATE ${LEAN_BSP_APP_DEFINES})

add_executable(firmware src/bsp_pico2.c src/bsp_nvm.c $<TARGET_OBJECTS:lean_app>)
set_target_properties(firmware PROPERTIES OUTPUT_NAME ${LEAN_BSP_APP_NAME} RUNTIME_OUTPUT_DIRECTORY ${LEAN_BSP_OUTPUT_DIR})
target_include_directories(firmware PRIVATE ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(firmware PRIVATE pico_stdlib hardware_watchdog hardware_xip_cache hardware_flash)
pico_set_program_name(firmware ${LEAN_BSP_APP_NAME})
pico_enable_stdio_usb(firmware 1)
pico_enable_stdio_uart(firmware 0)
# linker/default.ld: pico-sdk's memmap_default.ld plus NVM region 0. An application's own script ends
# with `INCLUDE default.ld`, found through the search path below, as is pico-sdk's script.
if(LEAN_BSP_APP_LINKER_SCRIPT)
    pico_set_linker_script(firmware ${LEAN_BSP_APP_LINKER_SCRIPT})
else()
    pico_set_linker_script(firmware ${CMAKE_CURRENT_LIST_DIR}/linker/default.ld)
endif()
target_link_options(firmware PRIVATE -L${CMAKE_CURRENT_LIST_DIR}/linker
                    -L${PICO_SDK_PATH}/src/rp2_common/pico_crt0/${PICO_CHIP})
pico_add_extra_outputs(firmware)
