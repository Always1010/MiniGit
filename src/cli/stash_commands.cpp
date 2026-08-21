#include "commands.hpp"
#include "command_support.hpp"
#include "minigit/stash.hpp"
#include "minigit/format.hpp"
#include "minigit/fs.hpp"
#include "minigit/repository.hpp"
#include "minigit/error.hpp"
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
namespace minigit::cli {
namespace {
int report_stash_restore(const minigit::StashRestoreResult& result) {
    if (result.outcome == minigit::StashRestoreOutcome::Aborted) {
        std::cout << "Stash restore aborted.\n";
        return 0;
    }
    if (result.outcome == minigit::StashRestoreOutcome::Applied) {
        std::cout << "Stash applied." << (result.dropped ? " Entry dropped.\n" : "\n");
        return 0;
    }
    std::string output = "Stash paused; resolve conflicts, add paths, then use stash --continue or stash --abort.\n";
    constexpr std::string_view omitted = "Additional conflict paths omitted; use status to inspect them.\n";
    for (const auto& item : result.conflicts) {
        const auto layer = item.layer == minigit::StashConflictLayer::Work ? "work" :
            item.layer == minigit::StashConflictLayer::Staging ? "index" : "both";
        const auto line = std::string("CONFLICT (") + layer + ") " + minigit::quote_path(item.conflict.path) + "\n";
        if (line.size() > minigit::max_file_size - output.size() - omitted.size()) {
            output += omitted;
            break;
        }
        output += line;
    }
    std::cout << output;
    return 1;
}

std::size_t parse_stash_position(std::string_view value) {
    if (value.size() > 4096 || !value.starts_with("stash@{") || !value.ends_with('}'))
        throw UsageError("stash entry must be stash@{number}");
    return parse_count(value.substr(7, value.size() - 8), "stash position");
}
void append_stash_line(std::string& output, const minigit::StashListEntry& entry) {
    const auto prefix = "stash@{" + std::to_string(entry.position) + "} " + entry.work.hex() + " ";
    bool quoted = false;
    std::size_t quoted_size = 0;
    for (const unsigned char c : entry.message) {
        const bool special = c <= ' ' || c == 127 || c == '"' || c == '\\';
        quoted = quoted || special;
        const std::size_t width = c == '\n' || c == '\t' || c == '\r' || c == '"' || c == '\\' ? 2 :
            c < 32 || c == 127 ? 4 : 1;
        if (width > minigit::max_file_size - quoted_size) throw minigit::Error("stash list output exceeds 64 MiB");
        quoted_size += width;
    }
    const auto remaining = minigit::max_file_size - output.size();
    const auto framing = prefix.size() + 1 + (quoted ? 2 : 0);
    if (framing > remaining || quoted_size > remaining - framing)
        throw minigit::Error("stash list output exceeds 64 MiB");
    output += prefix;
    output += minigit::quote_path(entry.message);
    output += '\n';
}

} // namespace

int run_stash(const Arguments& args) {
    const auto action = args.size() == 1 ? std::string_view("push") : args[1];
    if (action == "--continue" || action == "--abort") {
        if (args.size() != 2) throw UsageError("stash --continue/--abort accepts no other arguments");
        const auto repo = minigit::Repository::discover();
        return report_stash_restore(action == "--continue" ? minigit::stash_continue(repo) : minigit::stash_abort(repo));
    }
    if (action == "apply" || action == "pop") {
        minigit::StashRestoreOptions options;
        options.pop = action == "pop";
        bool parse_options = true, selected = false;
        for (std::size_t i = 2; i < args.size(); ++i) {
            if (parse_options && args[i] == "--") { parse_options = false; continue; }
            if (parse_options && args[i] == "--index") {
                if (options.restore_index) throw UsageError("stash --index may be specified once");
                options.restore_index = true;
                continue;
            }
            if ((parse_options && args[i].starts_with('-')) || selected)
                throw UsageError("stash apply/pop accepts --index and one stash@{number}");
            options.position = parse_stash_position(args[i]);
            selected = true;
        }
        return report_stash_restore(minigit::stash_restore(minigit::Repository::discover(), options));
    }
    if (action == "push") {
        minigit::StashPushOptions options;
        bool untracked_set = false;
        for (std::size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "-u" || args[i] == "--include-untracked") {
                if (untracked_set) throw UsageError("stash untracked option may be specified once");
                untracked_set = true;
                options.include_untracked = true;
            } else if (args[i] == "-m") {
                if (options.message || i + 1 == args.size()) throw UsageError("stash -m requires one message");
                options.message = args[++i];
            } else if (args[i] == "--" && i + 1 == args.size()) {
                break;
            } else throw UsageError("stash push accepts only -u and -m message");
        }
        const auto id = minigit::stash_push(minigit::Repository::discover(), options);
        if (id) std::cout << "Saved stash " << id->hex() << '\n';
        else std::cout << "No local changes to save.\n";
        return 0;
    }
    if (action == "list" || action == "clear") {
        if (args.size() != 2) throw UsageError("stash list/clear accepts no arguments");
        const auto repo = minigit::Repository::discover();
        if (action == "clear") minigit::stash_clear(repo);
        else {
            std::string output;
            for (const auto& entry : minigit::stash_list(repo)) append_stash_line(output, entry);
            std::cout << output;
        }
        return 0;
    }
    if (action == "show" || action == "drop") {
        std::size_t i = 2;
        if (i < args.size() && args[i] == "--") ++i;
        if (args.size() > i + 1) throw UsageError("stash show/drop accepts one entry");
        const auto position = i == args.size() ? 0 : parse_stash_position(args[i]);
        const auto repo = minigit::Repository::discover();
        if (action == "drop") minigit::stash_drop(repo, position);
        else std::cout << minigit::stash_show(repo, position);
        return 0;
    }
    throw UsageError("unknown stash action");
}

}
