# uc8179_grey: 4-grey ESPHome driver for UC8179 7.5" e-paper

A display platform for 800×480 panels with the UC8179 controller, driven in the panel's 4-grey mode.
It was written for the Seeed **XIAO 7.5" ePaper Panel** (glass: Seeed 104990861, the same panel as the
Waveshare 7.5" V2). That panel has a XIAO ESP32-C3 without PSRAM, so the design keeps RAM flat.

Target: **ESPHome 2026.6.5** (ESP-IDF). It uses `display::DisplayBuffer`, `spi::SPIDevice` and
`http_request::HttpContainer` as they are in that release. Upstream's newer `epaper_spi` component (2026.9+)
has a black-and-white UC8179 driver but no greyscale.

## Use

On this branch it is a built-in platform. To use it with a stock ESPHome 2026.6.5 instead, copy this folder
to `components/uc8179_grey` next to your config and load it as an external component:

```yaml
external_components:
  - source: { type: local, path: components }

spi:
  clk_pin: GPIO8
  mosi_pin: GPIO10

http_request:          # required (photo streaming), even if load_photo() is never called

display:
  - platform: uc8179_grey
    id: epaper
    cs_pin: GPIO3
    dc_pin: GPIO5
    reset_pin: GPIO2
    busy_pin: { number: GPIO4, inverted: true }   # BUSY is low while the controller works
    update_interval: never
    lambda: |-
      it.fill(Color(255, 255, 255, 255));
      it.print(10, 10, id(my_font), Color(0, 0, 0, 255), "Hello");
```

Options: the standard display options plus `dc_pin`, `reset_pin`, `busy_pin` (all required), the SPI
device options (`cs_pin`, `data_rate`), and `http_request_id` (defaults to the only `http_request`).

## How it works

**Panel.** Every refresh is a full refresh with the 4-grey waveform stored in the panel's OTP. The
sequence is Waveshare's `EPD_7IN5_V2_Init_4Gray` / `Display_4Gray`
([waveshareteam/e-Paper](https://github.com/waveshareteam/e-Paper), MIT). Forcing the temperature
register (`0xE5 ← 0x5F`, with `0xE0 ← 0x02`) selects that waveform. The panel is reset and initialised
before each refresh and put into deep sleep after it. A refresh takes about 2.5 s: 0.37 s of SPI data at
4 MHz plus 2.1 s for the refresh itself. It blocks the main loop for that time, feeding the watchdog,
so ESPHome logs "took a long time" warnings.

**Framebuffer.** The controller takes two 1-bit planes, old RAM `0x10` and new RAM `0x13`, which
together encode 2 bits per pixel. The driver keeps exactly those planes, as two separate 48 KB blocks
allocated in `setup()` before Wi-Fi fragments the heap. A C3 rarely has one free 96 KB block.

| Level | Grey | Code | Old plane bit | New plane bit |
|---|---|---|---|---|
| 3 | white | 11 | 0 | 0 |
| 2 | light grey | 10 | 1 | 0 |
| 1 | dark grey | 01 | 0 | 1 |
| 0 | black | 00 | 1 | 1 |

Luminance chooses the level: `0.30 R + 0.59 G + 0.11 B`, with thresholds at 43, 128 and 213.

**Colours and antialiased text.** Use opaque colours, which have `w` of 0 or 255:
`Color(0, 0, 0, 255)`, `Color(85, 85, 85, 255)`, `Color(170, 170, 170, 255)`, `Color(255, 255, 255, 255)`.
Fonts with `bpp: 2` (or 4) antialias their edges. ESPHome blends each edge pixel between the text colour
and `print()`'s `background` argument, which defaults to transparent black `Color(0, 0, 0, 0)`.
Blending towards that default gives a premultiplied colour whose `w` channel equals the pixel's coverage.
The driver composites such pixels over what is already in the framebuffer:

```
lum = premultiplied_lum + current_level * 85 * (255 - w) / 255
```

As a result, text smooths correctly on any background, including white on black and black on grey,
without passing `background`. A colour with `w` strictly between 0 and 255 is always treated as
coverage, so don't use such colours on purpose.

**Photos.** `load_photo(url)` starts an HTTP GET. `loop()` then streams the body straight into the
planes, about 30 ms of work per loop pass, so no image buffer is needed. Afterwards `photo_state()`
reports `OK` or `FAILED`, and `show()` sends the planes without running the lambda. Supported input is an
uncompressed BMP, 800×480, 1, 4 or 8 bpp with a palette, bottom-up or top-down. Palette entries are
mapped to levels by luminance. An 800×480 4-bpp BMP is 192 KB and decodes in about 1.8 s over Wi-Fi.
The download clears the framebuffer to white first, so a failed download leaves a partial image in
the buffer. It is still not on the panel until `show()`.

```yaml
- lambda: 'id(epaper).load_photo("http://192.168.1.10/photo.bmp");'
- wait_until:
    condition:
      lambda: "return id(epaper).photo_state() != uc8179_grey::UC8179Grey::Photo::LOADING;"
    timeout: 60s
- if:
    condition:
      lambda: "return id(epaper).photo_state() == uc8179_grey::UC8179Grey::Photo::OK;"
    then:
      - lambda: "id(epaper).show();"
```

**C++ API:** `show()`, `get_level(x, y)` (0..3), `has_buffer()`, `load_photo(url)`, `photo_state()`.
`get_level()` is enough to export the framebuffer, for example as a 4-bpp BMP from a web handler.

## Limits

- Full refreshes only: no partial or fast refresh, which the panel only offers in black-and-white mode.
- 800×480 only (`W`/`H` constants).
- Refreshes block the main loop (see above).
- Tested only on the XIAO 7.5" panel. Ghosting is minimal because every refresh is a full one.

## Files

`display.py` holds the schema and codegen. `uc8179_grey.h` / `.cpp` hold the driver.
`tests/components/uc8179_grey` has a build test (ESP32-C3, ESP-IDF).
