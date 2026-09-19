// Generated-schema consistency: the versioned integration artifacts must match
// the authoritative registry exactly. This is what turns schema drift into a
// test failure (and, in CI, an exporter + `git diff --exit-code` failure).
#include "voxlink_registry.h"
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

using namespace voxlink;

namespace {
int g_failures = 0;
void check(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
  }
}

std::string mangle(const char *key) {
  std::string out = "VOXP4_PARAM_";
  for (const char *p = key; *p; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    out.push_back(std::isalnum(c) ? static_cast<char>(std::toupper(c)) : '_');
  }
  return out;
}
} // namespace

int main() {
#ifndef VOXP4_SOURCE_DIR
#error "VOXP4_SOURCE_DIR must be defined for the schema test"
#endif
  const std::string path =
      std::string(VOXP4_SOURCE_DIR) + "/integration/VoxP4ParamIds.h";
  std::ifstream file(path);
  if (!file) {
    std::fprintf(stderr, "FAIL: cannot open generated header %s\n", path.c_str());
    return 1;
  }

  int declared_count = -1;
  int generated_entries = 0;
  std::string line;
  while (std::getline(file, line)) {
    if (line.rfind("#define VOXP4_PARAM_COUNT", 0) == 0) {
      if (std::sscanf(line.c_str(), "#define VOXP4_PARAM_COUNT %du",
                      &declared_count) != 1)
        declared_count = -1;
      continue;
    }
    if (line.rfind("#define VOXP4_PARAM_", 0) != 0)
      continue;
    char name[80] = {0};
    unsigned id = 0;
    if (std::sscanf(line.c_str(), "#define %79s 0x%xu", name, &id) != 2)
      continue;
    if (std::strcmp(name, "VOXP4_PARAM_COUNT") == 0)
      continue;

    const ParamDescriptor *d = find_param(static_cast<uint16_t>(id));
    if (d == nullptr) {
      std::fprintf(stderr, "FAIL: generated id 0x%04X not in registry\n", id);
      ++g_failures;
    } else if (mangle(d->key) != name) {
      std::fprintf(stderr, "FAIL: generated name %s != registry key %s\n",
                   name, d->key);
      ++g_failures;
    }
    ++generated_entries;
  }

  check(declared_count == static_cast<int>(registry_count()),
        "VOXP4_PARAM_COUNT equals registry_count()");
  check(generated_entries == static_cast<int>(registry_count()),
        "generated header lists exactly the registry parameters");

  if (g_failures == 0)
    std::printf("voxlink_schema_tests: PASS (%zu parameters)\n",
                registry_count());
  return g_failures == 0 ? 0 : 1;
}
