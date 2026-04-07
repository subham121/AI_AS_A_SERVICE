#pragma once

#include <string>
#include <vector>

namespace edgeai {

struct ProcessResult {
    int exit_code = -1;
    std::string output;
};

ProcessResult runCommandCapture(const std::string& command);
ProcessResult runCommandCapture(const std::vector<std::string>& argv);
std::string shellEscape(const std::string& value);

}  // namespace edgeai
