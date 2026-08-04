#pragma once

#include <array>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

/**
 * @brief C++ wrapper for the PGMutagen C# library that interfaces with Bethesda ES plugin files.
 *
 * Provides static methods for initializing the managed runtime, populating plugin object data,
 * retrieving and updating model uses, and finalizing the output plugin.
 */
class PGMutagenWrapper {
private:
    static constexpr int NUM_PLUGIN_TEXTURE_SLOTS = 8;
    static constexpr int DEFAULT_BUFFER_SIZE = 1024;

    static std::mutex s_libMutex;

    static void libLogMessageIfExists();
    static void libThrowExceptionIfExists();

public:
    /**
     * @brief A plain (mod, local FormID) reference - used for a LandscapeTextureEntry's own
     * Grasses list, and for the replacement Grasses list of a seasonal LTEX duplicate.
     */
    struct FormKeyRef {
        std::wstring modName;
        unsigned int formID = 0;
    };

    /**
     * @brief Holds an alternate texture assignment for a single texture slot in a model record.
     */
    struct AlternateTexture {
        int slotID = 0; ///< Original texture slot index referenced by the plugin record.
        int slotIDNew = 0; ///< New texture slot index after patching (may differ from slotID).
        std::array<std::wstring, NUM_PLUGIN_TEXTURE_SLOTS> slots; ///< Resolved texture paths for all slots.
    };

    /**
     * @brief Describes a single use of a mesh model within a plugin record.
     */
    struct ModelUse {
        std::wstring modName; ///< Name of the plugin (mod) that owns this record.
        unsigned int formID; ///< FormID of the record referencing this model.
        std::string subModel; ///< Sub-model identifier within the record.
        bool isWeighted; ///< Whether the model uses a weighted (skinned) mesh.
        std::wstring meshFile; ///< Path to the mesh file referenced by this record.
        bool singlepassMATO; ///< Whether this record uses single-pass MATO rendering.
        bool isIgnored; ///< Whether this model use should be skipped during patching.
        std::string type; ///< Record type string (e.g. "STAT", "ACTI").
        std::vector<AlternateTexture> alternateTextures; ///< List of alternate texture entries for this model.
        std::wstring editorID; ///< The base record's own EditorID; empty if unset.
    };

    /**
     * @brief Initializes the PGMutagen C# library and loads the specified game's plugin data.
     *
     * @param gameType  Integer identifier for the game type (maps to BethesdaGame::GameType).
     * @param exePath   Path to the game executable.
     * @param dataPath  Path to the game's Data directory.
     * @param loadOrder Ordered list of active plugin names; defaults to empty (auto-detected).
     * @param lang      Language code used for localised string resolution; defaults to 0 (English).
     */
    static void libInitialize(const int& gameType,
                              const std::wstring& exePath,
                              const std::wstring& dataPath,
                              const std::vector<std::wstring>& loadOrder = {},
                              const unsigned int& lang = 0);

    /**
     * @brief Populates the internal plugin object graph, optionally merging an existing output mod.
     *
     * @param existingModPath Path to an existing output mod to merge into the session; empty to start fresh.
     */
    static void libPopulateObjs(const std::filesystem::path& existingModPath = {});

    /**
     * @brief Resets mutable plugin-patching state to the post-populate baseline.
     */
    static void libResetPatchingState();

    /**
     * @brief Writes all pending changes to the output plugin file and finalises the session.
     *
     * @param outputPath Path where the output plugin file should be written.
     * @param esmMode    0 = ESM flag only PGPatcher.esp, 1 = ESM flag all output plugins, 2 = no ESM flags.
     */
    static void libFinalize(const std::filesystem::path& outputPath,
                            int esmMode);

    /**
     * @brief Retrieves all plugin records that reference the given model path.
     *
     * @param modelPath Relative mesh path (e.g. "meshes/foo/bar.nif") to look up.
     * @return Vector of ModelUse structs describing each record that uses this model.
     */
    static auto libGetModelUses(const std::wstring& modelPath) -> std::vector<ModelUse>;

    /**
     * @brief One shape's alternate-texture override within a seasonal duplicate request. For STAT
     * there is one of these per NIF shape with a seasonal texture sibling; for LTEX (no shape
     * concept) there is always exactly one, with shapeIndex/shapeName unused.
     */
    struct SeasonalTextureOverride {
        int shapeIndex = -1; ///< Shape's position in NifFile::GetShapes() order == plugin AlternateTextures Index.
        std::wstring shapeName; ///< The NiShape's own name; only used when creating a brand-new entry.
        std::array<std::wstring, NUM_PLUGIN_TEXTURE_SLOTS> slots; ///< Full texture set for this shape.
    };

    /**
     * @brief Describes a request to duplicate an existing model-referencing record into a new
     * record pointing at a seasonal texture set, leaving the original record untouched.
     */
    struct SeasonalDuplicateRequest {
        std::wstring modName; ///< Name of the plugin (mod) that owns the base record.
        unsigned int formID = 0; ///< FormID of the base record to duplicate.
        std::string type; ///< Record type string ("STAT" or "LTEX").
        std::vector<SeasonalTextureOverride> overrides; ///< Per-shape overrides (LTEX: always exactly one).
        std::string seasonSuffix; ///< Season suffix (e.g. "AUT") appended to the generated EditorID.
        /// LTEX only: if true, the duplicate's Grasses list is replaced with grassOverrides
        /// (which may be empty, meaning no grass at all for this season) instead of inheriting
        /// the base record's.
        bool overrideGrasses = false;
        std::vector<FormKeyRef> grassOverrides;
    };

    /**
     * @brief Result of a single seasonal duplicate request.
     */
    struct SeasonalDuplicateResult {
        std::wstring modName; ///< Name of the plugin that owned the base record (echoed back).
        unsigned int formID = 0; ///< FormID of the base record (echoed back).
        std::wstring newModName; ///< Name of the plugin the new duplicated record was added to.
        unsigned int newFormID = 0; ///< FormID of the new duplicated record; 0 if the duplicate failed.
    };

    /**
     * @brief Duplicates a batch of model-referencing records into new records using seasonal texture sets.
     *
     * @param requests List of duplicate requests to process.
     * @return Vector of SeasonalDuplicateResult, one per request, in the same order.
     */
    static auto libGenerateSeasonalDuplicates(const std::vector<SeasonalDuplicateRequest>& requests)
        -> std::vector<SeasonalDuplicateResult>;

    /**
     * @brief Describes a single LTEX record's current diffuse texture.
     */
    struct LandscapeTextureEntry {
        std::wstring modName; ///< Name of the plugin that owns this LTEX record.
        unsigned int formID = 0; ///< FormID of the LTEX record.
        std::array<std::wstring, NUM_PLUGIN_TEXTURE_SLOTS> slots; ///< Current 8-slot texture set; all empty if unset.
        std::wstring editorID; ///< The LTEX record's own EditorID; empty if unset.
        std::vector<FormKeyRef> grasses; ///< The LTEX's own Grasses list, in order; empty if none.
    };

    /**
     * @brief Enumerates every LTEX record's current diffuse texture across the whole load order.
     *
     * @return Vector of LandscapeTextureEntry, one per LTEX record.
     */
    static auto libEnumerateLandscapeTextures() -> std::vector<LandscapeTextureEntry>;

    /**
     * @brief Looks up TerrainHelper.esp's "LandscapeDefault" TextureSet (the engine's default,
     * no-LTEX-assigned terrain texture) and returns its current diffuse texture path.
     *
     * @return The diffuse texture path, or an empty string if TerrainHelper.esp isn't in the load
     * order (no such record exists to find).
     */
    static auto libGetLandscapeDefaultDiffuse() -> std::wstring;

    /**
     * @brief Overrides TerrainHelper.esp's "LandscapeDefault" TextureSet with a Height (parallax)
     * slot pointing at parallaxPath - switches on TerrainHelper's extended terrain shader
     * globally. Not season-specific - this is the engine's single, always-active fallback
     * texture. A no-op if TerrainHelper.esp isn't in the load order.
     *
     * @param parallaxPath Path to the parallax texture (with or without the "textures\" prefix).
     */
    static void libPatchLandscapeDefaultParallax(const std::wstring& parallaxPath);

    /**
     * @brief Looks up a GRAS (grass) record's own mesh path.
     *
     * @param modName Name of the plugin that owns the GRAS record.
     * @param formID FormID of the GRAS record.
     * @return The mesh path, or an empty string if the record can't be resolved or has no Model.
     */
    static auto libGetGrassMeshPath(const std::wstring& modName, unsigned int formID) -> std::wstring;

    /// @brief Result of a single GRAS duplication request.
    struct GrassDuplicateResult {
        std::wstring newModName; ///< Name of the plugin the new duplicated GRAS record was added to.
        unsigned int newFormID = 0; ///< FormID of the new duplicated record; 0 if the duplicate failed.
    };

    /**
     * @brief Duplicates a GRAS record with its Model pointed at a different mesh file, leaving
     * the original record untouched. Never touches AlternateTextures/TXST - grass appearance
     * comes entirely from whichever mesh the GRAS record's own Model references.
     *
     * @param modName Name of the plugin that owns the base GRAS record.
     * @param formID FormID of the base GRAS record.
     * @param newMeshPath Mesh path the duplicate's Model should point at.
     * @param seasonSuffix Season suffix (e.g. "AUT") appended to the generated EditorID.
     * @return The duplicate's mod name/FormID, or newFormID == 0 on failure.
     */
    static auto libDuplicateGrass(const std::wstring& modName, unsigned int formID, const std::wstring& newMeshPath,
        const std::string& seasonSuffix) -> GrassDuplicateResult;

private:
    // Helpers
    static auto utf8toUTF16(const std::string& str) -> std::wstring;
    static auto utf16toUTF8(const std::wstring& wStr) -> std::string;
};
