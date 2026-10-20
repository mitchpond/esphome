// 4-grey driver for the UC8179 7.5" 800x480 panel (XIAO 7.5" ePaper Panel, Seeed glass 104990861).
//
// Every refresh uses the panel's own 4-grey waveform (Waveshare EPD_7IN5_V2_Init_4Gray: forced
// temperature 0x5F selects it from OTP). The framebuffer is the controller's two RAM planes, kept as two
// separate 48 KB blocks (the C3 has no PSRAM and rarely a free 96 KB block):
//   level (0 black .. 3 white) == 2-bit code;  old plane 0x10 bit = !(code & 1),  new plane 0x13 bit = !(code & 2)
//
// Colours: luminance picks the level. Antialiased fonts (bpp: 2) blend from the default background
// (transparent black), so a glyph edge pixel arrives premultiplied with its coverage in Color::w; it is
// composited over what is already in the framebuffer, so text smooths correctly on any background without
// passing one to print(). Colours with w == 0 or 255 (e.g. Color(r, g, b)) are opaque.
//
// Photos: load_photo(url) streams a BMP (1/4/8 bpp, 800x480, palette greys) straight into the planes
// in loop(), then show() refreshes the panel without running the display lambda.
#pragma once

#include <array>
#include <memory>
#include <string>

#include "esphome/components/display/display_buffer.h"
#include "esphome/components/http_request/http_request.h"
#include "esphome/components/spi/spi.h"
#include "esphome/core/component.h"

namespace esphome::uc8179_grey {

class UC8179Grey : public display::DisplayBuffer,
                   public spi::SPIDevice<spi::BIT_ORDER_MSB_FIRST, spi::CLOCK_POLARITY_LOW,
                                         spi::CLOCK_PHASE_LEADING, spi::DATA_RATE_4MHZ> {
 public:
  static constexpr int W = 800, H = 480;
  static constexpr size_t PLANE = W * H / 8;
  enum class Photo : uint8_t { IDLE, LOADING, OK, FAILED };

  void set_dc_pin(GPIOPin *pin) { this->dc_pin_ = pin; }
  void set_reset_pin(GPIOPin *pin) { this->reset_pin_ = pin; }
  void set_busy_pin(GPIOPin *pin) { this->busy_pin_ = pin; }
  void set_http(http_request::HttpRequestComponent *http) { this->http_ = http; }

  void setup() override;
  void loop() override;
  void update() override;  // run the lambda, then show()
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::PROCESSOR; }
  display::DisplayType get_display_type() override { return display::DisplayType::DISPLAY_TYPE_GRAYSCALE; }
  void fill(Color color) override;

  /// Send the framebuffer to the panel and refresh (blocks for the refresh, ~4 s).
  void show();
  /// Grey level 0 (black) .. 3 (white) at x, y.
  uint8_t get_level(int x, int y) const;
  bool has_buffer() const { return this->old_ != nullptr && this->new_ != nullptr; }

  void load_photo(const std::string &url);
  Photo photo_state() const { return this->photo_; }

 protected:
  int get_width_internal() override { return W; }
  int get_height_internal() override { return H; }
  void draw_absolute_pixel_internal(int x, int y, Color color) override;
  void set_level_(int x, int y, uint8_t level);
  static uint8_t level_of_(uint32_t lum) { return lum >= 213 ? 3 : lum >= 128 ? 2 : lum >= 43 ? 1 : 0; }

  void command_(uint8_t c);
  void data_(uint8_t d);
  void plane_(uint8_t c, const uint8_t *buf);
  bool wait_idle_(uint32_t timeout_ms);

  void photo_byte_(uint8_t b);
  void photo_finish_(bool ok);

  GPIOPin *dc_pin_{nullptr}, *reset_pin_{nullptr}, *busy_pin_{nullptr};
  http_request::HttpRequestComponent *http_{nullptr};
  uint8_t *old_{nullptr}, *new_{nullptr};

  // photo download / BMP parser state
  Photo photo_{Photo::IDLE};
  std::shared_ptr<http_request::HttpContainer> download_;
  uint32_t last_data_ms_{0}, started_ms_{0};
  uint32_t pos_{0}, data_off_{0}, pal_off_{0}, pal_n_{0}, stride_{0};
  uint8_t bpp_{0};
  bool top_down_{false};
  int rows_{0};
  std::array<uint8_t, 54> hdr_{};
  std::array<uint8_t, 256> lut_{};  // palette index -> level
  uint8_t pal_b_{0}, pal_g_{0};
};

}  // namespace esphome::uc8179_grey
