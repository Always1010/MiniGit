#include "commands.hpp"
#include "command_support.hpp"
#include "minigit/repository.hpp"
#include "minigit/transaction.hpp"
#include "minigit/merge.hpp"
#include "minigit/replay.hpp"
#include "minigit/rebase.hpp"
#include "minigit/gc.hpp"
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
namespace minigit::cli {
int run_merge(const Arguments& args) {
    const auto command = args.front();
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

int run_rebase(const Arguments& args) {
    const auto command = args.front();
    const auto parsed = parse_history_arguments(args, std::nullopt);
    minigit::RebaseOptions rebase;
    switch (parsed.action) {
    case HistoryAction::Start: rebase.action = minigit::RebaseAction::Start; break;
    case HistoryAction::Continue: rebase.action = minigit::RebaseAction::Continue; break;
    case HistoryAction::Abort: rebase.action = minigit::RebaseAction::Abort; break;
    }
    rebase.revision = parsed.revision;
    const auto result = minigit::rebase_repository(minigit::Repository::discover(), rebase);
    switch (result.outcome) {
    case minigit::RebaseOutcome::UpToDate: std::cout << "Already up to date.\n"; break;
    case minigit::RebaseOutcome::Applied: std::cout << "Rebased " << result.head.id->hex() << '\n'; break;
    case minigit::RebaseOutcome::Aborted: std::cout << "Rebase aborted.\n"; break;
    case minigit::RebaseOutcome::Conflicted: return report_conflicts("Rebase", command, result.conflicts);
    }
    return 0;
}

int run_replay(const Arguments& args) {
    const auto command = args.front();
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

int run_gc(const Arguments& args) {
    if (args.size() > 2 || (args.size() == 2 && args[1] != "--prune-now"))
        throw UsageError("gc accepts only --prune-now");
    const auto result = minigit::collect_garbage(minigit::Repository::discover(),
                                               {args.size() == 2});
    std::cout << "GC retained " << result.retained << " objects; removed " << result.removed << " objects.\n";
    return 0;
}

int run_recover(const Arguments& args) {
    if (args.size() != 1) throw UsageError("recover accepts no arguments");
    const auto repo = minigit::Repository::discover();
    const minigit::RepoLock lock(repo.metadata());
    minigit::recover_transaction(repo);
    std::cout << "Recovery completed\n";
    return 0;
}

}
