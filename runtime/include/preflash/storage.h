#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace preflash {

struct NvsKey {
  std::string key;
  size_t size;
};

struct NvsNamespace {
  std::string name;
  std::vector<NvsKey> keys;
};

// What's committed to NVS, sorted by name. Values stay out of it, as they can be Wi-Fi
// credentials. Empty if the app keeps no saved state.
std::vector<NvsNamespace> nvs_contents();

}  // namespace preflash
