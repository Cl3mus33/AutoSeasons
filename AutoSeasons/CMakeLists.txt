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
# Translations (GUI localization - ASLocale reads these from an "AutoSeasons_translations" folder
# next to the exe. Namespaced with the same "AutoSeasons_" prefix as AutoSeasons_cshaders above -
# PGPatcher, built from the same shared codebase, ships its own "translations" folder at the same
# relative path, and since both tools get installed as MO2 mods, an unprefixed name would collide
# and silently load whichever tool's translations win the MO2 conflict.)
#
add_custom_command(TARGET ${EXE_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -rf $<TARGET_FILE_DIR:${EXE_NAME}>/AutoSeasons_translations
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${EXE_NAME}>/AutoSeasons_translations
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/AutoSeasons_translations
        $<TARGET_FILE_DIR:${EXE_NAME}>/AutoSeasons_translations
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

#
# OpenMP runtime (vcomp140.dll) - the one genuine VC++ runtime DLL this exe still dynamically
# depends on despite the static CRT linkage set in the root CMakeLists.txt (Microsoft doesn't ship
# a static OpenMP runtime). Confirmed via dumpbin /dependents. Copied from the toolset's own Redist
# folder (populated by vcvars64.bat, which build.bat always calls first) rather than assuming the
# target machine already has it.
#
if(NOT DEFINED ENV{VCToolsRedistDir})
    message(FATAL_ERROR "VCToolsRedistDir is not set - configure/build from a Developer Command Prompt (or via vcvars64.bat, like build.bat does) so vcomp140.dll can be located.")
endif()
file(GLOB VCOMP140_DLL "$ENV{VCToolsRedistDir}x64/Microsoft.VC*.OpenMP/vcomp140.dll")
list(GET VCOMP140_DLL 0 VCOMP140_DLL)
add_custom_command(TARGET ${EXE_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${VCOMP140_DLL}"
        $<TARGET_FILE_DIR:${EXE_NAME}>/vcomp140.dll
)
