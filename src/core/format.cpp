#include "minigit/format.hpp"
namespace minigit {
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
