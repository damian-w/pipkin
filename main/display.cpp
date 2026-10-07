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

#include <algorithm>
#include <array>
#include <cstddef>

namespace {

constexpr int kWidth = 320;
constexpr int kHeight = 240;
constexpr std::size_t kBandBytes = kWidth * kDisplayBandRows * sizeof(uint16_t);

esp_lcd_panel_io_handle_t lcd;
spi_device_handle_t touch;
std::array<uint8_t*, 2> band_buffers{};
std::size_t next_buffer = 0;

void lcd_command(int command, const void* bytes = nullptr, std::size_t length = 0) {
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(lcd, command, bytes, length));
}

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

} // namespace

void display_backlight(int permille) {
    const int level = std::clamp(permille, 0, 1000);
    // Squared so equal steps look even; 10-bit duty.
    const auto duty = static_cast<uint32_t>(level * level * 1023 / 1000000);
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));
}

void display_init() {
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

    lcd_command(0x01);
    vTaskDelay(pdMS_TO_TICKS(150));
    lcd_command(0x11);
    vTaskDelay(pdMS_TO_TICKS(120));
    const uint8_t format = 0x55;
    lcd_command(0x3A, &format, 1);
    lcd_command(0x36, &board::kLcdMemoryAccess, 1);
    lcd_command(0x29);

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

void display_rows(int y, int rows, const uint16_t* pixels) {
    ESP_ERROR_CHECK(y >= 0 && rows > 0 && rows <= kDisplayBandRows && y + rows <= kHeight
                        ? ESP_OK
                        : ESP_ERR_INVALID_ARG);
    // Parameter writes drain queued colour transfers, so this buffer's previous band has been sent.
    auto* buffer = band_buffers[next_buffer];
    next_buffer ^= 1;
    for (int i = 0; i < rows * kWidth; ++i) {
        buffer[i * 2] = pixels[i] >> 8;
        buffer[i * 2 + 1] = pixels[i] & 0xff;
    }
    const uint8_t columns[] = {0, 0, (kWidth - 1) >> 8, (kWidth - 1) & 0xff};
    const int last = y + rows - 1;
    const uint8_t range[] = {static_cast<uint8_t>(y >> 8), static_cast<uint8_t>(y & 0xff),
                             static_cast<uint8_t>(last >> 8), static_cast<uint8_t>(last & 0xff)};
    lcd_command(0x2A, columns, sizeof(columns));
    lcd_command(0x2B, range, sizeof(range));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(lcd, 0x2C, buffer, rows * kWidth * sizeof(uint16_t)));
}

bool touch_read(int& x, int& y) {
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
    return true;
}
