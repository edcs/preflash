#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace preflash::winlink {

// A file the app writes and the window maps, holding the panel's latest frame as RGB565LE.
// One writer, any number of readers. A seqlock keeps a reader from keeping a torn frame:
// `seq` is odd while a write is in progress.
class FrameBuffer {
 public:
  static constexpr uint32_t kMagic = 0x50464652;  // "PFFR"

  // Maps the file for writing. One already there with the same width, height and size is kept,
  // frame and count, so that a window still mapping it from an earlier run carries on. Any other
  // is replaced by a new file, never cut short under a reader. Throws std::runtime_error if it
  // can't.
  static FrameBuffer create(const std::string& path, int width, int height);
  // Maps an existing file. Nullopt if it's missing, too short, or not a frame file.
  static std::optional<FrameBuffer> open(const std::string& path);

  FrameBuffer(FrameBuffer&& other) noexcept;
  FrameBuffer& operator=(FrameBuffer&& other) noexcept;
  FrameBuffer(const FrameBuffer&) = delete;
  FrameBuffer& operator=(const FrameBuffer&) = delete;
  ~FrameBuffer();

  // `rgb565` holds width*height*2 bytes. Single writer only.
  void write(const uint8_t* rgb565);
  // False if no frame is complete yet, nothing is newer than *last_seq, or a write kept
  // overlapping the copy. On true, *last_seq and *out hold the frame just read.
  bool read_if_newer(uint64_t* last_seq, std::vector<uint8_t>* out);

  int width() const { return width_; }
  int height() const { return height_; }

 private:
  struct Header {
    uint32_t magic;
    uint32_t width;
    uint32_t height;
    uint32_t reserved;
    std::atomic<uint64_t> seq;
  };
  static_assert(sizeof(Header) == 24);

  FrameBuffer(void* map, size_t size, int width, int height);
  uint8_t* pixels() const { return static_cast<uint8_t*>(map_) + sizeof(Header); }
  Header* header() const { return static_cast<Header*>(map_); }
  size_t frame_bytes() const { return size_t(width_) * height_ * 2; }
  void release();

  void* map_ = nullptr;
  size_t size_ = 0;
  int width_ = 0;
  int height_ = 0;
};

}  // namespace preflash::winlink
