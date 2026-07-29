#include "minigit/format.hpp"
#include "minigit/error.hpp"
#include "minigit/fs.hpp"
namespace minigit {
namespace {
void append(std::string& output, std::string_view value) {
    if (value.size() > max_file_size - output.size()) throw Error("格式化输出超过 64 MiB 限制");
    output.append(value);
}
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
std::string format_commit(const ObjectId& id, const Commit& commit, bool oneline) {
    std::string output;
    if (oneline) {
        append(output, id.hex().substr(0, 12));
        append(output, " ");
        append(output, std::string_view(commit.message).substr(0, commit.message.find('\n')));
        append(output, "\n");
        return output;
    }
    append(output, "commit ");
    append(output, id.hex());
    append(output, "\n");
    for (const auto& parent : commit.parents) {
        append(output, "parent ");
        append(output, parent.hex());
        append(output, "\n");
    }
    append(output, "author ");
    append(output, commit.author);
    append(output, "\ncommitter ");
    append(output, commit.committer);
    append(output, "\n\n");
    std::string_view remaining(commit.message);
    do {
        const auto newline = remaining.find('\n');
        append(output, "    ");
        append(output, remaining.substr(0, newline));
        append(output, "\n");
        if (newline == std::string_view::npos) break;
        remaining.remove_prefix(newline + 1);
    } while (!remaining.empty());
    append(output, "\n");
    return output;
}
}
