#include "commands.hpp"
#include "command_support.hpp"
#include "minigit/error.hpp"
#include "minigit/fs.hpp"
#include "minigit/repository.hpp"
#include "minigit/inspection.hpp"
#include "minigit/reflog.hpp"
#include "minigit/dag.hpp"
#include "minigit/format.hpp"
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
namespace minigit::cli {
int run_diff(const Arguments& args) {
    minigit::DiffOptions request;
    bool options = true;
    bool context_set = false;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (options && args[i] == "--") { options = false; continue; }
        if (options && (args[i] == "--cached" || args[i] == "--staged")) {
            request.staged = true;
            continue;
        }
        std::optional<std::string_view> context;
        if (options && (args[i] == "-U" || args[i] == "--unified")) {
            if (i + 1 >= args.size()) throw UsageError("diff context requires a count");
            context = args[++i];
        } else if (options && args[i].starts_with("-U")) context = args[i].substr(2);
        else if (options && args[i].starts_with("--unified=")) context = args[i].substr(10);
        if (context) {
            if (context_set) throw UsageError("diff context may be specified once");
            request.context = parse_count(*context, "diff context");
            if (request.context > 100000) throw UsageError("diff context exceeds 100000 lines");
            context_set = true;
            continue;
        }
        if ((options && args[i].starts_with('-')) || request.revisions.size() == 2)
            throw UsageError("diff accepts --cached, -U <count> and at most two revisions");
        request.revisions.emplace_back(args[i]);
    }
    if (request.staged && request.revisions.size() > 1)
        throw UsageError("diff --cached accepts at most one revision");
    std::cout << minigit::repository_diff(minigit::Repository::discover(), request);
    return 0;
}

int run_show(const Arguments& args) {
    bool options = true;
    std::optional<std::string_view> spec;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (options && args[i] == "--") { options = false; continue; }
        if ((options && args[i].starts_with('-')) || spec)
            throw UsageError("show accepts one object, revision:path or :stage:path");
        spec = args[i];
    }
    const auto content = minigit::show_object(minigit::Repository::discover(), spec.value_or("HEAD"));
    if (!content.empty()) std::cout.write(reinterpret_cast<const char*>(content.data()),
                                         static_cast<std::streamsize>(content.size()));
    return 0;
}

int run_log(const Arguments& args) {
    bool oneline = false;
    bool options = true;
    std::optional<std::string_view> revision;
    std::optional<std::size_t> maximum;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (options && args[i] == "--") { options = false; continue; }
        if (options && args[i] == "--oneline") { oneline = true; continue; }
        if (options && args[i] == "-n" && !maximum && i + 1 < args.size()) {
            const auto value = args[++i];
            maximum = parse_count(value, "log count");
            continue;
        }
        if ((options && args[i].starts_with('-')) || revision)
            throw UsageError("log accepts --oneline, -n <count> and one revision");
        revision = args[i];
    }
    const auto entries = minigit::log_history(minigit::Repository::discover(),
        revision.value_or("HEAD"), maximum);
    std::string output;
    if (entries.empty() && !oneline && maximum != 0) output = "No commits yet\n";
    for (const auto& entry : entries) {
        const auto formatted = minigit::format_commit(entry.id, entry.commit, oneline);
        if (formatted.size() > minigit::max_file_size - output.size())
            throw minigit::Error("日志总输出超过 64 MiB 限制");
        output += formatted;
    }
    std::cout << output;
    return 0;
}

int run_reflog(const Arguments& args) {
    bool options = true;
    std::optional<std::string_view> selected;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (options && args[i] == "--") { options = false; continue; }
        if ((options && args[i].starts_with('-')) || selected)
            throw UsageError("reflog accepts HEAD or one branch");
        selected = args[i];
    }
    const std::string ref = !selected || *selected == "HEAD" ? "HEAD" :
        selected->starts_with("refs/") ? std::string(*selected) : "refs/heads/" + std::string(*selected);
    const auto repo = minigit::Repository::discover();
    const minigit::RepoLock lock(repo.metadata());
    minigit::require_no_transaction(repo);
    const auto entries = minigit::read_reflog(repo, ref);
    std::string text;
    for (std::size_t i = entries.size(); i > 0; --i) {
        const auto& entry = entries[i - 1];
        const auto line = minigit::quote_path(ref) + "@{" + std::to_string(entries.size() - i) + "} " +
            (entry.old_id ? entry.old_id->hex() : "-") + " -> " +
            (entry.new_id ? entry.new_id->hex() : "-") + " " + entry.reason + "\n";
        if (line.size() > minigit::max_file_size - text.size()) throw minigit::Error("reflog output exceeds 64 MiB");
        text += line;
    }
    std::cout << text;
    return 0;
}

}
