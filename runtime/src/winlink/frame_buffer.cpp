#include "winlink/frame_buffer.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <new>
#include <stdexcept>

namespace preflash::winlink {

namespace {

struct Fd {
  explicit Fd(int fd) : fd(fd) {}
  ~Fd() {
    if (fd >= 0) close(fd);
  }
  int fd;
};

}  // namespace

FrameBuffer::FrameBuffer(void* map, size_t size, int width, int height)
    : map_(map), size_(size), width_(width), height_(height) {}

FrameBuffer::FrameBuffer(FrameBuffer&& other) noexcept
    : map_(other.map_), size_(other.size_), width_(other.width_), height_(other.height_) {
  other.map_ = nullptr;
}

FrameBuffer& FrameBuffer::operator=(FrameBuffer&& other) noexcept {
  if (this != &other) {
    release();
    map_ = other.map_;
    size_ = other.size_;
    width_ = other.width_;
    height_ = other.height_;
    other.map_ = nullptr;
  }
  return *this;
}

FrameBuffer::~FrameBuffer() { release(); }

void FrameBuffer::release() {
  if (map_) munmap(map_, size_);
  map_ = nullptr;
}

FrameBuffer FrameBuffer::create(const std::string& path, int width, int height) {
  if (width <= 0 || height <= 0 || width > 65535 || height > 65535) {
    throw std::runtime_error("frame buffer needs a size from 1 to 65535 each way");
  }
  const size_t size = sizeof(Header) + size_t(width) * height * 2;
  // A window outlives the app and may still map the file from the app's last run. Cutting
  // that file shorter would kill the window with SIGBUS on its next read, so a file of the
  // same shape is taken over as it is, frame and all, and any other is replaced by a new one
  // that leaves the window its old mapping until it opens the new file.
  if (auto same = open(path); same && same->size_ == size && same->width_ == width &&
                              same->height_ == height) {
    Header* head = same->header();
    // An app that died mid-write left the count odd. The frame is torn either way, and the
    // next write finishes it.
    const uint64_t seq = head->seq.load(std::memory_order_relaxed);
    if (seq & 1) head->seq.store(seq + 1, std::memory_order_release);
    return std::move(*same);
  }
  if (::unlink(path.c_str()) != 0 && errno != ENOENT) throw std::runtime_error("can't replace " + path);
  Fd file(::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600));
  if (file.fd < 0) throw std::runtime_error("can't create " + path);
  if (ftruncate(file.fd, off_t(size)) != 0) throw std::runtime_error("can't size " + path);
  void* map = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, file.fd, 0);
  if (map == MAP_FAILED) throw std::runtime_error("can't map " + path);
  // The file is zero-filled, so a reader that opens early sees seq 0: no frame yet.
  auto* head = new (map) Header;
  head->magic = kMagic;
  head->width = uint32_t(width);
  head->height = uint32_t(height);
  head->reserved = 0;
  head->seq.store(0, std::memory_order_release);
  return FrameBuffer(map, size, width, height);
}

std::optional<FrameBuffer> FrameBuffer::open(const std::string& path) {
  Fd file(::open(path.c_str(), O_RDWR));
  if (file.fd < 0) return std::nullopt;
  struct stat info;
  if (fstat(file.fd, &info) != 0 || size_t(info.st_size) < sizeof(Header)) return std::nullopt;
  const size_t size = size_t(info.st_size);
  void* map = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, file.fd, 0);
  if (map == MAP_FAILED) return std::nullopt;
  auto* head = static_cast<Header*>(map);
  const uint64_t pixels = uint64_t(head->width) * head->height * 2;
  if (head->magic != kMagic || head->width == 0 || head->height == 0 || head->width > 65535 ||
      head->height > 65535 || sizeof(Header) + pixels > size) {
    munmap(map, size);
    return std::nullopt;
  }
  return FrameBuffer(map, size, int(head->width), int(head->height));
}

void FrameBuffer::write(const uint8_t* rgb565) {
  Header* head = header();
  const uint64_t seq = head->seq.load(std::memory_order_relaxed);
  head->seq.store(seq + 1, std::memory_order_relaxed);
  // Keeps the odd marker ahead of the copy for any reader that sees a byte of it.
  std::atomic_thread_fence(std::memory_order_release);
  std::memcpy(pixels(), rgb565, frame_bytes());
  head->seq.store(seq + 2, std::memory_order_release);
}

bool FrameBuffer::read_if_newer(uint64_t* last_seq, std::vector<uint8_t>* out) {
  Header* head = header();
  for (int attempt = 0; attempt < 2; attempt++) {
    const uint64_t before = head->seq.load(std::memory_order_acquire);
    if ((before & 1) || before == *last_seq) return false;
    out->resize(frame_bytes());
    std::memcpy(out->data(), pixels(), frame_bytes());
    // Orders the copy before the re-check, so a write that overlapped it shows in `after`.
    std::atomic_thread_fence(std::memory_order_acquire);
    const uint64_t after = head->seq.load(std::memory_order_relaxed);
    if (after == before) {
      *last_seq = before;
      return true;
    }
  }
  return false;
}

}  // namespace preflash::winlink
