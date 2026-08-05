#include "minigit/version.hpp"
#include "minigit/bytes.hpp"
#include "minigit/error.hpp"
#include "minigit/fs.hpp"
#include "minigit/object_store.hpp"
#include "minigit/objects.hpp"
#include "minigit/repository.hpp"
#include "minigit/worktree.hpp"
#include "minigit/history.hpp"
#include "minigit/status.hpp"
#include "minigit/dag.hpp"
#include "minigit/tags.hpp"
#include "minigit/format.hpp"
#include "minigit/inspection.hpp"
#include "minigit/transaction.hpp"
#include "minigit/reflog.hpp"
#include "minigit/branches.hpp"
#include "minigit/switch.hpp"
#include "minigit/restore.hpp"
#include "minigit/reset.hpp"
#include "minigit/remove.hpp"
#include "minigit/move.hpp"
#include "minigit/merge.hpp"
#include "minigit/replay.hpp"
#include "minigit/stash.hpp"

#include <charconv>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string_view>
#include <vector>

namespace {
class UsageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

std::size_t parse_count(std::string_view value, std::string_view description) {
    std::size_t count = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), count);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        throw UsageError(std::string(description) + " must be a nonnegative integer");
    return count;
}

enum class HistoryAction { Start, Continue, Abort };
struct HistoryArguments {
    HistoryAction action = HistoryAction::Start;
    std::string revision;
    std::optional<std::string> message;
};
HistoryArguments parse_history_arguments(const std::vector<std::string_view>& args, std::string_view message_option) {
    HistoryArguments parsed;
    bool options = true, action_set = false, revision_set = false;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (options && args[i] == "--") { options = false; continue; }
        if (options && (args[i] == "--continue" || args[i] == "--abort")) {
            if (action_set) throw UsageError("history action may be specified once");
            action_set = true;
            parsed.action = args[i] == "--continue" ? HistoryAction::Continue : HistoryAction::Abort;
            continue;
        }
        if (options && args[i] == message_option) {
            if (parsed.message || i + 1 == args.size()) throw UsageError("message option requires one message");
            parsed.message = args[++i];
            continue;
        }
        if ((options && args[i].starts_with('-')) || revision_set)
            throw UsageError("history operation accepts one revision");
        revision_set = true;
        parsed.revision = args[i];
    }
    if ((!action_set && (!revision_set || parsed.revision.empty())) ||
        (action_set && (revision_set || parsed.message)))
        throw UsageError("history operation requires a revision, or --continue/--abort without other arguments");
    return parsed;
}
int report_conflicts(std::string_view label, std::string_view command,
                     const std::vector<minigit::MergeConflict>& conflicts) {
    std::string output = std::string(label) + " paused; resolve conflicts, add paths, then use " +
        std::string(command) + " --continue or " + std::string(command) + " --abort.\n";
    constexpr std::string_view omitted = "Additional conflict paths omitted; use status to inspect them.\n";
    for (const auto& conflict : conflicts) {
        const auto line = "CONFLICT " + minigit::quote_path(conflict.path) + "\n";
        if (line.size() > minigit::max_file_size - output.size() - omitted.size()) {
            output += omitted;
            break;
        }
        output += line;
    }
    std::cout << output;
    return 1;
}
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

int run(const std::vector<std::string_view>& args) {
    if (args.empty()) throw UsageError("invalid arguments; use --help");
    const auto command = args.front();
    if ((command == "--help" || command == "help") && args.size() == 1) {
        std::cout << "MiniGit - local version control\n"
                     "Usage: minigit <command> [options]\n"
                     "  init [directory]           Initialize a repository\n"
                     "  hash-object [-w] <file>    Hash or store file content\n"
                     "  cat-file -t|-p <object>    Inspect an object\n"
                     "  add [-A] [--] <paths...>  Stage file contents or deletions\n"
                     "  config <key> [value]       Read or set local commit identity\n"
                     "  commit -m <message> [--allow-empty]  Commit the index\n"
                     "  status [--porcelain|-s]    Compare HEAD, index and worktree\n"
                     "  diff [--cached] [-U count] [revision [revision]]  Compare snapshots\n"
                     "  show [object|revision:path|:stage:path]  Inspect saved content\n"
                     "  log [--oneline] [-n count] [revision]  Walk commit history\n"
                     "  tag [-f] [name [revision]] | tag -d name  Manage light tags\n"
                     "  recover                  Recover an interrupted transaction\n"
                     "  reflog [HEAD|branch]     Inspect local reference movements\n"
                     "  branch [-f] [name [revision]] | branch -d|-D name  Manage branches\n"
                     "  switch <branch> | switch -c <name> [start] | switch --detach [revision]\n"
                     "  restore [-s revision] [-S] [-W] [--] <paths...>  Restore selected paths\n"
                     "  reset [--soft|--mixed|--hard] [revision]  Reset HEAD and optional state\n"
                     "  rm [-r] [-f] [--cached] [--] <paths...>  Remove tracked paths\n"
                     "  mv [--] <source> <destination>  Move a tracked file or directory\n"
                     "  merge [-m message] <revision> | merge --continue|--abort\n"
                     "  cherry-pick [--message text] <revision> | cherry-pick --continue|--abort\n"
                     "  revert [--message text] <revision> | revert --continue|--abort\n"
                     "  stash [push [-u] [-m message]] | stash list|show|drop|clear\n"
                     "  stash show|drop [stash@{number}]  Inspect or remove an entry\n"
                     "  stash apply|pop [--index] [stash@{number}]  Restore saved changes\n"
                     "  stash --continue|--abort        Finish or undo a conflicted restore\n"
                     "  --help | --version\n";
        return 0;
    }
    if (command == "--version" && args.size() == 1) {
        std::cout << "minigit " << minigit::version() << '\n';
        return 0;
    }
    if (command == "init") {
        if (args.size() > 2 || (args.size() == 2 && args[1].starts_with('-')))
            throw UsageError("init accepts one directory");
        const auto repo = minigit::Repository::init(args.size() == 2
            ? std::filesystem::path(args[1]) : std::filesystem::current_path());
        std::cout << "Initialized MiniGit repository in " << repo.metadata().string() << '\n';
        return 0;
    }
    if (command == "hash-object") {
        bool write = false;
        bool options = true;
        std::filesystem::path file;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (options && args[i] == "--") { options = false; continue; }
            if (options && args[i] == "-w") { write = true; continue; }
            if ((options && args[i].starts_with('-')) || !file.empty())
                throw UsageError("hash-object accepts [-w] and one file");
            file = args[i];
        }
        if (file.empty()) throw UsageError("hash-object requires a file");
        const auto payload = minigit::read_file(file);
        auto id = minigit::hash_object(minigit::ObjectType::Blob, payload);
        if (write) {
            const auto repo = minigit::Repository::discover();
            const minigit::RepoLock lock(repo.metadata());
            minigit::require_no_transaction(repo);
            id = minigit::ObjectStore(repo.metadata() / "objects").write(minigit::ObjectType::Blob, payload);
        }
        std::cout << id.hex() << '\n';
        return 0;
    }
    if (command == "cat-file") {
        if (args.size() != 3 || (args[1] != "-t" && args[1] != "-p"))
            throw UsageError("cat-file requires -t or -p and a full object ID");
        const auto repo = minigit::Repository::discover();
        const auto object = minigit::ObjectStore(repo.metadata() / "objects").read(minigit::ObjectId::from_hex(args[2]));
        if (args[1] == "-t") {
            switch (object.type) {
            case minigit::ObjectType::Blob: std::cout << "blob\n"; break;
            case minigit::ObjectType::Tree: std::cout << "tree\n"; break;
            case minigit::ObjectType::Commit: std::cout << "commit\n"; break;
            }
        } else if (object.type == minigit::ObjectType::Tree) {
            for (const auto& entry : minigit::decode_tree(object.payload))
                std::cout << entry.mode << ' ' << entry.id.hex() << ' ' << entry.name << '\n';
        } else {
            std::cout.write(reinterpret_cast<const char*>(object.payload.data()),
                            static_cast<std::streamsize>(object.payload.size()));
        }
        if (!std::cout) throw minigit::Error("cannot write output");
        return 0;
    }
    if (command == "add") {
        bool all = false;
        bool options = true;
        std::vector<std::filesystem::path> paths;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (options && args[i] == "--") { options = false; continue; }
            if (options && (args[i] == "-A" || args[i] == "--all")) { all = true; continue; }
            if (options && args[i].starts_with('-')) throw UsageError("unknown add option");
            paths.emplace_back(args[i]);
        }
        if ((!all && paths.empty()) || (all && !paths.empty()))
            throw UsageError("add requires paths, or -A without paths");
        minigit::add_paths(minigit::Repository::discover(), paths, all);
        return 0;
    }
    if (command == "config") {
        if (args.size() < 2 || args.size() > 3 ||
            (args[1] != "user.name" && args[1] != "user.email"))
            throw UsageError("config requires user.name or user.email and an optional value");
        const auto repo = minigit::Repository::discover();
        if (args.size() == 3) minigit::configure_identity(repo, args[1], args[2]);
        else {
            const minigit::RepoLock lock(repo.metadata());
            minigit::require_no_transaction(repo);
            const auto value = minigit::identity_value(repo, args[1]);
            if (!value) throw minigit::Error("identity value is not configured");
            std::cout << *value << '\n';
        }
        return 0;
    }
    if (command == "commit") {
        bool allow_empty = false;
        std::optional<std::string_view> message;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--allow-empty") { allow_empty = true; continue; }
            if (args[i] == "-m" && !message && i + 1 < args.size()) {
                message = args[++i];
                continue;
            }
            throw UsageError("commit requires -m <message> and optional --allow-empty");
        }
        if (!message) throw UsageError("commit requires -m <message>");
        const auto id = minigit::create_commit(minigit::Repository::discover(), *message, allow_empty);
        std::cout << id.hex() << ' ' << message->substr(0, message->find('\n')) << '\n';
        return 0;
    }
    if (command == "status") {
        if (args.size() > 2 || (args.size() == 2 && args[1] != "--porcelain" &&
            args[1] != "--short" && args[1] != "-s"))
            throw UsageError("status accepts --porcelain, --short or -s");
        const auto report = minigit::collect_status(minigit::Repository::discover());
        if (args.size() == 1) {
            if (report.head.symbolic)
                std::cout << "On branch " << minigit::quote_path(report.head.symbolic->substr(11)) << '\n';
            else std::cout << "HEAD detached at " << report.head.id->hex().substr(0, 12) << '\n';
            if (!report.head.id) std::cout << "No commits yet\n";
            if (report.entries.empty()) std::cout << "working tree clean\n";
        }
        for (const auto& entry : report.entries)
            std::cout << entry.staged << entry.unstaged << ' ' << minigit::quote_path(entry.path) << '\n';
        return 0;
    }
    if (command == "diff") {
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
    if (command == "reflog") {
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
    if (command == "recover") {
        if (args.size() != 1) throw UsageError("recover accepts no arguments");
        const auto repo = minigit::Repository::discover();
        const minigit::RepoLock lock(repo.metadata());
        minigit::recover_transaction(repo);
        std::cout << "Recovery completed\n";
        return 0;
    }
    if (command == "show") {
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
    if (command == "log") {
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
    if (command == "switch") {
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
    if (command == "mv") {
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
    if (command == "rm") {
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
    if (command == "reset") {
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
    if (command == "restore") {
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
    if (command == "branch") {
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
    if (command == "merge") {
        const auto parsed = parse_history_arguments(args, "-m");
        minigit::MergeOptions merge;
        switch (parsed.action) {
        case HistoryAction::Start: merge.action = minigit::MergeAction::Start; break;
        case HistoryAction::Continue: merge.action = minigit::MergeAction::Continue; break;
        case HistoryAction::Abort: merge.action = minigit::MergeAction::Abort; break;
        }
        merge.revision = parsed.revision;
        merge.message = parsed.message;
        const auto result = minigit::merge_repository(minigit::Repository::discover(), merge);
        switch (result.outcome) {
        case minigit::MergeOutcome::UpToDate: std::cout << "Already up to date.\n"; break;
        case minigit::MergeOutcome::FastForward: std::cout << "Fast-forward " << result.head.id->hex() << '\n'; break;
        case minigit::MergeOutcome::Merged: std::cout << "Merged " << result.head.id->hex() << '\n'; break;
        case minigit::MergeOutcome::Aborted: std::cout << "Merge aborted.\n"; break;
        case minigit::MergeOutcome::Conflicted: return report_conflicts("Merge", command, result.conflicts);
        }
        return 0;
    }
    if (command == "stash") {
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
    if (command == "cherry-pick" || command == "revert") {
        const auto parsed = parse_history_arguments(args, "--message");
        minigit::ReplayOptions replay;
        switch (parsed.action) {
        case HistoryAction::Start: replay.action = minigit::ReplayAction::Start; break;
        case HistoryAction::Continue: replay.action = minigit::ReplayAction::Continue; break;
        case HistoryAction::Abort: replay.action = minigit::ReplayAction::Abort; break;
        }
        replay.revision = parsed.revision;
        replay.message = parsed.message;
        const auto repo = minigit::Repository::discover();
        const bool cherry = command == "cherry-pick";
        const auto result = cherry ? minigit::cherry_pick_repository(repo, replay) : minigit::revert_repository(repo, replay);
        switch (result.outcome) {
        case minigit::ReplayOutcome::Applied: std::cout << (cherry ? "Cherry-picked " : "Reverted ") << result.head.id->hex() << '\n'; break;
        case minigit::ReplayOutcome::Aborted: std::cout << (cherry ? "Cherry-pick aborted.\n" : "Revert aborted.\n"); break;
        case minigit::ReplayOutcome::Conflicted: return report_conflicts(cherry ? "Cherry-pick" : "Revert", command, result.conflicts);
        }
        return 0;
    }
    if (command == "tag") {
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
    throw UsageError("invalid arguments; use --help");
}
} // namespace

int main(int argc, char* argv[]) {
    try {
        const std::vector<std::string_view> args(argv + 1, argv + argc);
        const int result = run(args);
        std::cout.flush();
        if (!std::cout) throw minigit::Error("cannot write output");
        return result;
    } catch (const UsageError& error) {
        std::cerr << "minigit: " << error.what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "minigit: " << error.what() << '\n';
        return 1;
    }
}
