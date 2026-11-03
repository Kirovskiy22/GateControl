#pragma once
#include <string>
#include <vector>
#include <set>
#include <unordered_map>
#include <algorithm>
#include <codecvt>
#include <locale>

namespace plate {

inline std::u32string to_u32(const std::string& s) {
    try {
        std::wstring_convert<std::codecvt_utf8<char32_t>, char32_t> conv;
        return conv.from_bytes(s);
    } catch (...) {
        return U"";
    }
}

inline std::string to_utf8(const std::u32string& s) {
    try {
        std::wstring_convert<std::codecvt_utf8<char32_t>, char32_t> conv;
        return conv.to_bytes(s);
    } catch (...) {
        return "";
    }
}

inline const std::u32string CYRILLIC_LETTERS = U"АВЕКМНОРСТУХ";

inline bool is_cyr_plate_letter(char32_t c) {
    return CYRILLIC_LETTERS.find(c) != std::u32string::npos;
}

inline bool is_ascii_digit(char32_t c) {
    return c >= U'0' && c <= U'9';
}

inline std::u32string normalize_u32(const std::string& raw) {
    std::u32string u = to_u32(raw);
    std::u32string out;
    for (char32_t c : u) {
        if (c >= U'a' && c <= U'z') c -= (U'a' - U'A');
        if (c >= U'а' && c <= U'я') c -= (U'а' - U'А');
        if (c == U'ё' || c == U'Ё') c = U'Е';

        switch (c) {
            case U'A': c = U'А'; break;
            case U'B': c = U'В'; break;
            case U'E': c = U'Е'; break;
            case U'K': c = U'К'; break;
            case U'M': c = U'М'; break;
            case U'H': c = U'Н'; break;
            case U'O': c = U'О'; break;
            case U'P': c = U'Р'; break;
            case U'C': c = U'С'; break;
            case U'T': c = U'Т'; break;
            case U'Y': case U'V': c = U'У'; break;
            case U'X': c = U'Х'; break;
            default: break;
        }

        if (is_ascii_digit(c) || (c >= U'A' && c <= U'Z') || (c >= U'А' && c <= U'Я')) {
            out.push_back(c);
        }
    }
    if (out.size() >= 3 && out.substr(out.size() - 3) == U"RUS") {
        out.resize(out.size() - 3);
    }
    return out;
}

inline std::string normalize_plate(const std::string& raw) {
    return to_utf8(normalize_u32(raw));
}

inline bool is_valid_u32(const std::u32string& p) {
    if (p.size() != 6) return false;
    return is_cyr_plate_letter(p[0]) &&
           is_ascii_digit(p[1]) &&
           is_ascii_digit(p[2]) &&
           is_ascii_digit(p[3]) &&
           is_cyr_plate_letter(p[4]) &&
           is_cyr_plate_letter(p[5]);
}

inline bool is_valid_ru_plate(const std::string& plate_utf8) {
    return is_valid_u32(normalize_u32(plate_utf8));
}

inline char32_t as_digit(char32_t c) {
    if (is_ascii_digit(c)) return c;
    switch (c) {
        case U'O': case U'О': case U'D': case U'Q': return U'0';
        case U'I': case U'L': return U'1';
        case U'Z': return U'2';
        case U'S': return U'5';
        case U'B': case U'Б': return U'8';
        case U'G': return U'6';
        default: return 0;
    }
}

inline char32_t as_letter(char32_t c) {
    if (is_cyr_plate_letter(c)) return c;
    if (c == U'0') return U'О';
    if (c == U'8') return U'В';
    return 0;
}

inline std::u32string coerce_chunk(const std::u32string& chunk) {
    if (chunk.size() < 6) return U"";
    char32_t l0 = as_letter(chunk[0]);
    char32_t d1 = as_digit(chunk[1]);
    char32_t d2 = as_digit(chunk[2]);
    char32_t d3 = as_digit(chunk[3]);
    char32_t l4 = as_letter(chunk[4]);
    char32_t l5 = as_letter(chunk[5]);
    if (!l0 || !d1 || !d2 || !d3 || !l4 || !l5) return U"";
    std::u32string candidate = {l0, d1, d2, d3, l4, l5};
    return is_valid_u32(candidate) ? candidate : U"";
}

inline std::u32string fix_leading_one(const std::u32string& text) {
    if (text.size() == 6 && text[0] == U'1' &&
        is_ascii_digit(text[1]) && is_ascii_digit(text[2]) && is_ascii_digit(text[3]) &&
        is_cyr_plate_letter(text[4]) && is_cyr_plate_letter(text[5])) {
        std::u32string cand = U"С" + text.substr(1, 5);
        if (is_valid_u32(cand)) return cand;
    }
    return U"";
}

inline void replace_all(std::u32string& str, const std::u32string& from, const std::u32string& to) {
    size_t pos = 0;
    while ((pos = str.find(from, pos)) != std::u32string::npos) {
        str.replace(pos, from.length(), to);
        pos += to.length();
    }
}

inline std::string repair_ocr_plate(const std::string& raw) {
    std::u32string text = normalize_u32(raw);
    replace_all(text, U"II", U"Н");
    replace_all(text, U"IL", U"Н");
    replace_all(text, U"LI", U"Н");

    if (text.size() >= 6) {
        for (size_t i = 0; i + 6 <= text.size(); ++i) {
            std::u32string sub = text.substr(i, 6);
            if (is_valid_u32(sub)) return to_utf8(sub);
        }
    }
    for (size_t len : {6, 8, 9, 7}) {
        if (text.size() < len) continue;
        for (size_t i = 0; i + len <= text.size(); ++i) {
            std::u32string coerced = coerce_chunk(text.substr(i, len));
            if (!coerced.empty()) return to_utf8(coerced);
        }
    }
    if (text.size() >= 6) {
        for (size_t i = 0; i + 6 <= text.size(); ++i) {
            std::u32string fixed = fix_leading_one(text.substr(i, 6));
            if (!fixed.empty()) return to_utf8(fixed);
        }
    }
    return "";
}

inline std::string format_plate(const std::string& plate_utf8) {
    std::u32string u = normalize_u32(plate_utf8);
    if (!is_valid_u32(u)) return plate_utf8;
    std::u32string formatted = {u[0], U' ', u[1], u[2], u[3], U' ', u[4], u[5]};
    return to_utf8(formatted);
}

struct PlateDecision {
    std::string plate;
    float confidence;
    bool open_gate;
    std::string reason;
    std::string kind;
};

class PlateAccessPolicy {
public:
    std::set<std::string> allowed;
    bool whitelist_only;
    bool require_valid_format;
    float min_confidence;
    float open_min_confidence;
    bool open_on_detect;

    PlateAccessPolicy(
        const std::vector<std::string>& allowed_plates,
        bool whitelist_only_,
        bool require_valid_format_,
        float min_conf,
        float open_min_conf,
        bool open_on_detect_
    ) : whitelist_only(whitelist_only_),
        require_valid_format(require_valid_format_),
        min_confidence(min_conf),
        open_min_confidence(open_min_conf),
        open_on_detect(open_on_detect_) {
        set_allowed(allowed_plates);
    }

    void set_allowed(const std::vector<std::string>& plates) {
        allowed.clear();
        for (const auto& p : plates) {
            std::string norm = repair_ocr_plate(p);
            if (norm.empty()) norm = normalize_plate(p);
            if (!norm.empty()) allowed.insert(norm);
        }
    }

    PlateDecision evaluate(const std::string& raw_text, float confidence) const {
        float conf = confidence > 1.0f ? confidence / 100.0f : confidence;
        conf = std::max(0.0f, std::min(conf, 1.0f));

        std::string repaired = repair_ocr_plate(raw_text);
        std::string plate_str = !repaired.empty() ? repaired : normalize_plate(raw_text);

        if (plate_str.empty() || to_u32(plate_str).size() < 4) {
            return {"", conf, false, "пустой номер", "warn"};
        }
        if (conf < min_confidence) {
            return {plate_str, conf, false, "низкая уверенность", "warn"};
        }
        if (require_valid_format) {
            if (repaired.empty() || !is_valid_ru_plate(repaired)) {
                return {plate_str, conf, false, "номер не похож на ГРЗ", "warn"};
            }
            plate_str = repaired;
        }
        if (!open_on_detect) {
            return {plate_str, conf, false, "автооткрытие выключено", "info"};
        }
        if (conf < open_min_confidence) {
            return {plate_str, conf, false, "недостаточная уверенность для открытия", "warn"};
        }

        bool in_wl = allowed.count(plate_str) > 0;
        if (whitelist_only) {
            if (allowed.empty()) {
                return {plate_str, conf, false, "белый список пуст — шлагбаум не открывается", "info"};
            }
            if (!in_wl) {
                return {plate_str, conf, false, "номера нет в белом списке", "warn"};
            }
        }
        return {plate_str, conf, true, in_wl ? "номер в белом списке" : "номер распознан", "success"};
    }
};

class OpenCooldown {
    double seconds_;
    double until_ = 0.0;
    std::string plate_;
public:
    explicit OpenCooldown(double sec) : seconds_(std::max(0.0, sec)) {}

    bool blocked(double now) const { return now < until_; }

    void mark(const std::string& plate, double now) {
        plate_ = plate;
        until_ = now + seconds_;
    }
};

} // namespace plate