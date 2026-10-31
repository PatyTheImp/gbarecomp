#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace gbarecomp::debug {

// Write/flush/close a sibling temporary before replacing the destination. Never
// remove the destination. On replacement failure retain the complete .tmp.
bool replace_file(const std::filesystem::path& path, const uint8_t* bytes,
                  std::size_t size, std::string* err);

#ifdef GBARECOMP_FILE_REPLACE_TEST
// Linked only by fault tests; production builds contain no injection hooks.
bool file_replace_test_fault(const char* phase);
#endif
}  // namespace gbarecomp::debug
