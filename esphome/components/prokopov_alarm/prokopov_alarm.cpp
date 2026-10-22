#include "prokopov_alarm.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/network/util.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <ctime>
#include <sstream>
#include <cstdlib>
#include <sys/stat.h>

extern "C" {
#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include "esp_rom_crc.h"
#include "mbedtls/sha256.h"
}

namespace esphome {
namespace prokopov_alarm {

static const char *const TAG = "prokopov_alarm";

class RecursiveLock {
 public:
  explicit RecursiveLock(SemaphoreHandle_t sem) : sem_(sem) {
    if (sem_) xSemaphoreTakeRecursive(sem_, portMAX_DELAY);
  }
  ~RecursiveLock() { if (sem_) xSemaphoreGiveRecursive(sem_); }
 private:
  SemaphoreHandle_t sem_;
};

// ---- internal flash FAT (partition label "storage"): cards + configuration, written only on sync
static const char *const FLASH_MOUNT = "/data";
static const char *const FLASH_PARTITION = "storage";
static const char *const CARDS_FILE = "/data/cards.dat";
static const char *const CONFIG_FILE = "/data/config.cfg";
static const char *const STATE_FILE = "/data/state.dat";   // fallback only when no FRAM is fitted
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;

// ---- FRAM layout (MB85RC256V, 32 KiB, 2-byte addressing, no write wear)
//  0x0000  header  "PRK3" + layout version
//  0x0010  runtime record A (32 bytes)   0x0030 runtime record B (32 bytes)   -> newest valid generation wins
//  0x0100  journal: 250 slots x 128 bytes, each = seq(8) len(2) crc(2) text(116)
static constexpr uint16_t FRAM_HDR = 0x0000;
static constexpr uint16_t FRAM_RT_A = 0x0010;
static constexpr uint16_t FRAM_RT_B = 0x0030;
static constexpr uint16_t FRAM_JOURNAL = 0x0100;
static constexpr uint16_t FRAM_SLOT = 128;
static constexpr uint16_t FRAM_SLOTS = 250;
static constexpr uint16_t FRAM_TEXT = FRAM_SLOT - 12;
static constexpr uint32_t FRAM_MAGIC = 0x334B5250;  // "PRK3"

struct __attribute__((packed)) RuntimeRecord {
  uint32_t magic;
  uint32_t generation;
  uint8_t state;
  uint8_t lock_mode;
  uint8_t door_locked;
  uint8_t reserved;
  uint64_t epoch;
  uint64_t event_seq;
  uint32_t crc;
};
static_assert(sizeof(RuntimeRecord) == 32, "runtime record must stay 32 bytes");

static const char *state_to_cstr(AlarmState s) {
  switch (s) {
    case AlarmState::DISARMED: return "DISARMED";
    case AlarmState::EXIT_DELAY: return "EXIT_DELAY";
    case AlarmState::ARMED_AWAY: return "ARMED_AWAY";
    case AlarmState::ARMED_HOME: return "ARMED_HOME";
    case AlarmState::ARMED_NIGHT: return "ARMED_NIGHT";
    case AlarmState::ENTRY_DELAY: return "ENTRY_DELAY";
    case AlarmState::ALARM: return "ALARM";
    case AlarmState::PANIC: return "PANIC";
    case AlarmState::SILENT_PANIC: return "SILENT_PANIC";
  }
  return "UNKNOWN";
}

static bool is_armed_or_alarm(AlarmState s) {
  return s == AlarmState::ARMED_AWAY || s == AlarmState::ARMED_HOME || s == AlarmState::ARMED_NIGHT ||
         s == AlarmState::ENTRY_DELAY || s == AlarmState::ALARM || s == AlarmState::PANIC ||
         s == AlarmState::SILENT_PANIC;
}

static ZoneRule parse_rule(const char *s) {
  if (s == nullptr) return ZoneRule::IGNORE;
  std::string v(s);
  std::transform(v.begin(), v.end(), v.begin(), ::tolower);
  if (v == "instant") return ZoneRule::INSTANT;
  if (v == "delayed") return ZoneRule::DELAYED;
  if (v == "follower") return ZoneRule::FOLLOWER;
  if (v == "24/7" || v == "always" || v == "24h") return ZoneRule::ALWAYS;
  return ZoneRule::IGNORE;
}

static const char *rule_to_cstr(ZoneRule r) {
  switch (r) {
    case ZoneRule::IGNORE: return "ignore";
    case ZoneRule::INSTANT: return "instant";
    case ZoneRule::DELAYED: return "delayed";
    case ZoneRule::FOLLOWER: return "follower";
    case ZoneRule::ALWAYS: return "24/7";
  }
  return "ignore";
}

void ProkopovAlarm::set_reader_pins(int index, int d0, int d1) {
  if (index < 0 || index > 1) return;
  this->readers_[index].d0_pin = d0;
  this->readers_[index].d1_pin = d1;
  this->readers_[index].name = index == 0 ? "reader_outside" : "reader_inside";
}

void ProkopovAlarm::set_output_pins(int lock, int siren, int reader_led, int reader_beep, int st_armed, int st_alarm,
                                    int wdt_kick) {
  this->lock_pin_ = lock;
  this->siren_pin_ = siren;
  this->reader_led_pin_ = reader_led;
  this->reader_beep_pin_ = reader_beep;
  this->st_armed_pin_ = st_armed;
  this->st_alarm_pin_ = st_alarm;
  this->wdt_pin_ = wdt_kick;
}

void ProkopovAlarm::set_ports(uint16_t management_port, uint16_t shelly_port) {
  this->management_port_ = management_port;
  this->shelly_port_ = shelly_port;
}

void ProkopovAlarm::set_keys(const std::string &control_key, const std::string &shelly_key) {
  this->control_key_ = control_key;
  this->shelly_key_ = shelly_key;
}

void ProkopovAlarm::set_timings(uint32_t arm_confirm_window_ms, uint32_t valid_pulse_ms,
                                uint32_t arm_pulse_ms, uint32_t frame_gap_ms, uint32_t glitch_us) {
  this->arm_confirm_window_ms_ = arm_confirm_window_ms;
  this->reader_valid_pulse_ms_ = valid_pulse_ms;
  this->reader_arm_pulse_ms_ = arm_pulse_ms;
  this->frame_gap_us_ = frame_gap_ms * 1000UL;
  this->glitch_us_ = glitch_us;
}

// Must run before anything that may drive outputs; I2C (FRAM) is set up at BUS priority before us.
float ProkopovAlarm::get_setup_priority() const { return setup_priority::HARDWARE; }

static void out_pin(int pin, int level) {
  if (pin < 0) return;
  gpio_config_t c{};
  c.intr_type = GPIO_INTR_DISABLE;
  c.mode = GPIO_MODE_OUTPUT;
  c.pin_bit_mask = 1ULL << pin;
  c.pull_down_en = GPIO_PULLDOWN_DISABLE;
  c.pull_up_en = GPIO_PULLUP_DISABLE;
  gpio_set_level((gpio_num_t) pin, level);
  gpio_config(&c);
  gpio_set_level((gpio_num_t) pin, level);
}

void ProkopovAlarm::setup() {
  ESP_LOGI(TAG, "Starting PROKOPOV alarm core v3 (main board Rev A)");
  this->storage_mutex_ = xSemaphoreCreateMutex();
  this->state_mutex_ = xSemaphoreCreateRecursiveMutex();

  // Safe boot levels: siren off, status outputs off, reader LED/beep idle, lock relay de-energised.
  out_pin(this->siren_pin_, 0);
  out_pin(this->lock_pin_, 0);
  out_pin(this->reader_led_pin_, 0);
  out_pin(this->reader_beep_pin_, 0);
  out_pin(this->st_armed_pin_, 0);
  out_pin(this->st_alarm_pin_, 0);
  out_pin(this->wdt_pin_, 0);
  // Boot grace: the rest of ESPHome's setup (Wi-Fi, API, first-boot flash format) may take longer than the
  // watchdog timeout. A timer kicks the watchdog until loop() has been running for 15 s, then loop() alone does.
  if (this->wdt_pin_ >= 0) {
    esp_timer_create_args_t ta{};
    ta.callback = [](void *arg) {
      auto *self = static_cast<ProkopovAlarm *>(arg);
      self->wdt_level_ = !self->wdt_level_;
      gpio_set_level((gpio_num_t) self->wdt_pin_, self->wdt_level_ ? 1 : 0);
    };
    ta.arg = this;
    ta.name = "wdt_boot";
    esp_timer_handle_t t = nullptr;
    if (esp_timer_create(&ta, &t) == ESP_OK && esp_timer_start_periodic(t, 200000) == ESP_OK) this->wdt_boot_timer_ = t;
  }
  this->door_locked_ = false;
  this->siren_on_ = false;
  this->kick_watchdog_();

  this->fram_ok_ = this->use_fram_ && this->fram_probe_();
  if (this->fram_ok_) this->fram_load_journal_();
  this->flash_ok_ = this->init_flash_();
  this->kick_watchdog_();
  this->load_config_();
  this->load_cards_();
  this->load_runtime_state_();
  this->kick_watchdog_();

  // Restore the stable state on the outputs.
  if (this->state_ == AlarmState::ARMED_AWAY || this->state_ == AlarmState::ARMED_HOME ||
      this->state_ == AlarmState::ARMED_NIGHT || this->state_ == AlarmState::SILENT_PANIC) {
    this->door_locked_ = true;
  } else if (this->state_ == AlarmState::ALARM || this->state_ == AlarmState::PANIC) {
    this->door_locked_ = true;
    this->siren_on_ = true;
    gpio_set_level((gpio_num_t) this->siren_pin_, 1);
    if (this->siren_timeout_s_ > 0) this->siren_deadline_ms_ = millis() + this->siren_timeout_s_ * 1000ULL;
  }
  this->apply_lock_output_();
  this->apply_status_outputs_();

  // Wiegand: 6N137 open-collector outputs with 2.2k pull-ups on the board; falling edge = bit.
  esp_err_t isr_res = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
  if (isr_res != ESP_OK && isr_res != ESP_ERR_INVALID_STATE)
    ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(isr_res));
  for (auto &p : this->readers_) {
    if (p.d0_pin < 0 || p.d1_pin < 0) continue;
    p.glitch_us = this->glitch_us_;
    gpio_config_t c{};
    c.intr_type = GPIO_INTR_NEGEDGE;
    c.mode = GPIO_MODE_INPUT;
    c.pin_bit_mask = (1ULL << p.d0_pin) | (1ULL << p.d1_pin);
    c.pull_up_en = GPIO_PULLUP_ENABLE;
    c.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&c);
    gpio_isr_handler_add((gpio_num_t) p.d0_pin, &ProkopovAlarm::d0_isr_, &p);
    gpio_isr_handler_add((gpio_num_t) p.d1_pin, &ProkopovAlarm::d1_isr_, &p);
  }

  // Network services start from loop() once the network is up (see v1 notes: httpd at HARDWARE priority asserts).
  this->http_start_retry_ms_ = 0;
  this->last_event_ = std::string("BOOT · restored ") + state_to_cstr(this->state_);
  this->queue_event_("SYSTEM", "Controller", std::string("BOOT_RESTORE_") + state_to_cstr(this->state_), "system");
}

void ProkopovAlarm::dump_config() {
  ESP_LOGCONFIG(TAG, "PROKOPOV Alarm Core v3:");
  for (auto &p : this->readers_)
    ESP_LOGCONFIG(TAG, "  %s: D0 GPIO%d / D1 GPIO%d", p.name, p.d0_pin, p.d1_pin);
  ESP_LOGCONFIG(TAG, "  Lock relay GPIO%d (%s) / Siren GPIO%d", this->lock_pin_,
                this->lock_type_ == LockType::MAGLOCK ? "maglock" : "strike", this->siren_pin_);
  ESP_LOGCONFIG(TAG, "  Reader LED GPIO%d / beep GPIO%d / Uni ARMED GPIO%d ALARM GPIO%d / WDT GPIO%d",
                this->reader_led_pin_, this->reader_beep_pin_, this->st_armed_pin_, this->st_alarm_pin_, this->wdt_pin_);
  ESP_LOGCONFIG(TAG, "  Management API: %u / Shelly webhook: %u", this->management_port_, this->shelly_port_);
  ESP_LOGCONFIG(TAG, "  Storage: %s", this->storage_status_string().c_str());
  ESP_LOGCONFIG(TAG, "  Cards: %u / Zones: %u / lockout %u tries", (unsigned) this->cards_.size(),
                (unsigned) this->zones_.size(), this->lockout_attempts_);
}

void ProkopovAlarm::kick_watchdog_() {
  // NE555 watchdog (~7 s): every rising edge discharges its timing capacitor. Toggle every 200 ms from the
  // main loop only, so a hung loop (not just a hung task) resets the board.
  if (this->wdt_pin_ < 0 || this->wdt_boot_timer_ != nullptr) return;
  const uint32_t now = millis();
  if (now - this->wdt_last_ms_ < 200) return;
  this->wdt_last_ms_ = now;
  this->wdt_level_ = !this->wdt_level_;
  gpio_set_level((gpio_num_t) this->wdt_pin_, this->wdt_level_ ? 1 : 0);
}

void ProkopovAlarm::loop() {
  if (this->wdt_boot_timer_ != nullptr) {
    if (!this->first_loop_ms_) this->first_loop_ms_ = millis() | 1;
    if (millis() - this->first_loop_ms_ > 15000) {
      esp_timer_stop(static_cast<esp_timer_handle_t>(this->wdt_boot_timer_));
      esp_timer_delete(static_cast<esp_timer_handle_t>(this->wdt_boot_timer_));
      this->wdt_boot_timer_ = nullptr;
      ESP_LOGI(TAG, "Hardware watchdog now kicked by the main loop only");
    }
  }
  this->kick_watchdog_();
  if (!this->led_rendered_after_boot_ && millis() > 2000) {
    this->render_led_();
    this->led_rendered_after_boot_ = true;
  }
  for (auto &p : this->readers_)
    while (this->process_wiegand_frame_(p)) {}
  this->tick_reader_feedback_();
  {
    RecursiveLock lock(this->state_mutex_);
    this->device_heartbeat_ms_["local"] = millis();   // board inputs are always present
  }
  this->tick_state_machine_();

  if (!this->http_servers_started_ && millis() >= this->http_start_retry_ms_ && network::is_connected()) {
    if (this->start_http_servers_()) {
      this->http_servers_started_ = true;
      ESP_LOGI(TAG, "Network APIs started after network became ready");
    } else {
      this->http_start_retry_ms_ = millis() + 5000;
      ESP_LOGW(TAG, "Network API start failed; retrying in 5 s");
    }
  }

  if (this->state_persist_pending_) this->persist_runtime_state_();
  // FRAM writes take ~1 ms per event: drain a few per loop.
  for (int i = 0; i < 4; i++) this->flush_one_event_();
}

void IRAM_ATTR ProkopovAlarm::d0_isr_(void *arg) { push_bit_(static_cast<WiegandPort *>(arg), 0); }
void IRAM_ATTR ProkopovAlarm::d1_isr_(void *arg) { push_bit_(static_cast<WiegandPort *>(arg), 1); }

void IRAM_ATTR ProkopovAlarm::push_bit_(WiegandPort *p, uint8_t bit) {
  const int64_t now = esp_timer_get_time();
  if (p->last_irq_us && (uint64_t) (now - p->last_irq_us) < p->glitch_us) {
    p->filtered++;
    return;
  }
  p->last_irq_us = now;
  uint16_t next = (p->head + 1) & (WiegandPort::RING - 1);
  if (next == p->tail) {
    p->overflow++;
    return;
  }
  p->bits[p->head] = bit;
  p->times[p->head] = now;
  p->head = next;
}

bool ProkopovAlarm::process_wiegand_frame_(WiegandPort &p) {
  uint16_t head = p.head;
  uint16_t tail = p.tail;
  if (tail == head) return false;

  uint16_t idx = tail;
  int64_t prev = p.times[idx];
  uint64_t raw = 0;
  uint8_t bits = 0;
  uint16_t frame_end = 0xFFFF;

  while (idx != head) {
    int64_t ts = p.times[idx];
    if (bits && (uint64_t) (ts - prev) >= this->frame_gap_us_) {
      frame_end = idx;
      break;
    }
    if (bits < 64) raw = (raw << 1) | p.bits[idx];
    bits++;
    prev = ts;
    idx = (idx + 1) & (WiegandPort::RING - 1);
  }

  if (frame_end == 0xFFFF) {
    if ((uint64_t) (esp_timer_get_time() - prev) < this->frame_gap_us_) return false;
    frame_end = idx;
  }
  p.tail = frame_end;

  bool parity = this->wiegand_parity_ok_(bits, raw);
  uint32_t card = this->wiegand_card_(bits, raw);
  ESP_LOGI(TAG, "%s: bits=%u card=%lu parity=%s filtered=%lu overflow=%lu", p.name, (unsigned) bits,
           (unsigned long) card, YESNO(parity), (unsigned long) p.filtered, (unsigned long) p.overflow);
  if ((bits != 26 && bits != 34) || !parity || card == 0) return true;

  this->process_card_(p, card);
  return true;
}

static uint8_t popcount32(uint32_t v) {
  uint8_t c = 0;
  while (v) { c += v & 1U; v >>= 1U; }
  return c;
}

bool ProkopovAlarm::wiegand_parity_ok_(uint8_t bits, uint64_t raw) {
  if (bits == 34) {
    uint8_t lead = (raw >> 33) & 1U;
    uint8_t trail = raw & 1U;
    uint16_t high16 = (raw >> 17) & 0xFFFFU;
    uint16_t low16 = (raw >> 1) & 0xFFFFU;
    return ((popcount32(high16) + lead) & 1U) == 0 && ((popcount32(low16) + trail) & 1U) == 1;
  }
  if (bits == 26) {
    uint8_t lead = (raw >> 25) & 1U;
    uint8_t trail = raw & 1U;
    uint16_t high12 = (raw >> 13) & 0x0FFFU;
    uint16_t low12 = (raw >> 1) & 0x0FFFU;
    return ((popcount32(high12) + lead) & 1U) == 0 && ((popcount32(low12) + trail) & 1U) == 1;
  }
  return false;
}

uint32_t ProkopovAlarm::wiegand_card_(uint8_t bits, uint64_t raw) {
  if (bits == 34) return (uint32_t) ((raw >> 1) & 0xFFFFU);
  if (bits == 26) return (uint32_t) ((raw >> 1) & 0xFFFFU);
  return 0;
}

std::string ProkopovAlarm::alarm_state_string() const { return state_to_cstr(this->state_); }

bool ProkopovAlarm::ready_for_current_mode() const {
  RecursiveLock lock(this->state_mutex_);
  AlarmState target = AlarmState::ARMED_AWAY;
  if (this->state_ == AlarmState::ARMED_HOME) target = AlarmState::ARMED_HOME;
  else if (this->state_ == AlarmState::ARMED_NIGHT) target = AlarmState::ARMED_NIGHT;
  return this->ready_for_(target);
}

std::string ProkopovAlarm::storage_status_string() const {
  std::string s = this->fram_ok_ ? "FRAM OK" : "flash (no FRAM fitted)";
  s += this->flash_ok_ ? " / flash OK" : " / flash ERROR";
  return s;
}

std::string ProkopovAlarm::lock_mode_string() const {
  switch (this->lock_mode_) {
    case LockMode::ALWAYS_LOCKED: return "always_locked";
    case LockMode::ALWAYS_UNLOCKED: return "always_unlocked";
    default: return "auto";
  }
}

bool ProkopovAlarm::lock_output_locked() const {
  if (this->lock_mode_ == LockMode::ALWAYS_LOCKED) return true;
  if (this->lock_mode_ == LockMode::ALWAYS_UNLOCKED) return false;
  return this->door_locked_;
}

void ProkopovAlarm::apply_lock_output_() {
  const bool locked = this->lock_output_locked();
  // MAGLOCK on NO: energise to hold. STRIKE on NO: energise to release.
  const bool energise = this->lock_type_ == LockType::MAGLOCK ? locked : !locked;
  gpio_set_level((gpio_num_t) this->lock_pin_, energise ? 1 : 0);
}

void ProkopovAlarm::set_door_locked_(bool locked, const std::string &reason) {
  const bool before = this->lock_output_locked();
  this->door_locked_ = locked;
  this->state_persist_pending_ = true;
  this->apply_lock_output_();
  if (before != this->lock_output_locked() || this->lock_mode_ == LockMode::AUTO)
    this->queue_event_("DOOR", "Door lock", locked ? "LOCKED" : "UNLOCKED", reason);
}

void ProkopovAlarm::set_lock_mode_(LockMode mode, const std::string &actor) {
  this->lock_mode_ = mode;
  this->state_persist_pending_ = true;
  this->apply_lock_output_();
  this->queue_event_("DOOR", "Lock mode", std::string("MODE_") + this->lock_mode_string(), actor);
}

void ProkopovAlarm::set_siren_(bool on, const std::string &reason) {
  this->siren_on_ = on;
  gpio_set_level((gpio_num_t) this->siren_pin_, on ? 1 : 0);
  if (on && this->siren_timeout_s_ > 0) this->siren_deadline_ms_ = millis() + this->siren_timeout_s_ * 1000ULL;
  else this->siren_deadline_ms_ = 0;
  this->queue_event_("SYSTEM", "Siren", on ? "ON" : "OFF", reason);
}

void ProkopovAlarm::apply_status_outputs_() {
  // Optocoupler outputs to the Shelly Plus Uni inputs: IN1 = protected (armed), IN2 = alarm.
  const bool armed = this->state_ == AlarmState::ARMED_AWAY || this->state_ == AlarmState::ARMED_HOME ||
                     this->state_ == AlarmState::ARMED_NIGHT || this->state_ == AlarmState::ENTRY_DELAY ||
                     this->state_ == AlarmState::EXIT_DELAY;
  const bool alarm = this->state_ == AlarmState::ALARM || this->state_ == AlarmState::PANIC ||
                     this->state_ == AlarmState::SILENT_PANIC;
  gpio_set_level((gpio_num_t) this->st_armed_pin_, armed ? 1 : 0);
  gpio_set_level((gpio_num_t) this->st_alarm_pin_, alarm ? 1 : 0);
}

void ProkopovAlarm::set_state_(AlarmState state, const std::string &reason) {
  this->state_ = state;
  this->state_persist_pending_ = true;
  this->last_event_ = std::string(state_to_cstr(state)) + (reason.empty() ? "" : " · " + reason);
  this->apply_status_outputs_();
  if (millis() > 2000) this->render_led_();
  this->queue_event_("ALARM", "Controller", state_to_cstr(state), reason.empty() ? "system" : reason);
}

void ProkopovAlarm::render_led_() {
  if (this->status_light_ == nullptr) return;
  float r = 0, g = 0, b = 0;
  switch (this->state_) {
    case AlarmState::DISARMED: g = 1.0f; break;
    case AlarmState::EXIT_DELAY: r = 1.0f; g = 0.55f; break;
    case AlarmState::ARMED_AWAY: b = 1.0f; break;
    case AlarmState::ARMED_HOME: g = 0.45f; b = 1.0f; break;
    case AlarmState::ARMED_NIGHT: g = 0.7f; b = 1.0f; break;
    case AlarmState::ENTRY_DELAY: r = 1.0f; g = 0.55f; break;
    case AlarmState::ALARM: case AlarmState::PANIC: r = 1.0f; break;
    case AlarmState::SILENT_PANIC: r = 0.72f; b = 0.48f; break;
  }
  auto call = this->status_light_->turn_on();
  call.set_rgb(r, g, b);
  call.set_brightness(0.35f);
  call.perform();
}

void ProkopovAlarm::set_reader_control_(bool active) {
  // LTV-847 outputs pull the readers' LED / buzzer lines low while the GPIO is high.
  gpio_set_level((gpio_num_t) this->reader_led_pin_, active ? 1 : 0);
  gpio_set_level((gpio_num_t) this->reader_beep_pin_, active ? 1 : 0);
}

void ProkopovAlarm::start_reader_feedback_(ReaderFeedback feedback) {
  // Dahua produces its own immediate card-read feedback.
  // Never overlap our controlled LED/BELL signal with that native response.
  //
  // Every custom feedback sequence therefore starts after a quiet delay.
  // If another card is read during that delay, this function is called again
  // and the obsolete pending sequence is replaced by the new real action.

  this->set_reader_control_(false);

  this->reader_feedback_steps_.fill(0);
  this->reader_feedback_len_ = 0;
  this->reader_feedback_index_ = 0;
  this->reader_feedback_deadline_ms_ = 0;
  this->reader_feedback_active_ = false;
  this->reader_feedback_waiting_start_ = false;

  switch (feedback) {
    case ReaderFeedback::UNLOCK:
      // One short confirmation.
      this->reader_feedback_steps_[0] = 300;
      this->reader_feedback_len_ = 1;
      break;

    case ReaderFeedback::LOCK:
      // Two short confirmations.
      this->reader_feedback_steps_[0] = 300;
      this->reader_feedback_steps_[1] = 250;
      this->reader_feedback_steps_[2] = 300;
      this->reader_feedback_len_ = 3;
      break;

    case ReaderFeedback::ARMED:
      // Very long, unmistakable ARM confirmation.
      this->reader_feedback_steps_[0] = 5000;
      this->reader_feedback_len_ = 1;
      break;

    case ReaderFeedback::DISARMED:
      // Three short confirmations.
      this->reader_feedback_steps_[0] = 300;
      this->reader_feedback_steps_[1] = 250;
      this->reader_feedback_steps_[2] = 300;
      this->reader_feedback_steps_[3] = 250;
      this->reader_feedback_steps_[4] = 300;
      this->reader_feedback_len_ = 5;
      break;

    case ReaderFeedback::DENIED:
      // Four rapid warning pulses.
      this->reader_feedback_steps_[0] = 180;
      this->reader_feedback_steps_[1] = 150;
      this->reader_feedback_steps_[2] = 180;
      this->reader_feedback_steps_[3] = 150;
      this->reader_feedback_steps_[4] = 180;
      this->reader_feedback_steps_[5] = 150;
      this->reader_feedback_steps_[6] = 180;
      this->reader_feedback_len_ = 7;
      break;

    case ReaderFeedback::ARM_BLOCKED:
      // Two long warning pulses.
      this->reader_feedback_steps_[0] = 900;
      this->reader_feedback_steps_[1] = 300;
      this->reader_feedback_steps_[2] = 900;
      this->reader_feedback_len_ = 3;
      break;

    case ReaderFeedback::LOCKED_OUT:
      // One long + rapid pulses: input is blocked for a while.
      this->reader_feedback_steps_[0] = 1500;
      this->reader_feedback_steps_[1] = 200;
      this->reader_feedback_steps_[2] = 150;
      this->reader_feedback_steps_[3] = 150;
      this->reader_feedback_steps_[4] = 150;
      this->reader_feedback_len_ = 5;
      break;

    case ReaderFeedback::ENROLL:
      this->reader_feedback_steps_[0] = 350;
      this->reader_feedback_len_ = 1;
      break;

    case ReaderFeedback::NONE:
    default:
      return;
  }

  // Wait one full second for the Dahua native two-beep/card-read response
  // to finish before driving LED/BELL_CTRL.
  this->reader_feedback_index_ = 0;
  this->reader_feedback_waiting_start_ = true;
  this->reader_feedback_deadline_ms_ = millis() + 1000;
}

void ProkopovAlarm::tick_reader_feedback_() {
  const uint32_t now = millis();

  // Delayed start keeps our signal separate from Dahua's own response.
  if (this->reader_feedback_waiting_start_) {
    if ((int32_t) (now - this->reader_feedback_deadline_ms_) < 0)
      return;

    this->reader_feedback_waiting_start_ = false;
    this->reader_feedback_active_ = true;
    this->reader_feedback_index_ = 0;

    this->set_reader_control_(true);
    this->reader_feedback_deadline_ms_ =
        now + this->reader_feedback_steps_[0];

    return;
  }

  if (!this->reader_feedback_active_)
    return;

  if ((int32_t) (now - this->reader_feedback_deadline_ms_) < 0)
    return;

  this->reader_feedback_index_++;

  if (this->reader_feedback_index_ >= this->reader_feedback_len_) {
    this->set_reader_control_(false);
    this->reader_feedback_active_ = false;
    this->reader_feedback_waiting_start_ = false;
    this->reader_feedback_deadline_ms_ = 0;
    return;
  }

  // Even steps = active LOW pulse.
  // Odd steps = high-impedance pause.
  const bool active =
      (this->reader_feedback_index_ % 2U) == 0U;

  this->set_reader_control_(active);

  this->reader_feedback_deadline_ms_ =
      now + this->reader_feedback_steps_[this->reader_feedback_index_];
}

bool ProkopovAlarm::locked_out_(WiegandPort *p) const {
  return p != nullptr && p->lockout_until_ms && millis() < p->lockout_until_ms;
}

void ProkopovAlarm::register_failure_(WiegandPort *p, const std::string &source) {
  if (p == nullptr || this->lockout_attempts_ == 0) return;
  const uint64_t now = millis();
  if (!p->fail_window_start_ms || now - p->fail_window_start_ms > this->lockout_window_ms_) {
    p->fail_window_start_ms = now;
    p->fails = 0;
  }
  if (++p->fails >= this->lockout_attempts_) {
    p->fails = 0;
    p->fail_window_start_ms = 0;
    p->lockout_until_ms = now + this->lockout_ms_;
    this->queue_event_("SYSTEM", source, "LOCKOUT_" + std::to_string(this->lockout_ms_ / 1000) + "S", "brute-force");
  }
}

RemoteReaderResult ProkopovAlarm::process_card_action_(uint32_t uid, bool local_feedback, const std::string &source) {
  RecursiveLock lock(this->state_mutex_);
  WiegandPort *port = nullptr;
  for (auto &p : this->readers_) if (source == p.name) port = &p;
  if (port == nullptr) port = &this->panel_port_;

  if (this->locked_out_(port)) {
    this->queue_event_("NFC", source, "LOCKED_OUT", std::to_string(uid));
    if (local_feedback) this->start_reader_feedback_(ReaderFeedback::LOCKED_OUT);
    return RemoteReaderResult::LOCKED_OUT;
  }

  if (this->enroll_active_) {
    this->enroll_uid_ = uid;
    this->enroll_active_ = false;
    this->queue_event_("NFC", source, "ENROLL_CAPTURE", std::to_string(uid));
    if (local_feedback) this->start_reader_feedback_(ReaderFeedback::ENROLL);
    return RemoteReaderResult::ENROLL_CAPTURED;
  }

  auto it = this->cards_.find(uid);
  if (it == this->cards_.end() || !it->second.enabled) {
    this->queue_event_("NFC", source, "DENIED", std::to_string(uid));
    this->register_failure_(port, source);
    if (local_feedback) this->start_reader_feedback_(ReaderFeedback::DENIED);
    return RemoteReaderResult::DENIED;
  }
  port->fails = 0;
  const CardPermissions &p = it->second;
  const std::string actor = p.user_name.empty() ? ("card:" + std::to_string(uid)) : p.user_name;
  this->queue_event_("NFC", source, "AUTHORIZED", actor);
  return this->authorize_(p, uid, actor, local_feedback, "card");
}

// Shared decision logic for cards and PINs.
// action "card" = the classic one-credential flow: disarm if armed, else toggle lock, same card twice = ARM AWAY.
// PIN actions from the panel are explicit: "arm_away", "arm_home", "arm_night", "disarm", "unlock", "lock".
RemoteReaderResult ProkopovAlarm::authorize_(const CardPermissions &p, uint32_t uid, const std::string &actor,
                                             bool local_feedback, const std::string &action) {
  const uint64_t now = millis();
  auto feedback = [&](ReaderFeedback value) {
    if (local_feedback) this->start_reader_feedback_(value);
  };
  auto deny = [&](const char *why) {
    this->queue_event_("NFC", "Access", why, actor);
    feedback(ReaderFeedback::DENIED);
    return RemoteReaderResult::DENIED;
  };

  if (action == "disarm" || (action == "card" && is_armed_or_alarm(this->state_))) {
    if (!p.can_disarm) return deny("DENIED_NO_DISARM");
    this->disarm_(actor);
    feedback(ReaderFeedback::DISARMED);
    return RemoteReaderResult::DISARMED_UNLOCKED;
  }
  if (action == "arm_away" || action == "arm_home" || action == "arm_night") {
    if (!p.can_arm) return deny("DENIED_NO_ARM");
    AlarmState target = action == "arm_home" ? AlarmState::ARMED_HOME
                        : action == "arm_night" ? AlarmState::ARMED_NIGHT : AlarmState::ARMED_AWAY;
    if (this->arm_(target, actor, true)) { feedback(ReaderFeedback::ARMED); return RemoteReaderResult::ARMED; }
    feedback(ReaderFeedback::ARM_BLOCKED);
    return RemoteReaderResult::ARM_BLOCKED;
  }
  if (action == "unlock" || action == "lock") {
    if (!p.can_unlock) return deny("DENIED_NO_UNLOCK");
    if (is_armed_or_alarm(this->state_)) return deny("DENIED_ARMED");
    this->set_door_locked_(action == "lock", actor);
    feedback(action == "lock" ? ReaderFeedback::LOCK : ReaderFeedback::UNLOCK);
    return action == "lock" ? RemoteReaderResult::LOCKED_ARM_WAIT : RemoteReaderResult::ACCESS_GRANTED;
  }
  if (action != "card") return deny("DENIED_BAD_ACTION");

  // Lock mode overrides: with a forced lock mode the card only arms/disarms.
  if (this->lock_mode_ != LockMode::AUTO) {
    if (this->arm_confirm_card_ == uid && this->arm_confirm_deadline_ms_ > now) {
      this->arm_confirm_card_ = 0;
      this->arm_confirm_deadline_ms_ = 0;
      if (p.can_arm && this->arm_(AlarmState::ARMED_AWAY, actor, false)) {
        feedback(ReaderFeedback::ARMED);
        return RemoteReaderResult::ARMED;
      }
      feedback(ReaderFeedback::ARM_BLOCKED);
      return RemoteReaderResult::ARM_BLOCKED;
    }
    this->arm_confirm_card_ = uid;
    this->arm_confirm_deadline_ms_ = now + this->arm_confirm_window_ms_;
    feedback(ReaderFeedback::LOCK);
    return RemoteReaderResult::LOCKED_ARM_WAIT;
  }

  // Door unlocked: lock, and open the confirmation window for ARM AWAY.
  if (!this->door_locked_) {
    if (!p.can_unlock) return deny("DENIED_NO_UNLOCK");
    this->set_door_locked_(true, actor);
    this->arm_confirm_card_ = uid;
    this->arm_confirm_deadline_ms_ = now + this->arm_confirm_window_ms_;
    feedback(ReaderFeedback::LOCK);
    return RemoteReaderResult::LOCKED_ARM_WAIT;
  }

  // Locked and the same card again inside the window: ARM AWAY.
  if (this->arm_confirm_card_ == uid && this->arm_confirm_deadline_ms_ > now) {
    this->arm_confirm_card_ = 0;
    this->arm_confirm_deadline_ms_ = 0;
    if (p.can_arm && this->arm_(AlarmState::ARMED_AWAY, actor, false)) {
      feedback(ReaderFeedback::ARMED);
      return RemoteReaderResult::ARMED;
    }
    this->queue_event_("ALARM", "Arming interlock", "ARM_BLOCKED", actor);
    feedback(ReaderFeedback::ARM_BLOCKED);
    return RemoteReaderResult::ARM_BLOCKED;
  }

  // Locked, not confirming: unlock.
  if (!p.can_unlock) return deny("DENIED_NO_UNLOCK");
  this->arm_confirm_card_ = 0;
  this->arm_confirm_deadline_ms_ = 0;
  this->set_door_locked_(false, actor);
  feedback(ReaderFeedback::UNLOCK);
  return RemoteReaderResult::ACCESS_GRANTED;
}

void ProkopovAlarm::process_card_(WiegandPort &port, uint32_t uid) {
  (void) this->process_card_action_(uid, true, port.name);
}

RemoteReaderResult ProkopovAlarm::process_remote_card(uint32_t uid) {
  return this->process_card_action_(uid, false, "remote_reader");
}

RemoteReaderResult ProkopovAlarm::process_pin(const std::string &pin, const std::string &action, const std::string &source) {
  RecursiveLock lock(this->state_mutex_);
  WiegandPort *port = &this->panel_port_;
  if (this->locked_out_(port)) {
    this->queue_event_("PIN", source, "LOCKED_OUT", "keypad");
    return RemoteReaderResult::LOCKED_OUT;
  }
  if (pin.size() < 4 || pin.size() > 12) {
    this->register_failure_(port, source);
    return RemoteReaderResult::DENIED;
  }
  const std::string h = sha256_hex_(pin);
  for (const auto &kv : this->cards_) {
    const CardPermissions &p = kv.second;
    if (!p.enabled || p.pin_hash.empty() || !const_time_equal_(p.pin_hash, h)) continue;
    port->fails = 0;
    const std::string actor = (p.user_name.empty() ? "pin:" + std::to_string(kv.first) : p.user_name) + "@" + source;
    this->queue_event_("PIN", source, "AUTHORIZED", actor);
    return this->authorize_(p, kv.first, actor, false, action);
  }
  this->queue_event_("PIN", source, "DENIED", "keypad");
  this->register_failure_(port, source);
  return RemoteReaderResult::DENIED;
}

uint8_t ProkopovAlarm::remote_reader_state_code() const {
  RecursiveLock lock(this->state_mutex_);
  const uint64_t now = millis();

  if (this->arm_confirm_card_ != 0 && this->arm_confirm_deadline_ms_ > now)
    return 2;  // ARM_WAIT

  if (this->state_ == AlarmState::EXIT_DELAY)
    return 2;  // ARM_WAIT / exit-delay indication

  if (is_armed_or_alarm(this->state_))
    return 3;  // ARMED / protected state

  return this->lock_output_locked() ? 0 : 1;  // LOCKED : UNLOCKED
}

uint32_t ProkopovAlarm::arm_confirm_remaining_ms() const {
  RecursiveLock lock(this->state_mutex_);
  if (this->arm_confirm_card_ == 0 || this->arm_confirm_deadline_ms_ == 0) return 0;

  const uint64_t now = millis();
  if (this->arm_confirm_deadline_ms_ <= now) return 0;

  const uint64_t remaining = this->arm_confirm_deadline_ms_ - now;
  return remaining > 0xFFFFFFFFULL ? 0xFFFFFFFFU : static_cast<uint32_t>(remaining);
}

bool ProkopovAlarm::arm_(AlarmState target, const std::string &actor, bool use_exit_delay) {
  if (!this->ready_for_(target) || this->state_ == AlarmState::ALARM || this->state_ == AlarmState::PANIC ||
      this->state_ == AlarmState::SILENT_PANIC) {
    this->queue_event_("ALARM", "Arming interlock", "ARM_BLOCKED", actor);
    return false;
  }
  this->set_siren_(false, actor);
  this->set_door_locked_(true, actor);
  if (use_exit_delay && this->exit_delay_s_ > 0) {
    this->pending_arm_target_ = target;
    this->pending_arm_actor_ = actor;
    this->state_deadline_ms_ = millis() + this->exit_delay_s_ * 1000ULL;
    this->set_state_(AlarmState::EXIT_DELAY, actor);
  } else {
    this->pending_arm_actor_.clear();
    this->state_deadline_ms_ = 0;
    this->set_state_(target, actor);
  }
  return true;
}

void ProkopovAlarm::disarm_(const std::string &actor) {
  this->arm_confirm_card_ = 0;
  this->arm_confirm_deadline_ms_ = 0;
  this->pending_arm_actor_.clear();
  this->state_deadline_ms_ = 0;
  this->armed_mode_before_alarm_ = AlarmState::DISARMED;
  this->set_siren_(false, actor);
  this->set_state_(AlarmState::DISARMED, actor);
  this->set_door_locked_(false, actor);
}

void ProkopovAlarm::trigger_alarm_(const std::string &reason) {
  if (this->state_ == AlarmState::PANIC || this->state_ == AlarmState::SILENT_PANIC) return;
  if (this->state_ == AlarmState::ARMED_AWAY || this->state_ == AlarmState::ARMED_HOME ||
      this->state_ == AlarmState::ARMED_NIGHT || this->state_ == AlarmState::ENTRY_DELAY) {
    this->armed_mode_before_alarm_ = this->state_ == AlarmState::ENTRY_DELAY ? this->armed_mode_before_alarm_ : this->state_;
  }
  this->state_deadline_ms_ = 0;
  this->set_siren_(true, reason);
  this->set_state_(AlarmState::ALARM, reason);
}

void ProkopovAlarm::trigger_panic_(bool silent, const std::string &actor) {
  this->state_deadline_ms_ = 0;
  if (silent) {
    this->set_siren_(false, actor);
    this->set_state_(AlarmState::SILENT_PANIC, actor);
  } else {
    this->set_siren_(true, actor);
    this->set_state_(AlarmState::PANIC, actor);
  }
}


bool ProkopovAlarm::command(const std::string &cmd, const std::string &actor) {
  RecursiveLock lock(this->state_mutex_);
  if (cmd == "arm_away" || cmd == "arm_full") return this->arm_(AlarmState::ARMED_AWAY, actor, true);
  if (cmd == "arm_home") return this->arm_(AlarmState::ARMED_HOME, actor, true);
  if (cmd == "arm_night") return this->arm_(AlarmState::ARMED_NIGHT, actor, true);
  if (cmd == "disarm" || cmd == "clear_alarm") { this->disarm_(actor); return true; }
  if (cmd == "panic") { this->trigger_panic_(false, actor); return true; }
  if (cmd == "silent_panic" || cmd == "silent_alarm") { this->trigger_panic_(true, actor); return true; }
  if (cmd == "siren_off" || cmd == "silence") { this->set_siren_(false, actor); return true; }
  if (cmd == "lock" || cmd == "lock_door") { this->set_door_locked_(true, actor); return true; }
  if (cmd == "unlock" || cmd == "unlock_door") {
    if (is_armed_or_alarm(this->state_)) return false;   // never open the door while protected
    this->set_door_locked_(false, actor);
    return true;
  }
  if (cmd == "lock_mode_auto") { this->set_lock_mode_(LockMode::AUTO, actor); return true; }
  if (cmd == "lock_mode_locked" || cmd == "always_locked") { this->set_lock_mode_(LockMode::ALWAYS_LOCKED, actor); return true; }
  if (cmd == "lock_mode_unlocked" || cmd == "always_unlocked") { this->set_lock_mode_(LockMode::ALWAYS_UNLOCKED, actor); return true; }
  if (cmd == "clear_lockout") {
    for (auto &p : this->readers_) { p.lockout_until_ms = 0; p.fails = 0; }
    this->panel_port_.lockout_until_ms = 0; this->panel_port_.fails = 0;
    this->queue_event_("SYSTEM", "Access", "LOCKOUT_CLEARED", actor);
    return true;
  }
  return false;
}

ZoneRule ProkopovAlarm::rule_for_(const ZoneConfig &zone, AlarmState mode) const {
  if (mode == AlarmState::ARMED_AWAY) return zone.away;
  if (mode == AlarmState::ARMED_HOME) return zone.home;
  if (mode == AlarmState::ARMED_NIGHT) return zone.night;
  if (mode == AlarmState::ENTRY_DELAY) return this->rule_for_(zone, this->armed_mode_before_alarm_);
  return ZoneRule::IGNORE;
}

bool ProkopovAlarm::zone_should_block_(const ZoneConfig &zone, AlarmState target) const {
  if (!zone.enabled || !zone.active) return false;
  return this->rule_for_(zone, target) != ZoneRule::IGNORE;
}

bool ProkopovAlarm::ready_for_(AlarmState target) const {
  // Fail-safe: never allow arming while running the bootstrap/fallback configuration.
  // Revision 0 means the production zone mapping has not been synchronized yet.
  if (this->config_revision_ == 0) return false;
  uint64_t now = millis();
  for (const auto &zone : this->zones_) {
    if (!zone.enabled) continue;
    ZoneRule rule = this->rule_for_(zone, target);
    if (rule == ZoneRule::IGNORE) continue;
    if (zone.active) return false;
    if (!zone.device.empty()) {
      auto hb = this->device_heartbeat_ms_.find(zone.device);
      if (hb == this->device_heartbeat_ms_.end() || now - hb->second > this->remote_device_timeout_s_ * 1000ULL) return false;
    }
  }
  return true;
}

void ProkopovAlarm::set_zone_state_(const std::string &device, int channel, bool active, const std::string &source) {
  RecursiveLock lock(this->state_mutex_);
  this->device_heartbeat_ms_[device] = millis();
  for (auto &zone : this->zones_) {
    if (zone.device == device && zone.channel == channel) {
      bool logical = zone.inverted ? !active : active;
      bool changed = zone.active != logical;
      zone.active = logical;
      if (changed) this->queue_event_("ZONE", zone.id, logical ? "ACTIVE" : "NORMAL", source);
      this->evaluate_zone_(zone, logical);
      return;
    }
  }
  this->queue_event_("SYSTEM", "Shelly", "UNMAPPED_INPUT " + device + ":" + std::to_string(channel), source);
}

void ProkopovAlarm::set_zone_state_by_id_(const std::string &id, bool active, const std::string &source) {
  RecursiveLock lock(this->state_mutex_);
  for (auto &zone : this->zones_) {
    if (zone.id == id) {
      bool changed = zone.active != active;
      zone.active = active;
      if (changed) this->queue_event_("ZONE", zone.id, active ? "ACTIVE" : "NORMAL", source);
      this->evaluate_zone_(zone, active);
      return;
    }
  }
}

void ProkopovAlarm::evaluate_zone_(ZoneConfig &zone, bool became_active) {
  if (!became_active || !zone.enabled) return;
  bool always = zone.away == ZoneRule::ALWAYS || zone.home == ZoneRule::ALWAYS || zone.night == ZoneRule::ALWAYS;
  if (always) {
    this->trigger_alarm_("zone:" + zone.id + ":24/7");
    return;
  }
  if (this->state_ == AlarmState::EXIT_DELAY || this->state_ == AlarmState::DISARMED) return;
  AlarmState mode = this->state_ == AlarmState::ENTRY_DELAY ? this->armed_mode_before_alarm_ : this->state_;
  ZoneRule rule = this->rule_for_(zone, mode);
  if (rule == ZoneRule::IGNORE) return;
  if (rule == ZoneRule::INSTANT) {
    this->trigger_alarm_("zone:" + zone.id);
  } else if (rule == ZoneRule::DELAYED) {
    if (this->state_ != AlarmState::ENTRY_DELAY) {
      this->armed_mode_before_alarm_ = mode;
      this->state_deadline_ms_ = millis() + this->entry_delay_s_ * 1000ULL;
      this->set_state_(AlarmState::ENTRY_DELAY, "zone:" + zone.id);
    }
  } else if (rule == ZoneRule::FOLLOWER) {
    if (this->state_ != AlarmState::ENTRY_DELAY) this->trigger_alarm_("zone:" + zone.id + ":follower");
  }
}

void ProkopovAlarm::tick_state_machine_() {
  RecursiveLock lock(this->state_mutex_);
  uint64_t now = millis();
  if (this->arm_confirm_card_ && this->arm_confirm_deadline_ms_ && now >= this->arm_confirm_deadline_ms_) {
    this->arm_confirm_card_ = 0;
    this->arm_confirm_deadline_ms_ = 0;
  }
  if (this->enroll_active_ && now >= this->enroll_deadline_ms_) this->enroll_active_ = false;

  if (this->state_ == AlarmState::EXIT_DELAY && this->state_deadline_ms_ && now >= this->state_deadline_ms_) {
    this->state_deadline_ms_ = 0;
    const std::string actor = this->pending_arm_actor_.empty() ? "system" : this->pending_arm_actor_;
    this->pending_arm_actor_.clear();
    if (this->ready_for_(this->pending_arm_target_)) {
      this->set_state_(this->pending_arm_target_, actor);
    } else {
      this->queue_event_("ALARM", "Arming interlock", "EXIT_DELAY_BLOCKED", actor);
      this->set_state_(AlarmState::DISARMED, actor);
      this->set_door_locked_(false, actor);
    }
  }
  if (this->state_ == AlarmState::ENTRY_DELAY && this->state_deadline_ms_ && now >= this->state_deadline_ms_) {
    this->state_deadline_ms_ = 0;
    this->trigger_alarm_("entry-delay-expired");
  }
  if (this->siren_on_ && this->siren_deadline_ms_ && now >= this->siren_deadline_ms_) {
    this->set_siren_(false, "timeout");
  }

  AlarmState mode = this->state_;
  if (mode == AlarmState::ENTRY_DELAY) mode = this->armed_mode_before_alarm_;
  if (mode == AlarmState::ARMED_AWAY || mode == AlarmState::ARMED_HOME || mode == AlarmState::ARMED_NIGHT) {
    bool fault = false;
    for (const auto &zone : this->zones_) {
      if (!zone.enabled || zone.device.empty() || this->rule_for_(zone, mode) == ZoneRule::IGNORE) continue;
      auto hb = this->device_heartbeat_ms_.find(zone.device);
      if (hb == this->device_heartbeat_ms_.end() || now - hb->second > this->remote_device_timeout_s_ * 1000ULL) { fault = true; break; }
    }
    if (fault && !this->remote_fault_latched_) {
      this->remote_fault_latched_ = true;
      this->queue_event_(
          "SYSTEM",
          "Remote input module",
          "REMOTE_MODULE_OFFLINE",
          "controller"
      );
    }

    if (!fault && this->remote_fault_latched_) {
      this->remote_fault_latched_ = false;
      this->queue_event_(
          "SYSTEM",
          "Remote input module",
          "REMOTE_MODULE_RESTORED",
          "controller"
      );
    }
  } else {
    this->remote_fault_latched_ = false;
  }
}

// ------------------------------------------------------------------ flash FAT (cards, configuration)
bool ProkopovAlarm::init_flash_() {
  esp_vfs_fat_mount_config_t mount{};
  mount.format_if_mount_failed = true;     // first boot of a new board: create the file system
  mount.max_files = 4;
  mount.allocation_unit_size = 4096;
  esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(FLASH_MOUNT, FLASH_PARTITION, &mount, &s_wl_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Flash FAT mount failed (partition '%s'): %s", FLASH_PARTITION, esp_err_to_name(err));
    return false;
  }
  ESP_LOGI(TAG, "Flash FAT mounted at %s", FLASH_MOUNT);
  return true;
}

bool ProkopovAlarm::read_file_(const std::string &path, std::string &out) {
  if (!this->flash_ok_) return false;
  if (this->storage_mutex_) xSemaphoreTake(this->storage_mutex_, portMAX_DELAY);
  FILE *f = fopen(path.c_str(), "rb");
  bool ok = false;
  if (f) {
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz >= 0 && sz <= 256 * 1024) {
      out.resize((size_t) sz);
      ok = sz == 0 || fread(out.data(), 1, (size_t) sz, f) == (size_t) sz;
    }
    fclose(f);
  }
  if (this->storage_mutex_) xSemaphoreGive(this->storage_mutex_);
  return ok;
}

bool ProkopovAlarm::write_file_atomic_(const std::string &path, const std::string &data) {
  if (!this->flash_ok_) return false;
  if (this->storage_mutex_) xSemaphoreTake(this->storage_mutex_, portMAX_DELAY);
  const std::string tmp = path + ".tmp";
  bool ok = false;
  FILE *f = fopen(tmp.c_str(), "wb");
  if (f) {
    ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = (fflush(f) == 0) && ok;
    ok = (fsync(fileno(f)) == 0) && ok;
    ok = (fclose(f) == 0) && ok;
    if (ok) {
      remove(path.c_str());
      ok = rename(tmp.c_str(), path.c_str()) == 0;
    }
  }
  if (!ok) {
    remove(tmp.c_str());
    ESP_LOGW(TAG, "Atomic flash write failed: %s errno=%d", path.c_str(), errno);
  }
  if (this->storage_mutex_) xSemaphoreGive(this->storage_mutex_);
  return ok;
}

// ------------------------------------------------------------------ FRAM
bool ProkopovAlarm::fram_read_(uint16_t addr, uint8_t *data, size_t len) {
  while (len) {
    size_t n = std::min<size_t>(len, 64);
    if (this->read_register16(addr, data, n) != i2c::ERROR_OK) return false;
    addr += n; data += n; len -= n;
  }
  return true;
}

bool ProkopovAlarm::fram_write_(uint16_t addr, const uint8_t *data, size_t len) {
  while (len) {
    size_t n = std::min<size_t>(len, 64);
    if (this->write_register16(addr, data, n) != i2c::ERROR_OK) return false;
    addr += n; data += n; len -= n;
  }
  return true;
}

bool ProkopovAlarm::fram_probe_() {
  uint32_t hdr[2] = {0, 0};
  if (!this->fram_read_(FRAM_HDR, reinterpret_cast<uint8_t *>(hdr), sizeof(hdr))) {
    ESP_LOGE(TAG, "FRAM not answering at 0x%02X", this->address_);
    return false;
  }
  if (hdr[0] != FRAM_MAGIC || hdr[1] != 1) {
    // New or foreign chip: format (header, empty runtime records, empty journal).
    ESP_LOGW(TAG, "FRAM blank: formatting journal");
    uint8_t zero[FRAM_SLOT] = {0};
    for (uint16_t a = FRAM_RT_A; a < FRAM_JOURNAL + FRAM_SLOTS * FRAM_SLOT; a += FRAM_SLOT)
      if (!this->fram_write_(a, zero, std::min<uint32_t>(FRAM_SLOT, FRAM_JOURNAL + FRAM_SLOTS * FRAM_SLOT - a))) return false;
    uint32_t h[2] = {FRAM_MAGIC, 1};
    if (!this->fram_write_(FRAM_HDR, reinterpret_cast<uint8_t *>(h), sizeof(h))) return false;
  }
  // Write/read-back test on a spare header byte.
  uint8_t t = (uint8_t) (esp_timer_get_time() & 0xFF), r = 0;
  if (!this->fram_write_(FRAM_HDR + 12, &t, 1) || !this->fram_read_(FRAM_HDR + 12, &r, 1) || r != t) {
    ESP_LOGE(TAG, "FRAM read-back mismatch");
    return false;
  }
  return true;
}

void ProkopovAlarm::fram_load_journal_() {
  struct Item { uint64_t seq; std::string line; };
  std::vector<Item> items;
  uint64_t best = 0;
  uint16_t best_slot = 0;
  uint8_t buf[FRAM_SLOT];
  for (uint16_t i = 0; i < FRAM_SLOTS; i++) {
    if (!this->fram_read_(FRAM_JOURNAL + i * FRAM_SLOT, buf, FRAM_SLOT)) break;
    uint64_t seq; uint16_t len, crc;
    memcpy(&seq, buf, 8); memcpy(&len, buf + 8, 2); memcpy(&crc, buf + 10, 2);
    if (!seq || len == 0 || len > FRAM_TEXT) continue;
    if ((uint16_t) esp_rom_crc16_le(0, buf + 12, len) != crc) continue;
    items.push_back({seq, std::string(reinterpret_cast<char *>(buf + 12), len)});
    if (seq > best) { best = seq; best_slot = i; }
  }
  std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.seq < b.seq; });
  for (auto &it : items) this->recent_event_lines_.push_back(it.line + "\n");
  while (this->recent_event_lines_.size() > 256) this->recent_event_lines_.pop_front();
  this->event_seq_ = std::max(this->event_seq_, best);
  this->event_persisted_seq_ = this->event_seq_;
  this->fram_next_slot_ = items.empty() ? 0 : (best_slot + 1) % FRAM_SLOTS;
  ESP_LOGI(TAG, "FRAM journal: %u events, last seq %llu", (unsigned) items.size(), (unsigned long long) best);
}

bool ProkopovAlarm::fram_append_event_(uint64_t seq, const std::string &line) {
  uint8_t buf[FRAM_SLOT] = {0};
  std::string text = line;
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
  if (text.size() > FRAM_TEXT) {
    // keep valid JSON: drop the actor, it is the only free-form field at the end
    size_t cut = text.rfind(",\"actor\"");
    text = cut != std::string::npos && cut + 2 <= FRAM_TEXT ? text.substr(0, cut) + "}" : text.substr(0, FRAM_TEXT);
  }
  uint16_t len = (uint16_t) text.size();
  memcpy(buf, &seq, 8);
  memcpy(buf + 8, &len, 2);
  memcpy(buf + 12, text.data(), len);
  uint16_t crc = (uint16_t) esp_rom_crc16_le(0, buf + 12, len);
  memcpy(buf + 10, &crc, 2);
  if (!this->fram_write_(FRAM_JOURNAL + this->fram_next_slot_ * FRAM_SLOT, buf, FRAM_SLOT)) return false;
  this->fram_next_slot_ = (this->fram_next_slot_ + 1) % FRAM_SLOTS;
  return true;
}

void ProkopovAlarm::load_cards_() {
  std::string body;
  if (this->read_file_(CARDS_FILE, body) && this->apply_cards_json_(body, false) && !this->cards_.empty()) {
    ESP_LOGI(TAG, "Loaded %u cards from flash", (unsigned) this->cards_.size());
    return;
  }
  for (uint32_t uid : this->bootstrap_cards_) {
    CardPermissions p; p.enabled = true; p.can_unlock = true; p.can_arm = true; p.can_disarm = true;
    this->cards_[uid] = p;
  }
  ESP_LOGW(TAG, "Using %u bootstrap cards", (unsigned) this->cards_.size());
}

void ProkopovAlarm::load_runtime_state_() {
  this->state_ = AlarmState::DISARMED;
  uint8_t saved_state = 0, saved_mode = 0, saved_door = 0;
  bool have = false;
  if (this->fram_ok_) {
    RuntimeRecord a{}, b{};
    bool va = this->fram_read_(FRAM_RT_A, reinterpret_cast<uint8_t *>(&a), sizeof(a)) && a.magic == FRAM_MAGIC &&
              esp_rom_crc32_le(0, reinterpret_cast<uint8_t *>(&a), sizeof(a) - 4) == a.crc;
    bool vb = this->fram_read_(FRAM_RT_B, reinterpret_cast<uint8_t *>(&b), sizeof(b)) && b.magic == FRAM_MAGIC &&
              esp_rom_crc32_le(0, reinterpret_cast<uint8_t *>(&b), sizeof(b) - 4) == b.crc;
    const RuntimeRecord *r = va && (!vb || a.generation >= b.generation) ? &a : vb ? &b : nullptr;
    if (r) {
      have = true;
      saved_state = r->state; saved_mode = r->lock_mode; saved_door = r->door_locked;
      this->fram_runtime_gen_ = r->generation;
      this->event_seq_ = std::max<uint64_t>(this->event_seq_, r->event_seq);
    }
  } else {
    std::string body;
    if (this->read_file_(STATE_FILE, body)) {
      cJSON *root = cJSON_Parse(body.c_str());
      if (root) {
        cJSON *v = cJSON_GetObjectItem(root, "s"); if (cJSON_IsNumber(v)) { saved_state = v->valueint; have = true; }
        v = cJSON_GetObjectItem(root, "m"); if (cJSON_IsNumber(v)) saved_mode = v->valueint;
        v = cJSON_GetObjectItem(root, "d"); if (cJSON_IsNumber(v)) saved_door = v->valueint;
        cJSON_Delete(root);
      }
    }
  }
  if (!have) return;
  AlarmState s = static_cast<AlarmState>(saved_state);
  if (s == AlarmState::ENTRY_DELAY) s = AlarmState::ALARM;          // never lose an intrusion over a reboot
  if (s == AlarmState::EXIT_DELAY || saved_state > (uint8_t) AlarmState::SILENT_PANIC) s = AlarmState::DISARMED;
  this->state_ = s;
  this->lock_mode_ = saved_mode <= 2 ? static_cast<LockMode>(saved_mode) : LockMode::AUTO;
  this->door_locked_ = saved_door != 0;
  ESP_LOGI(TAG, "Runtime state restored: %s, lock mode %s", state_to_cstr(this->state_), this->lock_mode_string().c_str());
}

void ProkopovAlarm::persist_runtime_state_() {
  RuntimeRecord r{};
  {
    RecursiveLock lock(this->state_mutex_);
    r.magic = FRAM_MAGIC;
    r.generation = ++this->fram_runtime_gen_;
    r.state = (uint8_t) this->state_;
    r.lock_mode = (uint8_t) this->lock_mode_;
    r.door_locked = this->door_locked_ ? 1 : 0;
    r.epoch = this->current_epoch_();
    r.event_seq = this->event_seq_;
    this->state_persist_pending_ = false;
  }
  if (this->fram_ok_) {
    r.crc = esp_rom_crc32_le(0, reinterpret_cast<uint8_t *>(&r), sizeof(r) - 4);
    // alternate A/B so a power cut during a write always leaves the previous record intact
    uint16_t addr = (r.generation & 1) ? FRAM_RT_A : FRAM_RT_B;
    if (!this->fram_write_(addr, reinterpret_cast<uint8_t *>(&r), sizeof(r))) this->state_persist_pending_ = true;
    return;
  }
  // Fallback without FRAM: flash, only on real state changes (rate-limited by the callers).
  char body[64];
  snprintf(body, sizeof(body), "{\"s\":%u,\"m\":%u,\"d\":%u}", r.state, r.lock_mode, r.door_locked);
  if (!this->write_file_atomic_(STATE_FILE, body)) this->state_persist_pending_ = true;
}

void ProkopovAlarm::load_config_() {
  std::string body;
  if (this->read_file_(CONFIG_FILE, body) && this->apply_config_json_(body, false) && !this->zones_.empty()) {
    ESP_LOGI(TAG, "Loaded configuration revision %lu from flash", static_cast<unsigned long>(this->config_revision_));
    return;
  }

  // Conservative fallback derived from the last working MicroPython zone set.
  // AWAY protects all old zones; NIGHT preserves the old NIGHT_ZONES set.
  const char *ids[] = {"pir_entrance","pir_dining","pir_living","pir_office","perimeter","pir_summer_kitchen","pir_shed","spare_input"};
  for (const char *id : ids) {
    ZoneConfig z; z.id = id; z.name = id; z.away = ZoneRule::INSTANT; z.home = ZoneRule::IGNORE;
    z.night = (z.id == "perimeter" || z.id == "pir_summer_kitchen" || z.id == "pir_shed") ? ZoneRule::INSTANT : ZoneRule::IGNORE;
    this->zones_.push_back(z);
  }
  ESP_LOGW(TAG, "No production config in flash; loaded safe legacy fallback with unmapped remote inputs");
}

bool ProkopovAlarm::apply_cards_json_(const std::string &body, bool persist) {
  {
    RecursiveLock lock(this->state_mutex_);
  cJSON *root = cJSON_Parse(body.c_str());
  if (!root) return false;
  cJSON *cards = cJSON_GetObjectItem(root, "cards");
  if (!cJSON_IsArray(cards)) { cJSON_Delete(root); return false; }
  std::unordered_map<uint32_t, CardPermissions> next;
  cJSON *item = nullptr;
  cJSON_ArrayForEach(item, cards) {
    cJSON *uidj = cJSON_GetObjectItem(item, "uid");
    uint32_t uid = 0;
    if (cJSON_IsString(uidj) && uidj->valuestring) uid = strtoul(uidj->valuestring, nullptr, 10);
    else if (cJSON_IsNumber(uidj)) uid = (uint32_t) uidj->valuedouble;
    if (!uid) continue;
    CardPermissions p;
    cJSON *v;
    v = cJSON_GetObjectItem(item, "user_name"); if (cJSON_IsString(v) && v->valuestring) p.user_name = v->valuestring;
    v = cJSON_GetObjectItem(item, "enabled"); if (cJSON_IsBool(v)) p.enabled = cJSON_IsTrue(v);
    v = cJSON_GetObjectItem(item, "can_unlock"); if (cJSON_IsBool(v)) p.can_unlock = cJSON_IsTrue(v);
    v = cJSON_GetObjectItem(item, "can_arm"); if (cJSON_IsBool(v)) p.can_arm = cJSON_IsTrue(v);
    v = cJSON_GetObjectItem(item, "can_disarm"); if (cJSON_IsBool(v)) p.can_disarm = cJSON_IsTrue(v);
    v = cJSON_GetObjectItem(item, "pin_hash");
    if (cJSON_IsString(v) && v->valuestring && strlen(v->valuestring) == 64) {
      p.pin_hash = v->valuestring;
      std::transform(p.pin_hash.begin(), p.pin_hash.end(), p.pin_hash.begin(), ::tolower);
    }
    next[uid] = p;
  }
  cJSON *rev = cJSON_GetObjectItem(root, "revision");
  if (cJSON_IsNumber(rev)) this->cards_revision_ = (uint32_t) rev->valuedouble;
  this->cards_.swap(next);
  cJSON_Delete(root);
  }

  // FAT metadata updates on this installation can take more than one second.
  // The HTTP server task may wait for persistence, but never keep state_mutex_
  // held while doing SD I/O: the autonomous alarm loop and ESPHome template
  // sensors must remain responsive during config/card synchronization.
  if (persist) return this->write_file_atomic_(CARDS_FILE, body);
  return true;
}

bool ProkopovAlarm::apply_config_json_(const std::string &body, bool persist) {
  {
    RecursiveLock lock(this->state_mutex_);
  cJSON *root = cJSON_Parse(body.c_str());
  if (!root) return false;
  cJSON *zones = cJSON_GetObjectItem(root, "zones");
  if (!cJSON_IsArray(zones)) { cJSON_Delete(root); return false; }

  cJSON *timings = cJSON_GetObjectItem(root, "timings");
  if (cJSON_IsObject(timings)) {
    cJSON *v;
    v = cJSON_GetObjectItem(timings, "exit_delay_s"); if (cJSON_IsNumber(v)) this->exit_delay_s_ = std::max(0, std::min(300, v->valueint));
    v = cJSON_GetObjectItem(timings, "entry_delay_s"); if (cJSON_IsNumber(v)) this->entry_delay_s_ = std::max(0, std::min(300, v->valueint));
    v = cJSON_GetObjectItem(timings, "siren_timeout_s"); if (cJSON_IsNumber(v)) this->siren_timeout_s_ = std::max(0, std::min(1800, v->valueint));
    v = cJSON_GetObjectItem(timings, "remote_device_timeout_s"); if (cJSON_IsNumber(v)) this->remote_device_timeout_s_ = std::max(10, std::min(600, v->valueint));
  }

  std::vector<ZoneConfig> next;
  cJSON *item = nullptr;
  cJSON_ArrayForEach(item, zones) {
    cJSON *idj = cJSON_GetObjectItem(item, "id");
    if (!cJSON_IsString(idj) || !idj->valuestring || !*idj->valuestring) continue;
    ZoneConfig z;
    z.id = idj->valuestring;
    cJSON *v;
    v = cJSON_GetObjectItem(item, "name"); z.name = cJSON_IsString(v) && v->valuestring ? v->valuestring : z.id;
    v = cJSON_GetObjectItem(item, "device"); z.device = cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
    v = cJSON_GetObjectItem(item, "channel"); z.channel = cJSON_IsNumber(v) ? v->valueint : -1;
    v = cJSON_GetObjectItem(item, "enabled"); if (cJSON_IsBool(v)) z.enabled = cJSON_IsTrue(v);
    v = cJSON_GetObjectItem(item, "inverted"); if (cJSON_IsBool(v)) z.inverted = cJSON_IsTrue(v);
    v = cJSON_GetObjectItem(item, "away"); z.away = parse_rule(cJSON_IsString(v) ? v->valuestring : nullptr);
    v = cJSON_GetObjectItem(item, "home"); z.home = parse_rule(cJSON_IsString(v) ? v->valuestring : nullptr);
    v = cJSON_GetObjectItem(item, "night"); z.night = parse_rule(cJSON_IsString(v) ? v->valuestring : nullptr);
    for (const auto &old : this->zones_) if (old.id == z.id) { z.active = old.active; break; }
    next.push_back(z);
  }
  cJSON *rev = cJSON_GetObjectItem(root, "revision");
  if (cJSON_IsNumber(rev)) this->config_revision_ = (uint32_t) rev->valuedouble;
  this->zones_.swap(next);
  cJSON_Delete(root);
  }

  // Persist only after releasing state_mutex_.  This preserves the existing
  // synchronous API contract (HTTP 200 still means the flash write succeeded)
  // without stalling the real-time alarm state machine while flash is busy.
  if (persist) return this->write_file_atomic_(CONFIG_FILE, body);
  return true;
}

uint64_t ProkopovAlarm::current_epoch_() const {
  time_t now = time(nullptr);
  return now > 1700000000 ? (uint64_t) now : 0;
}

void ProkopovAlarm::queue_event_(const std::string &type, const std::string &source,
                                 const std::string &event, const std::string &actor) {
  RecursiveLock lock(this->state_mutex_);
  this->last_event_ = event + " · " + source;
  cJSON *root = cJSON_CreateObject();
  uint64_t seq = ++this->event_seq_;
  cJSON_AddNumberToObject(root, "seq", (double) seq);
  cJSON_AddNumberToObject(root, "epoch", (double) this->current_epoch_());
  cJSON_AddStringToObject(root, "type", type.c_str());
  cJSON_AddStringToObject(root, "source", source.c_str());
  cJSON_AddStringToObject(root, "event", event.c_str());
  cJSON_AddStringToObject(root, "actor", actor.c_str());
  char *printed = cJSON_PrintUnformatted(root);
  if (printed) {
    const std::string line = std::string(printed) + "\n";
    if (this->pending_event_lines_.size() >= 256) this->pending_event_lines_.pop_front();
    this->pending_event_lines_.emplace_back(seq, line);
    if (this->recent_event_lines_.size() >= 256) this->recent_event_lines_.pop_front();
    this->recent_event_lines_.push_back(line);
    cJSON_free(printed);
  }
  cJSON_Delete(root);
}

// Called from loop() only (I2C is not shared with the HTTP tasks).
void ProkopovAlarm::flush_one_event_() {
  std::pair<uint64_t, std::string> item;
  {
    RecursiveLock lock(this->state_mutex_);
    if (this->pending_event_lines_.empty()) return;
    item = this->pending_event_lines_.front();
    this->pending_event_lines_.pop_front();
  }
  if (!this->fram_ok_) {               // RAM ring only; the server still collects via /events
    this->event_persisted_seq_ = std::max(this->event_persisted_seq_, item.first);
    return;
  }
  if (this->fram_append_event_(item.first, item.second)) {
    this->event_persisted_seq_ = std::max(this->event_persisted_seq_, item.first);
    return;
  }
  this->event_write_failures_++;
  RecursiveLock lock(this->state_mutex_);
  this->pending_event_lines_.push_front(item);
  if (this->event_write_failures_ == 1 || (this->event_write_failures_ % 50) == 0)
    ESP_LOGW(TAG, "FRAM journal write failed (%lu)", (unsigned long) this->event_write_failures_);
}

// ------------------------------------------------------------------ board inputs (from YAML)
void ProkopovAlarm::set_local_input(int channel, bool active) {
  this->set_zone_state_("local", channel, active, "board");
}

void ProkopovAlarm::uni_command(bool arm) {
  // Shelly Plus Uni outputs are pulsed by the Shelly app/scene; ignore chatter inside 2 s.
  RecursiveLock lock(this->state_mutex_);
  const uint64_t now = millis();
  if (this->uni_last_ms_ && now - this->uni_last_ms_ < 2000) return;
  this->uni_last_ms_ = now;
  if (arm) this->arm_(AlarmState::ARMED_AWAY, "shelly-uni", true);
  else this->disarm_("shelly-uni");
}

void ProkopovAlarm::set_tamper(bool open) {
  RecursiveLock lock(this->state_mutex_);
  if (open == this->tamper_open_) return;
  this->tamper_open_ = open;
  this->queue_event_("SYSTEM", "Enclosure", open ? "TAMPER_OPEN" : "TAMPER_CLOSED", "board");
  if (open && (this->state_ == AlarmState::ARMED_AWAY || this->state_ == AlarmState::ARMED_HOME ||
               this->state_ == AlarmState::ARMED_NIGHT || this->state_ == AlarmState::ENTRY_DELAY ||
               this->state_ == AlarmState::EXIT_DELAY))
    this->trigger_alarm_("tamper");
}

void ProkopovAlarm::set_mains(bool present) {
  RecursiveLock lock(this->state_mutex_);
  if (present == this->mains_present_) return;
  this->mains_present_ = present;
  this->queue_event_("POWER", "Mains 24 V", present ? "MAINS_RESTORED" : "MAINS_LOST", "board");
}

void ProkopovAlarm::set_siren_line_fault(bool fault) {
  RecursiveLock lock(this->state_mutex_);
  if (fault == this->siren_line_fault_) return;
  this->siren_line_fault_ = fault;
  this->queue_event_("SYSTEM", "Siren line", fault ? "SIREN_LINE_FAULT" : "SIREN_LINE_OK", "board");
}

void ProkopovAlarm::set_battery(float volts, bool test_result_valid, bool test_passed) {
  RecursiveLock lock(this->state_mutex_);
  this->battery_v_ = volts;
  const bool low = volts > 1.0f && volts < 11.5f;
  if (low != this->battery_low_latched_) {
    this->battery_low_latched_ = low;
    this->queue_event_("POWER", "Battery", low ? "BATTERY_LOW" : "BATTERY_OK", "board");
  }
  if (test_result_valid)
    this->queue_event_("POWER", "Battery", test_passed ? "BATTERY_TEST_PASS" : "BATTERY_TEST_FAIL", "self-test");
}

uint32_t ProkopovAlarm::state_remaining_ms() const {
  const uint64_t now = millis();
  return this->state_deadline_ms_ > now ? (uint32_t) (this->state_deadline_ms_ - now) : 0;
}

void ProkopovAlarm::panel_snapshot(uint8_t &state, uint8_t &flags, uint16_t &zones_active, uint16_t &remaining_s) const {
  RecursiveLock lock(this->state_mutex_);
  state = (uint8_t) this->state_;
  flags = (this->ready_for_(AlarmState::ARMED_AWAY) ? 0x01 : 0) | (this->ready_for_(AlarmState::ARMED_HOME) ? 0x02 : 0) |
          (this->ready_for_(AlarmState::ARMED_NIGHT) ? 0x04 : 0) | (this->lock_output_locked() ? 0x08 : 0) |
          (this->siren_on_ ? 0x10 : 0) | (this->mains_present_ ? 0x20 : 0) | (this->tamper_open_ ? 0x40 : 0) |
          (this->siren_line_fault_ ? 0x80 : 0);
  zones_active = 0;
  for (size_t i = 0; i < this->zones_.size() && i < 16; i++)
    if (this->zones_[i].active) zones_active |= (1U << i);
  remaining_s = (uint16_t) std::min<uint32_t>(this->state_remaining_ms() / 1000U, 0xFFFF);
}

bool ProkopovAlarm::start_http_servers_() {
  if (this->management_http_ != nullptr && this->shelly_http_ != nullptr) return true;

  if (this->management_http_ == nullptr) {
    if (!this->start_http_server_(&this->management_http_, this->management_port_, false)) return false;
  }

  if (this->shelly_http_ == nullptr) {
    if (!this->start_http_server_(&this->shelly_http_, this->shelly_port_, true)) {
      if (this->management_http_ != nullptr) {
        httpd_stop(this->management_http_);
        this->management_http_ = nullptr;
      }
      return false;
    }
  }
  return true;
}

bool ProkopovAlarm::start_http_server_(httpd_handle_t *handle, uint16_t port, bool shelly) {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = port;
  cfg.ctrl_port = shelly ? 32769 : 32768;
  cfg.max_uri_handlers = shelly ? 4 : 16;
  cfg.stack_size = 6144;
  cfg.lru_purge_enable = true;

  // The alarm-server uses persistent HTTP connections.  Do not expire a
  // healthy pooled session during normal gaps between status/events/reconcile
  // requests.  The client expires its own idle connection sooner.
  cfg.recv_wait_timeout = 10;
  cfg.send_wait_timeout = 5;
  cfg.uri_match_fn = httpd_uri_match_wildcard;
  if (httpd_start(handle, &cfg) != ESP_OK) {
    ESP_LOGE(TAG, "HTTP server failed on port %u", port);
    return false;
  }

  auto reg = [&](const char *uri, httpd_method_t method, esp_err_t (*fn)(httpd_req_t *)) {
    httpd_uri_t h{}; h.uri = uri; h.method = method; h.handler = fn; h.user_ctx = this;
    return httpd_register_uri_handler(*handle, &h) == ESP_OK;
  };

  if (shelly) {
    reg("/shelly*", HTTP_GET, &ProkopovAlarm::shelly_handler_);
    reg("/heartbeat*", HTTP_GET, &ProkopovAlarm::heartbeat_handler_);
    reg("/health", HTTP_GET, &ProkopovAlarm::health_handler_);
  } else {
    reg("/status", HTTP_GET, &ProkopovAlarm::status_handler_);
    reg("/command", HTTP_POST, &ProkopovAlarm::command_handler_);
    reg("/config", HTTP_PUT, &ProkopovAlarm::config_handler_);
    reg("/cards", HTTP_GET, &ProkopovAlarm::cards_handler_);
    reg("/cards", HTTP_PUT, &ProkopovAlarm::cards_handler_);
    reg("/zones/reconcile", HTTP_POST, &ProkopovAlarm::reconcile_handler_);
    reg("/card-scan/begin", HTTP_POST, &ProkopovAlarm::card_scan_begin_handler_);
    reg("/card-scan/status", HTTP_GET, &ProkopovAlarm::card_scan_status_handler_);
    reg("/card-scan/stop", HTTP_POST, &ProkopovAlarm::card_scan_stop_handler_);
    reg("/events*", HTTP_GET, &ProkopovAlarm::events_handler_);
    reg("/health", HTTP_GET, &ProkopovAlarm::health_handler_);
  }
  ESP_LOGI(TAG, "%s HTTP API listening on %u", shelly ? "Shelly" : "Management", port);
  return true;
}

bool ProkopovAlarm::management_authorized_(httpd_req_t *req) const {
  if (this->control_key_.empty()) return true;
  size_t len = httpd_req_get_hdr_value_len(req, "X-Alarm-Key");
  if (!len || len > 256) return false;
  std::vector<char> buf(len + 1, 0);
  if (httpd_req_get_hdr_value_str(req, "X-Alarm-Key", buf.data(), buf.size()) != ESP_OK) return false;
  return const_time_equal_(this->control_key_, buf.data());
}

bool ProkopovAlarm::read_request_body_(httpd_req_t *req, std::string &body) const {
  if (req->content_len <= 0 || req->content_len > 64 * 1024) return false;
  body.resize(req->content_len);
  size_t got = 0;
  while (got < body.size()) {
    int n = httpd_req_recv(req, body.data() + got, body.size() - got);
    if (n <= 0) return false;
    got += n;
  }
  return true;
}

void ProkopovAlarm::send_json_(httpd_req_t *req, int code, const std::string &body) const {
  char status[32]; snprintf(status, sizeof(status), "%d %s", code, code == 200 ? "OK" : code == 401 ? "Unauthorized" : "Error");
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_send(req, body.c_str(), body.size());
}

void ProkopovAlarm::send_text_(httpd_req_t *req, int code, const std::string &body) const {
  char status[32]; snprintf(status, sizeof(status), "%d %s", code, code == 200 ? "OK" : "Forbidden");
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_send(req, body.c_str(), body.size());
}

std::string ProkopovAlarm::build_status_json_() const {
  RecursiveLock lock(this->state_mutex_);
  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "ok", true);
  cJSON_AddStringToObject(root, "alarm_state", state_to_cstr(this->state_));
  cJSON_AddBoolToObject(root, "door_locked", this->door_locked_);
  cJSON_AddBoolToObject(root, "siren_on", this->siren_on_);
  cJSON_AddStringToObject(root, "firmware", "v3.0.0");
  cJSON_AddBoolToObject(root, "sd_ok", this->fram_ok_ && this->flash_ok_);   // kept for alarm-server compatibility
  cJSON_AddBoolToObject(root, "fram_ok", this->fram_ok_);
  cJSON_AddBoolToObject(root, "flash_ok", this->flash_ok_);
  cJSON_AddStringToObject(root, "lock_mode", this->lock_mode_string().c_str());
  cJSON_AddStringToObject(root, "lock_type", this->lock_type_ == LockType::MAGLOCK ? "maglock" : "strike");
  cJSON_AddBoolToObject(root, "lock_output_locked", this->lock_output_locked());
  cJSON_AddBoolToObject(root, "mains", this->mains_present_);
  cJSON_AddBoolToObject(root, "tamper", this->tamper_open_);
  cJSON_AddBoolToObject(root, "siren_line_fault", this->siren_line_fault_);
  cJSON_AddNumberToObject(root, "battery_v", this->battery_v_);
  cJSON_AddBoolToObject(root, "ready_away", this->ready_for_(AlarmState::ARMED_AWAY));
  cJSON_AddBoolToObject(root, "ready_home", this->ready_for_(AlarmState::ARMED_HOME));
  cJSON_AddBoolToObject(root, "ready_night", this->ready_for_(AlarmState::ARMED_NIGHT));
  cJSON_AddNumberToObject(root, "config_revision", this->config_revision_);
  cJSON_AddNumberToObject(root, "cards_revision", this->cards_revision_);
  cJSON_AddNumberToObject(root, "cards", (double) this->cards_.size());
  cJSON_AddNumberToObject(root, "wiegand_filtered", this->readers_[0].filtered + this->readers_[1].filtered);
  cJSON_AddNumberToObject(root, "wiegand_overflow", this->readers_[0].overflow + this->readers_[1].overflow);
  cJSON_AddStringToObject(root, "last_event", this->last_event_.c_str());
  cJSON_AddNumberToObject(root, "event_seq", (double) this->event_seq_);
  cJSON_AddNumberToObject(root, "event_persisted_seq", (double) this->event_persisted_seq_);
  cJSON_AddNumberToObject(root, "event_pending", (double) this->pending_event_lines_.size());
  cJSON_AddNumberToObject(root, "event_write_failures", (double) this->event_write_failures_);
  cJSON_AddNumberToObject(root, "arm_confirm_remaining_ms", this->arm_confirm_deadline_ms_ > millis() ? (double) (this->arm_confirm_deadline_ms_ - millis()) : 0);
  cJSON_AddNumberToObject(root, "state_remaining_ms", this->state_deadline_ms_ > millis() ? (double) (this->state_deadline_ms_ - millis()) : 0);
  cJSON *da = cJSON_AddArrayToObject(root, "remote_devices");
  std::vector<std::string> seen_devices;
  for (const auto &z : this->zones_) {
    if (z.device.empty() || std::find(seen_devices.begin(), seen_devices.end(), z.device) != seen_devices.end()) continue;
    seen_devices.push_back(z.device);
    cJSON *d = cJSON_CreateObject(); cJSON_AddStringToObject(d, "device", z.device.c_str());
    auto hb = this->device_heartbeat_ms_.find(z.device);
    bool online = hb != this->device_heartbeat_ms_.end() && millis() - hb->second <= this->remote_device_timeout_s_ * 1000ULL;
    cJSON_AddBoolToObject(d, "online", online);
    cJSON_AddNumberToObject(d, "age_ms", hb == this->device_heartbeat_ms_.end() ? -1 : (double)(millis() - hb->second));
    cJSON_AddItemToArray(da, d);
  }

  cJSON *za = cJSON_AddArrayToObject(root, "zones");
  for (const auto &z : this->zones_) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", z.id.c_str());
    cJSON_AddStringToObject(o, "name", z.name.c_str());
    cJSON_AddBoolToObject(o, "active", z.active);
    cJSON_AddBoolToObject(o, "enabled", z.enabled);
    cJSON_AddStringToObject(o, "device", z.device.c_str());
    cJSON_AddNumberToObject(o, "channel", z.channel);
    cJSON_AddStringToObject(o, "away", rule_to_cstr(z.away));
    cJSON_AddStringToObject(o, "home", rule_to_cstr(z.home));
    cJSON_AddStringToObject(o, "night", rule_to_cstr(z.night));
    cJSON_AddItemToArray(za, o);
  }
  char *p = cJSON_PrintUnformatted(root);
  std::string out = p ? p : "{}";
  if (p) cJSON_free(p);
  cJSON_Delete(root);
  return out;
}

std::string ProkopovAlarm::build_cards_json_() const {
  RecursiveLock lock(this->state_mutex_);
  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "ok", true);
  cJSON_AddNumberToObject(root, "revision", this->cards_revision_);
  cJSON *arr = cJSON_AddArrayToObject(root, "cards");
  for (const auto &kv : this->cards_) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "uid", std::to_string(kv.first).c_str());
    cJSON_AddStringToObject(o, "user_name", kv.second.user_name.c_str());
    cJSON_AddBoolToObject(o, "enabled", kv.second.enabled);
    cJSON_AddBoolToObject(o, "can_unlock", kv.second.can_unlock);
    cJSON_AddBoolToObject(o, "can_arm", kv.second.can_arm);
    cJSON_AddBoolToObject(o, "can_disarm", kv.second.can_disarm);
    cJSON_AddBoolToObject(o, "has_pin", !kv.second.pin_hash.empty());   // the hash itself never leaves the board
    cJSON_AddItemToArray(arr, o);
  }
  char *p = cJSON_PrintUnformatted(root);
  std::string out = p ? p : "{}";
  if (p) cJSON_free(p);
  cJSON_Delete(root);
  return out;
}

std::string ProkopovAlarm::build_events_json_(uint64_t after_seq) const {
  // Served from the RAM ring (restored from the FRAM journal at boot) so /status is never blocked by I/O.
  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "ok", true);
  cJSON *arr = cJSON_AddArrayToObject(root, "events");

  int count = 0;

  {
    RecursiveLock lock(this->state_mutex_);

    for (const auto &line : this->recent_event_lines_) {
      if (count >= 200) break;

      cJSON *o = cJSON_Parse(line.c_str());
      if (!o) continue;

      cJSON *seq = cJSON_GetObjectItem(o, "seq");

      if (cJSON_IsNumber(seq) &&
          (uint64_t) seq->valuedouble > after_seq) {
        cJSON_AddItemToArray(arr, o);
        count++;
      } else {
        cJSON_Delete(o);
      }
    }

    cJSON_AddNumberToObject(
        root,
        "last_seq",
        (double) this->event_seq_
    );
  }

  char *printed = cJSON_PrintUnformatted(root);
  std::string out = printed ? printed : "{}";

  if (printed) cJSON_free(printed);
  cJSON_Delete(root);

  return out;
}

esp_err_t ProkopovAlarm::status_handler_(httpd_req_t *req) {
  auto *self = static_cast<ProkopovAlarm *>(req->user_ctx);
  if (!self->management_authorized_(req)) { self->send_json_(req, 401, "{\"ok\":false,\"error\":\"unauthorized\"}"); return ESP_OK; }
  self->send_json_(req, 200, self->build_status_json_()); return ESP_OK;
}

esp_err_t ProkopovAlarm::command_handler_(httpd_req_t *req) {
  auto *self = static_cast<ProkopovAlarm *>(req->user_ctx);
  if (!self->management_authorized_(req)) { self->send_json_(req, 401, "{\"ok\":false,\"error\":\"unauthorized\"}"); return ESP_OK; }
  std::string body; if (!self->read_request_body_(req, body)) { self->send_json_(req, 400, "{\"ok\":false,\"error\":\"body\"}"); return ESP_OK; }
  cJSON *root = cJSON_Parse(body.c_str());
  cJSON *cmd = root ? cJSON_GetObjectItem(root, "command") : nullptr;
  cJSON *actor = root ? cJSON_GetObjectItem(root, "actor") : nullptr;
  std::string c = cJSON_IsString(cmd) ? cmd->valuestring : "";
  std::string a = cJSON_IsString(actor) ? actor->valuestring : "web";
  bool ok = !c.empty() && self->command(c, a);
  if (root) cJSON_Delete(root);
  std::string response = std::string("{\"ok\":") + (ok ? "true" : "false") + ",\"status\":" + self->build_status_json_() + "}";
  self->send_json_(req, ok ? 200 : 400, response); return ESP_OK;
}

esp_err_t ProkopovAlarm::config_handler_(httpd_req_t *req) {
  auto *self = static_cast<ProkopovAlarm *>(req->user_ctx);
  if (!self->management_authorized_(req)) { self->send_json_(req, 401, "{\"ok\":false,\"error\":\"unauthorized\"}"); return ESP_OK; }
  std::string body; if (!self->read_request_body_(req, body)) { self->send_json_(req, 400, "{\"ok\":false}"); return ESP_OK; }
  bool ok = self->apply_config_json_(body, true);
  if (ok) self->queue_event_("SYSTEM", "Config", "CONFIG_SYNC", "alarm-server");
  self->send_json_(req, ok ? 200 : 400, ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"config\"}"); return ESP_OK;
}

esp_err_t ProkopovAlarm::cards_handler_(httpd_req_t *req) {
  auto *self = static_cast<ProkopovAlarm *>(req->user_ctx);
  if (!self->management_authorized_(req)) { self->send_json_(req, 401, "{\"ok\":false,\"error\":\"unauthorized\"}"); return ESP_OK; }
  if (req->method == HTTP_GET) { self->send_json_(req, 200, self->build_cards_json_()); return ESP_OK; }
  std::string body; if (!self->read_request_body_(req, body)) { self->send_json_(req, 400, "{\"ok\":false}"); return ESP_OK; }
  bool ok = self->apply_cards_json_(body, true);
  if (ok) self->queue_event_("NFC", "Card database", "CARD_SYNC", "alarm-server");
  self->send_json_(req, ok ? 200 : 400, ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"cards\"}"); return ESP_OK;
}

esp_err_t ProkopovAlarm::reconcile_handler_(httpd_req_t *req) {
  auto *self = static_cast<ProkopovAlarm *>(req->user_ctx);
  if (!self->management_authorized_(req)) { self->send_json_(req, 401, "{\"ok\":false}"); return ESP_OK; }
  std::string body; if (!self->read_request_body_(req, body)) { self->send_json_(req, 400, "{\"ok\":false}"); return ESP_OK; }
  cJSON *root = cJSON_Parse(body.c_str());
  cJSON *zones = root ? cJSON_GetObjectItem(root, "zones") : nullptr;
  if (cJSON_IsObject(zones)) {
    cJSON *item = nullptr;
    cJSON_ArrayForEach(item, zones) {
      if (cJSON_IsBool(item)) self->set_zone_state_by_id_(item->string ? item->string : "", cJSON_IsTrue(item), "reconcile");
      else if (cJSON_IsObject(item)) {
        cJSON *a = cJSON_GetObjectItem(item, "active");
        if (cJSON_IsBool(a)) self->set_zone_state_by_id_(item->string ? item->string : "", cJSON_IsTrue(a), "reconcile");
      }
    }
  }
  if (root) cJSON_Delete(root);
  self->send_json_(req, 200, "{\"ok\":true}"); return ESP_OK;
}

esp_err_t ProkopovAlarm::card_scan_begin_handler_(httpd_req_t *req) {
  auto *self = static_cast<ProkopovAlarm *>(req->user_ctx);
  if (!self->management_authorized_(req)) { self->send_json_(req, 401, "{\"ok\":false}"); return ESP_OK; }
  self->enroll_active_ = true; self->enroll_uid_ = 0; self->enroll_deadline_ms_ = millis() + 30000;
  self->send_json_(req, 200, "{\"ok\":true,\"waiting\":true,\"timeout_s\":30}"); return ESP_OK;
}

esp_err_t ProkopovAlarm::card_scan_status_handler_(httpd_req_t *req) {
  auto *self = static_cast<ProkopovAlarm *>(req->user_ctx);
  if (!self->management_authorized_(req)) { self->send_json_(req, 401, "{\"ok\":false}"); return ESP_OK; }
  std::ostringstream s; s << "{\"ok\":true,\"waiting\":" << (self->enroll_active_ ? "true" : "false") << ",\"uid\":";
  if (self->enroll_uid_) s << "\"" << self->enroll_uid_ << "\""; else s << "null";
  s << "}"; self->send_json_(req, 200, s.str()); return ESP_OK;
}

esp_err_t ProkopovAlarm::card_scan_stop_handler_(httpd_req_t *req) {
  auto *self = static_cast<ProkopovAlarm *>(req->user_ctx);
  if (!self->management_authorized_(req)) { self->send_json_(req, 401, "{\"ok\":false}"); return ESP_OK; }
  self->enroll_active_ = false; self->enroll_uid_ = 0; self->send_json_(req, 200, "{\"ok\":true}"); return ESP_OK;
}

esp_err_t ProkopovAlarm::events_handler_(httpd_req_t *req) {
  auto *self = static_cast<ProkopovAlarm *>(req->user_ctx);
  if (!self->management_authorized_(req)) { self->send_json_(req, 401, "{\"ok\":false}"); return ESP_OK; }
  char query[256]{}; uint64_t after = 0;
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    std::string v = query_value_(query, "after"); if (!v.empty()) after = strtoull(v.c_str(), nullptr, 10);
  }
  self->send_json_(req, 200, self->build_events_json_(after)); return ESP_OK;
}

esp_err_t ProkopovAlarm::shelly_handler_(httpd_req_t *req) {
  auto *self = static_cast<ProkopovAlarm *>(req->user_ctx);
  char query[768]{};
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) { self->send_text_(req, 403, "DENY"); return ESP_OK; }
  std::string q(query);
  if (!self->shelly_key_.empty() && !const_time_equal_(query_value_(q, "key"), self->shelly_key_)) { self->send_text_(req, 403, "DENY"); return ESP_OK; }
  std::string device = query_value_(q, "device");
  std::string ch = query_value_(q, "channel");
  std::string st = query_value_(q, "state");
  if (device.empty() || ch.empty()) { self->send_text_(req, 403, "DENY"); return ESP_OK; }
  bool active = st == "1" || st == "on" || st == "true" || st == "active" || st == "open";
  self->send_text_(req, 200, "OK");
  self->set_zone_state_(device, atoi(ch.c_str()), active, "shelly-webhook");
  return ESP_OK;
}

esp_err_t ProkopovAlarm::heartbeat_handler_(httpd_req_t *req) {
  auto *self = static_cast<ProkopovAlarm *>(req->user_ctx);
  char query[768]{};
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) { self->send_text_(req, 403, "DENY"); return ESP_OK; }
  std::string q(query);
  if (!self->shelly_key_.empty() && !const_time_equal_(query_value_(q, "key"), self->shelly_key_)) { self->send_text_(req, 403, "DENY"); return ESP_OK; }
  std::string device = query_value_(q, "device");
  if (device.empty()) { self->send_text_(req, 403, "DENY"); return ESP_OK; }
  {
    RecursiveLock lock(self->state_mutex_);
    self->device_heartbeat_ms_[device] = millis();
  }
  self->send_text_(req, 200, "OK");
  return ESP_OK;
}

esp_err_t ProkopovAlarm::health_handler_(httpd_req_t *req) {
  auto *self = static_cast<ProkopovAlarm *>(req->user_ctx);
  self->send_json_(req, 200, "{\"ok\":true}"); return ESP_OK;
}

std::string ProkopovAlarm::query_value_(const std::string &query, const std::string &key) {
  size_t start = 0;
  while (start < query.size()) {
    size_t end = query.find('&', start); if (end == std::string::npos) end = query.size();
    size_t eq = query.find('=', start);
    if (eq != std::string::npos && eq < end) {
      if (query.substr(start, eq - start) == key) return url_decode_(query.substr(eq + 1, end - eq - 1));
    }
    start = end + 1;
  }
  return "";
}

std::string ProkopovAlarm::url_decode_(const std::string &value) {
  std::string out; out.reserve(value.size());
  for (size_t i = 0; i < value.size(); i++) {
    if (value[i] == '+' ) out.push_back(' ');
    else if (value[i] == '%' && i + 2 < value.size()) {
      char hex[3] = {value[i+1], value[i+2], 0}; out.push_back((char) strtol(hex, nullptr, 16)); i += 2;
    } else out.push_back(value[i]);
  }
  return out;
}

bool ProkopovAlarm::const_time_equal_(const std::string &a, const std::string &b) {
  // length is not secret; the content comparison must not leak via timing
  if (a.size() != b.size()) return false;
  uint8_t d = 0;
  for (size_t i = 0; i < a.size(); i++) d |= (uint8_t) (a[i] ^ b[i]);
  return d == 0;
}

std::string ProkopovAlarm::sha256_hex_(const std::string &data) {
  uint8_t out[32];
  mbedtls_sha256(reinterpret_cast<const unsigned char *>(data.data()), data.size(), out, 0);
  static const char *hex = "0123456789abcdef";
  std::string s(64, '0');
  for (int i = 0; i < 32; i++) { s[2 * i] = hex[out[i] >> 4]; s[2 * i + 1] = hex[out[i] & 15]; }
  return s;
}

}  // namespace prokopov_alarm
}  // namespace esphome
