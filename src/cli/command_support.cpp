#include "command_support.hpp"
#include "minigit/format.hpp"
#include "minigit/fs.hpp"
#include <charconv>
#include <iostream>
namespace minigit::cli {
std::size_t parse_count(std::string_view value, std::string_view description) {
    std::size_t count = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), count);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        throw UsageError(std::string(description) + " must be a nonnegative integer");
    return count;
}

HistoryArguments parse_history_arguments(const std::vector<std::string_view>& args,
                                        std::optional<std::string_view> message_option) {
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
        if (options && message_option && args[i] == *message_option) {
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
}
