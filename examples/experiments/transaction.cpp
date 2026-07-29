#include "minigit/error.hpp"
#include "minigit/index.hpp"
#include "minigit/object_store.hpp"
#include "minigit/transaction.hpp"
#include <iostream>
#include <utility>
namespace {
struct Interrupted {};
}
int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "Usage: minigit_transaction_lab <empty-directory>\n"; return 2; }
    try {
        const std::filesystem::path path(argv[1]);
        if (std::filesystem::exists(path) &&
            (!std::filesystem::is_directory(path) || !std::filesystem::is_empty(path)))
            throw minigit::Error("Experiment requires an empty directory");
        const auto repo = minigit::Repository::init(path);
        minigit::atomic_write_with_permissions(repo.root() / "a", minigit::to_bytes("old A\n"), 0644);
        minigit::atomic_write_with_permissions(repo.root() / "b", minigit::to_bytes("old B\n"), 0644);
        const minigit::RepoLock lock(repo.metadata());
        const auto before_a = minigit::capture_work_node(repo, "a");
        const auto before_b = minigit::capture_work_node(repo, "b");
        minigit::save_index(repo, {{"a", "100644", *before_a.blob}, {"b", "100644", *before_b.blob}});
        minigit::ObjectStore store(repo.metadata() / "objects");
        const auto after_a = store.write(minigit::ObjectType::Blob, minigit::to_bytes("new A\n"));
        const auto after_b = store.write(minigit::ObjectType::Blob, minigit::to_bytes("new B\n"));
        minigit::TransactionPlan plan;
        plan.work = {{"a", before_a, {minigit::NodeKind::File, after_a, 0644}},
                     {"b", before_b, {minigit::NodeKind::File, after_b, 0644}}};
        minigit::execute_transaction(repo, std::move(plan), [](std::size_t, std::string_view label) {
            // Deliberately stop after one durable write, before Committed.
            if (label == "work.write:a") throw Interrupted{};
        });
        throw minigit::Error("Expected interruption point was not reached");
    } catch (const Interrupted&) {
        std::cout << "Stopped with Prepared journal: a is new, b is old. Run minigit recover.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
