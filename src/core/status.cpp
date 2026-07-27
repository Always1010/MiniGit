#include "minigit/status.hpp"
#include "minigit/error.hpp"
#include "minigit/history.hpp"
#include "minigit/object_store.hpp"
#include <map>
#include <set>
#include <utility>
namespace minigit {
namespace {
std::pair<char, char> conflict_codes(unsigned mask) {
    switch (mask) {
    case 14: return {'U', 'U'};
    case 12: return {'A', 'A'};
    case 6: return {'U', 'D'};
    case 10: return {'D', 'U'};
    case 4: return {'A', 'U'};
    case 8: return {'U', 'A'};
    case 2: return {'D', 'D'};
    default: throw Error("索引冲突阶段组合非法");
    }
}
}
StatusReport collect_status(const Repository& repo) {
    RepoLock lock(repo.metadata());
    StatusReport report{read_head(repo), {}};
    const auto head = head_snapshot(repo);
    const auto index = load_index(repo);
    const ObjectStore store(repo.metadata() / "objects");
    std::set<ObjectId> checked;
    std::set<std::string> indexed_paths;
    Snapshot staged;
    std::map<std::string, unsigned> conflicts;
    for (const auto& entry : index) {
        if (checked.insert(entry.id).second && store.read(entry.id).type != ObjectType::Blob)
            throw Error("索引文件引用的对象不是 blob");
        indexed_paths.insert(entry.path);
        if (entry.stage == 0) staged.emplace(entry.path, FileState{entry.mode, entry.id});
        else conflicts[entry.path] |= 1U << entry.stage;
    }
    const auto work = scan_worktree(repo);
    std::set<std::string> paths = indexed_paths;
    for (const auto& [path, state] : head) {
        (void)state;
        paths.insert(path);
    }
    for (const auto& path : paths) {
        const auto conflict = conflicts.find(path);
        if (conflict != conflicts.end()) {
            const auto [x, y] = conflict_codes(conflict->second);
            report.entries.push_back({path, x, y});
            continue;
        }
        StatusEntry entry{path};
        const auto before = head.find(path);
        const auto current = staged.find(path);
        if (before == head.end()) entry.staged = 'A';
        else if (current == staged.end()) entry.staged = 'D';
        else if (before->second != current->second) entry.staged = 'M';
        if (current != staged.end()) {
            const auto working = work.find(path);
            if (working == work.end()) entry.unstaged = 'D';
            else if (current->second != working->second) entry.unstaged = 'M';
        }
        if (entry.staged != ' ' || entry.unstaged != ' ') report.entries.push_back(std::move(entry));
    }
    for (const auto& [path, state] : work) {
        (void)state;
        if (!indexed_paths.contains(path)) report.entries.push_back({path, '?', '?'});
    }
    return report;
}
std::string quote_path(std::string_view path) {
    bool quoted = false;
    for (const unsigned char c : path) {
        if (c <= ' ' || c == 127 || c == '"' || c == '\\') {
            quoted = true;
            break;
        }
    }
    if (!quoted) return std::string(path);
    std::string result = "\"";
    for (const unsigned char c : path) {
        switch (c) {
        case '\n': result += "\\n"; break;
        case '\t': result += "\\t"; break;
        case '\r': result += "\\r"; break;
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        default:
            if (c < 32 || c == 127) {
                result += '\\';
                result += static_cast<char>('0' + ((c >> 6) & 7));
                result += static_cast<char>('0' + ((c >> 3) & 7));
                result += static_cast<char>('0' + (c & 7));
            } else result += static_cast<char>(c);
        }
    }
    result += '"';
    return result;
}
}
