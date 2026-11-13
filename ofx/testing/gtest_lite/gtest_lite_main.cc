// main() for gtest_lite, the tiny local stand-in for GoogleTest (see
// gtest/gtest.h). Runs every registered TEST / TEST_F and exits non-zero if
// any of them fails.
//
// Usage: <test binary> [--gtest_filter=SUBSTRING | SUBSTRING]
// The filter is a plain substring of "Suite.Name" (no GoogleTest globs).
#include <cstring>
#include <string>

#include "gtest/gtest.h"

// Parses the optional filter argument and runs the selected tests.
int main(int argc, char** argv) {
  std::string filter;
  for (int i = 1; i < argc; ++i) {
    constexpr char kFlag[] = "--gtest_filter=";
    if (std::strncmp(argv[i], kFlag, sizeof(kFlag) - 1) == 0) {
      filter = argv[i] + sizeof(kFlag) - 1;
    } else {
      filter = argv[i];
    }
  }
  return gtest_lite::RunAllTests(filter);
}
