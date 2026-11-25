#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "bsp/audio.h"
#include "fake_clock.h"
#include "preflash/audio.h"

namespace {

class FakeSink : public preflash::audio::Sink {
 public:
  void start(int r, int c) override {
    rate = r;
    channels = c;
  }
  bool queue(const void* data, size_t n) override {
    ++queues;
    if (fail_queue || (fail_at_queue && queues == fail_at_queue)) return false;
    bytes += n;
    const auto* p = static_cast<const int16_t*>(data);
    last.assign(p, p + n / sizeof(int16_t));
    if (drain_on_queue) waiting += 250;  // each piece lands on a busy device
    return true;
  }
  int queued_ms() override {
    ++polls;
    const int shown = waiting;
    waiting = waiting > 100 ? waiting - 100 : 0;  // each look finds it drained a little
    return shown;
  }
  int rate = 0, channels = 0, waiting = 0, queues = 0, fail_at_queue = 0;
  std::atomic<int> polls{0};
  bool fail_queue = false, drain_on_queue = false;
  size_t bytes = 0;
  std::vector<int16_t> last;
};

class FakeTap : public preflash::audio::Tap {
 public:
  void format(preflash::audio::TapFormat f) override {
    formats.push_back(f);
    events.push_back('f');
  }
  void samples(const int16_t* data, size_t frames) override {
    writes.emplace_back(data, data + frames * (formats.empty() ? 1 : formats.back().channels));
    events.push_back('s');
  }
  void stop() override {
    ++stops;
    events.push_back('x');
  }
  std::vector<preflash::audio::TapFormat> formats;
  std::vector<std::vector<int16_t>> writes;
  int stops = 0;
  std::string events;
};

esp_codec_dev_sample_info_t mono(uint32_t rate) {
  esp_codec_dev_sample_info_t fs = {};
  fs.sample_rate = rate;
  fs.channel = 1;
  fs.bits_per_sample = 16;
  return fs;
}

class Codec : public testing::Test {
 protected:
  void SetUp() override {
    preflash::audio::set_sink(nullptr);
    preflash::audio::set_device_request({});
  }
  void TearDown() override {
    preflash::audio::set_clock_for_test({});
    esp_codec_dev_close(speaker_);
    preflash::audio::set_sink(nullptr);
    preflash::audio::set_device_request({});
  }
  // Paces sound with nowhere to go by fake_, which TearDown takes out again.
  void use_fake_clock() { preflash::audio::set_clock_for_test(fake_.clock()); }
  esp_codec_dev_handle_t speaker_ = bsp_audio_codec_speaker_init();
  FakeClock fake_;
};

}  // namespace

TEST_F(Codec, WriteCountsSamplesAndMatchesOpenRate) {
  FakeSink sink;
  preflash::audio::set_sink(&sink);
  auto fs = mono(16000);
  ASSERT_EQ(esp_codec_dev_open(speaker_, &fs), ESP_CODEC_DEV_OK);
  EXPECT_EQ(sink.rate, 16000);
  EXPECT_EQ(sink.channels, 1);

  int16_t pcm[320] = {};
  const size_t before = preflash::audio_samples_written();
  ASSERT_EQ(esp_codec_dev_write(speaker_, pcm, sizeof(pcm)), ESP_CODEC_DEV_OK);
  EXPECT_EQ(preflash::audio_samples_written() - before, 320u);
  EXPECT_EQ(sink.bytes, sizeof(pcm));
}

TEST_F(Codec, VolumeScalesWhatIsPlayed) {
  FakeSink sink;
  preflash::audio::set_sink(&sink);
  auto fs = mono(16000);
  ASSERT_EQ(esp_codec_dev_open(speaker_, &fs), ESP_CODEC_DEV_OK);
  ASSERT_EQ(esp_codec_dev_set_out_vol(speaker_, 50), ESP_CODEC_DEV_OK);
  int16_t pcm[2] = {1000, -1000};
  ASSERT_EQ(esp_codec_dev_write(speaker_, pcm, sizeof(pcm)), ESP_CODEC_DEV_OK);
  EXPECT_EQ(sink.last, (std::vector<int16_t>{500, -500}));
  EXPECT_EQ(esp_codec_dev_set_out_vol(speaker_, 101), ESP_CODEC_DEV_INVALID_ARG);
  ASSERT_EQ(esp_codec_dev_set_out_vol(speaker_, 100), ESP_CODEC_DEV_OK);
}

TEST_F(Codec, WriteWaitsWhileMoreThan100msIsQueued) {
  FakeSink sink;
  sink.waiting = 350;
  preflash::audio::set_sink(&sink);
  auto fs = mono(16000);
  ASSERT_EQ(esp_codec_dev_open(speaker_, &fs), ESP_CODEC_DEV_OK);
  int16_t pcm[16] = {};
  ASSERT_EQ(esp_codec_dev_write(speaker_, pcm, sizeof(pcm)), ESP_CODEC_DEV_OK);
  EXPECT_GE(sink.polls, 3);  // 350, 250, 150 are over the limit; 50 is not
  EXPECT_EQ(sink.bytes, sizeof(pcm));
}

TEST_F(Codec, LongWriteIsFedInPiecesSoItPaces) {
  FakeSink sink;
  sink.drain_on_queue = true;
  preflash::audio::set_sink(&sink);
  auto fs = mono(16000);
  ASSERT_EQ(esp_codec_dev_open(speaker_, &fs), ESP_CODEC_DEV_OK);
  std::vector<int16_t> pcm(16000 * 2);  // two seconds
  ASSERT_EQ(esp_codec_dev_write(speaker_, pcm.data(), static_cast<int>(pcm.size() * 2)),
            ESP_CODEC_DEV_OK);
  EXPECT_EQ(sink.bytes, pcm.size() * 2);
  EXPECT_GE(sink.queues, 20);
  EXPECT_GT(sink.polls, sink.queues);  // it waited for room between pieces
}

TEST_F(Codec, SinkThatRefusesSoundIsAbandoned) {
  FakeSink sink;
  sink.fail_queue = true;
  preflash::audio::set_sink(&sink);
  auto fs = mono(16000);
  ASSERT_EQ(esp_codec_dev_open(speaker_, &fs), ESP_CODEC_DEV_OK);
  std::vector<int16_t> pcm(16000);
  const size_t before = preflash::audio_samples_written();
  ASSERT_EQ(esp_codec_dev_write(speaker_, pcm.data(), static_cast<int>(pcm.size() * 2)),
            ESP_CODEC_DEV_OK);
  ASSERT_EQ(esp_codec_dev_write(speaker_, pcm.data(), static_cast<int>(pcm.size() * 2)),
            ESP_CODEC_DEV_OK);
  EXPECT_EQ(sink.queues, 1);  // not tried again
  EXPECT_EQ(preflash::audio_samples_written() - before, 32000u);
}

TEST_F(Codec, ReopeningGivesTheDeviceAFreshChance) {
  FakeSink sink;
  sink.fail_queue = true;
  preflash::audio::set_sink(&sink);
  auto fs = mono(16000);
  ASSERT_EQ(esp_codec_dev_open(speaker_, &fs), ESP_CODEC_DEV_OK);
  std::vector<int16_t> pcm(160);
  ASSERT_EQ(esp_codec_dev_write(speaker_, pcm.data(), static_cast<int>(pcm.size() * 2)),
            ESP_CODEC_DEV_OK);
  ASSERT_EQ(sink.queues, 1);
  ASSERT_EQ(esp_codec_dev_close(speaker_), ESP_CODEC_DEV_OK);

  sink.fail_queue = false;
  ASSERT_EQ(esp_codec_dev_open(speaker_, &fs), ESP_CODEC_DEV_OK);
  ASSERT_EQ(esp_codec_dev_write(speaker_, pcm.data(), static_cast<int>(pcm.size() * 2)),
            ESP_CODEC_DEV_OK);
  EXPECT_EQ(sink.queues, 2);
  EXPECT_EQ(sink.bytes, pcm.size() * 2);
}

TEST_F(Codec, CloseDoesNotWaitOutAWriteThatIsPacing) {
  FakeSink sink;
  sink.waiting = 1'000'000'000;  // never drains enough to have room
  preflash::audio::set_sink(&sink);
  auto fs = mono(16000);
  ASSERT_EQ(esp_codec_dev_open(speaker_, &fs), ESP_CODEC_DEV_OK);
  std::thread writer([&] {
    int16_t pcm[160] = {};
    esp_codec_dev_write(speaker_, pcm, sizeof(pcm));
  });
  while (sink.polls < 3) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(esp_codec_dev_close(speaker_), ESP_CODEC_DEV_OK);
  // A close that waited out the write would wait for a sink that never drains, so it
  // would not return at all. The limit only has to be far under that, which keeps a
  // loaded machine from tripping it.
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(10));
  writer.join();
}

TEST_F(Codec, DeviceIsAskedForOnTheFirstOpenOnly) {
  std::atomic<int> asked{0};
  preflash::audio::set_device_request([&] { ++asked; });
  auto fs = mono(16000);
  ASSERT_EQ(esp_codec_dev_open(speaker_, &fs), ESP_CODEC_DEV_OK);
  EXPECT_EQ(asked, 1);
  ASSERT_EQ(esp_codec_dev_close(speaker_), ESP_CODEC_DEV_OK);
  ASSERT_EQ(esp_codec_dev_open(speaker_, &fs), ESP_CODEC_DEV_OK);
  EXPECT_EQ(asked, 1);
}

TEST_F(Codec, DeviceThatArrivesAfterOpenPlaysFromThenOn) {
  FakeSink sink;
  std::atomic<int> asked{0};
  preflash::audio::set_device_request([&] { ++asked; });
  auto fs = mono(16000);
  ASSERT_EQ(esp_codec_dev_open(speaker_, &fs), ESP_CODEC_DEV_OK);
  ASSERT_EQ(asked, 1);

  // Until the device arrives, sound is dropped and still takes its time.
  std::vector<int16_t> pcm(1600);  // 100 ms
  const auto start = std::chrono::steady_clock::now();
  ASSERT_EQ(esp_codec_dev_write(speaker_, pcm.data(), static_cast<int>(pcm.size() * 2)),
            ESP_CODEC_DEV_OK);
  EXPECT_GE(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(100));
  EXPECT_EQ(sink.queues, 0);

  preflash::audio::set_sink(&sink);
  EXPECT_EQ(sink.rate, 16000) << "started with the format that is open";
  EXPECT_EQ(sink.channels, 1);
  ASSERT_EQ(esp_codec_dev_write(speaker_, pcm.data(), static_cast<int>(pcm.size() * 2)),
            ESP_CODEC_DEV_OK);
  EXPECT_EQ(sink.bytes, pcm.size() * 2);
}

TEST_F(Codec, WriteBeforeOpenFails) {
  int16_t pcm[16] = {};
  const size_t before = preflash::audio_samples_written();
  EXPECT_EQ(esp_codec_dev_write(speaker_, pcm, sizeof(pcm)), ESP_CODEC_DEV_WRONG_STATE);
  EXPECT_EQ(preflash::audio_samples_written(), before);
}

TEST_F(Codec, HeadlessAcceptsAudioAndPlaysNothing) {
  use_fake_clock();
  auto fs = mono(16000);
  ASSERT_EQ(esp_codec_dev_open(speaker_, &fs), ESP_CODEC_DEV_OK);
  int16_t pcm[160] = {};  // 10 ms
  const size_t before = preflash::audio_samples_written();
  const auto start = fake_.now;
  for (int i = 0; i < 20; ++i)
    ASSERT_EQ(esp_codec_dev_write(speaker_, pcm, sizeof(pcm)), ESP_CODEC_DEV_OK);
  EXPECT_EQ(preflash::audio_samples_written() - before, 3200u);
  // Each write waits out its own 10 ms, so the waits add up to the sound's 200 ms.
  ASSERT_EQ(fake_.wakes.size(), 20u);
  std::chrono::steady_clock::duration waited{};
  auto from = start;
  for (const auto wake : fake_.wakes) {
    waited += wake - from;
    from = wake;
  }
  EXPECT_EQ(std::chrono::duration_cast<std::chrono::microseconds>(waited).count(), 200'000);
}

TEST_F(Codec, DroppedSoundTakesAsLongAsItWouldToPlay) {
  auto fs = mono(16000);
  ASSERT_EQ(esp_codec_dev_open(speaker_, &fs), ESP_CODEC_DEV_OK);
  std::vector<int16_t> pcm(1600);  // 100 ms
  const auto start = std::chrono::steady_clock::now();
  ASSERT_EQ(esp_codec_dev_write(speaker_, pcm.data(), static_cast<int>(pcm.size() * 2)),
            ESP_CODEC_DEV_OK);
  EXPECT_GE(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(100));
}

TEST_F(Codec, SinkFailingMidWriteOnlyPacesWhatItDropped) {
  use_fake_clock();
  FakeSink sink;
  sink.fail_at_queue = 10;
  preflash::audio::set_sink(&sink);
  auto fs = mono(16000);
  ASSERT_EQ(esp_codec_dev_open(speaker_, &fs), ESP_CODEC_DEV_OK);
  std::vector<int16_t> pcm(16000);  // one second, of which the sink takes 900 ms
  const auto start = fake_.now;
  ASSERT_EQ(esp_codec_dev_write(speaker_, pcm.data(), static_cast<int>(pcm.size() * 2)),
            ESP_CODEC_DEV_OK);
  EXPECT_EQ(sink.bytes, 9u * 1600 * 2);
  // Only the 100 ms the sink never took is waited out.
  EXPECT_EQ(fake_.wakes_after(start), (std::vector<long long>{100'000}));
}

TEST_F(Codec, RejectsFormatsItCannotPlay) {
  esp_codec_dev_sample_info_t fs = mono(16000);
  fs.bits_per_sample = 24;
  EXPECT_EQ(esp_codec_dev_open(speaker_, &fs), ESP_CODEC_DEV_INVALID_ARG);
}

TEST_F(Codec, TapSeesOpenWritesAndClose) {
  FakeTap tap;
  const int id = preflash::audio::add_tap(&tap);
  EXPECT_TRUE(tap.events.empty());  // codec is closed
  auto fs = mono(16000);
  ASSERT_EQ(esp_codec_dev_open(speaker_, &fs), ESP_CODEC_DEV_OK);
  ASSERT_EQ(tap.formats.size(), 1u);
  EXPECT_EQ(tap.formats[0].rate, 16000);
  EXPECT_EQ(tap.formats[0].channels, 1);
  int16_t pcm[4] = {1, -2, 3, -4};
  ASSERT_EQ(esp_codec_dev_write(speaker_, pcm, sizeof(pcm)), ESP_CODEC_DEV_OK);
  ASSERT_EQ(tap.writes.size(), 1u);
  EXPECT_EQ(tap.writes[0], (std::vector<int16_t>{1, -2, 3, -4}));
  ASSERT_EQ(esp_codec_dev_close(speaker_), ESP_CODEC_DEV_OK);
  EXPECT_EQ(tap.stops, 1);
  EXPECT_EQ(tap.events, "fsx");
  preflash::audio::remove_tap(id);
  ASSERT_EQ(esp_codec_dev_open(speaker_, &fs), ESP_CODEC_DEV_OK);
  EXPECT_EQ(tap.events, "fsx");  // removed taps hear nothing
}

TEST_F(Codec, TapCarriesVolume) {
  FakeTap tap;
  const int id = preflash::audio::add_tap(&tap);
  auto fs = mono(16000);
  ASSERT_EQ(esp_codec_dev_open(speaker_, &fs), ESP_CODEC_DEV_OK);
  ASSERT_EQ(esp_codec_dev_set_out_vol(speaker_, 50), ESP_CODEC_DEV_OK);
  int16_t pcm[2] = {1000, -1000};
  ASSERT_EQ(esp_codec_dev_write(speaker_, pcm, sizeof(pcm)), ESP_CODEC_DEV_OK);
  ASSERT_EQ(esp_codec_dev_set_out_vol(speaker_, 100), ESP_CODEC_DEV_OK);
  ASSERT_EQ(esp_codec_dev_write(speaker_, pcm, sizeof(pcm)), ESP_CODEC_DEV_OK);
  ASSERT_EQ(tap.writes.size(), 2u);
  EXPECT_EQ(tap.writes[0], (std::vector<int16_t>{500, -500}));
  EXPECT_EQ(tap.writes[1], (std::vector<int16_t>{1000, -1000}));
  preflash::audio::remove_tap(id);
}

TEST_F(Codec, TapAddedWhileOpenGetsTheFormat) {
  auto fs = mono(16000);
  ASSERT_EQ(esp_codec_dev_open(speaker_, &fs), ESP_CODEC_DEV_OK);
  FakeTap tap;
  const int id = preflash::audio::add_tap(&tap);
  ASSERT_EQ(tap.formats.size(), 1u);
  EXPECT_EQ(tap.formats[0].rate, 16000);
  EXPECT_EQ(tap.formats[0].channels, 1);
  preflash::audio::remove_tap(id);
}

TEST_F(Codec, TapDoesNotChangePacing) {
  use_fake_clock();
  auto fs = mono(16000);
  ASSERT_EQ(esp_codec_dev_open(speaker_, &fs), ESP_CODEC_DEV_OK);
  std::vector<int16_t> pcm(1600);  // 100 ms
  // The wakes one write asks for, counted from when it started.
  auto paced = [&] {
    const auto start = fake_.now;
    const size_t first = fake_.wakes.size();
    EXPECT_EQ(esp_codec_dev_write(speaker_, pcm.data(), static_cast<int>(pcm.size() * 2)),
              ESP_CODEC_DEV_OK);
    return fake_.wakes_after(start, first);
  };
  const auto without = paced();
  FakeTap tap;
  const int id = preflash::audio::add_tap(&tap);
  const auto with = paced();
  preflash::audio::remove_tap(id);
  EXPECT_EQ(without, (std::vector<long long>{100'000}));
  EXPECT_EQ(with, without);
  EXPECT_EQ(tap.writes.size(), 1u);
}

TEST_F(Codec, TapWithASinkStillQueues) {
  FakeSink sink;
  sink.drain_on_queue = true;
  preflash::audio::set_sink(&sink);
  FakeTap tap;
  const int id = preflash::audio::add_tap(&tap);
  auto fs = mono(16000);
  ASSERT_EQ(esp_codec_dev_open(speaker_, &fs), ESP_CODEC_DEV_OK);
  ASSERT_EQ(esp_codec_dev_set_out_vol(speaker_, 50), ESP_CODEC_DEV_OK);
  std::vector<int16_t> pcm(16000 * 2, 1000);  // two seconds
  ASSERT_EQ(esp_codec_dev_write(speaker_, pcm.data(), static_cast<int>(pcm.size() * 2)),
            ESP_CODEC_DEV_OK);
  EXPECT_EQ(sink.bytes, pcm.size() * 2);
  EXPECT_GE(sink.queues, 20);
  EXPECT_GT(sink.polls, sink.queues);
  ASSERT_EQ(tap.writes.size(), 1u);  // the tap sees the write once, whole
  EXPECT_EQ(tap.writes[0].size(), pcm.size());
  EXPECT_EQ(tap.writes[0][0], 500);
  preflash::audio::remove_tap(id);
}

TEST(Expander, SetThenGetLevel) {
  esp_io_expander_handle_t io = bsp_io_expander_init();
  ASSERT_EQ(esp_io_expander_set_dir(io, IO_EXPANDER_PIN_NUM_3, IO_EXPANDER_OUTPUT), ESP_OK);
  ASSERT_EQ(esp_io_expander_set_level(io, IO_EXPANDER_PIN_NUM_3, 1), ESP_OK);
  uint32_t level = 0;
  ASSERT_EQ(esp_io_expander_get_level(io, IO_EXPANDER_PIN_NUM_3, &level), ESP_OK);
  EXPECT_NE(level, 0u);
  ASSERT_EQ(esp_io_expander_set_level(io, IO_EXPANDER_PIN_NUM_3, 0), ESP_OK);
  ASSERT_EQ(esp_io_expander_get_level(io, IO_EXPANDER_PIN_NUM_3, &level), ESP_OK);
  EXPECT_EQ(level, 0u);
}

TEST(Expander, SettingAnInputFails) {
  esp_io_expander_handle_t io = bsp_io_expander_init();
  ASSERT_EQ(esp_io_expander_set_dir(io, IO_EXPANDER_PIN_NUM_5, IO_EXPANDER_INPUT), ESP_OK);
  EXPECT_EQ(esp_io_expander_set_level(io, IO_EXPANDER_PIN_NUM_5, 1), ESP_ERR_INVALID_STATE);
}

TEST(Expander, UnknownDirectionIsRejectedAndChangesNothing) {
  esp_io_expander_handle_t io = bsp_io_expander_init();
  ASSERT_EQ(esp_io_expander_set_dir(io, IO_EXPANDER_PIN_NUM_6, IO_EXPANDER_OUTPUT), ESP_OK);
  EXPECT_EQ(esp_io_expander_set_dir(io, IO_EXPANDER_PIN_NUM_6, static_cast<esp_io_expander_dir_t>(7)),
            ESP_ERR_INVALID_ARG);
  EXPECT_EQ(esp_io_expander_set_level(io, IO_EXPANDER_PIN_NUM_6, 1), ESP_OK);
}

TEST(Expander, SpeakerInitSwitchesTheAmpOn) {
  bsp_audio_codec_speaker_init();
  uint32_t level = 0;
  ASSERT_EQ(esp_io_expander_get_level(bsp_io_expander_init(), BSP_POWER_AMP_IO, &level), ESP_OK);
  EXPECT_NE(level, 0u);
  ASSERT_EQ(bsp_audio_poweramp_enable(false), ESP_OK);
  ASSERT_EQ(esp_io_expander_get_level(bsp_io_expander_init(), BSP_POWER_AMP_IO, &level), ESP_OK);
  EXPECT_EQ(level, 0u);
}
