#include <Engine_classes.hpp>
#include <string>

namespace Util
{
    static bool IsInChimeraMain()
    {
        auto* s_world = SDK::UWorld::GetWorld();
        if (!s_world) return false;
        std::string worldName = s_world->GetName();
        return worldName == "ChimeraMain";
    }
}
