#include <catch2/catch_test_macros.hpp>
#include "minigit/error.hpp"
#include "minigit/fs.hpp"
#include "minigit/repository.hpp"
#include "temp_directory.hpp"
#include <memory>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
using namespace minigit;
TEST_CASE("Relative filesystem paths and current-directory repositories work", "[repository][filesystem]") {
    test::TempDirectory temp;
    struct RestoreCurrentDirectory {
        std::filesystem::path original = std::filesystem::current_path();
        ~RestoreCurrentDirectory() { std::error_code ignored; std::filesystem::current_path(original, ignored); }
    } restore;
    std::filesystem::current_path(temp.path());
    atomic_write("file", to_bytes("relative"));
    REQUIRE(to_string(read_file("file")) == "relative");
    const auto repo = Repository::init(".");
    REQUIRE(repo.root() == temp.path());
    REQUIRE(Repository::discover(".").root() == temp.path());
    REQUIRE(Repository::init(temp.path() / "").root() == temp.path());
}
TEST_CASE("Repository initializes and discovers from nested paths", "[repository]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path() / "repo");
    REQUIRE(repo.root() == temp.path() / "repo");
    REQUIRE(to_string(read_file(repo.metadata() / "HEAD")) == "ref: refs/heads/main\n");
    REQUIRE(to_string(read_file(repo.metadata() / "config")) == "format=1\nhash=sha256\n");
    REQUIRE(std::filesystem::is_directory(repo.metadata() / "objects"));
    REQUIRE(std::filesystem::is_directory(repo.metadata() / "refs" / "tags"));
    const auto nested = repo.root() / "nested" / "deeper";
    std::filesystem::create_directories(nested);
    REQUIRE(Repository::discover(nested).root() == repo.root());
    atomic_write(nested / "file", to_bytes("working file"));
    REQUIRE(Repository::discover(nested / "file").root() == repo.root());
    REQUIRE_THROWS_AS(Repository::discover(repo.root() / "missing" / ".."), Error);
    REQUIRE_THROWS_AS(Repository::discover(temp.path()), Error);
}
TEST_CASE("Repeated init preserves HEAD, objects and index", "[repository]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    atomic_write(repo.metadata() / "HEAD", to_bytes("ref: refs/heads/custom\n"));
    atomic_write(repo.metadata() / "index", to_bytes("index fixture"));
    atomic_write(repo.metadata() / "objects" / "fixture", to_bytes("object fixture"));
    const auto again = Repository::init(temp.path());
    REQUIRE(again.root() == repo.root());
    REQUIRE(to_string(read_file(repo.metadata() / "HEAD")) == "ref: refs/heads/custom\n");
    REQUIRE(to_string(read_file(repo.metadata() / "index")) == "index fixture");
    REQUIRE(to_string(read_file(repo.metadata() / "objects" / "fixture")) == "object fixture");
}
TEST_CASE("Repository rejects malformed metadata and unsupported versions", "[repository]") {
    test::TempDirectory temp;
    SECTION("unknown metadata directory") {
        std::filesystem::create_directory(temp.path() / ".minigit");
        atomic_write(temp.path() / ".minigit" / "unknown", to_bytes("preserve"));
        REQUIRE_THROWS_AS(Repository::init(temp.path()), Error);
        REQUIRE(to_string(read_file(temp.path() / ".minigit" / "unknown")) == "preserve");
    }
    SECTION("metadata is regular file") {
        atomic_write(temp.path() / ".minigit", to_bytes("unknown"));
        REQUIRE_THROWS_AS(Repository::init(temp.path()), Error);
    }
    SECTION("wrong config") {
        const auto repo = Repository::init(temp.path());
        atomic_write(repo.metadata() / "config", to_bytes("format=2\nhash=sha256\n"));
        REQUIRE_THROWS_AS(Repository::init(temp.path()), Error);
        REQUIRE_THROWS_AS(Repository::discover(temp.path()), Error);
    }
    SECTION("bad HEAD") {
        const auto repo = Repository::init(temp.path());
        atomic_write(repo.metadata() / "HEAD", to_bytes("ref: refs/heads/../../outside\n"));
        REQUIRE_THROWS_AS(Repository::init(temp.path()), Error);
    }
    SECTION("missing objects") {
        const auto repo = Repository::init(temp.path());
        std::filesystem::remove(repo.metadata() / "objects");
        REQUIRE_THROWS_AS(Repository::init(temp.path()), Error);
    }
}
TEST_CASE("Metadata and filesystem operations reject symlinks", "[repository][filesystem]") {
    test::TempDirectory temp;
    auto outside = temp.path() / "outside";
    std::filesystem::create_directory(outside);
    atomic_write(outside / "data", to_bytes("preserve"));
    SECTION("marker symlink") {
        std::filesystem::create_directory_symlink(outside, temp.path() / ".minigit");
        REQUIRE_THROWS_AS(Repository::init(temp.path()), Error);
        REQUIRE_THROWS_AS(Repository::discover(temp.path()), Error);
    }
    SECTION("file symlink") {
        auto link = temp.path() / "link";
        std::filesystem::create_symlink(outside / "data", link);
        REQUIRE_THROWS_AS(read_file(link), Error);
        REQUIRE_THROWS_AS(read_file_data(link), Error);
        REQUIRE_THROWS_AS(atomic_write(link, to_bytes("overwrite")), Error);
        REQUIRE_THROWS_AS(atomic_write_with_permissions(link, to_bytes("overwrite"), 0755), Error);
        REQUIRE(to_string(read_file(outside / "data")) == "preserve");
    }
    SECTION("parent symlink") {
        auto link = temp.path() / "link";
        std::filesystem::create_directory_symlink(outside, link);
        REQUIRE_THROWS_AS(read_file(link / "data"), Error);
        REQUIRE_THROWS_AS(read_file_data(link / "data"), Error);
        REQUIRE_THROWS_AS(atomic_write(link / "data", to_bytes("overwrite")), Error);
        REQUIRE_THROWS_AS(atomic_write_with_permissions(link / "data", to_bytes("overwrite"), 0755), Error);
    }
    SECTION("dot-dot cannot conceal a traversed symlink") {
        std::filesystem::create_directory(outside / "deep");
        atomic_write(temp.path() / "file", to_bytes("lexical target"));
        atomic_write(outside / "file", to_bytes("operating-system target"));
        auto link = temp.path() / "link";
        std::filesystem::create_directory_symlink(outside / "deep", link);
        const auto traversal = link / "..";
        REQUIRE_THROWS_AS(read_file(traversal / "file"), Error);
        REQUIRE_THROWS_AS(read_file_data(traversal / "file"), Error);
        REQUIRE_THROWS_AS(atomic_write(traversal / "file", to_bytes("overwrite")), Error);
        REQUIRE_THROWS_AS(atomic_write_with_permissions(traversal / "file", to_bytes("overwrite"), 0755), Error);
        REQUIRE_THROWS_AS(Repository::init(traversal), Error);
        REQUIRE_THROWS_AS(Repository::discover(traversal), Error);
        REQUIRE(to_string(read_file(temp.path() / "file")) == "lexical target");
        REQUIRE(to_string(read_file(outside / "file")) == "operating-system target");
    }
    SECTION("lock symlink") {
        auto repo = Repository::init(temp.path() / "repo");
        std::filesystem::create_symlink(outside / "data", repo.metadata() / "write.lock");
        REQUIRE_THROWS_AS(RepoLock(repo.metadata()), Error);
    }
    SECTION("FIFO is rejected without waiting for a writer") {
        auto fifo = temp.path() / "fifo";
        REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);
        REQUIRE_THROWS_AS(read_file(fifo), Error);
        REQUIRE_THROWS_AS(read_file_data(fifo), Error);
        REQUIRE_THROWS_AS(atomic_write(fifo, to_bytes("data")), Error);
        REQUIRE_THROWS_AS(atomic_write_with_permissions(fifo, to_bytes("data"), 0755), Error);
    }
}
TEST_CASE("Atomic writes preserve old contents on validation failure", "[filesystem]") {
    test::TempDirectory temp;
    auto target = temp.path() / "file";
    atomic_write(target, to_bytes("old"));
    REQUIRE_THROWS_AS(atomic_write(target, Bytes(max_file_size + 1)), Error);
    REQUIRE(to_string(read_file(target)) == "old");
    atomic_write(target, to_bytes("replacement"));
    REQUIRE(to_string(read_file(target)) == "replacement");
    REQUIRE(std::distance(std::filesystem::directory_iterator(temp.path()), std::filesystem::directory_iterator{}) == 1);
}
TEST_CASE("File data captures exact permissions and explicit atomic writes preserve requested modes", "[filesystem]") {
    test::TempDirectory temp;
    struct RestoreMask {
        mode_t previous = ::umask(0077);
        ~RestoreMask() { ::umask(previous); }
    } mask;
    const auto target = temp.path() / "file";
    const Bytes binary{0, 10, 255, 1};
    atomic_write(target, binary);
    const auto original = read_file_data(target);
    REQUIRE(original.bytes == binary);
    REQUIRE(original.permissions == 0600);
    REQUIRE(read_file(target) == original.bytes);
    SECTION("explicit permissions override restrictive umask with content") {
        atomic_write_with_permissions(target, to_bytes("replacement"), 0755);
        const auto replaced = read_file_data(target);
        REQUIRE(to_string(replaced.bytes) == "replacement");
        REQUIRE(replaced.permissions == 0755);
        atomic_write_with_permissions(target, {}, 0644);
        const auto empty = read_file_data(target);
        REQUIRE(empty.bytes.empty());
        REQUIRE(empty.permissions == 0644);
        atomic_write_with_permissions(target, binary, 0740);
        REQUIRE(read_file_data(target).permissions == 0740);
        REQUIRE(read_file_data(target).bytes == binary);
    }
    SECTION("default writes continue to respect umask") {
        atomic_write_with_permissions(target, binary, 0755);
        atomic_write(target, to_bytes("default"));
        REQUIRE(read_file_data(target).permissions == 0600);
        REQUIRE(to_string(read_file(target)) == "default");
    }
    SECTION("invalid mode preserves contents permissions and directory entries") {
        REQUIRE_THROWS_AS(atomic_write_with_permissions(target, to_bytes("bad"), 010000), Error);
        REQUIRE(read_file_data(target).bytes == original.bytes);
        REQUIRE(read_file_data(target).permissions == original.permissions);
        REQUIRE(std::distance(std::filesystem::directory_iterator(temp.path()), std::filesystem::directory_iterator{}) == 1);
    }
}
TEST_CASE("Permanent repository lock serializes separate processes", "[repository][lock]") {
    test::TempDirectory temp;
    const auto repo = Repository::init(temp.path());
    auto held = std::make_unique<RepoLock>(repo.metadata());
    struct stat before{};
    REQUIRE(::stat((repo.metadata() / "write.lock").c_str(), &before) == 0);
    int notification[2];
    REQUIRE(::pipe(notification) == 0);
    const auto child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        ::close(notification[0]);
        const char started = 's';
        if (::write(notification[1], &started, 1) != 1) ::_exit(2);
        try {
            RepoLock acquired(repo.metadata());
            const char done = 'd';
            if (::write(notification[1], &done, 1) != 1) ::_exit(3);
        } catch (...) { ::_exit(4); }
        ::_exit(0);
    }
    ::close(notification[1]);
    struct Guard {
        pid_t child; int fd;
        ~Guard() { if (child > 0) { ::kill(child, SIGKILL); ::waitpid(child, nullptr, 0); } ::close(fd); }
    } guard{child, notification[0]};
    pollfd ready{notification[0], POLLIN, 0};
    REQUIRE(::poll(&ready, 1, 2000) == 1);
    char signal = 0;
    REQUIRE(::read(notification[0], &signal, 1) == 1);
    REQUIRE(signal == 's');
    REQUIRE(::poll(&ready, 1, 100) == 0);
    held.reset();
    REQUIRE(::poll(&ready, 1, 2000) == 1);
    REQUIRE(::read(notification[0], &signal, 1) == 1);
    REQUIRE(signal == 'd');
    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    guard.child = -1;
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 0);
    struct stat after{};
    REQUIRE(::stat((repo.metadata() / "write.lock").c_str(), &after) == 0);
    REQUIRE(before.st_ino == after.st_ino);
    RepoLock next(repo.metadata());
}
TEST_CASE("Transaction filesystem publication pins nodes preserves modes and refuses unsafe replacement", "[repository][filesystem]") {
    test::TempDirectory temp;
    const auto staging = temp.path() / "staging";
    struct RestoreUmask {
        mode_t previous = ::umask(0777);
        ~RestoreUmask() { ::umask(previous); }
    } mask;
    detail::create_directory_exact(staging, 0750);
    struct stat state{};
    REQUIRE(::stat(staging.c_str(), &state) == 0);
    REQUIRE((state.st_mode & 07777) == 0750);
    atomic_write_with_permissions(staging / "file", to_bytes("prepared"), 0740);
    const auto target = temp.path() / "target";
    detail::rename_staged(staging / "file", target);
    REQUIRE_FALSE(std::filesystem::exists(staging / "file"));
    REQUIRE(read_file_data(target).permissions == 0740);
    REQUIRE(to_string(read_file(target)) == "prepared");
    atomic_write_with_permissions(staging / "file", to_bytes("replacement"), 0644);
    detail::rename_staged(staging / "file", target);
    REQUIRE(to_string(read_file(target)) == "replacement");
    REQUIRE(read_file_data(target).permissions == 0644);
    detail::create_directory_exact(staging / "directory", 0755);
    detail::rename_staged(staging / "directory", temp.path() / "published");
    REQUIRE(std::filesystem::is_directory(temp.path() / "published"));
    detail::chmod_directory(temp.path() / "published", 0710);
    REQUIRE(::stat((temp.path() / "published").c_str(), &state) == 0);
    REQUIRE((state.st_mode & 07777) == 0710);
    detail::chmod_directory(temp.path() / "published", 0000);
    detail::chmod_directory(temp.path() / "published", 0755);
    REQUIRE(::stat((temp.path() / "published").c_str(), &state) == 0);
    REQUIRE((state.st_mode & 07777) == 0755);
    REQUIRE(detail::directory_device(staging) == detail::directory_device(temp.path()));
    REQUIRE(detail::node_device(target) == detail::directory_device(temp.path()));
    detail::create_directory_exact(staging / "directory", 0755);
    REQUIRE_THROWS_AS(detail::rename_staged(staging / "directory", temp.path() / "published"), Error);
    REQUIRE(std::filesystem::exists(staging / "directory"));
    atomic_write_with_permissions(staging / "file", to_bytes("preserve"), 0644);
    REQUIRE_THROWS_AS(detail::rename_staged(staging / "file", temp.path() / "published"), Error);
    REQUIRE(to_string(read_file(staging / "file")) == "preserve");
    std::filesystem::create_symlink(target, staging / "link");
    REQUIRE_THROWS_AS(detail::rename_staged(staging / "link", target), Error);
    REQUIRE_THROWS_AS(detail::remove_regular(staging / "link"), Error);
    REQUIRE_THROWS_AS(detail::node_device(staging / "link"), Error);
    REQUIRE_THROWS_AS(detail::create_directory_exact(staging / "link", 0755), Error);
    REQUIRE_THROWS_AS(detail::remove_empty_directory(staging), Error);
    REQUIRE_THROWS_AS(detail::create_directory_exact(staging / "invalid", 010000), Error);
    REQUIRE_FALSE(std::filesystem::exists(staging / "invalid"));
    detail::remove_regular(target);
    detail::remove_empty_directory(temp.path() / "published");
    REQUIRE_FALSE(std::filesystem::exists(target));
    REQUIRE_FALSE(std::filesystem::exists(temp.path() / "published"));
}
