cmake_minimum_required(VERSION 3.16)

project(BasicTelemetry VERSION 1.0.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(BASICTELEMETRY_BUILD_TESTS "Build BasicTelemetry tests" ON)
option(BASICTELEMETRY_BUILD_TOOLS "Build the basic-telemetry command-line tool" ON)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(Tracy CONFIG REQUIRED)

add_library(BasicTelemetryCore STATIC
    src/Telemetry.cpp
)
add_library(BasicTelemetry::Core ALIAS BasicTelemetryCore)
set_target_properties(BasicTelemetryCore PROPERTIES EXPORT_NAME Core)
execute_process(
    COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE BASICTELEMETRY_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT BASICTELEMETRY_GIT_COMMIT)
    set(BASICTELEMETRY_GIT_COMMIT "unknown")
endif()
target_compile_definitions(BasicTelemetryCore
    PRIVATE BASICTELEMETRY_GIT_COMMIT="${BASICTELEMETRY_GIT_COMMIT}")
target_compile_features(BasicTelemetryCore PUBLIC cxx_std_23)
target_include_directories(BasicTelemetryCore
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

add_library(BasicTelemetryTracy INTERFACE)
add_library(BasicTelemetry::Tracy ALIAS BasicTelemetryTracy)
set_target_properties(BasicTelemetryTracy PROPERTIES EXPORT_NAME Tracy)
target_link_libraries(BasicTelemetryTracy
    INTERFACE
        BasicTelemetryCore
        Tracy::TracyClient
)
target_include_directories(BasicTelemetryTracy
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

add_library(BasicTelemetryArtifacts STATIC
    src/Artifacts.cpp
)
add_library(BasicTelemetry::Artifacts ALIAS BasicTelemetryArtifacts)
set_target_properties(BasicTelemetryArtifacts PROPERTIES EXPORT_NAME Artifacts)
target_compile_features(BasicTelemetryArtifacts PUBLIC cxx_std_23)
target_link_libraries(BasicTelemetryArtifacts
    PUBLIC
        BasicTelemetryCore
    PRIVATE
        nlohmann_json::nlohmann_json
        unofficial::sqlite3::sqlite3
)
target_include_directories(BasicTelemetryArtifacts
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

if(BASICTELEMETRY_BUILD_TOOLS)
    add_executable(basic-telemetry tools/basic-telemetry.cpp)
    target_compile_features(basic-telemetry PRIVATE cxx_std_23)
    target_link_libraries(basic-telemetry
        PRIVATE
            BasicTelemetryArtifacts
            nlohmann_json::nlohmann_json
            unofficial::sqlite3::sqlite3
    )
endif()

if(BASICTELEMETRY_BUILD_TESTS)
    enable_testing()
    add_executable(BasicTelemetryTests tests/TelemetryTests.cpp)
    target_compile_features(BasicTelemetryTests PRIVATE cxx_std_23)
    target_link_libraries(BasicTelemetryTests PRIVATE BasicTelemetryArtifacts)
    add_test(NAME BasicTelemetryTests COMMAND BasicTelemetryTests)
    add_executable(BasicTelemetryBenchmark tests/TelemetryBenchmark.cpp)
    target_compile_features(BasicTelemetryBenchmark PRIVATE cxx_std_23)
    target_link_libraries(BasicTelemetryBenchmark PRIVATE BasicTelemetryCore)
endif()

install(TARGETS
    BasicTelemetryCore
    BasicTelemetryTracy
    BasicTelemetryArtifacts
    EXPORT BasicTelemetryTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
if(BASICTELEMETRY_BUILD_TOOLS)
    install(TARGETS basic-telemetry RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT BasicTelemetryTargets
    FILE BasicTelemetryTargets.cmake
    NAMESPACE BasicTelemetry::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/BasicTelemetry
)

configure_package_config_file(
    cmake/BasicTelemetryConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/BasicTelemetryConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/BasicTelemetry
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/BasicTelemetryConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/BasicTelemetryConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/BasicTelemetryConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/BasicTelemetry
)
