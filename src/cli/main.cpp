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
                     "  log [--oneline] [-n count] [revision]  Walk commit history\n"
                     "  tag [-f] [name [revision]] | tag -d name  Manage light tags\n"
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
                std::size_t count = 0;
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), count);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
                    throw UsageError("log count must be a nonnegative integer");
                maximum = count;
                continue;
            }
            if ((options && args[i].starts_with('-')) || revision)
                throw UsageError("log accepts --oneline, -n <count> and one revision");
            revision = args[i];
        }
        const auto entries = minigit::log_history(minigit::Repository::discover(),
            revision.value_or("HEAD"), maximum);
        if (entries.empty() && !oneline && maximum != 0) std::cout << "No commits yet\n";
        for (const auto& entry : entries) {
            if (oneline) {
                std::cout << entry.id.hex().substr(0, 12) << ' '
                          << entry.commit.message.substr(0, entry.commit.message.find('\n')) << '\n';
                continue;
            }
            std::cout << "commit " << entry.id.hex() << '\n';
            for (const auto& parent : entry.commit.parents) std::cout << "parent " << parent.hex() << '\n';
            std::cout << "author " << entry.commit.author << '\n'
                      << "committer " << entry.commit.committer << "\n\n";
            std::string_view remaining(entry.commit.message);
            do {
                const auto newline = remaining.find('\n');
                std::cout << "    " << remaining.substr(0, newline) << '\n';
                if (newline == std::string_view::npos) break;
                remaining.remove_prefix(newline + 1);
            } while (!remaining.empty());
            std::cout << '\n';
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
