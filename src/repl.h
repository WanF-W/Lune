#pragma once
#include <hostcore.h>
#include <string>

namespace repl {
    // Lua input classification remains a CLI concern.
    bool IsStatement(const std::string& input);
    bool ExecuteCommand(HC_Session session, const std::string& code, int timeoutMs);
}
