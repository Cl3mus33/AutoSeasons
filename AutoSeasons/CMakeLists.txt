#
# Includes and Sources
#
include_directories("include")
file(GLOB_RECURSE HEADERS CONFIGURE_DEPENDS include/*.hpp)
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)

#
# Executable
#
set(EXE_NAME "AutoSeasons")
add_executable(${EXE_NAME}
    ${SOURCES}
    ${HEADERS}
    ${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.rc
)
target_compile_options(${EXE_NAME} PRIVATE /utf-8)
target_include_directories(${EXE_NAME} PUBLIC include)

# Delay-load ASMutagenWrapper.dll: it eagerly imports ASMutagenNE.dll (the DNNE native-export
# DLL), which only lives under dotnetlib/ - a search path we register from main() via
# AddDllDirectory. Without delay-loading, Windows resolves the whole import graph (including
# ASMutagenNE.dll) before main() ever runs, and the process fails to start.
if(MSVC)
    target_link_options(${EXE_NAME} PRIVATE "/DELAYLOAD:ASMutagenWrapper.dll")
    target_link_libraries(${EXE_NAME} PRIVATE delayimp)
endif()

#
# VCPKG Dependencies
#
find_package(spdlog REQUIRED CONFIG)
find_package(bsa REQUIRED CONFIG)
find_package(Boost REQUIRED COMPONENTS locale)
find_package(directxtex REQUIRED CONFIG)
find_package(nlohmann_json REQUIRED CONFIG)
find_package(cpptrace REQUIRED CONFIG)
find_package(CLI11 REQUIRED CONFIG)
find_package(wxWidgets REQUIRED COMPONENTS core base)

set(D3D_LIBS
    d3d11.lib
    dxgi.lib
    dxguid.lib
    uuid.lib
    kernel32.lib
    user32.lib
    comdlg32.lib
    advapi32.lib
    shell32.lib
    ole32.lib
    oleaut32.lib
    D3DCompiler.lib
)
target_link_libraries(${EXE_NAME} PRIVATE
    spdlog::spdlog
    bsa::bsa
    ${Boost_LIBRARIES}
    nifly
    Microsoft::DirectXTex
    ${D3D_LIBS}
    Shlwapi
    nlohmann_json::nlohmann_json
    cpptrace::cpptrace
    CLI11::CLI11
    ${wxWidgets_LIBRARIES}
    ASMutagenWrapper
)

add_dependencies(${EXE_NAME} ASMutagenWrapper)

#
# Shader Files (needed at runtime by PGD3D for GPU-based texture classification)
#
add_custom_command(TARGET ${EXE_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -rf $<TARGET_FILE_DIR:${EXE_NAME}>/AutoSeasons_cshaders
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${EXE_NAME}>/AutoSeasons_cshaders
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/AutoSeasons_cshaders
        $<TARGET_FILE_DIR:${EXE_NAME}>/AutoSeasons_cshaders
)

#
# Translations (GUI localization - ASLocale reads these from a "translations" folder next to the exe)
#
add_custom_command(TARGET ${EXE_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -rf $<TARGET_FILE_DIR:${EXE_NAME}>/translations
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${EXE_NAME}>/translations
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/translations
        $<TARGET_FILE_DIR:${EXE_NAME}>/translations
)

#
# Mod manager marker file (an empty AS.ini next to the exe, so MO2/Vortex reliably recognize the
# output folder as a mod even though it otherwise ships no game-relevant file types)
#
add_custom_command(TARGET ${EXE_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/AS.ini
        $<TARGET_FILE_DIR:${EXE_NAME}>/AS.ini
)
