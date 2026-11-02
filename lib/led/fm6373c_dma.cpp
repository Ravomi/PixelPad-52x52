// LCD_CAM setup adapted from ESP32 HUB75 LED MATRIX PANEL DMA Display,
// gdma_lcd_parallel16.cpp (Faptastic; original peripheral work by Adafruit /
// PaintYourDragon). See LICENSE-HUB75.txt for the retained MIT license.
#include "fm6373c_dma.h"
#include "fm6373c_log.h"
#include <driver/gpio.h>
#include <driver/periph_ctrl.h>
#include <esp_heap_caps.h>
#include <esp_rom_gpio.h>
#include <esp32s3/rom/cache.h>
#include <hal/gpio_hal.h>
#include <soc/gpio_sig_map.h>
#include <soc/lcd_cam_struct.h>
#include <string.h>

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "FM6373C LCD_CAM backend requires ESP32-S3"
#endif

namespace fm6373c {
// R1 G1 B1 R2 G2 B2 LE ROW A B C D E. Same PCB pins as the old driver.
static const int8_t pins[] = {5,6,7,15,16,17,8,3,4,21,14,2,1};
static constexpr gpio_num_t clockPin = GPIO_NUM_18;
static void* dmaAlloc(size_t size) {
    return heap_caps_calloc(1, size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
}
static void descriptor(dma_descriptor_t& d, void* buffer, size_t bytes,
                       dma_descriptor_t* next, bool eof = false) {
    memset(&d, 0, sizeof(d));
    d.dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
    d.dw0.size = d.dw0.length = bytes;
    d.dw0.suc_eof = eof;
    d.buffer = buffer;
    d.next = next;
}
static void safePins() {
    for (int8_t pin : pins) {
        gpio_reset_pin(static_cast<gpio_num_t>(pin));
        gpio_set_direction(static_cast<gpio_num_t>(pin), GPIO_MODE_OUTPUT);
        gpio_set_level(static_cast<gpio_num_t>(pin), 0);
    }
    gpio_reset_pin(clockPin);
    gpio_set_direction(clockPin, GPIO_MODE_OUTPUT);
    gpio_set_level(clockPin, 0);
    // DIS_OP is a command, not an OE level. Also handles a warm MCU restart.
    gpio_set_level(GPIO_NUM_8, 1);
    for (unsigned i = 0; i < 12; ++i) {
        gpio_set_level(clockPin, 1); delayMicroseconds(1);
        gpio_set_level(clockPin, 0); delayMicroseconds(1);
    }
    gpio_set_level(GPIO_NUM_8, 0);
}
void Dma::disableOutputs() { safePins(); }
void Dma::linkBank(unsigned bank) {
    Bank& b = banks_[bank];
    for (unsigned n = 0; n < kDescriptors; ++n) {
        const Segment segment = frameSegment(n);
        uint16_t* base = segment.storage == Storage::Mutable ? b.data :
                         segment.storage == Storage::Idle ? common_ + kHeaderClocks : common_;
        descriptor(b.desc[n], base + segment.offset, segment.words * 2,
                   n+1 == kDescriptors ? b.desc : &b.desc[n+1], n == 0);
    }
}
bool Dma::begin() {
    safePins();
    common_ = static_cast<uint16_t*>(dmaAlloc((kHeaderClocks + kIdleWords + kHeaderClocks) * 2));
    init_ = static_cast<uint16_t*>(dmaAlloc(initializationWords() * 2));
    // Initialization fits two descriptors, each <=4000 bytes, aligned to 4.
    initDesc_ = static_cast<dma_descriptor_t*>(dmaAlloc(2 * sizeof(dma_descriptor_t)));
    enableDesc_ = static_cast<dma_descriptor_t*>(dmaAlloc(sizeof(dma_descriptor_t)));
    for (Bank& b : banks_) {
        // S3 OPI PSRAM can feed GDMA directly. Keeping 141 KB of waveform
        // data internal starves Bluedroid's internal-only timer allocations.
        b.data = static_cast<uint16_t*>(heap_caps_aligned_alloc(
            64, kMutableWords * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        b.desc = static_cast<dma_descriptor_t*>(dmaAlloc(kDescriptors * sizeof(dma_descriptor_t)));
    }
    if (!common_ || !init_ || !initDesc_ || !enableDesc_ ||
        !banks_[0].data || !banks_[0].desc || !banks_[1].data || !banks_[1].desc) {
        logPort().println("FM6373C: DMA allocation failed"); end(); return false;
    }
    header(common_);
    idleGroup(common_ + kHeaderClocks);
    uint16_t* enable = common_ + kHeaderClocks + kIdleWords;
    for (unsigned i = 0; i < kHeaderClocks; ++i) enable[i] = 25 << kAddressShift;
    control(enable, kHeaderClocks, Command::Enable);
    initialization(init_);
    for (unsigned b = 0; b < 2; ++b) {
        frame(banks_[b].data, nullptr, 0); linkBank(b);
        // SDK supplies the patched Cache_WriteBack_Addr implementation.
        if (Cache_WriteBack_Addr(reinterpret_cast<uint32_t>(banks_[b].data), kMutableWords * 2) != 0) {
            logPort().println("FM6373C: initial PSRAM cache sync failed"); end(); return false;
        }
    }
    static_assert((35 + 6) * kLineClocks * 2 <= 8000, "Increase init descriptors");
    const size_t bytes = initializationWords() * 2;
    if (bytes <= 4000 || bytes > 8000) { end(); return false; }
    descriptor(initDesc_[0], init_, 4000, &initDesc_[1]);
    descriptor(initDesc_[1], init_ + 2000, bytes - 4000, banks_[0].desc);
    descriptor(*enableDesc_, enable, kHeaderClocks * 2, banks_[1].desc);
    logPort().println("FM6373C: DMA allocated; configuring LCD_CAM");
    if (!configure()) { logPort().println("FM6373C: LCD_CAM/GDMA configuration failed"); end(); return false; }
    if (gdma_start(channel_, reinterpret_cast<intptr_t>(initDesc_)) != ESP_OK) {
        end(); return false;
    }
    delayMicroseconds(100);
    LCD_CAM.lcd_user.lcd_start = 1;
    logPort().println("FM6373C: clock started; sending configuration and black frames");
    // Eight complete black frames before and after the one-shot EN_OP.
    if (!waitFrames(0, 9) || !present(1, true) || !waitFrames(1, 8)) {
        logPort().println("FM6373C: startup DMA EOF timeout"); end(); return false;
    }
    // The new header EOF proves the initialization chain is no longer read.
    heap_caps_free(init_); init_ = nullptr;
    heap_caps_free(initDesc_); initDesc_ = nullptr;
    return true;
}
bool Dma::configure() {
    periph_module_enable(PERIPH_LCD_CAM_MODULE);
    periph_module_reset(PERIPH_LCD_CAM_MODULE);
    peripheral_ = true;
    LCD_CAM.lcd_user.lcd_reset = 1;
    delayMicroseconds(1000);
    LCD_CAM.lcd_clock.lcd_clk_sel = 3;
    LCD_CAM.lcd_clock.lcd_ck_out_edge = 0;
    LCD_CAM.lcd_clock.lcd_ck_idle_edge = 0;
    LCD_CAM.lcd_clock.lcd_clkcnt_n = 1;
    LCD_CAM.lcd_clock.lcd_clk_equ_sysclk = 1;
    LCD_CAM.lcd_clock.lcd_clkm_div_num = 160000000 / kClockHz;
    LCD_CAM.lcd_clock.lcd_clkm_div_a = 1;
    LCD_CAM.lcd_clock.lcd_clkm_div_b = 0;
    LCD_CAM.lcd_ctrl.lcd_rgb_mode_en = 0;
    LCD_CAM.lcd_rgb_yuv.lcd_conv_bypass = 0;
    LCD_CAM.lcd_misc.lcd_next_frame_en = 0;
    LCD_CAM.lcd_misc.lcd_bk_en = 1;
    LCD_CAM.lcd_data_dout_mode.val = 0;
    LCD_CAM.lcd_user.lcd_always_out_en = 1;
    LCD_CAM.lcd_user.lcd_8bits_order = 0;
    LCD_CAM.lcd_user.lcd_bit_order = 0;
    LCD_CAM.lcd_user.lcd_2byte_en = 1;
    LCD_CAM.lcd_user.lcd_dummy = 1;
    LCD_CAM.lcd_user.lcd_dummy_cyclelen = 1;
    LCD_CAM.lcd_user.lcd_cmd = 0;
    for (unsigned i = 0; i < sizeof(pins); ++i) {
        esp_rom_gpio_connect_out_signal(pins[i], LCD_DATA_OUT0_IDX + i, false, false);
        gpio_hal_iomux_func_sel(GPIO_PIN_MUX_REG[pins[i]], PIN_FUNC_GPIO);
        gpio_set_drive_capability(static_cast<gpio_num_t>(pins[i]), GPIO_DRIVE_CAP_3);
    }
    esp_rom_gpio_connect_out_signal(clockPin, LCD_PCLK_IDX, false, false);
    gpio_hal_iomux_func_sel(GPIO_PIN_MUX_REG[clockPin], PIN_FUNC_GPIO);
    gpio_set_drive_capability(clockPin, GPIO_DRIVE_CAP_3);
    gdma_channel_alloc_config_t alloc = {};
    alloc.direction = GDMA_CHANNEL_DIRECTION_TX;
    if (gdma_new_channel(&alloc, &channel_) != ESP_OK) return false;
    if (gdma_connect(channel_, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_LCD, 0)) != ESP_OK) return false;
    connected_ = true;
    gdma_strategy_config_t strategy = {};
    // Circular descriptors remain owned by DMA; CPU only modifies inactive data.
    strategy.owner_check = false;
    strategy.auto_update_desc = false;
    gdma_transfer_ability_t ability = {};
    ability.sram_trans_align = 4; // no burst; allocations and every length are 4-aligned
    ability.psram_trans_align = 64;
    gdma_tx_event_callbacks_t callbacks = {};
    callbacks.on_trans_eof = eof;
    if (gdma_apply_strategy(channel_, &strategy) != ESP_OK ||
        gdma_set_transfer_ability(channel_, &ability) != ESP_OK ||
        gdma_register_tx_event_callbacks(channel_, &callbacks, this) != ESP_OK ||
        gdma_reset(channel_) != ESP_OK) return false;
    LCD_CAM.lcd_user.lcd_dout = 1;
    LCD_CAM.lcd_user.lcd_update = 1;
    LCD_CAM.lcd_misc.lcd_afifo_reset = 1;
    return true;
}
bool IRAM_ATTR Dma::eof(gdma_channel_handle_t, gdma_event_data_t* event, void* arg) {
    auto* self = static_cast<Dma*>(arg);
    portENTER_CRITICAL_ISR(&self->mux_);
    for (unsigned bank = 0; bank < 2; ++bank) {
        if (event->tx_eof_desc_addr == reinterpret_cast<intptr_t>(self->banks_[bank].desc)) {
            self->active_ = bank;
            ++self->counts_[bank];
        }
    }
    portEXIT_CRITICAL_ISR(&self->mux_);
    return false;
}
unsigned Dma::active() {
    portENTER_CRITICAL(&mux_); const int value = active_; portEXIT_CRITICAL(&mux_);
    return value < 0 ? 0 : static_cast<unsigned>(value);
}
uint32_t Dma::frames() {
    portENTER_CRITICAL(&mux_); const uint32_t value = counts_[0] + counts_[1]; portEXIT_CRITICAL(&mux_);
    return value;
}
bool Dma::waitFrames(unsigned bank, unsigned count, uint32_t timeoutMs) {
    portENTER_CRITICAL(&mux_); const uint32_t start = counts_[bank]; portEXIT_CRITICAL(&mux_);
    const uint32_t started = millis();
    while (millis() - started < timeoutMs) {
        portENTER_CRITICAL(&mux_); const uint32_t current = counts_[bank]; portEXIT_CRITICAL(&mux_);
        if (current - start >= count) return true;
        vTaskDelay(1);
    }
    return false;
}
bool Dma::present(unsigned bank, bool enableOnce) {
    const unsigned old = active();
    if (bank > 1 || bank == old || !channel_) return false;
    if (Cache_WriteBack_Addr(reinterpret_cast<uint32_t>(banks_[bank].data), kMutableWords * 2) != 0) {
        logPort().println("FM6373C: PSRAM cache sync failed"); return false;
    }
    // Restore target self-loop BEFORE exposing it to DMA. An extra old frame
    // is harmless if its tail has already been fetched. Never stop DCLK.
    banks_[bank].desc[kDescriptors - 1].next = banks_[bank].desc;
    __sync_synchronize();
    banks_[old].desc[kDescriptors - 1].next = enableOnce ? enableDesc_ : banks_[bank].desc;
    __sync_synchronize();
    const uint32_t started = millis();
    while (millis() - started < 1000) {
        if (active() == bank) return true;
        vTaskDelay(1);
    }
    return false; // caller must stop backend; neither bank may be reused
}
size_t Dma::allocatedBytes() const {
    return 2 * (kMutableWords * 2 + kDescriptors * sizeof(dma_descriptor_t)) +
        (2*kHeaderClocks + kIdleWords) * 2 + sizeof(dma_descriptor_t);
}
void Dma::end() {
    if (peripheral_) {
        LCD_CAM.lcd_user.lcd_reset = 1;
        LCD_CAM.lcd_user.lcd_update = 1;
    }
    if (channel_) {
        gdma_stop(channel_);
        if (connected_) gdma_disconnect(channel_);
        gdma_del_channel(channel_);
        channel_ = nullptr;
    }
    connected_ = false;
    if (peripheral_) periph_module_disable(PERIPH_LCD_CAM_MODULE);
    peripheral_ = false;
    safePins();
    for (Bank& b : banks_) {
        heap_caps_free(b.data); b.data = nullptr;
        heap_caps_free(b.desc); b.desc = nullptr;
    }
    heap_caps_free(common_); common_ = nullptr;
    heap_caps_free(init_); init_ = nullptr;
    heap_caps_free(initDesc_); initDesc_ = nullptr;
    heap_caps_free(enableDesc_); enableDesc_ = nullptr;
    portENTER_CRITICAL(&mux_);
    active_ = -1; counts_[0] = counts_[1] = 0;
    portEXIT_CRITICAL(&mux_);
}
} // namespace fm6373c
