# Export all symbols in the DLL so we don't have to use __declspec in the code
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

#
# Includes and Sources
#
include_directories("include")
include_directories(
    spdlog::spdlog
)
file(GLOB_RECURSE HEADERS CONFIGURE_DEPENDS include/*.hpp)
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)

#
# Library
#
set(LIBRARY_NAME "ASMutagenWrapper")
add_library(${LIBRARY_NAME} SHARED ${SOURCES} ${HEADERS})
target_compile_options(${LIBRARY_NAME} PRIVATE /utf-8)

#
# VCPKG Dependencies
#
find_package(spdlog REQUIRED CONFIG)
find_package(flatbuffers REQUIRED)

target_link_libraries(${LIBRARY_NAME}
    PRIVATE
        flatbuffers::flatbuffers
        spdlog::spdlog
)

# Define paths
set(ASMUTAGENNE_BUILD_DIR "${CMAKE_BINARY_DIR}/ASMutagenNE")
set(ASMUTAGENNE_BIN_DIR "${ASMUTAGENNE_BUILD_DIR}/bin")
set(ASMUTAGENNE_OBJ_DIR "${ASMUTAGENNE_BUILD_DIR}/obj")
set(ASMUTAGENNE_NUGET_DIR "${ASMUTAGENNE_BUILD_DIR}/.nuget/packages")

set(GENERATED_CSPROJ "${CMAKE_CURRENT_SOURCE_DIR}/ASMutagen.csproj")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/ASMutagen.csproj.in"
    "${GENERATED_CSPROJ}"
    @ONLY
)

set(ASMUTAGEN_LIB
    ${ASMUTAGENNE_BIN_DIR}/ASMutagenNE.lib
)
target_link_libraries(${LIBRARY_NAME}
    PUBLIC
        "${ASMUTAGEN_LIB}"
)
target_include_directories(${LIBRARY_NAME} PUBLIC include)

#
# C# Project Build
#

# Flatbuffers first
find_program(FLATC_EXECUTABLE flatc)
if(NOT FLATC_EXECUTABLE)
    message(FATAL_ERROR "flatc not found! Please install FlatBuffers compiler and add to your path.")
endif()

set(FLATBUFFERS_SCHEMA ${CMAKE_CURRENT_SOURCE_DIR}/ASMutagenBuffers.fbs)
set(GENERATED_FLATBUFFERS_HEADER ${ASMUTAGENNE_OBJ_DIR}/flatbuffers/ASMutagenBuffers_generated.h)

file(MAKE_DIRECTORY "${ASMUTAGENNE_OBJ_DIR}/flatbuffers")

execute_process(
    COMMAND ${FLATC_EXECUTABLE} --cpp --csharp
            -o "${ASMUTAGENNE_OBJ_DIR}/flatbuffers"
            ${FLATBUFFERS_SCHEMA}
    RESULT_VARIABLE _flatc_result
    OUTPUT_VARIABLE _flatc_out
    ERROR_VARIABLE _flatc_err
)

if(NOT _flatc_result EQUAL 0)
    message(FATAL_ERROR "FlatBuffers generation failed:\n${_flatc_err}")
endif()

include_directories(${ASMUTAGENNE_OBJ_DIR}/flatbuffers)

# Nuget restore during configure step
execute_process(
    COMMAND dotnet restore "${GENERATED_CSPROJ}"
        --packages "${ASMUTAGENNE_BUILD_DIR}/.nuget/packages"
        -r win-x64
        /p:BaseIntermediateOutputPath="${ASMUTAGENNE_BUILD_DIR}/obj/"
        /p:BaseOutputPath="${ASMUTAGENNE_BUILD_DIR}/bin/"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE _nuget_restore_result
)

if(NOT _nuget_restore_result EQUAL 0)
    message(FATAL_ERROR "NuGet restore failed for ASMutagen.csproj")
endif()

if (${CMAKE_BUILD_TYPE} STREQUAL "Release" OR ${CMAKE_BUILD_TYPE} STREQUAL "RelWithDebInfo")
    set(MSBUILD_CONFIGURATION "Release")
elseif(${CMAKE_BUILD_TYPE} STREQUAL "Debug")
    set(MSBUILD_CONFIGURATION "Debug")
else()
    set(MSBUILD_CONFIGURATION "Release")
endif()

set(CSHARP_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/ASMutagen.cs
    ${CMAKE_CURRENT_SOURCE_DIR}/ASMutagen.csproj
    ${FLATBUFFERS_SCHEMA}
)

add_custom_command(
    OUTPUT ${ASMUTAGEN_LIB}
    # OutputPath controls where DNNE emits ASMutagenNE.lib (used by the C++ linker at build time).
    # --output (PublishDir) controls where the runtime and managed DLLs land for shipping.
    # Both point to the same directory so all artifacts end up in one place.
    COMMAND dotnet publish "${GENERATED_CSPROJ}"
        -c ${MSBUILD_CONFIGURATION}
        -r win-x64
        --self-contained true
        --no-restore
        -p:Platform=x64
        -p:OutputPath=${ASMUTAGENNE_BIN_DIR}/
        -p:BaseIntermediateOutputPath=${ASMUTAGENNE_OBJ_DIR}/
        --output ${ASMUTAGENNE_BIN_DIR}/
    DEPENDS
        ${CSHARP_SOURCES}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Building ASMutagenNE.lib (self-contained)"
)

add_custom_target(ASMutagenNE
    DEPENDS ${ASMUTAGEN_LIB}
)

add_dependencies(${LIBRARY_NAME} ASMutagenNE)

include_directories(${ASMUTAGENNE_BIN_DIR}/)

#
# C# DLLs Copy
#
#
# Deliberately named AutoSeasons_dotnetlib rather than the generic "dotnetlib" - PGPatcher ships
# its own dotnetlib/ folder full of the same self-contained .NET runtime files (same .NET/Mutagen
# version), so a generic name would collide almost entirely when both tools are installed as MO2
# mods in the same modlist. A unique folder name means no overlapping paths, so no MO2 conflict.
#
add_custom_target(CopyASMutagenDLLs ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin/AutoSeasons_dotnetlib"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${ASMUTAGENNE_BIN_DIR}/" "${CMAKE_BINARY_DIR}/bin/AutoSeasons_dotnetlib"
    COMMENT "Copying ASMutagen DLLs to build/bin/AutoSeasons_dotnetlib"
)

add_dependencies(CopyASMutagenDLLs ASMutagenNE)
add_dependencies(${LIBRARY_NAME} CopyASMutagenDLLs)
