#include "PGGlobals.hpp"

#include "PGDirectory.hpp"
#include "common/BethesdaGame.hpp"
#include "util/TaskQueue.hpp"
#include <stdexcept>

BethesdaGame* PGGlobals::s_BG = nullptr;
auto PGGlobals::getBG() -> BethesdaGame*
{
    if (!isBGSet()) {
        throw std::runtime_error("BG is not set");
    }
    return s_BG;
}
auto PGGlobals::isBGSet() -> bool { return s_BG != nullptr; }
void PGGlobals::setBG(BethesdaGame* bg) { s_BG = bg; }

PGDirectory* PGGlobals::s_PGD = nullptr;
auto PGGlobals::getPGD() -> PGDirectory*
{
    if (!isPGDSet()) {
        throw std::runtime_error("PGD is not set");
    }
    return s_PGD;
}
auto PGGlobals::isPGDSet() -> bool { return s_PGD != nullptr; }
void PGGlobals::setPGD(PGDirectory* pgd) { s_PGD = pgd; }

auto PGGlobals::getFileSaver() -> TaskQueue&
{
    static TaskQueue fileSaver;
    return fileSaver;
}
