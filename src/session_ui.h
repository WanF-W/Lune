#pragma once
#include <hostcore.h>

namespace ui {
    // Maps HostCore events to the existing CLI presentation.
    void HC_CALL DisplaySessionEvent(void* context, const HC_Event* event);
}
