#pragma once
#include "commands.hpp"
#include "minigit/merge_snapshot.hpp"
#include <optional>
#include <string>
namespace minigit::cli {
std::size_t parse_count(std::string_view value, std::string_view description);
enum class HistoryAction { Start, Continue, Abort };
struct HistoryArguments {
    HistoryAction action = HistoryAction::Start;
    std::string revision;
    std::optional<std::string> message;
};
HistoryArguments parse_history_arguments(const Arguments& args,
                                        std::optional<std::string_view> message_option);
int report_conflicts(std::string_view label, std::string_view command,
                     const std::vector<MergeConflict>& conflicts);
}
