#pragma once

// Small text and file helpers shared by the hdrspace tools.

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace hdrspace::text {

std::string trim(const std::string& text);

// Empty for blank text and for the old placeholder words that text boxes once held as values.
std::string normalizedTextBoxValue(std::string text);

std::string readFile(const fs::path& path);

// Single-quotes a value for a POSIX shell command line.
std::string shellQuote(const std::string& value);

bool tryParseInteger(const std::string& text, int& value);

bool tryParseFloating(const std::string& text, double& value);

std::vector<std::string> splitWhitespace(const std::string& text);

std::vector<std::string> splitShellLike(const std::string& text);

std::string createViewLower(std::string text);

} // namespace hdrspace::text
