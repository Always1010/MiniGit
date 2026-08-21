#include "commands.hpp"
#include "minigit/error.hpp"
#include <iostream>
namespace {
int run(const minigit::cli::Arguments& args) {
    using namespace minigit::cli;
    if (args.empty()) throw UsageError("invalid arguments; use --help");
    const auto command = args.front();
    if ((command == "--help" || command == "help") && args.size() == 1) return run_help(args);
    if (command == "--version" && args.size() == 1) return run_version(args);
    if (command == "init") return run_init(args);
    if (command == "hash-object") return run_hash_object(args);
    if (command == "cat-file") return run_cat_file(args);
    if (command == "add") return run_add(args);
    if (command == "config") return run_config(args);
    if (command == "commit") return run_commit(args);
    if (command == "status") return run_status(args);
    if (command == "diff") return run_diff(args);
    if (command == "reflog") return run_reflog(args);
    if (command == "recover") return run_recover(args);
    if (command == "show") return run_show(args);
    if (command == "log") return run_log(args);
    if (command == "switch") return run_switch(args);
    if (command == "mv") return run_mv(args);
    if (command == "rm") return run_rm(args);
    if (command == "reset") return run_reset(args);
    if (command == "restore") return run_restore(args);
    if (command == "branch") return run_branch(args);
    if (command == "merge") return run_merge(args);
    if (command == "gc") return run_gc(args);
    if (command == "rebase") return run_rebase(args);
    if (command == "stash") return run_stash(args);
    if (command == "cherry-pick" || command == "revert") return run_replay(args);
    if (command == "tag") return run_tag(args);
    throw UsageError("invalid arguments; use --help");
}
} // namespace

int main(int argc, char* argv[]) {
    try {
        const minigit::cli::Arguments args(argv + 1, argv + argc);
        const int result = run(args);
        std::cout.flush();
        if (!std::cout) throw minigit::Error("cannot write output");
        return result;
    } catch (const minigit::cli::UsageError& error) {
        std::cerr << "minigit: " << error.what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "minigit: " << error.what() << '\n';
        return 1;
    }
}
