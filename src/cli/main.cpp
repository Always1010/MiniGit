#include "minigit/version.hpp"
#include "minigit/bytes.hpp"
#include "minigit/error.hpp"
#include "minigit/fs.hpp"
#include "minigit/object_store.hpp"
#include "minigit/objects.hpp"
#include "minigit/repository.hpp"

#include <filesystem>
#include <iostream>
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
