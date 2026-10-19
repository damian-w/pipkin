#include "display.h"

#include "board.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pipkin/model.h"
#if PIPKIN_BOARD_ES3C40P
#include "driver/i2c_master.h"
#endif

#include <algorithm>
#include <array>
#include <cstddef>

namespace {

constexpr int kWidth = pipkin::kDisplayWidth;
constexpr int kHeight = pipkin::kDisplayHeight;
constexpr std::size_t kBandBytes = kWidth * kDisplayBandRows * sizeof(uint16_t);

esp_lcd_panel_io_handle_t lcd;
std::array<uint8_t*, 2> band_buffers{};
std::size_t next_buffer = 0;

void lcd_command(int command, const void* bytes = nullptr, std::size_t length = 0) {
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(lcd, command, bytes, length));
}

#if PIPKIN_BOARD_ES3C40P
i2c_master_dev_handle_t touch;

// Resets the FT6336U; the panel's start-up commands give it time to start before touch_init().
void touch_reset() {
    gpio_config_t reset{};
    reset.pin_bit_mask = 1ULL << board::kTouchReset;
    reset.mode = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&reset));
    ESP_ERROR_CHECK(gpio_set_level(static_cast<gpio_num_t>(board::kTouchReset), 0));
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_ERROR_CHECK(gpio_set_level(static_cast<gpio_num_t>(board::kTouchReset), 1));
}

void touch_init() {
    i2c_master_bus_config_t bus{};
    bus.i2c_port = I2C_NUM_0;
    bus.sda_io_num = static_cast<gpio_num_t>(board::kTouchSda);
    bus.scl_io_num = static_cast<gpio_num_t>(board::kTouchScl);
    bus.clk_source = I2C_CLK_SRC_DEFAULT;
    bus.glitch_ignore_cnt = 7;
    i2c_master_bus_handle_t handle;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus, &handle));
    i2c_device_config_t device{};
    device.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device.device_address = board::kTouchAddress;
    device.scl_speed_hz = 400'000;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(handle, &device, &touch));
    // Polling mode keeps INT low while a finger is down. A controller that is still starting
    // ignores this, and touch_read() polls the touch registers anyway.
    const uint8_t polling[] = {0xA4, 0x00};
    i2c_master_transmit(touch, polling, sizeof(polling), 50);
}
#else
spi_device_handle_t touch;

uint16_t touch_sample(uint8_t command) {
    spi_transaction_t transaction{};
    transaction.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
    transaction.length = 24;
    transaction.tx_data[0] = command;
    ESP_ERROR_CHECK(spi_device_polling_transmit(touch, &transaction));
    return ((transaction.rx_data[1] << 8) | transaction.rx_data[2]) >> 3;
}

int touch_median(uint8_t command) {
    std::array<uint16_t, 3> samples{};
    for (auto& sample : samples)
        sample = touch_sample(command);
    std::sort(samples.begin(), samples.end());
    return samples[1];
}

void touch_init(spi_bus_config_t bus) {
    bus.mosi_io_num = board::kTouchMosi;
    bus.miso_io_num = board::kTouchMiso;
    bus.sclk_io_num = board::kTouchClock;
    bus.max_transfer_sz = 3;
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_DISABLED));
    spi_device_interface_config_t device{};
    device.clock_speed_hz = 1'000'000;
    device.spics_io_num = board::kTouchSelect;
    device.queue_size = 1;
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &device, &touch));
    gpio_config_t input{};
    input.pin_bit_mask = 1ULL << board::kTouchInterrupt;
    input.mode = GPIO_MODE_INPUT;
    ESP_ERROR_CHECK(gpio_config(&input));
}
#endif

} // namespace

void display_backlight(int permille) {
    const int level = std::clamp(permille, 0, 1000);
    // Squared so equal steps look even; 10-bit duty.
    const auto duty = static_cast<uint32_t>(level * level * 1023 / 1000000);
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));
}

// Display off (0x28) and on (0x29). A dark panel left showing a still frame, such as the
// setup QR code, can keep a faint ghost of it, so it shows nothing while the backlight is off.
void display_power(bool on) { lcd_command(on ? 0x29 : 0x28); }

void display_init() {
#if PIPKIN_BOARD_ES3C40P
    touch_reset();
#endif
    ledc_timer_config_t timer{};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.duty_resolution = LEDC_TIMER_10_BIT;
    timer.timer_num = LEDC_TIMER_0;
    timer.freq_hz = 5000;
    timer.clk_cfg = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ledc_channel_config_t backlight{};
    backlight.gpio_num = board::kBacklight;
    backlight.speed_mode = LEDC_LOW_SPEED_MODE;
    backlight.channel = LEDC_CHANNEL_0;
    backlight.timer_sel = LEDC_TIMER_0;
    backlight.duty = 0;
    backlight.flags.output_invert = !board::kBacklightActiveHigh;
    ESP_ERROR_CHECK(ledc_channel_config(&backlight));

    spi_bus_config_t bus{};
    bus.mosi_io_num = board::kLcdMosi;
    bus.miso_io_num = board::kLcdMiso;
    bus.sclk_io_num = board::kLcdClock;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = kBandBytes;
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_spi_config_t io{};
    io.cs_gpio_num = board::kLcdSelect;
    io.dc_gpio_num = board::kLcdDataCommand;
    io.pclk_hz = board::kLcdClockHz;
    io.trans_queue_depth = 2;
    io.lcd_cmd_bits = 8;
    io.lcd_param_bits = 8;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        static_cast<esp_lcd_spi_bus_handle_t>(SPI2_HOST), &io, &lcd));
    for (auto& buffer : band_buffers) {
        buffer = static_cast<uint8_t*>(heap_caps_malloc(kBandBytes, MALLOC_CAP_DMA));
        ESP_ERROR_CHECK(buffer ? ESP_OK : ESP_ERR_NO_MEM);
    }

    for (const auto& step : board::kLcdInit) {
        lcd_command(step.command, step.length ? step.data : nullptr, step.length);
        if (step.delay_ms)
            vTaskDelay(pdMS_TO_TICKS(step.delay_ms));
    }

#if PIPKIN_BOARD_ES3C40P
    touch_init();
#else
    touch_init(bus);
#endif
}

void display_rows(int y, const uint16_t* pixels) {
    // Parameter writes drain queued colour transfers, so this buffer's previous band has been sent.
    auto* buffer = band_buffers[next_buffer];
    next_buffer ^= 1;
    for (int i = 0; i < kDisplayBandRows * kWidth; ++i) {
        buffer[i * 2] = pixels[i] >> 8;
        buffer[i * 2 + 1] = pixels[i] & 0xff;
    }
    const uint8_t columns[] = {0, 0, (kWidth - 1) >> 8, (kWidth - 1) & 0xff};
    const int last = y + kDisplayBandRows - 1;
    const uint8_t range[] = {static_cast<uint8_t>(y >> 8), static_cast<uint8_t>(y & 0xff),
                             static_cast<uint8_t>(last >> 8), static_cast<uint8_t>(last & 0xff)};
    lcd_command(0x2A, columns, sizeof(columns));
    lcd_command(0x2B, range, sizeof(range));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(lcd, 0x2C, buffer, kBandBytes));
}

bool touch_read(int& x, int& y) {
#if PIPKIN_BOARD_ES3C40P
    // TD_STATUS, then the first point: event and X high bits, X low, Y high bits, Y low.
    const uint8_t start = 0x02;
    uint8_t point[5]{};
    if (i2c_master_transmit_receive(touch, &start, 1, point, sizeof(point), 10) != ESP_OK)
        return false;
    const int count = point[0] & 0x0f;
    const int event = point[1] >> 6;
    // Counts above two are noise; event 1 is a lift.
    if (count < 1 || count > 2 || event == 1)
        return false;
    board::touch_to_panel(((point[1] & 0x0f) << 8) | point[2], ((point[3] & 0x0f) << 8) | point[4],
                          x, y);
#else
    if (gpio_get_level(static_cast<gpio_num_t>(board::kTouchInterrupt)) != 0)
        return false;
    int raw_x = touch_median(0xD0);
    int raw_y = touch_median(0x90);
    if (gpio_get_level(static_cast<gpio_num_t>(board::kTouchInterrupt)) != 0)
        return false;
    if (board::kTouchSwapXY)
        std::swap(raw_x, raw_y);
    x = board::touch_coordinate(raw_x, board::kTouchXStart, board::kTouchXEnd, kWidth);
    y = board::touch_coordinate(raw_y, board::kTouchYStart, board::kTouchYEnd, kHeight);
#endif
    // Panel pixels to the layout's coordinates.
    x = x * pipkin::kLayoutWidth / kWidth;
    y = y * pipkin::kLayoutHeight / kHeight;
    return true;
}
