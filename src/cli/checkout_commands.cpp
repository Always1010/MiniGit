#include "commands.hpp"
#include "minigit/error.hpp"
#include "minigit/fs.hpp"
#include "minigit/repository.hpp"
#include "minigit/branches.hpp"
#include "minigit/switch.hpp"
#include "minigit/move.hpp"
#include "minigit/remove.hpp"
#include "minigit/reset.hpp"
#include "minigit/restore.hpp"
#include "minigit/tags.hpp"
#include "minigit/format.hpp"
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
namespace minigit::cli {
int run_switch(const Arguments& args) {
    minigit::SwitchOptions options;
    bool parse_options = true;
    std::optional<std::string_view> target;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (parse_options && args[i] == "--") { parse_options = false; continue; }
        if (parse_options && args[i] == "--detach") {
            if (options.detach) throw UsageError("switch --detach may be specified once");
            options.detach = true;
            continue;
        }
        if (parse_options && args[i] == "-c") {
            if (options.new_branch || i + 1 == args.size()) throw UsageError("switch -c requires one branch name");
            options.new_branch = std::string(args[++i]);
            continue;
        }
        if ((parse_options && args[i].starts_with('-')) || target)
            throw UsageError("switch accepts one branch or revision");
        target = args[i];
    }
    if ((options.detach && options.new_branch) || (!options.detach && !options.new_branch && !target))
        throw UsageError("switch requires a branch, -c name, or --detach");
    if (target) options.target = *target;
    const auto head = minigit::switch_worktree(minigit::Repository::discover(), options);
    if (head.symbolic) std::cout << "Switched to branch " << minigit::quote_path(head.symbolic->substr(11)) << '\n';
    else std::cout << "HEAD detached at " << head.id->hex() << '\n';
    return 0;
}

int run_mv(const Arguments& args) {
    bool options = true;
    std::vector<std::filesystem::path> paths;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (options && args[i] == "--") { options = false; continue; }
        if ((options && args[i].starts_with('-')) || paths.size() == 2)
            throw UsageError("mv accepts one source and one destination, without options");
        paths.emplace_back(args[i]);
    }
    if (paths.size() != 2) throw UsageError("mv requires one source and one destination");
    minigit::move_path(minigit::Repository::discover(), paths[0], paths[1]);
    return 0;
}

int run_rm(const Arguments& args) {
    minigit::RemoveOptions request;
    bool options = true;
    std::vector<std::filesystem::path> paths;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (options && args[i] == "--") { options = false; continue; }
        if (options && (args[i] == "-r" || args[i] == "--recursive")) {
            if (request.recursive) throw UsageError("rm recursive flag may be specified once");
            request.recursive = true;
            continue;
        }
        if (options && (args[i] == "-f" || args[i] == "--force")) {
            if (request.force) throw UsageError("rm force flag may be specified once");
            request.force = true;
            continue;
        }
        if (options && args[i] == "--cached") {
            if (request.cached) throw UsageError("rm cached flag may be specified once");
            request.cached = true;
            continue;
        }
        if (options && args[i].starts_with('-')) throw UsageError("unknown rm option");
        paths.emplace_back(args[i]);
    }
    if (paths.empty()) throw UsageError("rm requires tracked paths");
    minigit::remove_paths(minigit::Repository::discover(), paths, request);
    return 0;
}

int run_reset(const Arguments& args) {
    minigit::ResetOptions options;
    bool parse_options = true;
    bool mode_set = false;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (parse_options && args[i] == "--") { parse_options = false; continue; }
        if (parse_options && (args[i] == "--soft" || args[i] == "--mixed" || args[i] == "--hard")) {
            if (mode_set) throw UsageError("reset mode may be specified once");
            options.mode = args[i] == "--soft" ? minigit::ResetMode::Soft :
                args[i] == "--hard" ? minigit::ResetMode::Hard : minigit::ResetMode::Mixed;
            mode_set = true;
            continue;
        }
        if ((parse_options && args[i].starts_with('-')) || options.revision)
            throw UsageError("reset accepts one revision and --soft, --mixed or --hard");
        options.revision = args[i];
    }
    minigit::reset_repository(minigit::Repository::discover(), options);
    return 0;
}

int run_restore(const Arguments& args) {
    minigit::RestoreOptions options;
    bool parse_options = true;
    std::vector<std::filesystem::path> paths;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (parse_options && args[i] == "--") { parse_options = false; continue; }
        if (parse_options && (args[i] == "--staged" || args[i] == "-S")) {
            if (options.staged) throw UsageError("restore --staged may be specified once");
            options.staged = true;
            continue;
        }
        if (parse_options && (args[i] == "--worktree" || args[i] == "-W")) {
            if (options.worktree) throw UsageError("restore --worktree may be specified once");
            options.worktree = true;
            continue;
        }
        if (parse_options && (args[i] == "--source" || args[i] == "-s" || args[i].starts_with("--source="))) {
            if (options.source) throw UsageError("restore source may be specified once");
            if (args[i].starts_with("--source=")) options.source = args[i].substr(9);
            else {
                if (i + 1 == args.size()) throw UsageError("restore --source requires a revision");
                options.source = args[++i];
            }
            if (options.source->empty()) throw UsageError("restore --source requires a revision");
            continue;
        }
        if (parse_options && args[i].starts_with('-')) throw UsageError("unknown restore option");
        paths.emplace_back(args[i]);
    }
    if (paths.empty()) throw UsageError("restore requires paths");
    minigit::restore_paths(minigit::Repository::discover(), paths, options);
    return 0;
}

int run_branch(const Arguments& args) {
    bool force = false;
    bool force_creation = false;
    bool remove = false;
    bool options = true;
    std::vector<std::string_view> values;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (options && args[i] == "--") { options = false; continue; }
        if (options && args[i] == "-f") { force = true; force_creation = true; continue; }
        if (options && (args[i] == "-d" || args[i] == "-D")) {
            if (remove) throw UsageError("branch deletion mode may be specified once");
            remove = true;
            if (args[i] == "-D") force = true;
            continue;
        }
        if (options && args[i].starts_with('-')) throw UsageError("unknown branch option");
        values.push_back(args[i]);
    }
    if (values.size() > 2 || (remove && (values.size() != 1 || force_creation)) || (force && values.empty()))
        throw UsageError("branch accepts [-f] <name> [revision], -d|-D <name>, or no arguments");
    const auto repo = minigit::Repository::discover();
    if (remove) minigit::delete_branch(repo, values[0], force);
    else if (values.empty()) {
        std::string text;
        for (const auto& branch : minigit::list_branches(repo)) {
            const auto line = std::string(branch.current ? "* " : "  ") + minigit::quote_path(branch.name) + " " +
                (branch.id ? branch.id->hex() : "(unborn)") + "\n";
            if (line.size() > minigit::max_file_size - text.size()) throw minigit::Error("branch output exceeds 64 MiB");
            text += line;
        }
        std::cout << text;
    } else minigit::create_branch(repo, values[0], values.size() == 2 ? values[1] : "HEAD", force);
    return 0;
}

int run_tag(const Arguments& args) {
    bool force = false;
    bool remove = false;
    bool options = true;
    std::vector<std::string_view> values;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (options && args[i] == "--") { options = false; continue; }
        if (options && args[i] == "-f") { force = true; continue; }
        if (options && args[i] == "-d") { remove = true; continue; }
        if (options && args[i].starts_with('-')) throw UsageError("unknown tag option");
        values.push_back(args[i]);
    }
    if (values.size() > 2 || (remove && (force || values.size() != 1)) ||
        (force && values.empty()))
        throw UsageError("tag accepts [-f] <name> [revision], -d <name>, or no arguments");
    const auto repo = minigit::Repository::discover();
    if (remove) minigit::delete_tag(repo, values[0]);
    else if (values.empty()) {
        for (const auto& [name, id] : minigit::list_tags(repo)) {
            (void)id;
            std::cout << minigit::quote_path(name) << '\n';
        }
    } else minigit::create_tag(repo, values[0], values.size() == 2 ? values[1] : "HEAD", force);
    return 0;
}

}
