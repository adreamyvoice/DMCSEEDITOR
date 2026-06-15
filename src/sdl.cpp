#include "sdl.h"
#include <cstring>
#include <cctype>

namespace sdl {

bool IsSDL(const Bytes& b) { return b.size() >= 8 && memcmp(b.data(), "SDL\0", 4) == 0; }

static bool isProp(const std::string& s) {        // property: "m" + Uppercase/p  (mPos, mpTexture)
    return s.size() >= 2 && s[0] == 'm' && (isupper((unsigned char)s[1]) || s[1] == 'p');
}
static bool isType(const std::string& s) {        // node type: starts uppercase, not a property
    return !s.empty() && !isProp(s) && s != "SDL" && isupper((unsigned char)s[0]);
}

std::string ToText(const Bytes& b) {
    if (!IsSDL(b)) return "(not an SDL file)";
    uint16_t ver = 0; memcpy(&ver, &b[4], 2);
    char hdr[96]; snprintf(hdr, sizeof hdr, "SDL scene  (version %u, %zu bytes)\n\n", ver, b.size());
    std::string out = hdr;

    // pull printable ASCII runs (the node-type and property names are stored as strings,
    // in scene order: a node type is followed by its properties).
    std::vector<std::string> strs; std::string cur;
    for (size_t i = 0; i < b.size(); ++i) {
        unsigned char c = b[i];
        if (c >= 0x20 && c < 0x7f) cur.push_back((char)c);
        else { if (cur.size() >= 3) strs.push_back(cur); cur.clear(); }
    }
    if (cur.size() >= 3) strs.push_back(cur);

    int nodes = 0, props = 0; bool started = false; std::string line;
    for (const std::string& s : strs) {
        if (!started) { if (s == "Root" || isType(s)) started = true; else continue; }
        if (isProp(s)) { ++props; if (!line.empty()) line += ", "; line += s; }
        else if (isType(s)) {
            if (!line.empty()) { out += "    " + line + "\n"; line.clear(); }
            out += s + "\n"; ++nodes;
        }
    }
    if (!line.empty()) out += "    " + line + "\n";
    char tail[80]; snprintf(tail, sizeof tail, "\n(%d nodes, %d properties)\n", nodes, props);
    out += tail;
    return out;
}

} // namespace sdl
