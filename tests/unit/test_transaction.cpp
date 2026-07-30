#include <catch2/catch_test_macros.hpp>
#include "minigit/transaction.hpp"
#include "minigit/error.hpp"
#include "minigit/index.hpp"
#include "minigit/object_store.hpp"
#include "minigit/refs.hpp"
#include "minigit/snapshot.hpp"
#include "minigit/worktree.hpp"
#include "temp_directory.hpp"
#include <algorithm>
#include <map>
#include <sys/wait.h>
#include <unistd.h>
using namespace minigit;
namespace {
void write(const Repository& repo, std::string_view path, std::string_view bytes, std::uint32_t permissions = 0600) {
    const auto target = repo.root() / std::string(path);
    detail::ensure_directory(target.parent_path());
    atomic_write_with_permissions(target, to_bytes(bytes), permissions);
}
NodeImage file_image(const Repository& repo, std::string_view bytes, std::uint32_t permissions) {
    return {NodeKind::File, ObjectStore(repo.metadata() / "objects").write(ObjectType::Blob, to_bytes(bytes)), permissions};
}
NodeImage actual(const Repository& repo, std::string_view path) {
    if (const auto file = read_worktree_file(repo, path)) return {NodeKind::File, file->state.id, file->data.permissions};
    std::error_code ec;
    const auto state = std::filesystem::symlink_status(repo.root() / std::string(path), ec);
    if (!ec && std::filesystem::is_directory(state))
        return {NodeKind::Directory, std::nullopt, static_cast<std::uint32_t>(state.permissions()) & 07777};
    if (ec && ec != std::errc::no_such_file_or_directory && ec != std::errc::not_a_directory) throw Error(ec.message());
    return {};
}
std::optional<Bytes> metadata_bytes(const Repository& repo, std::string_view path) {
    const auto target = repo.metadata() / std::string(path);
    if (!std::filesystem::exists(target)) return std::nullopt;
    return read_file(target);
}
ObjectId commit(const Repository& repo, const Snapshot& snapshot) {
    return ObjectStore(repo.metadata() / "objects").write(ObjectType::Commit,
        encode_commit({write_snapshot(repo, snapshot), {}, "Test <a@b> 0 +0000", "Test <a@b> 0 +0000", "transaction fixture"}));
}
struct Fixture {
    test::TempDirectory temp;
    Repository repo = Repository::init(temp.path());
    TransactionPlan plan;
    ObjectId next_commit;
    Fixture() {
        RepoLock lock(repo.metadata());
        write(repo, "dir/child", "old child", 0640);
        detail::chmod_directory(repo.root() / "dir", 0700);
        write(repo, "file", "old file", 0640);
        write(repo, "stay", "old stay", 0600);
        plan.work = {
            {"dir", capture_work_node(repo, "dir"), file_image(repo, "new directory replacement", 0751)},
            {"dir/child", capture_work_node(repo, "dir/child"), {}},
            {"file", capture_work_node(repo, "file"), {NodeKind::Directory, std::nullopt, 0750}},
            {"file/new", capture_work_node(repo, "file/new"), file_image(repo, "new child", 0600)},
            {"stay", capture_work_node(repo, "stay"), file_image(repo, "new stay", 0644)}
        };
        Index before, after;
        Snapshot snapshot;
        for (const auto& change : plan.work) {
            if (change.before.kind == NodeKind::File) before.push_back({change.path, "100644", *change.before.blob});
            if (change.after.kind == NodeKind::File) {
                const auto mode = (change.after.permissions & 0111) ? "100755" : "100644";
                after.push_back({change.path, mode, *change.after.blob});
                snapshot.emplace(change.path, FileState{mode, *change.after.blob});
            }
        }
        save_index(repo, before);
        next_commit = commit(repo, snapshot);
        plan.metadata = {
            {"HEAD", read_file(repo.metadata() / "HEAD"), to_bytes("ref: refs/heads/topic/sub\n")},
            {"index", encode_index(before), encode_index(after)},
            {"refs/heads/topic/sub", std::nullopt, to_bytes(next_commit.hex() + "\n")}
        };
    }
    void assert_state(bool after) const {
        for (const auto& change : plan.work) {
            INFO(change.path);
            REQUIRE(actual(repo, change.path) == (after ? change.after : change.before));
        }
        for (const auto& change : plan.metadata) {
            INFO(change.path);
            REQUIRE(metadata_bytes(repo, change.path) == (after ? change.after : change.before));
        }
    }
};
void child(const std::function<void()>& operation, int expected) {
    const auto pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        try { operation(); ::_exit(0); }
        catch (...) { ::_exit(90); }
    }
    int state = 0;
    REQUIRE(::waitpid(pid, &state, 0) == pid);
    REQUIRE(WIFEXITED(state));
    REQUIRE(WEXITSTATUS(state) == expected);
}
void crash_execute(Fixture& fixture, std::size_t step) {
    child([&] {
        RepoLock lock(fixture.repo.metadata());
        execute_transaction(fixture.repo, fixture.plan, [=](std::size_t current, std::string_view) { if (current == step) ::_exit(77); });
    }, 77);
}
void recover_twice(const Repository& repo) {
    RepoLock lock(repo.metadata());
    recover_transaction(repo);
    recover_transaction(repo);
    REQUIRE_NOTHROW(require_no_transaction(repo));
    REQUIRE(transaction_roots(repo).empty());
}
std::vector<std::string> execution_steps() {
    Fixture fixture;
    RepoLock lock(fixture.repo.metadata());
    std::vector<std::string> labels;
    execute_transaction(fixture.repo, fixture.plan, [&](std::size_t, std::string_view label) { labels.emplace_back(label); });
    fixture.assert_state(true);
    return labels;
}
std::size_t find_step(const std::vector<std::string>& labels, std::string_view label) {
    const auto found = std::find(labels.begin(), labels.end(), label);
    REQUIRE(found != labels.end());
    return static_cast<std::size_t>(found - labels.begin()) + 1;
}
void cli_recover(const Repository& repo) {
    const auto executable = std::filesystem::read_symlink("/proc/self/exe").parent_path() / "minigit";
    child([&] {
        if (::chdir(repo.root().c_str()) != 0) ::_exit(91);
        ::execl(executable.c_str(), executable.c_str(), "recover", static_cast<char*>(nullptr));
        ::_exit(92);
    }, 0);
}
}
TEST_CASE("Transaction captures actual bytes and permissions and publishes both images", "[transaction]") {
    Fixture fixture;
    RepoLock lock(fixture.repo.metadata());
    const auto captured = capture_work_node(fixture.repo, "stay");
    REQUIRE(captured == fixture.plan.work.back().before);
    REQUIRE(to_string(ObjectStore(fixture.repo.metadata() / "objects").read(*captured.blob).payload) == "old stay");
    REQUIRE(capture_work_node(fixture.repo, "file/hidden").kind == NodeKind::Absent);
    REQUIRE(capture_work_node(fixture.repo, "missing/child").kind == NodeKind::Absent);
    execute_transaction(fixture.repo, fixture.plan);
    fixture.assert_state(true);
    REQUIRE_NOTHROW(require_no_transaction(fixture.repo));
    recover_transaction(fixture.repo);
    fixture.assert_state(true);
    REQUIRE(transaction_roots(fixture.repo).empty());
}
TEST_CASE("Transaction recovers every durable execution hook including a real CLI recovery", "[transaction]") {
    const auto labels = execution_steps();
    const auto committed = find_step(labels, "journal.committed");
    const auto prepared = find_step(labels, "journal.prepared");
    for (std::size_t step = 1; step <= labels.size(); ++step) {
        INFO("crash " << step << " " << labels[step - 1]);
        Fixture fixture;
        crash_execute(fixture, step);
        if (step == prepared) {
            { RepoLock lock(fixture.repo.metadata()); REQUIRE_THROWS_AS(require_no_transaction(fixture.repo), Error); }
            cli_recover(fixture.repo);
        }
        recover_twice(fixture.repo);
        fixture.assert_state(step >= committed);
    }
}
TEST_CASE("Transaction recovery itself can crash and retry Prepared and Committed states", "[transaction]") {
    const auto labels = execution_steps();
    for (const bool committed : {false, true}) {
        const auto crash = find_step(labels, committed ? "journal.committed" : "work.mkdir:file");
        Fixture pilot;
        crash_execute(pilot, crash);
        std::vector<std::string> recovery_labels;
        {
            RepoLock lock(pilot.repo.metadata());
            recover_transaction(pilot.repo, [&](std::size_t, std::string_view label) { recovery_labels.emplace_back(label); });
        }
        pilot.assert_state(committed);
        REQUIRE_FALSE(recovery_labels.empty());
        for (std::size_t step = 1; step <= recovery_labels.size(); ++step) {
            INFO("recovery state " << committed << " step " << step << " " << recovery_labels[step - 1]);
            Fixture fixture;
            crash_execute(fixture, crash);
            child([&] {
                RepoLock lock(fixture.repo.metadata());
                recover_transaction(fixture.repo, [=](std::size_t current, std::string_view) { if (current == step) ::_exit(77); });
            }, 77);
            recover_twice(fixture.repo);
            fixture.assert_state(committed);
        }
    }
}
TEST_CASE("Transaction validates all plans before publishing a journal", "[transaction]") {
    Fixture fixture;
    RepoLock lock(fixture.repo.metadata());
    auto plan = fixture.plan;
    bool tested = false;
    SECTION("missing backup") { plan.work.back().after.blob = ObjectId::from_hex(std::string(64, 'f')); }
    SECTION("wrong typed backup") { plan.work.back().after.blob = fixture.next_commit; }
    SECTION("unsafe path") { plan.work.back().path = "../escape"; }
    SECTION("overlong path") { plan.work.back().path = std::string(4097, 'a'); }
    SECTION("excess depth") { plan.work.back().path.clear(); for (int i = 0; i < 129; ++i) plan.work.back().path += (i ? "/a" : "a"); }
    SECTION("invalid permission") { plan.work.back().after.permissions = 010000; }
    SECTION("unreadable file") { plan.work.back().after.permissions = 0000; }
    SECTION("untraversable directory") { plan.work[2].after.permissions = 0000; }
    SECTION("unwritable parent directory") { plan.work[2].after.permissions = 0500; }
    SECTION("invalid absent image") { plan.work[1].after.permissions = 0644; }
    SECTION("duplicate work path") { plan.work.push_back(plan.work.back()); }
    SECTION("stale before image") { plan.work.back().before = plan.work.back().after; }
    SECTION("file ancestor conflict") { plan.work[2].after = plan.work[0].after; }
    SECTION("unrecorded directory child") {
        write(fixture.repo, "dir/unrecorded", "outside");
        REQUIRE_THROWS_AS(execute_transaction(fixture.repo, plan), Error);
        REQUIRE(to_string(read_file(fixture.repo.root() / "dir/unrecorded")) == "outside");
        std::filesystem::remove(fixture.repo.root() / "dir/unrecorded");
        tested = true;
    }
    SECTION("unrecorded directory symlink") {
        std::filesystem::create_symlink(fixture.repo.root() / "stay", fixture.repo.root() / "dir/link");
        REQUIRE_THROWS_AS(execute_transaction(fixture.repo, plan), Error);
        REQUIRE(std::filesystem::is_symlink(fixture.repo.root() / "dir/link"));
        std::filesystem::remove(fixture.repo.root() / "dir/link");
        tested = true;
    }
    SECTION("invalid index") { plan.metadata[1].after = to_bytes("not index"); }
    SECTION("unknown metadata") { plan.metadata.push_back({"operation", std::nullopt, to_bytes("unknown")}); }
    SECTION("undefined logs") { plan.metadata.push_back({"logs/HEAD", std::nullopt, to_bytes("unknown")}); }
    SECTION("metadata ancestor conversion") { plan.metadata.push_back({"refs/heads/topic", std::nullopt, to_bytes(fixture.next_commit.hex() + "\n")}); }
    SECTION("bad symbolic HEAD") { plan.metadata[0].after = to_bytes("ref: refs/tags/tag\n"); }
    SECTION("delete HEAD") { plan.metadata[0].after.reset(); }
    SECTION("absent HEAD before image") { plan.metadata[0].before.reset(); }
    SECTION("wrong typed ref") { plan.metadata[2].after = to_bytes(plan.work.back().after.blob->hex() + "\n"); }
    if (!tested) REQUIRE_THROWS_AS(execute_transaction(fixture.repo, plan), Error);
    REQUIRE_FALSE(std::filesystem::exists(fixture.repo.metadata() / "transaction"));
    REQUIRE_FALSE(std::filesystem::exists(fixture.repo.metadata() / "transaction-staging"));
    fixture.assert_state(false);
}
namespace {
std::map<std::string, Bytes> all_metadata(const Repository& repo) {
    std::map<std::string, Bytes> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(repo.metadata()))
        if (!entry.is_symlink() && entry.is_regular_file())
            result.emplace(entry.path().lexically_relative(repo.metadata()).generic_string(), read_file(entry.path()));
    return result;
}
void checksum(Bytes& journal) {
    const auto value = sha256(std::span<const std::uint8_t>(journal).first(journal.size() - 32));
    std::copy(value.bytes().begin(), value.bytes().end(), journal.end() - 32);
}
}
TEST_CASE("Transaction refuses damaged journals backups staging and external edits before restoring", "[transaction]") {
    const auto labels = execution_steps();
    Fixture fixture;
    crash_execute(fixture, find_step(labels, "journal.prepared"));
    RepoLock lock(fixture.repo.metadata());
    const auto journal_path = fixture.repo.metadata() / "transaction";
    SECTION("journal checksum") {
        auto bytes = read_file(journal_path);
        bytes[8] ^= 1;
        atomic_write(journal_path, bytes);
    }
    SECTION("authenticated unknown state") {
        auto bytes = read_file(journal_path);
        bytes[8] = 2;
        checksum(bytes);
        atomic_write(journal_path, bytes);
    }
    SECTION("authenticated unsupported version") {
        auto bytes = read_file(journal_path);
        bytes[7] = 2;
        checksum(bytes);
        atomic_write(journal_path, bytes);
    }
    SECTION("truncated journal") { atomic_write(journal_path, to_bytes("MGTR")); }
    SECTION("missing backup") {
        std::filesystem::remove(ObjectStore(fixture.repo.metadata() / "objects").object_path(*fixture.plan.work.back().before.blob));
    }
    SECTION("corrupt backup") {
        atomic_write(ObjectStore(fixture.repo.metadata() / "objects").object_path(*fixture.plan.work.back().before.blob), to_bytes("broken zlib"));
    }
    SECTION("external work edit") { write(fixture.repo, "stay", "outside writer", 0600); }
    SECTION("external metadata edit") { atomic_write(fixture.repo.metadata() / "HEAD", to_bytes("ref: refs/heads/outside\n")); }
    SECTION("modified stage") {
        atomic_write_with_permissions(fixture.repo.metadata() / "transaction-staging/a-w-4-f", to_bytes("outside stage"), 0644);
    }
    SECTION("unknown staging entry") { atomic_write(fixture.repo.metadata() / "transaction-staging/unknown", to_bytes("unknown")); }
    SECTION("symlink staging entry") {
        std::filesystem::create_symlink(fixture.repo.root() / "stay", fixture.repo.metadata() / "transaction-staging/a-w-99999-f");
    }
    const auto before = all_metadata(fixture.repo);
    std::map<std::string, NodeImage> work;
    for (const auto& change : fixture.plan.work) work.emplace(change.path, actual(fixture.repo, change.path));
    REQUIRE_THROWS_AS(recover_transaction(fixture.repo), Error);
    REQUIRE(all_metadata(fixture.repo) == before);
    for (const auto& [path, node] : work) REQUIRE(actual(fixture.repo, path) == node);
    REQUIRE(std::filesystem::is_regular_file(journal_path));
}
TEST_CASE("Transaction rechecks long staging preparation and cleans only canonical orphan entries", "[transaction]") {
    Fixture fixture;
    RepoLock lock(fixture.repo.metadata());
    bool injected = false;
    SECTION("work changes during preparation") {
        REQUIRE_THROWS_AS(execute_transaction(fixture.repo, fixture.plan, [&](std::size_t, std::string_view label) {
            if (!injected && label.starts_with("staging.publish:")) {
                injected = true;
                write(fixture.repo, "stay", "outside writer", 0600);
            }
        }), Error);
        REQUIRE(injected);
        REQUIRE_FALSE(std::filesystem::exists(fixture.repo.metadata() / "transaction"));
        REQUIRE_THROWS_AS(require_no_transaction(fixture.repo), Error);
        recover_transaction(fixture.repo);
        REQUIRE(to_string(read_file(fixture.repo.root() / "stay")) == "outside writer");
        REQUIRE_NOTHROW(require_no_transaction(fixture.repo));
    }
    SECTION("stage changes during preparation") {
        REQUIRE_THROWS_AS(execute_transaction(fixture.repo, fixture.plan, [&](std::size_t, std::string_view label) {
            if (!injected && label == "staging.publish:a-w-4-f") {
                injected = true;
                atomic_write_with_permissions(fixture.repo.metadata() / "transaction-staging/a-w-4-f", to_bytes("outside stage"), 0644);
            }
        }), Error);
        REQUIRE(injected);
        REQUIRE_FALSE(std::filesystem::exists(fixture.repo.metadata() / "transaction"));
        fixture.assert_state(false);
        recover_transaction(fixture.repo);
        REQUIRE_NOTHROW(require_no_transaction(fixture.repo));
    }
    SECTION("unknown orphan contents remain untouched") {
        detail::create_directory_exact(fixture.repo.metadata() / "transaction-staging", 0700);
        atomic_write(fixture.repo.metadata() / "transaction-staging/unknown", to_bytes("outside"));
        REQUIRE_THROWS_AS(recover_transaction(fixture.repo), Error);
        REQUIRE(to_string(read_file(fixture.repo.metadata() / "transaction-staging/unknown")) == "outside");
    }
}
TEST_CASE("Transaction preserves semantic roots and safely prunes deleted reference parents", "[transaction]") {
    Fixture fixture;
    const auto labels = execution_steps();
    crash_execute(fixture, find_step(labels, "journal.prepared"));
    {
        RepoLock lock(fixture.repo.metadata());
        const auto roots = transaction_roots(fixture.repo);
        REQUIRE(roots.contains(fixture.next_commit));
        for (const auto& change : fixture.plan.work)
            for (const auto* image : {&change.before, &change.after}) if (image->blob) REQUIRE(roots.contains(*image->blob));
        for (const auto& change : fixture.plan.metadata)
            for (const auto* bytes : {&change.before, &change.after}) if (*bytes) REQUIRE(roots.contains(hash_object(ObjectType::Blob, **bytes)));
        recover_transaction(fixture.repo);
        fixture.assert_state(false);
        execute_transaction(fixture.repo, fixture.plan);
        fixture.assert_state(true);
        const auto path = fixture.repo.metadata() / "refs/heads/topic/sub";
        TransactionPlan deletion{{}, {{"refs/heads/topic/sub", read_file(path), std::nullopt}}};
        execute_transaction(fixture.repo, deletion);
        REQUIRE_FALSE(std::filesystem::exists(fixture.repo.metadata() / "refs/heads/topic"));
        REQUIRE(std::filesystem::is_directory(fixture.repo.metadata() / "refs/heads"));
        // A parent may now become a ref file without an old empty directory blocking it.
        TransactionPlan recreation{{}, {{"refs/heads/topic", std::nullopt, to_bytes(fixture.next_commit.hex() + "\n")}}};
        execute_transaction(fixture.repo, recreation);
        REQUIRE(read_ref(fixture.repo, "refs/heads/topic") == fixture.next_commit);
        recover_transaction(fixture.repo);
    }
    SECTION("all index stages are semantic roots") {
        RepoLock lock(fixture.repo.metadata());
        const auto stage1 = file_image(fixture.repo, "stage one", 0600);
        const auto stage2 = file_image(fixture.repo, "stage two", 0600);
        const auto stage3 = file_image(fixture.repo, "stage three", 0600);
        const auto old_index = read_file(fixture.repo.metadata() / "index");
        const auto conflict = encode_index({{"conflict", "100644", *stage1.blob, 1}, {"conflict", "100644", *stage2.blob, 2}, {"conflict", "100644", *stage3.blob, 3}});
        TransactionPlan plan{{}, {{"index", old_index, conflict}}};
        REQUIRE_THROWS_AS(execute_transaction(fixture.repo, plan, [](std::size_t, std::string_view label) {
            if (label == "journal.prepared") throw Error("injected interruption");
        }), Error);
        const auto roots = transaction_roots(fixture.repo);
        REQUIRE(roots.contains(*stage1.blob));
        REQUIRE(roots.contains(*stage2.blob));
        REQUIRE(roots.contains(*stage3.blob));
        recover_transaction(fixture.repo);
        REQUIRE(read_file(fixture.repo.metadata() / "index") == old_index);
    }
}
TEST_CASE("Transaction pure budget validates combined counts payloads paths and journal without repository IO", "[transaction]") {
    TransactionPlan plan;
    SECTION("budget accepts unsaved hashes and unvalidated schemas without repository access") {
        plan.work.push_back({"../not-a-repository-path", {NodeKind::File, ObjectId{}, 0644}, {}});
        plan.metadata.push_back({"unknown-metadata", to_bytes("not a metadata schema"), Bytes{}});
        REQUIRE_NOTHROW(validate_transaction_budget(plan));
        REQUIRE(plan.work.front().before.blob == ObjectId{});
        REQUIRE(plan.metadata.front().after->empty());
    }
    SECTION("work and metadata count together and guard subtraction") {
        plan.work.resize(100000);
        REQUIRE_NOTHROW(validate_transaction_budget(plan));
        plan.metadata.push_back({"", std::nullopt, std::nullopt});
        REQUIRE_THROWS_AS(validate_transaction_budget(plan), Error);
        plan.metadata.clear();
        plan.work.resize(100001);
        REQUIRE_THROWS_AS(validate_transaction_budget(plan), Error);
    }
    SECTION("before and after metadata payloads share the byte budget") {
        plan.metadata.push_back({"index", std::nullopt, std::nullopt});
        auto& change = plan.metadata.front();
        change.before.emplace(max_file_size / 2, 0);
        change.after.emplace(max_file_size / 2 + 1, 0);
        REQUIRE_THROWS_AS(validate_transaction_budget(plan), Error);
        REQUIRE(change.after->size() == max_file_size / 2 + 1);
        change.after->pop_back();
        REQUIRE_NOTHROW(validate_transaction_budget(plan));
    }
    SECTION("journal overhead and optional empty payloads count even below the path byte cap") {
        plan.work.push_back({"", {NodeKind::File, ObjectId{}, 0644}, {NodeKind::File, ObjectId{}, 0644}});
        auto& path = plan.work.front().path;
        // A single deliberately invalid schema keeps the size boundary test
        // small in entry count, with no duplicate encoded buffer or IO.
        path.reserve(max_file_size + 1);
        path.assign(max_file_size - 129, 'p'); // 49 header + 4 + 2*(6+32).
        REQUIRE_NOTHROW(validate_transaction_budget(plan));
        plan.metadata.push_back({"", Bytes{}, std::nullopt});
        try {
            validate_transaction_budget(plan);
            FAIL("a present empty metadata payload still adds a journal Blob ID");
        } catch (const Error& error) {
            REQUIRE(std::string(error.what()).find("journal") != std::string::npos);
        }
        path.resize(max_file_size);
        plan.metadata.front().path = "x";
        try {
            validate_transaction_budget(plan);
            FAIL("work and metadata paths must share one byte budget");
        } catch (const Error& error) {
            REQUIRE(std::string(error.what()).find("事务数据超过") != std::string::npos);
        }
    }
}
