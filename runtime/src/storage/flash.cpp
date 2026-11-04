#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <list>
#include <mutex>
#include <string>
#include <vector>

#include "esp_flash.h"
#include "esp_partition.h"
#include "preflash/fail.h"
#include "preflash/trace.h"

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kChipSize = 16 * 1024 * 1024;
constexpr uint32_t kSectorSize = 4096;

esp_flash_t g_chip = {kChipSize};

std::mutex g_mutex;
// A list keeps each partition where firmware was told it is.
std::list<esp_partition_t> g_partitions;

fs::path image_path() {
  const char* state = std::getenv("PREFLASH_STATE");
  if (!state || !*state) {
    preflash_unsupported("flash", "PREFLASH_STATE names no folder to keep saved state in");
  }
  return fs::path(state) / "flash.bin";
}

// Opens the chip image, creating it sparse on first use. The file keeps every byte
// inverted, so a hole, which the host reads as zeros, is an erased 0xFF byte.
int open_image(bool create) {
  const fs::path path = image_path();
  int fd = ::open(path.c_str(), O_RDWR | (create ? O_CREAT : 0), 0600);
  if (fd < 0) return -1;
  if (create && ::ftruncate(fd, kChipSize) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

bool known(const esp_partition_t* p) {
  for (const auto& q : g_partitions) {
    if (&q == p) return true;
  }
  return false;
}

// As IDF does: an offset past the end is a bad argument, a span running past it a bad size.
esp_err_t check_span(const esp_partition_t* p, size_t offset, size_t size) {
  if (offset > p->size) return ESP_ERR_INVALID_ARG;
  if (size > p->size - offset) return ESP_ERR_INVALID_SIZE;
  return ESP_OK;
}

// Reads are left out: a log scan would bury everything else.
void trace_op(const esp_partition_t* p, const char* op, size_t offset, size_t size) {
  if (!preflash::trace::on()) return;
  preflash::trace::note(preflash::trace::Stream::Nvs, p->label, op,
                        std::to_string(size) + " bytes at " + std::to_string(offset));
}

}  // namespace

extern "C" {

esp_flash_t* esp_flash_default_chip = &g_chip;

esp_err_t esp_flash_get_size(esp_flash_t* chip, uint32_t* out_size) {
  if (!out_size) return ESP_ERR_INVALID_ARG;
  *out_size = (chip ? chip : esp_flash_default_chip)->size;
  return ESP_OK;
}

esp_err_t esp_partition_register_external(esp_flash_t* flash_chip, size_t offset, size_t size,
                                          const char* label, esp_partition_type_t type,
                                          esp_partition_subtype_t subtype,
                                          const esp_partition_t** out_partition) {
  if (!label || !out_partition || size == 0) return ESP_ERR_INVALID_ARG;
  if (!flash_chip) flash_chip = esp_flash_default_chip;
  if (flash_chip != esp_flash_default_chip) return ESP_ERR_INVALID_ARG;
  if (offset > flash_chip->size || size > flash_chip->size - offset) return ESP_ERR_INVALID_SIZE;
  std::lock_guard<std::mutex> lock(g_mutex);
  for (const auto& q : g_partitions) {
    // Both spans already fit the chip, but widen first so no sum can wrap.
    const size_t q_start = q.address;
    const size_t q_size = q.size;
    const bool starts_before_q_ends = offset < q_start || offset - q_start < q_size;
    const bool q_starts_before_end = q_start < offset || q_start - offset < size;
    if (starts_before_q_ends && q_starts_before_end) return ESP_ERR_INVALID_ARG;
  }
  // Fails at once, as on a device, when nowhere is given to keep the image.
  const int fd = open_image(true);
  if (fd < 0) return ESP_FAIL;
  ::close(fd);
  esp_partition_t p = {};
  p.flash_chip = flash_chip;
  p.type = type;
  p.subtype = subtype;
  p.address = static_cast<uint32_t>(offset);
  p.size = static_cast<uint32_t>(size);
  p.erase_size = kSectorSize;
  std::strncpy(p.label, label, sizeof p.label - 1);
  g_partitions.push_back(p);
  *out_partition = &g_partitions.back();
  return ESP_OK;
}

esp_err_t esp_partition_deregister_external(const esp_partition_t* partition) {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (auto it = g_partitions.begin(); it != g_partitions.end(); ++it) {
    if (&*it == partition) {
      g_partitions.erase(it);
      return ESP_OK;
    }
  }
  return ESP_ERR_NOT_FOUND;
}

esp_err_t esp_partition_read(const esp_partition_t* partition, size_t src_offset, void* dst,
                             size_t size) {
  if (!partition || (!dst && size > 0)) return ESP_ERR_INVALID_ARG;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!known(partition)) return ESP_ERR_INVALID_ARG;
  if (esp_err_t err = check_span(partition, src_offset, size)) return err;
  if (size == 0) return ESP_OK;
  const int fd = open_image(true);
  if (fd < 0) return ESP_FAIL;
  auto* out = static_cast<uint8_t*>(dst);
  size_t done = 0;
  while (done < size) {
    const ssize_t n = ::pread(fd, out + done, size - done, partition->address + src_offset + done);
    if (n <= 0) break;
    done += static_cast<size_t>(n);
  }
  ::close(fd);
  if (done < size) return ESP_FAIL;
  // Undo the inversion the image is kept in.
  for (size_t i = 0; i < size; ++i) out[i] = static_cast<uint8_t>(~out[i]);
  return ESP_OK;
}

esp_err_t esp_partition_write(const esp_partition_t* partition, size_t dst_offset, const void* src,
                              size_t size) {
  if (!partition || (!src && size > 0)) return ESP_ERR_INVALID_ARG;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!known(partition)) return ESP_ERR_INVALID_ARG;
  if (partition->readonly) return ESP_ERR_NOT_ALLOWED;
  if (esp_err_t err = check_span(partition, dst_offset, size)) return err;
  if (size == 0) return ESP_OK;
  const int fd = open_image(true);
  if (fd < 0) return ESP_FAIL;
  const off_t base = static_cast<off_t>(partition->address) + static_cast<off_t>(dst_offset);
  std::vector<uint8_t> bytes(size);
  size_t done = 0;
  while (done < size) {
    const ssize_t n = ::pread(fd, bytes.data() + done, size - done, base + static_cast<off_t>(done));
    if (n <= 0) break;
    done += static_cast<size_t>(n);
  }
  if (done < size) {
    ::close(fd);
    return ESP_FAIL;
  }
  const auto* in = static_cast<const uint8_t*>(src);
  // Stored inverted, so clearing a bit on the chip sets it in the file.
  for (size_t i = 0; i < size; ++i) bytes[i] |= static_cast<uint8_t>(~in[i]);
  done = 0;
  while (done < size) {
    const ssize_t n = ::pwrite(fd, bytes.data() + done, size - done, base + static_cast<off_t>(done));
    if (n <= 0) break;
    done += static_cast<size_t>(n);
  }
  ::close(fd);
  if (done < size) return ESP_FAIL;
  trace_op(partition, "write", dst_offset, size);
  return ESP_OK;
}

esp_err_t esp_partition_erase_range(const esp_partition_t* partition, size_t offset, size_t size) {
  if (!partition) return ESP_ERR_INVALID_ARG;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!known(partition)) return ESP_ERR_INVALID_ARG;
  if (partition->readonly) return ESP_ERR_NOT_ALLOWED;
  if (esp_err_t err = check_span(partition, offset, size)) return err;
  if (size % kSectorSize != 0) return ESP_ERR_INVALID_SIZE;
  if (offset % kSectorSize != 0) return ESP_ERR_INVALID_ARG;
  const int fd = open_image(true);
  if (fd < 0) return ESP_FAIL;
  const std::vector<uint8_t> zeros(kSectorSize, 0);
  bool ok = true;
  for (size_t at = 0; at < size && ok; at += kSectorSize) {
    const off_t pos = static_cast<off_t>(partition->address) + static_cast<off_t>(offset + at);
    ok = ::pwrite(fd, zeros.data(), kSectorSize, pos) == static_cast<ssize_t>(kSectorSize);
  }
  ::close(fd);
  if (!ok) return ESP_FAIL;
  trace_op(partition, "erase", offset, size);
  return ESP_OK;
}

}  // extern "C"
