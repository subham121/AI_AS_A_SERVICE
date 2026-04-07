#include <edgeai/process_utils.h>

#include <array>
#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <sys/wait.h>

namespace edgeai {

std::string shellEscape(const std::string& value) {
    std::ostringstream escaped;
    escaped << '\'';
    for (char ch : value) {
        if (ch == '\'') {
            escaped << "'\\''";
        } else {
            escaped << ch;
        }
    }
    escaped << '\'';
    return escaped.str();
}

ProcessResult runCommandCapture(const std::string& command) {
    ProcessResult result;
    std::array<char, 256> buffer{};
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("Failed to execute command: " + command);
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        result.output += buffer.data();
    }
    const int status = pclose(pipe);
    if (status == -1) {
        result.exit_code = -1;
    } else if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else {
        result.exit_code = status;
    }
    return result;
}

ProcessResult runCommandCapture(const std::vector<std::string>& argv) {
    std::ostringstream command;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i != 0) {
            command << ' ';
        }
        command << shellEscape(argv[i]);
    }
    return runCommandCapture(command.str());
}

}  // namespace edgeai
