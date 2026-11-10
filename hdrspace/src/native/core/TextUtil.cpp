#include "TextUtil.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>

namespace hdrspace::text {

std::string trim(const std::string& text) {
    size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) {
        ++start;
    }

    size_t end = text.size();
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }

    return text.substr(start, end - start);
}

std::string normalizedTextBoxValue(std::string text) {
    text = trim(text);
    if (text.empty()) {
        return {};
    }

    const std::string lowered = createViewLower(text);
    static const std::set<std::string> sentinels = {
        "untitled",
        "default",
        "optional",
        "optional override",
        "advanced options",
        "output folder",
        "reference hdr",
        "test hdr",
        "ref_cells.txt",
        "test_cells.txt",
        "shutterc",
        "x y w h",
        "r g b [g2]",
        "mergehdr binary",
        "oiiotool",
        "default (profile / libraw)",
        "default (profile / libraw defaults)",
    };

    if (sentinels.contains(lowered)) {
        return {};
    }

    return text;
}

std::string readFile(const fs::path& path) {
    std::ifstream input{path};
    if (!input) {
        return {};
    }

    std::ostringstream oss;
    oss << input.rdbuf();
    return oss.str();
}

std::string shellQuote(const std::string& value) {
    std::string out = "'";
    for (char ch : value) {
        if (ch == '\'') {
            out += "'\\''";
        } else {
            out += ch;
        }
    }
    out += "'";
    return out;
}

bool tryParseInteger(const std::string& text, int& value) {
    try {
        size_t consumed = 0;
        value = std::stoi(text, &consumed);
        return consumed == text.size();
    } catch (...) {
        return false;
    }
}

bool tryParseFloating(const std::string& text, double& value) {
    try {
        size_t consumed = 0;
        value = std::stod(text, &consumed);
        return consumed == text.size();
    } catch (...) {
        return false;
    }
}

std::vector<std::string> splitWhitespace(const std::string& text) {
    std::vector<std::string> tokens;
    std::istringstream stream(text);
    std::string token;
    while (stream >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

std::vector<std::string> splitShellLike(const std::string& text) {
    std::vector<std::string> tokens;
    std::string current;
    char quote = '\0';
    bool escape = false;
    for (char c : text) {
        if (escape) {
            current.push_back(c);
            escape = false;
            continue;
        }
        if (c == '\\') {
            escape = true;
            continue;
        }
        if (quote != '\0') {
            if (c == quote) {
                quote = '\0';
            } else {
                current.push_back(c);
            }
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }
    return tokens;
}

std::string createViewLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

} // namespace hdrspace::text
