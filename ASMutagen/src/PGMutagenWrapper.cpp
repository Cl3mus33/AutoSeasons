#include "PGMutagenWrapper.hpp"

#include "ASMutagenBuffers_generated.h"
#include "ASMutagenNE.h"
#include "dnne.h"

#include <flatbuffers/buffer.h>
#include <flatbuffers/flatbuffer_builder.h>
#include <flatbuffers/flatbuffers.h>
#include <flatbuffers/string.h>
#include <flatbuffers/verifier.h>
#include <spdlog/spdlog.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <windows.h>

using namespace std;

namespace {
constexpr auto DOTNET_RUNTIME_PRELOAD_ERROR_MESSAGE
    = "DotNet Wrapper: .NET runtime failed to preload (error: 0x{:08X}).";

void dnneFailure(enum failure_type type,
                 int errorCode)
{
    switch (type) {
    case failure_load_runtime:
        spdlog::critical(DOTNET_RUNTIME_PRELOAD_ERROR_MESSAGE, static_cast<unsigned int>(errorCode));
        break;
    case failure_load_export:
        spdlog::critical("DotNet Wrapper failed to load a managed export (error: 0x{:08X}). "
                         "Ensure PGMutagen.dll is present and matches the expected version.",
                         static_cast<unsigned int>(errorCode));
        break;
    default:
        spdlog::critical("DotNet Wrapper failed with unknown type {} (error: 0x{:08X}).",
                         static_cast<int>(type),
                         static_cast<unsigned int>(errorCode));
        break;
    }
}

// FlatBuffers strings are UTF-8 (C# writes them via builder.CreateString(string), which encodes
// as UTF-8) - widening each byte directly into a wstring (the old `wstring(s->begin(), s->end())`
// pattern) mangles any mod name, EditorID, or texture path containing non-ASCII characters (e.g.
// accented Latin, Cyrillic, CJK - common in real Nexus mod names). Convert properly instead.
auto fbStrToWString(const flatbuffers::String* fbStr) -> wstring
{
    if (fbStr == nullptr || fbStr->size() == 0) {
        return L"";
    }
    const int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, fbStr->c_str(), static_cast<int>(fbStr->size()), nullptr, 0);
    wstring wStr(sizeNeeded, 0);
    MultiByteToWideChar(CP_UTF8, 0, fbStr->c_str(), static_cast<int>(fbStr->size()), wStr.data(), sizeNeeded);
    return wStr;
}
} // namespace

mutex PGMutagenWrapper::s_libMutex;

void PGMutagenWrapper::libLogMessageIfExists()
{
    static constexpr unsigned int TRACE_LOG = 0;
    static constexpr unsigned int DEBUG_LOG = 1;
    static constexpr unsigned int INFO_LOG = 2;
    static constexpr unsigned int WARN_LOG = 3;
    static constexpr unsigned int ERROR_LOG = 4;
    static constexpr unsigned int CRITICAL_LOG = 5;

    int level = 0;
    wchar_t* message = nullptr;
    GetLogMessage(&message, &level);

    while (message != nullptr) {
        const wstring messageOut(message);
        LocalFree(static_cast<HGLOBAL>(message)); // Only free if memory was allocated.
        message = nullptr;

        // log the message
        switch (level) {
        case TRACE_LOG:
            spdlog::trace(L"{}", messageOut);
            break;
        case DEBUG_LOG:
            spdlog::debug(L"{}", messageOut);
            break;
        case INFO_LOG:
            spdlog::info(L"{}", messageOut);
            break;
        case WARN_LOG:
            spdlog::warn(L"{}", messageOut);
            break;
        case ERROR_LOG:
            spdlog::error(L"{}", messageOut);
            break;
        case CRITICAL_LOG:
            spdlog::critical(L"{}", messageOut);
            break;
        }

        // Get the next message
        GetLogMessage(&message, &level);
    }
}

void PGMutagenWrapper::libThrowExceptionIfExists()
{
    wchar_t* message = nullptr;
    GetLastException(&message);

    if (message == nullptr) {
        return;
    }

    const wstring messageOut(message);
    LocalFree(static_cast<HGLOBAL>(message)); // Only free if memory was allocated.

    throw runtime_error("PGMutagenWrapper: " + utf16toUTF8(messageOut));
}

void PGMutagenWrapper::libInitialize(const int& gameType,
                                     const std::wstring& exePath,
                                     const wstring& dataPath,
                                     const vector<wstring>& loadOrder,
                                     const unsigned int& lang)
{
    // Proactively try to load the .NET runtime. try_preload_runtime() returns an error
    // code on failure instead of calling abort(), giving us the chance to surface a
    // proper exception to the caller.
    const int runtimeRC = try_preload_runtime();
    if (runtimeRC != 0) {
        spdlog::critical(DOTNET_RUNTIME_PRELOAD_ERROR_MESSAGE, static_cast<unsigned int>(runtimeRC));
        throw runtime_error("PGMutagenWrapper: .NET runtime failed to initialize. "
                            "Check the log for details.");
    }

    set_failure_callback(dnneFailure);

    // Use vector to manage the memory for LoadOrderArr
    vector<const wchar_t*> loadOrderArr;
    if (!loadOrder.empty()) {
        loadOrderArr.reserve(loadOrder.size()); // Pre-allocate the vector size
        for (const auto& mod : loadOrder) {
            loadOrderArr.push_back(mod.c_str()); // Populate the vector with the c_str pointers
        }
    }

    // Add the null terminator to the end
    loadOrderArr.push_back(nullptr);

    {
        const lock_guard<mutex> lock(s_libMutex);

        Initialize(gameType, exePath.c_str(), dataPath.c_str(), loadOrderArr.data(), lang);
        libLogMessageIfExists();
        libThrowExceptionIfExists();
    }
}

void PGMutagenWrapper::libPopulateObjs(const filesystem::path& existingModPath)
{
    const lock_guard<mutex> lock(s_libMutex);

    PopulateObjs(existingModPath.wstring().c_str());
    libLogMessageIfExists();
    libThrowExceptionIfExists();
}

void PGMutagenWrapper::libResetPatchingState()
{
    const lock_guard<mutex> lock(s_libMutex);

    ResetPatchingState(0);
    libLogMessageIfExists();
    libThrowExceptionIfExists();
}

void PGMutagenWrapper::libFinalize(const filesystem::path& outputPath,
                                   int esmMode)
{
    const lock_guard<mutex> lock(s_libMutex);

    Finalize(outputPath.c_str(), esmMode);
    libLogMessageIfExists();
    libThrowExceptionIfExists();
}

namespace {
// Shared by libGetModelUses (per-mesh query) and libEnumerateAllModelUses (whole-load-order
// dump) - parses one ASMutagenBuffers::ModelUse table entry into the plain PGMutagenWrapper struct.
auto parseModelUse(const ASMutagenBuffers::ModelUse* mu) -> PGMutagenWrapper::ModelUse
{
    auto curUse = PGMutagenWrapper::ModelUse();

    curUse.modName = fbStrToWString(mu->mod_name());
    curUse.formID = mu->form_id();
    curUse.subModel = string(mu->sub_model()->begin(), mu->sub_model()->end());
    curUse.isWeighted = mu->is_weighted();
    curUse.singlepassMATO = mu->singlepass_mato();
    curUse.isIgnored = mu->is_ignored();
    curUse.type = string(mu->type()->begin(), mu->type()->end());
    curUse.editorID = fbStrToWString(mu->editor_id());
    curUse.meshFile = fbStrToWString(mu->mesh_file());

    for (const auto* const altTex : *mu->alternate_textures()) {
        auto curAltTex = PGMutagenWrapper::AlternateTexture();
        curAltTex.slotID = altTex->slot_id();

        // no slots
        if (altTex->slots() == nullptr || altTex->slots()->textures() == nullptr) {
            continue;
        }

        const auto* textures = altTex->slots()->textures();
        for (int i = 0; std::cmp_less(i, curAltTex.slots.size()) && std::cmp_less(i, textures->size()); ++i) {
            curAltTex.slots.at(i) = fbStrToWString(textures->Get(i));
        }

        curUse.alternateTextures.push_back(curAltTex);
    }

    return curUse;
}
} // namespace

auto PGMutagenWrapper::libGetModelUses(const std::wstring& modelPath) -> std::vector<ModelUse>
{
    uint8_t* buffer = nullptr;
    uint32_t length = 0;

    {
        const lock_guard<mutex> lock(s_libMutex);
        GetModelUses(modelPath.c_str(), &length, &buffer);
        libLogMessageIfExists();
        libThrowExceptionIfExists();
    }

    if ((buffer == nullptr) || length == 0) {
        return {};
    }

    flatbuffers::Verifier verifier(buffer, length);
    if (!ASMutagenBuffers::VerifyModelUsesBuffer(verifier)) {
        // Freed with LocalFree, not CoTaskMemFree - the C# side allocates this buffer with
        // Marshal.AllocHGlobal (LocalAlloc-backed on Windows), matching the LocalFree convention
        // already used throughout this file for every other Marshal.StringToHGlobalUni allocation.
        LocalFree(static_cast<HGLOBAL>(buffer));
        return {};
    }

    vector<ModelUse> modelUsesOut;

    const auto* const modelUses = ASMutagenBuffers::GetModelUses(buffer);
    for (const auto* const mu : *modelUses->uses()) {
        modelUsesOut.push_back(parseModelUse(mu));
    }

    LocalFree(static_cast<HGLOBAL>(buffer));

    return modelUsesOut;
}

auto PGMutagenWrapper::libEnumerateAllModelUses() -> std::vector<ModelUse>
{
    uint8_t* buffer = nullptr;
    uint32_t length = 0;

    {
        const lock_guard<mutex> lock(s_libMutex);
        EnumerateAllModelUses(&length, &buffer);
        libLogMessageIfExists();
        libThrowExceptionIfExists();
    }

    if ((buffer == nullptr) || length == 0) {
        return {};
    }

    flatbuffers::Verifier verifier(buffer, length);
    if (!ASMutagenBuffers::VerifyModelUsesBuffer(verifier)) {
        LocalFree(static_cast<HGLOBAL>(buffer));
        return {};
    }

    vector<ModelUse> modelUsesOut;

    const auto* const modelUses = ASMutagenBuffers::GetModelUses(buffer);
    for (const auto* const mu : *modelUses->uses()) {
        modelUsesOut.push_back(parseModelUse(mu));
    }

    LocalFree(static_cast<HGLOBAL>(buffer));

    return modelUsesOut;
}

auto PGMutagenWrapper::libGenerateSeasonalDuplicates(const std::vector<SeasonalDuplicateRequest>& requests)
    -> std::vector<SeasonalDuplicateResult>
{
    flatbuffers::FlatBufferBuilder builder(DEFAULT_BUFFER_SIZE);

    vector<flatbuffers::Offset<ASMutagenBuffers::SeasonalDuplicateRequest>> reqOffsets;
    reqOffsets.reserve(requests.size());

    for (const auto& req : requests) {
        const auto modNameOffset = builder.CreateString(utf16toUTF8(req.modName));
        const auto typeOffset = builder.CreateString(req.type);
        const auto seasonOffset = builder.CreateString(req.seasonSuffix);

        vector<flatbuffers::Offset<ASMutagenBuffers::SeasonalTextureOverride>> overrideOffsets;
        overrideOffsets.reserve(req.overrides.size());
        for (const auto& override : req.overrides) {
            const auto shapeNameOffset = builder.CreateString(utf16toUTF8(override.shapeName));

            vector<flatbuffers::Offset<flatbuffers::String>> texOffsets;
            texOffsets.reserve(NUM_PLUGIN_TEXTURE_SLOTS);
            for (const auto& tex : override.slots) {
                texOffsets.push_back(builder.CreateString(utf16toUTF8(tex)));
            }
            const auto slotsOffset = ASMutagenBuffers::CreateTextureSet(builder, builder.CreateVector(texOffsets));

            overrideOffsets.push_back(ASMutagenBuffers::CreateSeasonalTextureOverride(
                builder, override.shapeIndex, shapeNameOffset, slotsOffset));
        }
        const auto overridesOffset = builder.CreateVector(overrideOffsets);

        vector<flatbuffers::Offset<ASMutagenBuffers::FormKeyRef>> grassOverrideOffsets;
        grassOverrideOffsets.reserve(req.grassOverrides.size());
        for (const auto& grassRef : req.grassOverrides) {
            grassOverrideOffsets.push_back(
                ASMutagenBuffers::CreateFormKeyRef(builder, builder.CreateString(utf16toUTF8(grassRef.modName)), grassRef.formID));
        }
        const auto grassOverridesOffset = builder.CreateVector(grassOverrideOffsets);
        const auto baseTXSTEditorIDOffset = builder.CreateString(utf16toUTF8(req.baseTXSTEditorID));

        const auto reqOffset = ASMutagenBuffers::CreateSeasonalDuplicateRequest(builder, modNameOffset, req.formID,
            typeOffset, overridesOffset, seasonOffset, req.overrideGrasses, grassOverridesOffset, baseTXSTEditorIDOffset);
        reqOffsets.push_back(reqOffset);
    }

    const auto reqVectorOffset = builder.CreateVector(reqOffsets);
    const auto reqRoot = ASMutagenBuffers::CreateSeasonalDuplicateRequests(builder, reqVectorOffset);
    builder.Finish(reqRoot);

    uint8_t const* buf = builder.GetBufferPointer();
    unsigned int const size = builder.GetSize();

    uint8_t* outBuffer = nullptr;
    uint32_t outLength = 0;

    {
        const lock_guard<mutex> lock(s_libMutex);
        GenerateSeasonalDuplicates(size, buf, &outLength, &outBuffer);
        libLogMessageIfExists();
        libThrowExceptionIfExists();
    }

    vector<SeasonalDuplicateResult> resultsOut;
    if ((outBuffer == nullptr) || outLength == 0) {
        return resultsOut;
    }

    flatbuffers::Verifier verifier(outBuffer, outLength);
    if (!verifier.VerifyBuffer<ASMutagenBuffers::SeasonalDuplicateResults>(nullptr)) {
        // See the LocalFree comment above libGetModelUses - same AllocHGlobal/LocalFree pairing.
    LocalFree(static_cast<HGLOBAL>(outBuffer));
        return resultsOut;
    }

    const auto* const results = flatbuffers::GetRoot<ASMutagenBuffers::SeasonalDuplicateResults>(outBuffer);
    resultsOut.reserve(results->results()->size());
    for (const auto* const r : *results->results()) {
        SeasonalDuplicateResult cur;
        cur.modName = fbStrToWString(r->mod_name());
        cur.formID = r->form_id();
        cur.newModName = fbStrToWString(r->new_mod_name());
        cur.newFormID = r->new_form_id();
        resultsOut.push_back(cur);
    }

    // See the LocalFree comment above libGetModelUses - same AllocHGlobal/LocalFree pairing.
    LocalFree(static_cast<HGLOBAL>(outBuffer));

    return resultsOut;
}

auto PGMutagenWrapper::libEnumerateLandscapeTextures() -> std::vector<LandscapeTextureEntry>
{
    uint8_t* buffer = nullptr;
    uint32_t length = 0;

    {
        const lock_guard<mutex> lock(s_libMutex);
        EnumerateLandscapeTextures(&length, &buffer);
        libLogMessageIfExists();
        libThrowExceptionIfExists();
    }

    vector<LandscapeTextureEntry> entriesOut;
    if ((buffer == nullptr) || length == 0) {
        return entriesOut;
    }

    flatbuffers::Verifier verifier(buffer, length);
    if (!verifier.VerifyBuffer<ASMutagenBuffers::LandscapeTextureEntries>(nullptr)) {
        // Freed with LocalFree, not CoTaskMemFree - the C# side allocates this buffer with
    // Marshal.AllocHGlobal (LocalAlloc-backed on Windows), matching the LocalFree convention
    // already used throughout this file for every other Marshal.StringToHGlobalUni allocation.
    LocalFree(static_cast<HGLOBAL>(buffer));
        return entriesOut;
    }

    const auto* const entries = flatbuffers::GetRoot<ASMutagenBuffers::LandscapeTextureEntries>(buffer);
    entriesOut.reserve(entries->entries()->size());
    for (const auto* const e : *entries->entries()) {
        LandscapeTextureEntry cur;
        cur.modName = fbStrToWString(e->mod_name());
        cur.formID = e->form_id();
        cur.editorID = fbStrToWString(e->editor_id());
        cur.txstEditorID = fbStrToWString(e->txst_editor_id());

        if (e->slots() != nullptr && e->slots()->textures() != nullptr) {
            const auto* textures = e->slots()->textures();
            for (uint32_t i = 0; i < NUM_PLUGIN_TEXTURE_SLOTS && i < textures->size(); i++) {
                cur.slots.at(i) = fbStrToWString(textures->Get(i));
            }
        }

        if (e->grasses() != nullptr) {
            for (const auto* const grassRef : *e->grasses()) {
                if (grassRef == nullptr || grassRef->mod_name() == nullptr) {
                    continue;
                }
                cur.grasses.push_back(FormKeyRef {
                    .modName = fbStrToWString(grassRef->mod_name()),
                    .formID = grassRef->form_id(),
                });
            }
        }

        entriesOut.push_back(cur);
    }

    // Freed with LocalFree, not CoTaskMemFree - the C# side allocates this buffer with
    // Marshal.AllocHGlobal (LocalAlloc-backed on Windows), matching the LocalFree convention
    // already used throughout this file for every other Marshal.StringToHGlobalUni allocation.
    LocalFree(static_cast<HGLOBAL>(buffer));

    return entriesOut;
}

auto PGMutagenWrapper::libGetLandscapeDefaultDiffuse() -> wstring
{
    wchar_t* diffuse = nullptr;

    {
        const lock_guard<mutex> lock(s_libMutex);
        GetLandscapeDefaultDiffuse(&diffuse);
        libLogMessageIfExists();
        libThrowExceptionIfExists();
    }

    if (diffuse == nullptr) {
        return L"";
    }

    const wstring result(diffuse);
    LocalFree(static_cast<HGLOBAL>(diffuse));
    return result;
}

void PGMutagenWrapper::libPatchLandscapeDefaultParallax(const wstring& parallaxPath)
{
    const lock_guard<mutex> lock(s_libMutex);

    PatchLandscapeDefaultParallax(parallaxPath.c_str());
    libLogMessageIfExists();
    libThrowExceptionIfExists();
}

void PGMutagenWrapper::libEnsureDefaultPBRLand(const array<wstring, NUM_PLUGIN_TEXTURE_SLOTS>& slots)
{
    flatbuffers::FlatBufferBuilder builder(DEFAULT_BUFFER_SIZE);

    vector<flatbuffers::Offset<flatbuffers::String>> texOffsets;
    texOffsets.reserve(NUM_PLUGIN_TEXTURE_SLOTS);
    for (const auto& tex : slots) {
        texOffsets.push_back(builder.CreateString(utf16toUTF8(tex)));
    }
    const auto slotsOffset = ASMutagenBuffers::CreateTextureSet(builder, builder.CreateVector(texOffsets));
    builder.Finish(slotsOffset);

    uint8_t const* buf = builder.GetBufferPointer();
    unsigned int const size = builder.GetSize();

    const lock_guard<mutex> lock(s_libMutex);

    EnsureDefaultPBRLand(size, buf);
    libLogMessageIfExists();
    libThrowExceptionIfExists();
}

auto PGMutagenWrapper::libGetGrassMeshPath(const wstring& modName, unsigned int formID) -> wstring
{
    wchar_t* meshPath = nullptr;

    {
        const lock_guard<mutex> lock(s_libMutex);
        GetGrassMeshPath(modName.c_str(), formID, &meshPath);
        libLogMessageIfExists();
        libThrowExceptionIfExists();
    }

    if (meshPath == nullptr) {
        return L"";
    }

    const wstring result(meshPath);
    LocalFree(static_cast<HGLOBAL>(meshPath));
    return result;
}

auto PGMutagenWrapper::libGetGrassEditorID(const wstring& modName, unsigned int formID) -> wstring
{
    wchar_t* editorID = nullptr;

    {
        const lock_guard<mutex> lock(s_libMutex);
        GetGrassEditorID(modName.c_str(), formID, &editorID);
        libLogMessageIfExists();
        libThrowExceptionIfExists();
    }

    if (editorID == nullptr) {
        return L"";
    }

    const wstring result(editorID);
    LocalFree(static_cast<HGLOBAL>(editorID));
    return result;
}

auto PGMutagenWrapper::libFindGrassByEditorID(const wstring& editorID) -> optional<FormKeyRef>
{
    wchar_t* modName = nullptr;
    uint32_t formID = 0;

    {
        const lock_guard<mutex> lock(s_libMutex);
        FindGrassByEditorID(editorID.c_str(), &modName, &formID);
        libLogMessageIfExists();
        libThrowExceptionIfExists();
    }

    if (modName == nullptr) {
        return nullopt;
    }

    FormKeyRef result { .modName = wstring(modName), .formID = formID };
    LocalFree(static_cast<HGLOBAL>(modName));
    return result;
}

auto PGMutagenWrapper::libDuplicateGrass(const wstring& modName, unsigned int formID, const wstring& newMeshPath,
    const string& seasonSuffix) -> GrassDuplicateResult
{
    wchar_t* newModName = nullptr;
    unsigned int newFormID = 0;

    {
        const lock_guard<mutex> lock(s_libMutex);
        DuplicateGrass(modName.c_str(), formID, newMeshPath.c_str(),
            utf8toUTF16(seasonSuffix).c_str(), &newModName, &newFormID);
        libLogMessageIfExists();
        libThrowExceptionIfExists();
    }

    GrassDuplicateResult result;
    result.newFormID = newFormID;
    if (newModName != nullptr) {
        result.newModName = wstring(newModName);
        LocalFree(static_cast<HGLOBAL>(newModName));
    }
    return result;
}

auto PGMutagenWrapper::utf8toUTF16(const string& str) -> wstring
{
    // Just return empty string if empty
    if (str.empty()) {
        return {};
    }

    // Convert string > wstring
    const int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.length(), nullptr, 0);
    std::wstring wStr(sizeNeeded, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.length(), wStr.data(), sizeNeeded);

    return wStr;
}

auto PGMutagenWrapper::utf16toUTF8(const wstring& wStr) -> string
{
    // Just return empty string if empty
    if (wStr.empty()) {
        return {};
    }

    // Convert wstring > string
    const int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wStr.data(), (int)wStr.size(), nullptr, 0, nullptr, nullptr);
    string str(sizeNeeded, 0);
    WideCharToMultiByte(CP_UTF8, 0, wStr.data(), (int)wStr.size(), str.data(), sizeNeeded, nullptr, nullptr);

    return str;
}
