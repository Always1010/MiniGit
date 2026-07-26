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
        REQUIRE_THROWS_AS(atomic_write(link, to_bytes("overwrite")), Error);
        REQUIRE(to_string(read_file(outside / "data")) == "preserve");
    }
    SECTION("parent symlink") {
        auto link = temp.path() / "link";
        std::filesystem::create_directory_symlink(outside, link);
        REQUIRE_THROWS_AS(read_file(link / "data"), Error);
        REQUIRE_THROWS_AS(atomic_write(link / "data", to_bytes("overwrite")), Error);
    }
    SECTION("dot-dot cannot conceal a traversed symlink") {
        std::filesystem::create_directory(outside / "deep");
        atomic_write(temp.path() / "file", to_bytes("lexical target"));
        atomic_write(outside / "file", to_bytes("operating-system target"));
        auto link = temp.path() / "link";
        std::filesystem::create_directory_symlink(outside / "deep", link);
        const auto traversal = link / "..";
        REQUIRE_THROWS_AS(read_file(traversal / "file"), Error);
        REQUIRE_THROWS_AS(atomic_write(traversal / "file", to_bytes("overwrite")), Error);
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
        REQUIRE_THROWS_AS(atomic_write(fifo, to_bytes("data")), Error);
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
