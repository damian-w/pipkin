#include "link.h"

#include "esp_check.h"

#if PIPKIN_BOARD_ES3C40P
#include "driver/usb_serial_jtag.h"

void link_init() {
    usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    config.rx_buffer_size = 2048;
    config.tx_buffer_size = 512;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&config));
}

// USB flow control holds the computer's writes until there is room, so nothing is lost.
int link_read(char* bytes, int size, TickType_t wait, bool& lost) {
    lost = false;
    return usb_serial_jtag_read_bytes(bytes, size, wait);
}

// Without a reader on the computer, the reply is dropped rather than stalling the display.
void link_write(const char* bytes, int count) {
    usb_serial_jtag_write_bytes(bytes, count, pdMS_TO_TICKS(50));
}
#else
#include "driver/uart.h"
#include "freertos/queue.h"

namespace {
QueueHandle_t events = nullptr;
}

void link_init() {
    uart_config_t uart{};
    uart.baud_rate = 115200;
    uart.data_bits = UART_DATA_8_BITS;
    uart.parity = UART_PARITY_DISABLE;
    uart.stop_bits = UART_STOP_BITS_1;
    uart.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart.source_clk = UART_SCLK_DEFAULT;
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &uart));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_0, 1, 3, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 2048, 0, 8, &events, 0));
}

int link_read(char* bytes, int size, TickType_t wait, bool& lost) {
    lost = false;
    uart_event_t event{};
    while (xQueueReceive(events, &event, 0) == pdTRUE) {
        if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL ||
            event.type == UART_FRAME_ERR || event.type == UART_PARITY_ERR) {
            ESP_ERROR_CHECK(uart_flush_input(UART_NUM_0));
            lost = true;
        }
    }
    const int count = uart_read_bytes(UART_NUM_0, bytes, size, wait);
    ESP_ERROR_CHECK(count >= 0 ? ESP_OK : ESP_FAIL);
    return count;
}

void link_write(const char* bytes, int count) {
    ESP_ERROR_CHECK(uart_write_bytes(UART_NUM_0, bytes, count) == count ? ESP_OK : ESP_FAIL);
}
#endif
