cmake_minimum_required(VERSION 3.22)
project(resilient_dds_data_plane VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(RDTF_BUILD_TESTS "Build unit tests" ON)
option(RDTF_ENABLE_FASTDDS "Build the Fast DDS transport and apps" OFF)
option(RDTF_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(RDTF_BUILD_BENCHMARK "Build the dependency-free core benchmark" ON)

if(RDTF_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

add_library(resilientdds_core
    src/anomaly_detector.cpp
    src/health_monitor.cpp
    src/metrics.cpp
    src/audit_sink.cpp
    src/qos_profiles.cpp
)

target_include_directories(resilientdds_core PUBLIC include)
target_compile_options(resilientdds_core PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)

add_executable(resilientdds_simulator apps/simulator.cpp)
target_link_libraries(resilientdds_simulator PRIVATE resilientdds_core)

if(RDTF_BUILD_BENCHMARK)
    add_executable(resilientdds_benchmark apps/benchmark_core.cpp)
    target_link_libraries(resilientdds_benchmark PRIVATE resilientdds_core)
endif()

if(RDTF_ENABLE_FASTDDS)
    find_package(fastrtps REQUIRED)
    find_package(fastcdr REQUIRED)
    find_program(FASTDDSGEN fastddsgen REQUIRED)

    set(RDTF_IDL ${CMAKE_CURRENT_SOURCE_DIR}/idl/SystemTelemetry.idl)
    set(RDTF_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
    set(RDTF_GEN_SRC ${RDTF_GEN_DIR}/SystemTelemetry.cxx
                     ${RDTF_GEN_DIR}/SystemTelemetryPubSubTypes.cxx)

    # Generated at build time from the IDL. Checking generated code in lets the
    # model and the wire format drift apart silently.
    add_custom_command(
        OUTPUT ${RDTF_GEN_SRC}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${RDTF_GEN_DIR}
        COMMAND ${FASTDDSGEN} -replace -d ${RDTF_GEN_DIR} ${RDTF_IDL}
        DEPENDS ${RDTF_IDL}
        COMMENT "fastddsgen: SystemTelemetry.idl"
        VERBATIM)

    add_library(resilientdds_dds src/dds_transport.cpp ${RDTF_GEN_SRC})
    target_include_directories(resilientdds_dds PUBLIC include ${RDTF_GEN_DIR})
    target_link_libraries(resilientdds_dds PUBLIC resilientdds_core fastrtps fastcdr)
    target_compile_options(resilientdds_dds PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>)

    add_executable(resilientdds_dds_publisher apps/dds_publisher.cpp)
    target_link_libraries(resilientdds_dds_publisher PRIVATE resilientdds_dds)
    add_executable(resilientdds_dds_subscriber apps/dds_subscriber.cpp)
    target_link_libraries(resilientdds_dds_subscriber PRIVATE resilientdds_dds)
endif()

if(RDTF_BUILD_TESTS)
    enable_testing()
    add_executable(resilientdds_tests tests/test_main.cpp)
    target_link_libraries(resilientdds_tests PRIVATE resilientdds_core)
    add_test(NAME resilientdds_tests COMMAND resilientdds_tests)
endif()
