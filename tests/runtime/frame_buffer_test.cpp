#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "winlink/frame_buffer.h"

namespace fs = std::filesystem;
using preflash::winlink::FrameBuffer;

namespace {

// Scratch files live under TMPDIR when it's set, so a Linux run can keep them off RAM-backed /tmp.
struct TempFolder {
  TempFolder() {
    std::string pattern = (fs::temp_directory_path() / "preflash-frame-XXXXXX").string();
    path = mkdtemp(pattern.data());
  }
  ~TempFolder() { fs::remove_all(path); }
  std::string path;
};

}  // namespace

TEST(FrameBuffer, AReaderSeesEachNewFrameOnce) {
  TempFolder dir;
  auto writer = FrameBuffer::create(dir.path + "/f", 2, 1);
  auto reader = FrameBuffer::open(dir.path + "/f");
  ASSERT_TRUE(reader);
  EXPECT_EQ(reader->width(), 2);
  EXPECT_EQ(reader->height(), 1);
  const uint8_t px[4] = {1, 2, 3, 4};
  writer.write(px);
  uint64_t seq = 0;
  std::vector<uint8_t> out;
  EXPECT_TRUE(reader->read_if_newer(&seq, &out));
  EXPECT_EQ(out, std::vector<uint8_t>({1, 2, 3, 4}));
  EXPECT_FALSE(reader->read_if_newer(&seq, &out));
}

TEST(FrameBuffer, OpeningAFileThatIsntOneFails) {
  TempFolder dir;
  {
    std::ofstream(dir.path + "/f") << "nope";
  }
  EXPECT_FALSE(FrameBuffer::open(dir.path + "/f"));
  EXPECT_FALSE(FrameBuffer::open(dir.path + "/missing"));
}

TEST(FrameBuffer, ASizeNoReaderWouldOpenIsRefused) {
  TempFolder dir;
  EXPECT_THROW(FrameBuffer::create(dir.path + "/f", 0, 1), std::runtime_error);
  EXPECT_THROW(FrameBuffer::create(dir.path + "/f", 65536, 1), std::runtime_error);
  EXPECT_THROW(FrameBuffer::create(dir.path + "/f", 1, 65536), std::runtime_error);
}

TEST(FrameBuffer, AReaderNeverSeesAHalfWrittenFrame) {
  TempFolder dir;
  constexpr int kW = 64, kH = 64;
  auto writer = FrameBuffer::create(dir.path + "/f", kW, kH);
  auto reader = FrameBuffer::open(dir.path + "/f");
  ASSERT_TRUE(reader);
  std::atomic<bool> done{false};
  std::thread producer([&] {
    std::vector<uint8_t> zero(kW * kH * 2, 0x00), ones(kW * kH * 2, 0xff);
    for (int i = 0; i < 10000; i++) writer.write(i % 2 ? ones.data() : zero.data());
    done = true;
  });
  uint64_t seq = 0;
  std::vector<uint8_t> out;
  int reads = 0;
  bool torn = false;
  // Nonfatal checks only until the producer is joined: a fatal one would return with the thread
  // still running.
  auto check = [&] {
    reads++;
    EXPECT_EQ(out.size(), size_t(kW * kH * 2));
    for (uint8_t b : out) {
      if (b != out[0]) {
        torn = true;
        break;
      }
    }
  };
  while (!done && !torn) {
    if (reader->read_if_newer(&seq, &out)) check();
  }
  producer.join();
  // A fast producer can finish before the reader's first look, so read its last frame too.
  if (!torn && reader->read_if_newer(&seq, &out)) check();
  EXPECT_FALSE(torn) << "a frame mixed two writes";
  EXPECT_GT(reads, 0);
}

// A restarted app makes its frame file again while the window still maps it. The window must
// keep the frame it has, and see the new app's frames in the same mapping.
TEST(FrameBuffer, ARestartKeepsTheFrameAWindowHasMapped) {
  TempFolder dir;
  const std::string path = dir.path + "/f";
  std::optional<FrameBuffer> writer = FrameBuffer::create(path, 2, 1);
  auto reader = FrameBuffer::open(path);
  ASSERT_TRUE(reader);
  const uint8_t first[4] = {1, 2, 3, 4};
  writer->write(first);
  uint64_t seq = 0;
  std::vector<uint8_t> out;
  ASSERT_TRUE(reader->read_if_newer(&seq, &out));

  writer.reset();
  writer = FrameBuffer::create(path, 2, 1);
  EXPECT_FALSE(reader->read_if_newer(&seq, &out)) << "nothing new has been drawn";
  auto reopened = FrameBuffer::open(path);
  ASSERT_TRUE(reopened);
  uint64_t fresh = 0;
  ASSERT_TRUE(reopened->read_if_newer(&fresh, &out)) << "a window that opens it again sees the last frame";
  EXPECT_EQ(out, std::vector<uint8_t>({1, 2, 3, 4}));

  const uint8_t second[4] = {5, 6, 7, 8};
  writer->write(second);
  ASSERT_TRUE(reader->read_if_newer(&seq, &out));
  EXPECT_EQ(out, std::vector<uint8_t>({5, 6, 7, 8}));
}

// A smaller panel after a restart mustn't cut the file from under a window that maps it, which
// would kill the window with SIGBUS the next time it read.
TEST(FrameBuffer, ASmallerPanelLeavesAMappedWindowItsFile) {
  TempFolder dir;
  const std::string path = dir.path + "/f";
  constexpr int kW = 64, kH = 64;
  std::optional<FrameBuffer> writer = FrameBuffer::create(path, kW, kH);
  auto reader = FrameBuffer::open(path);
  ASSERT_TRUE(reader);
  const std::vector<uint8_t> big(kW * kH * 2, 0x5a);
  writer->write(big.data());

  writer.reset();
  writer = FrameBuffer::create(path, 2, 1);
  uint64_t seq = 0;
  std::vector<uint8_t> out;
  ASSERT_TRUE(reader->read_if_newer(&seq, &out));
  EXPECT_EQ(out, big);

  auto reopened = FrameBuffer::open(path);
  ASSERT_TRUE(reopened);
  EXPECT_EQ(reopened->width(), 2);
  EXPECT_EQ(reopened->height(), 1);
}
