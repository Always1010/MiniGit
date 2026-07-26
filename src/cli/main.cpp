#include "minigit/version.hpp"

#include <iostream>
#include <string_view>

int main(int argc, char* argv[]) {
    if (argc == 2) {
        const std::string_view arg(argv[1]);
        if (arg == "--help" || arg == "help") {
            std::cout << "MiniGit - local version control\n"
                         "Usage: minigit --help | --version\n"
                         "Version control commands are not implemented yet.\n";
            return 0;
        }
        if (arg == "--version") {
            std::cout << "minigit " << minigit::version() << '\n';
            return 0;
        }
    }
    std::cerr << "minigit: invalid arguments; use --help\n";
    return 2;
}
