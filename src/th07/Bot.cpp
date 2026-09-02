#include "Bot.h"
#include "Controller.hpp"

namespace th07
{
namespace bot
{

unsigned short GameplayMask(unsigned frame, int seat, int lead)
{
    return TH_BUTTON_SHOOT;
}

bool SoloEnabled()
{
    return false;
}

}
}
