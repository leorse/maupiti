#include "ui.hpp"

#include <fstream>
#include <map>
#include <sstream>

namespace maupiti {

std::vector<uint8_t> glyphs_from_utf8(const std::string& s) {
    static const std::map<char32_t, uint8_t> special = {
        {U' ', 0x4C}, {U'.', 0x54}, {U',', 0x55}, {U':', 0x57}, {U'!', 0x58}, {U'?', 0x59}, {U'(', 0x5B},
        {U')', 0x5C}, {U'-', 0x5E}, {U'+', 0x5D}, {U'/', 0x5A}, {U'=', 0x5F}, {U'\'', 0x53}, {U'"', 0x52},
        {U'%', 0x51}, {U';', 0x56}, {U'é', 0x43}, {U'è', 0x46}, {U'ê', 0x44}, {U'à', 0x41}, {U'â', 0x3F},
        {U'ç', 0x42}, {U'î', 0x47}, {U'ï', 0x48}, {U'ô', 0x49}, {U'û', 0x4E}, {U'ù', 0x4F}, {U'ë', 0x45},
    };
    std::vector<uint8_t> out;
    for (size_t i = 0; i < s.size();) {
        char32_t c = static_cast<unsigned char>(s[i]);
        int n = 1;
        if (c >= 0xF0) { c &= 0x07; n = 4; }
        else if (c >= 0xE0) { c &= 0x0F; n = 3; }
        else if (c >= 0xC0) { c &= 0x1F; n = 2; }
        for (int k = 1; k < n && i + k < s.size(); ++k) c = (c << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        i += n;
        if (c >= U'a' && c <= U'z') out.push_back(static_cast<uint8_t>(1 + (c - U'a')));
        else if (c >= U'A' && c <= U'Z') out.push_back(static_cast<uint8_t>(0x1B + (c - U'A')));
        else if (c >= U'0' && c <= U'9') out.push_back(static_cast<uint8_t>(0x35 + (c - U'0')));
        else if (auto it = special.find(c); it != special.end()) out.push_back(it->second);
    }
    return out;
}

std::vector<VoiceVariant> load_voice_variants(const std::filesystem::path& pack_dir) {
    std::vector<VoiceVariant> out;
    std::ifstream f(pack_dir / "voices" / "voices.txt");
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        VoiceVariant v;
        if (std::getline(ss, v.dir, ';') && std::getline(ss, v.voice, ';') && std::getline(ss, v.style))
            out.push_back(v);
    }
    return out;
}

int text_width(const Font& f, const std::vector<uint8_t>& g) {
    int w = 0;
    for (uint8_t c : g)
        if (c > 0 && c <= f.glyphs.size()) w += f.glyphs[c - 1].advance;
    return w;
}

void Toolbar::draw(Screen& scr, const Font& f, int text_id, const std::string& voice, const std::string& style) {
    buttons_.clear();
    const uint32_t bar = 0xFF202830u, btn = 0xFF3C5064u, edge = 0xFF8CA0B4u, ink = 0xFFFFFFFFu, hl = 0xFFE0B040u;
    // deux lignes : texte, style et JOUER en haut ; voix (libellés longs) en dessous
    scr.fill(0, 0, kScreenW, 22, bar);
    int x = 2, y = 0;
    auto button = [&](const std::string& label, Action a, uint32_t color) {
        const auto g = glyphs_from_utf8(label);
        const int w = text_width(f, g) + 6;
        scr.fill(x, y + 1, w, 9, edge);
        scr.fill(x + 1, y + 2, w - 2, 7, btn);
        scr.text(f, g, x + 3, y + 1, color);
        buttons_.push_back({{x, y, w, 11}, a});
        x += w + 1;
    };
    auto label = [&](const std::string& s) {
        const auto g = glyphs_from_utf8(s);
        scr.text(f, g, x + 1, y + 1, ink);
        x += text_width(f, g) + 3;
    };
    button("-", Action::TextPrev, ink);
    label(std::to_string(text_id));
    button("+", Action::TextNext, ink);
    x += 6;
    button("-", Action::StylePrev, ink);
    label(style);
    button("+", Action::StyleNext, ink);
    x += 6;
    button("JOUER", Action::Play, hl);
    x = 2;
    y = 11;
    button("-", Action::VoicePrev, ink);
    label(voice);
    button("+", Action::VoiceNext, ink);
}

Action Toolbar::hit(int x, int y) const {
    for (const auto& b : buttons_)
        if (x >= b.r.x && x < b.r.x + b.r.w && y >= b.r.y && y < b.r.y + b.r.h) return b.a;
    return Action::None;
}

}  // namespace maupiti
