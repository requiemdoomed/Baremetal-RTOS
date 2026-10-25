# BareMetal-RTOS
# Copyright (c) 2026 Swayam Shakya
# SPDX-License-Identifier: MIT
#
# Two build flavours from one tree:
#   * Firmware (Cortex-M3): cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
#   * Host unit tests:      cmake -B build-host
cmake_minimum_required(VERSION 3.16)
project(BareMetalRTOS VERSION 1.1.0 LANGUAGES C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# Keep absolute build paths out of __FILE__ in assertion messages.
add_compile_options(-fmacro-prefix-map=${CMAKE_SOURCE_DIR}/=)

set(WARNINGS -Wall -Wextra -Wshadow -Wdouble-promotion -Wundef -Werror)

enable_testing()

if(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm")
  # ------------------------------------------------------------- firmware
  set(CPU_FLAGS -mcpu=cortex-m3 -mthumb)
  find_program(QEMU_ARM qemu-system-arm)

  add_library(rtos_kernel STATIC
    kernel/src/sched.c
    kernel/src/sync.c
    port/cortex-m3/port.c
    port/cortex-m3/context_switch.S)
  target_include_directories(rtos_kernel PUBLIC kernel/include port/cortex-m3)
  target_compile_options(rtos_kernel PUBLIC ${CPU_FLAGS} -ffunction-sections -fdata-sections
    $<$<COMPILE_LANGUAGE:C,CXX>:${WARNINGS}>
    $<$<COMPILE_LANGUAGE:C>:-Wstrict-prototypes -Wmissing-prototypes>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit>)
  target_link_options(rtos_kernel PUBLIC ${CPU_FLAGS})

  add_library(board_lm3s6965 STATIC
    board/lm3s6965/board.c
    board/lm3s6965/startup.c)
  target_include_directories(board_lm3s6965 PUBLIC board/lm3s6965)
  target_link_libraries(board_lm3s6965 PUBLIC rtos_kernel)

  function(add_firmware name)
    add_executable(${name} ${ARGN})
    set_target_properties(${name} PROPERTIES SUFFIX ".elf")
    target_link_libraries(${name} PRIVATE board_lm3s6965 rtos_kernel)
    target_link_options(${name} PRIVATE
      -T${CMAKE_SOURCE_DIR}/board/lm3s6965/lm3s6965.ld
      -nostartfiles --specs=nano.specs --specs=nosys.specs
      -Wl,--gc-sections -Wl,-Map=${name}.map -Wl,--print-memory-usage)
    add_custom_command(TARGET ${name} POST_BUILD
      COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${name}> ${name}.bin
      COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${name}>)
  endfunction()

  add_firmware(telemetry_demo
    app/telemetry/main.cpp
    app/telemetry/sensor.cpp
    app/telemetry/pipeline.cpp)
  target_include_directories(telemetry_demo PRIVATE app/telemetry)

  add_firmware(kernel_tests tests/target/test_kernel.c)

  if(QEMU_ARM)
    # Real-time mode: emulated clocks follow the host clock (exposes host jitter).
    set(QEMU_CMD ${QEMU_ARM} -M lm3s6965evb -nographic -semihosting -monitor none -serial stdio)
    # Deterministic mode: virtual time advances 64 ns per instruction, so every run is
    # cycle-for-cycle reproducible regardless of host load.
    set(QEMU_CMD_DET ${QEMU_CMD} -icount shift=6,align=off,sleep=on)

    # Kernel tests in both modes. The real-time run catches ordering assumptions that only
    # break when ticks arrive in bursts on a busy host.
    foreach(mode det rt)
      if(mode STREQUAL "det")
        set(cmd ${QEMU_CMD_DET})
      else()
        set(cmd ${QEMU_CMD})
      endif()
      add_test(NAME kernel_tests_qemu_${mode} COMMAND ${cmd} -kernel $<TARGET_FILE:kernel_tests>)
      set_tests_properties(kernel_tests_qemu_${mode} PROPERTIES TIMEOUT 120
        PASS_REGULAR_EXPRESSION "RESULT: ALL TESTS PASSED"
        FAIL_REGULAR_EXPRESSION "FAIL|PANIC|FAULT|UNHANDLED")
    endforeach()
    # The demo checks the sensor sample rate exactly, which is only meaningful with a
    # deterministic clock: a starved host makes QEMU coalesce periodic timer events.
    add_test(NAME telemetry_demo_qemu COMMAND ${QEMU_CMD_DET} -kernel $<TARGET_FILE:telemetry_demo>)
    set_tests_properties(telemetry_demo_qemu PROPERTIES TIMEOUT 120
      PASS_REGULAR_EXPRESSION "DEMO RESULT: PASS")
    add_custom_target(run_demo COMMAND ${QEMU_CMD_DET} -kernel $<TARGET_FILE:telemetry_demo>
      DEPENDS telemetry_demo USES_TERMINAL)
    add_custom_target(run_tests COMMAND ${QEMU_CMD_DET} -kernel $<TARGET_FILE:kernel_tests>
      DEPENDS kernel_tests USES_TERMINAL)
  endif()
else()
  # ------------------------------------------------------------ host tests
  find_package(Threads REQUIRED)
  add_executable(host_ringbuf_tests tests/host/test_ringbuf.cpp)
  target_include_directories(host_ringbuf_tests PRIVATE kernel/include)
  target_compile_options(host_ringbuf_tests PRIVATE ${WARNINGS})
  target_link_libraries(host_ringbuf_tests PRIVATE Threads::Threads)
  add_test(NAME host_ringbuf_tests COMMAND host_ringbuf_tests)
endif()
