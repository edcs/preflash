#include <gtest/gtest.h>

#include <chrono>
#include <vector>

#include "bsp/audio.h"
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
    if (fail_queue) return false;
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
  int rate = 0, channels = 0, waiting = 0, polls = 0, queues = 0;
  bool fail_queue = false, drain_on_queue = false;
  size_t bytes = 0;
  std::vector<int16_t> last;
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
  void SetUp() override { preflash::audio::set_sink(nullptr); }
  void TearDown() override {
    esp_codec_dev_close(speaker_);
    preflash::audio::set_sink(nullptr);
  }
  esp_codec_dev_handle_t speaker_ = bsp_audio_codec_speaker_init();
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

TEST_F(Codec, WriteBeforeOpenFails) {
  int16_t pcm[16] = {};
  const size_t before = preflash::audio_samples_written();
  EXPECT_EQ(esp_codec_dev_write(speaker_, pcm, sizeof(pcm)), ESP_CODEC_DEV_WRONG_STATE);
  EXPECT_EQ(preflash::audio_samples_written(), before);
}

TEST_F(Codec, HeadlessAcceptsAudioAndPlaysNothing) {
  auto fs = mono(16000);
  ASSERT_EQ(esp_codec_dev_open(speaker_, &fs), ESP_CODEC_DEV_OK);
  int16_t pcm[160] = {};
  const size_t before = preflash::audio_samples_written();
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 20; ++i)
    ASSERT_EQ(esp_codec_dev_write(speaker_, pcm, sizeof(pcm)), ESP_CODEC_DEV_OK);
  EXPECT_EQ(preflash::audio_samples_written() - before, 3200u);
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1));
}

TEST_F(Codec, RejectsFormatsItCannotPlay) {
  esp_codec_dev_sample_info_t fs = mono(16000);
  fs.bits_per_sample = 24;
  EXPECT_EQ(esp_codec_dev_open(speaker_, &fs), ESP_CODEC_DEV_INVALID_ARG);
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
