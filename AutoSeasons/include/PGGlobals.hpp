#pragma once

#include "PGDirectory.hpp"
#include "common/BethesdaGame.hpp"
#include "util/TaskQueue.hpp"

#include <filesystem>
#include <unordered_set>

/**
 * @brief Provides global singleton accessors for the core PGPatcher subsystem objects.
 *
 * Each subsystem (BethesdaGame, PGDirectory) is stored as a static
 * pointer and accessed via typed get/set/isSet methods. Throws std::runtime_error if a
 * getter is called before the corresponding pointer has been set.
 */
class PGGlobals {
private:
    static BethesdaGame* s_BG;
    static PGDirectory* s_PGD;

public:
    const static inline std::unordered_set<std::filesystem::path> s_foldersToMap
        = {"meshes", "textures", "pbrnifpatcher", "pbrtexturesets", "lightplacer"};

    /**
     * @brief Returns the global BethesdaGame instance.
     *
     * @return Pointer to the BethesdaGame object.
     * @throws std::runtime_error if the BethesdaGame has not been set.
     */
    static auto getBG() -> BethesdaGame*;

    /**
     * @brief Returns whether the global BethesdaGame pointer has been set.
     *
     * @return true if set, false otherwise.
     */
    static auto isBGSet() -> bool;

    /**
     * @brief Sets the global BethesdaGame pointer.
     *
     * @param bg Pointer to the BethesdaGame instance to store.
     */
    static void setBG(BethesdaGame* bg);

    /**
     * @brief Returns the global PGDirectory instance.
     *
     * @return Pointer to the PGDirectory object.
     * @throws std::runtime_error if the PGDirectory has not been set.
     */
    static auto getPGD() -> PGDirectory*;

    /**
     * @brief Returns whether the global PGDirectory pointer has been set.
     *
     * @return true if set, false otherwise.
     */
    static auto isPGDSet() -> bool;

    /**
     * @brief Sets the global PGDirectory pointer.
     *
     * @param pgd Pointer to the PGDirectory instance to store.
     */
    static void setPGD(PGDirectory* pgd);

    /**
     * @brief Returns the singleton TaskQueue used for serialized file-save operations.
     *
     * @return Reference to the global file-saver TaskQueue.
     */
    static auto getFileSaver() -> TaskQueue&;
};
