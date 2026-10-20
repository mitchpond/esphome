#include "uc8179_grey.h"

#include <cstdlib>
#include <cstring>

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::uc8179_grey {

static const char *const TAG = "uc8179_grey";

void UC8179Grey::setup() {
  this->spi_setup();
  this->dc_pin_->setup();
  this->reset_pin_->setup();
  this->reset_pin_->digital_write(true);
  this->busy_pin_->setup();
  // Two separate 48 KB blocks, allocated before Wi-Fi fragments the heap.
  this->old_ = static_cast<uint8_t *>(malloc(PLANE));
  this->new_ = static_cast<uint8_t *>(malloc(PLANE));
  if (!this->has_buffer()) {
    ESP_LOGE(TAG, "Could not allocate the 2 x 48 KB framebuffer");
    this->mark_failed();
    return;
  }
  this->fill(Color(255, 255, 255));
}

void UC8179Grey::dump_config() {
  LOG_DISPLAY("", "UC8179 4-grey e-paper", this);
  LOG_PIN("  DC Pin: ", this->dc_pin_);
  LOG_PIN("  Reset Pin: ", this->reset_pin_);
  LOG_PIN("  Busy Pin: ", this->busy_pin_);
  LOG_UPDATE_INTERVAL(this);
}

// ---------------------------------------------------------------- framebuffer

uint8_t UC8179Grey::get_level(int x, int y) const {
  const size_t i = (y * W + x) >> 3;
  const uint8_t bit = 0x80 >> (x & 7);
  return ((this->old_[i] & bit) ? 0 : 1) | ((this->new_[i] & bit) ? 0 : 2);
}

void UC8179Grey::set_level_(int x, int y, uint8_t level) {
  const size_t i = (y * W + x) >> 3;
  const uint8_t bit = 0x80 >> (x & 7);
  if (level & 1) this->old_[i] &= ~bit; else this->old_[i] |= bit;
  if (level & 2) this->new_[i] &= ~bit; else this->new_[i] |= bit;
}

void UC8179Grey::draw_absolute_pixel_internal(int x, int y, Color c) {
  if (x < 0 || y < 0 || x >= W || y >= H || !this->has_buffer())
    return;
  uint32_t lum = (c.r * 77 + c.g * 150 + c.b * 29) >> 8;
  if (c.w != 0 && c.w != 255)  // antialiased edge: premultiplied colour over the current pixel
    lum = std::min<uint32_t>(255, lum + this->get_level(x, y) * 85 * (255 - c.w) / 255);
  this->set_level_(x, y, level_of_(lum));
}

void UC8179Grey::fill(Color c) {
  if (!this->has_buffer())
    return;
  const uint8_t level = level_of_((c.r * 77 + c.g * 150 + c.b * 29) >> 8);
  memset(this->old_, (level & 1) ? 0x00 : 0xFF, PLANE);
  memset(this->new_, (level & 2) ? 0x00 : 0xFF, PLANE);
}

// ---------------------------------------------------------------- panel

void UC8179Grey::command_(uint8_t c) {
  this->dc_pin_->digital_write(false);
  this->enable();
  this->write_byte(c);
  this->disable();
}

void UC8179Grey::data_(uint8_t d) {
  this->dc_pin_->digital_write(true);
  this->enable();
  this->write_byte(d);
  this->disable();
}

void UC8179Grey::plane_(uint8_t c, const uint8_t *buf) {
  this->command_(c);
  this->dc_pin_->digital_write(true);
  this->enable();
  for (size_t at = 0; at < PLANE; at += 4000) {
    this->write_array(buf + at, std::min<size_t>(4000, PLANE - at));
    App.feed_wdt();
  }
  this->disable();
}

// BUSY reads true (pin inverted in YAML) while the controller works.
bool UC8179Grey::wait_idle_(uint32_t timeout_ms) {
  const uint32_t t0 = millis();
  delay(5);
  while (this->busy_pin_->digital_read()) {
    if (millis() - t0 > timeout_ms)
      return false;
    delay(10);
    App.feed_wdt();
  }
  return true;
}

void UC8179Grey::update() {
  this->do_update_();
  this->show();
}

void UC8179Grey::show() {
  if (!this->has_buffer())
    return;
  const uint32_t t0 = millis();
  this->reset_pin_->digital_write(true);
  delay(20);
  this->reset_pin_->digital_write(false);
  delay(2);
  this->reset_pin_->digital_write(true);
  delay(20);

  // Waveshare EPD_7IN5_V2_Init_4Gray
  this->command_(0x00); this->data_(0x1F);                     // panel setting: KW mode, LUT from OTP
  this->command_(0x50); this->data_(0x10); this->data_(0x07);  // VCOM / data interval
  this->command_(0x04);                                        // power on
  delay(100);
  if (!this->wait_idle_(5000)) {
    ESP_LOGE(TAG, "Panel busy timeout after power on");
    return;
  }
  this->command_(0x06);                                        // booster soft start
  this->data_(0x27); this->data_(0x27); this->data_(0x18); this->data_(0x17);
  this->command_(0x61);                                        // resolution 800 x 480
  this->data_(0x03); this->data_(0x20); this->data_(0x01); this->data_(0xE0);
  this->command_(0xE0); this->data_(0x02);                     // cascade: use forced temperature
  this->command_(0xE5); this->data_(0x5F);                     // forced temperature -> OTP 4-grey waveform

  this->plane_(0x10, this->old_);
  this->plane_(0x13, this->new_);
  const uint32_t t1 = millis();
  this->command_(0x12);                                        // refresh
  delay(100);
  const bool ok = this->wait_idle_(30000);
  ESP_LOGI(TAG, "Refresh %s: data %u ms, refresh %u ms", ok ? "done" : "TIMED OUT", (unsigned) (t1 - t0),
           (unsigned) (millis() - t1));

  this->command_(0x50); this->data_(0xF7);                     // panel deep sleep (the image stays)
  this->command_(0x02);
  this->wait_idle_(5000);
  this->command_(0x07); this->data_(0xA5);
}

// ---------------------------------------------------------------- photos

void UC8179Grey::load_photo(const std::string &url) {
  if (this->photo_ == Photo::LOADING || !this->has_buffer())
    return;
  this->download_ = this->http_->get(url);
  if (this->download_ == nullptr || this->download_->status_code != 200) {
    ESP_LOGW(TAG, "Photo request failed (HTTP %d)", this->download_ ? this->download_->status_code : -1);
    this->photo_finish_(false);
    return;
  }
  this->pos_ = this->data_off_ = this->pal_off_ = this->pal_n_ = this->stride_ = 0;
  this->bpp_ = 0;
  this->rows_ = 0;
  this->lut_.fill(3);
  this->started_ms_ = this->last_data_ms_ = millis();
  this->fill(Color(255, 255, 255));
  this->photo_ = Photo::LOADING;
}

void UC8179Grey::loop() {
  if (this->photo_ != Photo::LOADING)
    return;
  uint8_t buf[1024];
  const uint32_t t0 = millis();
  while (this->photo_ == Photo::LOADING && millis() - t0 < 30) {
    const int n = this->download_->read(buf, sizeof(buf));
    if (n > 0) {
      this->last_data_ms_ = millis();
      for (int i = 0; i < n && this->photo_ == Photo::LOADING; i++)
        this->photo_byte_(buf[i]);
    } else if (n < 0) {
      this->photo_finish_(false);
    } else if (this->download_->is_read_complete() ||
               (this->download_->content_length > 0 &&
                this->download_->get_bytes_read() >= this->download_->content_length)) {
      this->photo_finish_(this->rows_ == H);
    } else if (millis() - this->last_data_ms_ > 10000) {
      ESP_LOGW(TAG, "Photo download stalled");
      this->photo_finish_(false);
    } else {
      break;  // no data yet: come back next loop
    }
  }
}

void UC8179Grey::photo_finish_(bool ok) {
  if (this->download_ != nullptr) {
    this->download_->end();
    this->download_ = nullptr;
  }
  if (ok)
    ESP_LOGI(TAG, "Photo: %d-bpp BMP decoded in %u ms", this->bpp_, (unsigned) (millis() - this->started_ms_));
  else if (this->photo_ == Photo::LOADING)
    ESP_LOGW(TAG, "Photo failed after %u bytes (%d rows)", (unsigned) this->pos_, this->rows_);
  this->photo_ = ok ? Photo::OK : Photo::FAILED;
}

// One byte of a BMP: 54-byte header, palette, pixel rows (bottom-up unless the height is negative).
void UC8179Grey::photo_byte_(uint8_t b) {
  const uint32_t p = this->pos_++;
  if (p < this->hdr_.size()) {
    this->hdr_[p] = b;
    if (p + 1 < this->hdr_.size())
      return;
    auto le = [&](int at, int n) {
      uint32_t v = 0;
      for (int i = n - 1; i >= 0; i--) v = (v << 8) | this->hdr_[at + i];
      return v;
    };
    const int32_t w = le(18, 4), h = le(22, 4);
    this->bpp_ = le(28, 2);
    this->data_off_ = le(10, 4);
    this->pal_off_ = 14 + le(14, 4);
    this->pal_n_ = le(46, 4) ? le(46, 4) : (1u << this->bpp_);
    this->top_down_ = h < 0;
    this->stride_ = ((W * this->bpp_ + 31) / 32) * 4;
    if (this->hdr_[0] != 'B' || this->hdr_[1] != 'M' || w != W || (h != H && h != -H) || le(30, 4) != 0 ||
        (this->bpp_ != 1 && this->bpp_ != 4 && this->bpp_ != 8) || this->pal_n_ > 256) {
      ESP_LOGW(TAG, "Unsupported BMP: %dx%d, %d bpp, compression %u (want 800x480, 1/4/8 bpp)", (int) w, (int) h,
               this->bpp_, (unsigned) le(30, 4));
      this->photo_finish_(false);
    }
    return;
  }
  if (p < this->data_off_) {
    if (p >= this->pal_off_ && p < this->pal_off_ + 4 * this->pal_n_) {
      const uint32_t k = p - this->pal_off_;
      switch (k & 3) {  // B, G, R, reserved
        case 0: this->pal_b_ = b; break;
        case 1: this->pal_g_ = b; break;
        case 2: this->lut_[k >> 2] = level_of_((b * 77 + this->pal_g_ * 150 + this->pal_b_ * 29) >> 8); break;
        default: break;
      }
    }
    return;
  }
  const uint32_t d = p - this->data_off_, row = d / this->stride_, col = d % this->stride_;
  if (row >= (uint32_t) H)
    return;
  const int y = this->top_down_ ? row : H - 1 - row;
  const int per = 8 / this->bpp_, x0 = col * per;
  for (int k = 0; k < per && x0 + k < W; k++) {
    const uint8_t idx = (b >> (8 - this->bpp_ * (k + 1))) & ((1 << this->bpp_) - 1);
    this->set_level_(x0 + k, y, this->lut_[idx]);
  }
  if (col == this->stride_ - 1)
    this->rows_ = row + 1;
}

}  // namespace esphome::uc8179_grey
