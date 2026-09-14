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
#include "driver/sdmmc_host.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
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
static const char *const SD_MOUNT = "/sd";
static const char *const BASE_DIR = "/sd/alarm";
static const char *const AUTH_DIR = "/sd/alarm/auth";
static const char *const LOG_DIR = "/sd/alarm/log";
static const char *const RUNTIME_DIR = "/sd/alarm/runtime";
static const char *const CARDS_FILE = "/sd/alarm/auth/cards.dat";
static const char *const CONFIG_FILE = "/sd/alarm/config.cfg";
static const char *const EVENTS_FILE = "/sd/alarm/log/events.log";
static const char *const STATE_FILE = "/sd/alarm/runtime/state.dat";

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

void ProkopovAlarm::set_pins(int d0, int d1, int door, int siren, int reader_control) {
  this->d0_pin_ = d0;
  this->d1_pin_ = d1;
  this->door_relay_pin_ = door;
  this->siren_relay_pin_ = siren;
  this->reader_control_pin_ = reader_control;
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

float ProkopovAlarm::get_setup_priority() const { return setup_priority::HARDWARE; }

void ProkopovAlarm::setup() {
  ESP_LOGI(TAG, "Starting PROKOPOV autonomous alarm core");
  this->storage_mutex_ = xSemaphoreCreateMutex();
  this->state_mutex_ = xSemaphoreCreateRecursiveMutex();

  gpio_config_t out_conf{};
  out_conf.intr_type = GPIO_INTR_DISABLE;
  out_conf.mode = GPIO_MODE_OUTPUT;
  out_conf.pin_bit_mask = (1ULL << this->door_relay_pin_) | (1ULL << this->siren_relay_pin_);
  out_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  out_conf.pull_up_en = GPIO_PULLUP_DISABLE;
  gpio_config(&out_conf);

  // Proven installation polarity from v0.31.x:
  // GPIO32 HIGH = unlocked, LOW = locked. GPIO33 HIGH = siren on.
  gpio_set_level((gpio_num_t) this->door_relay_pin_, 1);
  gpio_set_level((gpio_num_t) this->siren_relay_pin_, 0);
  this->door_locked_ = false;
  this->siren_on_ = false;

  gpio_config_t reader_ctl{};
  reader_ctl.intr_type = GPIO_INTR_DISABLE;
  reader_ctl.mode = GPIO_MODE_INPUT;  // high impedance when idle
  reader_ctl.pin_bit_mask = (1ULL << this->reader_control_pin_);
  reader_ctl.pull_down_en = GPIO_PULLDOWN_DISABLE;
  reader_ctl.pull_up_en = GPIO_PULLUP_DISABLE;
  gpio_config(&reader_ctl);

  this->sd_ok_ = this->init_sd_();
  this->load_config_();
  this->load_cards_();
  this->load_runtime_state_();

  // Apply the restored stable state only after the safe boot levels have been established.
  if (this->state_ == AlarmState::ARMED_AWAY || this->state_ == AlarmState::ARMED_HOME || this->state_ == AlarmState::ARMED_NIGHT) {
    gpio_set_level((gpio_num_t) this->door_relay_pin_, 0);
    this->door_locked_ = true;
  } else if (this->state_ == AlarmState::ALARM || this->state_ == AlarmState::PANIC) {
    gpio_set_level((gpio_num_t) this->door_relay_pin_, 0);
    gpio_set_level((gpio_num_t) this->siren_relay_pin_, 1);
    this->door_locked_ = true;
    this->siren_on_ = true;
    if (this->siren_timeout_s_ > 0) this->siren_deadline_ms_ = millis() + this->siren_timeout_s_ * 1000ULL;
  } else if (this->state_ == AlarmState::SILENT_PANIC) {
    gpio_set_level((gpio_num_t) this->door_relay_pin_, 0);
    this->door_locked_ = true;
  }

  // Preserve the proven v0.31.x electrical setup: D0 floating input, D1 with pull-up.
  gpio_config_t d0_conf{};
  d0_conf.intr_type = GPIO_INTR_NEGEDGE;
  d0_conf.mode = GPIO_MODE_INPUT;
  d0_conf.pin_bit_mask = (1ULL << this->d0_pin_);
  d0_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  d0_conf.pull_up_en = GPIO_PULLUP_DISABLE;
  gpio_config(&d0_conf);

  gpio_config_t d1_conf{};
  d1_conf.intr_type = GPIO_INTR_NEGEDGE;
  d1_conf.mode = GPIO_MODE_INPUT;
  d1_conf.pin_bit_mask = (1ULL << this->d1_pin_);
  d1_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  d1_conf.pull_up_en = GPIO_PULLUP_ENABLE;
  gpio_config(&d1_conf);

  esp_err_t isr_res = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
  if (isr_res != ESP_OK && isr_res != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(isr_res));
  }
  gpio_isr_handler_add((gpio_num_t) this->d0_pin_, &ProkopovAlarm::d0_isr_, this);
  gpio_isr_handler_add((gpio_num_t) this->d1_pin_, &ProkopovAlarm::d1_isr_, this);

  // Network services are intentionally NOT started from setup().
  // At HARDWARE setup priority the ESP-IDF/LwIP TCP/IP task may not exist yet.
  // Starting esp_http_server here can assert inside xQueueSemaphoreTake.
  // The alarm core therefore becomes fully operational first and loop() starts
  // the HTTP APIs only after ESPHome reports an active network connection.
  this->http_start_retry_ms_ = 0;
  this->last_event_ = std::string("BOOT · restored ") + state_to_cstr(this->state_);
  this->queue_event_("SYSTEM", "Controller", std::string("BOOT_RESTORE_") + state_to_cstr(this->state_), "system");
}

void ProkopovAlarm::dump_config() {
  ESP_LOGCONFIG(TAG, "PROKOPOV Alarm Core:");
  ESP_LOGCONFIG(TAG, "  Wiegand D0 GPIO%d / D1 GPIO%d", this->d0_pin_, this->d1_pin_);
  ESP_LOGCONFIG(TAG, "  Door relay GPIO%d / Siren GPIO%d", this->door_relay_pin_, this->siren_relay_pin_);
  ESP_LOGCONFIG(TAG, "  Dahua feedback GPIO%d", this->reader_control_pin_);
  ESP_LOGCONFIG(TAG, "  Management API: %u / Shelly webhook: %u", this->management_port_, this->shelly_port_);
  ESP_LOGCONFIG(TAG, "  SD: %s", this->sd_ok_ ? "OK" : "ERROR");
  ESP_LOGCONFIG(TAG, "  Cards: %u / Zones: %u", (unsigned) this->cards_.size(), (unsigned) this->zones_.size());
}

void ProkopovAlarm::loop() {
  if (!this->led_rendered_after_boot_ && millis() > 2000) {
    this->render_led_();
    this->led_rendered_after_boot_ = true;
  }
  while (this->process_wiegand_frame_()) {}
  this->tick_reader_feedback_();
  this->tick_state_machine_();

  // Defer LwIP/httpd creation until the network stack is fully initialized.
  // This also preserves autonomous alarm operation when Wi-Fi is unavailable.
  if (!this->http_servers_started_ && millis() >= this->http_start_retry_ms_ && network::is_connected()) {
    if (this->start_http_servers_()) {
      this->http_servers_started_ = true;
      ESP_LOGI(TAG, "Network APIs started after network became ready");
    } else {
      this->http_start_retry_ms_ = millis() + 5000;
      ESP_LOGW(TAG, "Network API start failed; retrying in 5 s");
    }
  }

  const int64_t now_us = esp_timer_get_time();
  const bool wiegand_idle = this->wiegand_head_ == this->wiegand_tail_ &&
      (this->last_irq_us_ == 0 || now_us - this->last_irq_us_ >= 350000);
  if (wiegand_idle && this->state_persist_pending_) this->persist_runtime_state_();

  // Audit logging is independent from Wiegand traffic. The ISR ring buffer protects
  // reader pulses while a short FAT append is in progress.
  this->flush_one_event_();
}

void IRAM_ATTR ProkopovAlarm::d0_isr_(void *arg) {
  static_cast<ProkopovAlarm *>(arg)->push_wiegand_bit_(0);
}
void IRAM_ATTR ProkopovAlarm::d1_isr_(void *arg) {
  static_cast<ProkopovAlarm *>(arg)->push_wiegand_bit_(1);
}

void ProkopovAlarm::push_wiegand_bit_(uint8_t bit) {
  const int64_t now = esp_timer_get_time();
  if (this->last_irq_us_ && (uint64_t) (now - this->last_irq_us_) < this->glitch_us_) {
    this->filtered_total_++;
    return;
  }
  this->last_irq_us_ = now;
  uint16_t next = (this->wiegand_head_ + 1) & (WIEGAND_RING_SIZE - 1);
  if (next == this->wiegand_tail_) {
    this->overflow_total_++;
    return;
  }
  this->wiegand_bits_[this->wiegand_head_] = bit;
  this->wiegand_times_[this->wiegand_head_] = now;
  this->wiegand_head_ = next;
}

bool ProkopovAlarm::process_wiegand_frame_() {
  uint16_t head = this->wiegand_head_;
  uint16_t tail = this->wiegand_tail_;
  if (tail == head) return false;

  uint16_t idx = tail;
  int64_t prev = this->wiegand_times_[idx];
  uint64_t raw = 0;
  uint8_t bits = 0;
  uint16_t frame_end = 0xFFFF;

  while (idx != head) {
    int64_t ts = this->wiegand_times_[idx];
    if (bits && (uint64_t) (ts - prev) >= this->frame_gap_us_) {
      frame_end = idx;
      break;
    }
    raw = (raw << 1) | this->wiegand_bits_[idx];
    bits++;
    prev = ts;
    idx = (idx + 1) & (WIEGAND_RING_SIZE - 1);
  }

  if (frame_end == 0xFFFF) {
    if ((uint64_t) (esp_timer_get_time() - prev) < this->frame_gap_us_) return false;
    frame_end = idx;
  }
  this->wiegand_tail_ = frame_end;

  bool parity = this->wiegand_parity_ok_(bits, raw);
  uint32_t card = this->wiegand_card_(bits, raw);
  ESP_LOGI(TAG, "Wiegand bits=%u card=%lu parity=%s filtered=%lu overflow=%lu",
           static_cast<unsigned>(bits), static_cast<unsigned long>(card), YESNO(parity),
           static_cast<unsigned long>(this->filtered_total_),
           static_cast<unsigned long>(this->overflow_total_));
  if ((bits != 26 && bits != 34) || !parity || card == 0) return true;

  this->process_card_(card);
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
  else if (this->state_ == AlarmState::ARMED_AWAY) target = AlarmState::ARMED_AWAY;
  return this->ready_for_(target);
}

void ProkopovAlarm::set_door_locked_(bool locked, const std::string &reason) {
  this->door_locked_ = locked;
  gpio_set_level((gpio_num_t) this->door_relay_pin_, locked ? 0 : 1);
  this->queue_event_("DOOR", "Door relay", locked ? "LOCKED" : "UNLOCKED", reason);
}

void ProkopovAlarm::set_siren_(bool on, const std::string &reason) {
  this->siren_on_ = on;
  gpio_set_level((gpio_num_t) this->siren_relay_pin_, on ? 1 : 0);
  if (on && this->siren_timeout_s_ > 0) this->siren_deadline_ms_ = millis() + this->siren_timeout_s_ * 1000ULL;
  else this->siren_deadline_ms_ = 0;
  this->queue_event_("SYSTEM", "Siren", on ? "ON" : "OFF", reason);
}

void ProkopovAlarm::set_state_(AlarmState state, const std::string &reason) {
  this->state_ = state;
  this->state_persist_pending_ = true;
  this->last_event_ = std::string(state_to_cstr(state)) + (reason.empty() ? "" : " · " + reason);
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
  if (active) {
    gpio_set_level((gpio_num_t) this->reader_control_pin_, 0);
    gpio_set_direction((gpio_num_t) this->reader_control_pin_, GPIO_MODE_OUTPUT);
  } else {
    // Dahua Wiegand response line: proven safe idle state = high impedance.
    gpio_set_direction((gpio_num_t) this->reader_control_pin_, GPIO_MODE_INPUT);
  }
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

void ProkopovAlarm::process_card_(uint32_t uid) {
  RecursiveLock lock(this->state_mutex_);
  const uint64_t now = millis();
  if (this->enroll_active_) {
    this->enroll_uid_ = uid;
    this->enroll_active_ = false;
    this->queue_event_("NFC", "Reader", "ENROLL_CAPTURE", std::to_string(uid));
    this->start_reader_feedback_(ReaderFeedback::ENROLL);
    return;
  }

  auto it = this->cards_.find(uid);
  if (it == this->cards_.end() || !it->second.enabled) {
    this->queue_event_("NFC", "Reader", "DENIED", std::to_string(uid));
    this->start_reader_feedback_(ReaderFeedback::DENIED);
    return;
  }
  const CardPermissions &p = it->second;
  const std::string card_actor = p.user_name.empty() ? ("card:" + std::to_string(uid)) : p.user_name;
  this->queue_event_("NFC", "Reader", "AUTHORIZED", card_actor);

  if (this->state_ == AlarmState::ARMED_AWAY || this->state_ == AlarmState::ARMED_HOME ||
      this->state_ == AlarmState::ARMED_NIGHT || this->state_ == AlarmState::ENTRY_DELAY ||
      this->state_ == AlarmState::ALARM || this->state_ == AlarmState::PANIC ||
      this->state_ == AlarmState::SILENT_PANIC) {
    if (!p.can_disarm) {
      this->queue_event_("NFC", "Reader", "DENIED_NO_DISARM", card_actor);
      this->start_reader_feedback_(ReaderFeedback::DENIED);
      return;
    }
    this->disarm_(card_actor);
    this->start_reader_feedback_(ReaderFeedback::DISARMED);
    return;
  }

  if (!this->door_locked_) {
    if (!p.can_unlock) {
      this->queue_event_("NFC", "Reader", "DENIED_NO_UNLOCK", card_actor);
      this->start_reader_feedback_(ReaderFeedback::DENIED);
      return;
    }
    this->set_door_locked_(true, card_actor);
    this->arm_confirm_card_ = uid;
    this->arm_confirm_deadline_ms_ = now + this->arm_confirm_window_ms_;
    this->start_reader_feedback_(ReaderFeedback::LOCK);
    return;
  }

  if (this->arm_confirm_card_ == uid && this->arm_confirm_deadline_ms_ > now) {
    this->arm_confirm_card_ = 0;
    this->arm_confirm_deadline_ms_ = 0;
    if (p.can_arm && this->arm_(AlarmState::ARMED_AWAY, card_actor, false)) {
      this->start_reader_feedback_(ReaderFeedback::ARMED);
    } else {
      this->queue_event_("ALARM", "Arming interlock", "ARM_BLOCKED", card_actor);
      this->start_reader_feedback_(ReaderFeedback::ARM_BLOCKED);
    }
    return;
  }

  if (!p.can_unlock) {
    this->queue_event_("NFC", "Reader", "DENIED_NO_UNLOCK", card_actor);
    this->start_reader_feedback_(ReaderFeedback::DENIED);
    return;
  }
  this->arm_confirm_card_ = 0;
  this->arm_confirm_deadline_ms_ = 0;
  this->set_door_locked_(false, card_actor);
  this->start_reader_feedback_(ReaderFeedback::UNLOCK);
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

void ProkopovAlarm::clear_alarm_(const std::string &actor) { this->disarm_(actor); }

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
  if (cmd == "unlock" || cmd == "unlock_door") { this->set_door_locked_(false, actor); return true; }
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

bool ProkopovAlarm::init_sd_() {
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.max_freq_khz = 400;  // proven conservative speed from the MicroPython system
  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.width = 1;

  esp_vfs_fat_sdmmc_mount_config_t mount{};
  mount.format_if_mount_failed = false;
  mount.max_files = 6;
  mount.allocation_unit_size = 16 * 1024;

  sdmmc_card_t *card = nullptr;
  esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT, &host, &slot, &mount, &card);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(err));
    return false;
  }
  this->sd_card_ = card;
  this->ensure_sd_dirs_();

  FILE *f = fopen(EVENTS_FILE, "r");
  if (f) {
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
      cJSON *root = cJSON_Parse(line);
      if (root) {
        cJSON *seq = cJSON_GetObjectItem(root, "seq");
        if (cJSON_IsNumber(seq)) {
          const uint64_t value = (uint64_t) seq->valuedouble;
          this->event_seq_ = std::max<uint64_t>(this->event_seq_, value);
          this->event_persisted_seq_ = std::max<uint64_t>(this->event_persisted_seq_, value);
        }
        cJSON_Delete(root);
      }
    }
    fclose(f);
  }
  ESP_LOGI(TAG, "SD mounted; event seq=%llu", (unsigned long long) this->event_seq_);
  return true;
}

void ProkopovAlarm::ensure_sd_dirs_() {
  mkdir(BASE_DIR, 0775);
  mkdir(AUTH_DIR, 0775);
  mkdir(LOG_DIR, 0775);
  mkdir(RUNTIME_DIR, 0775);
}

bool ProkopovAlarm::read_file_(const std::string &path, std::string &out) {
  if (!this->sd_ok_) return false;
  if (this->storage_mutex_) xSemaphoreTake(this->storage_mutex_, portMAX_DELAY);
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) {
    if (this->storage_mutex_) xSemaphoreGive(this->storage_mutex_);
    return false;
  }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz < 0 || sz > 256 * 1024) { fclose(f); if (this->storage_mutex_) xSemaphoreGive(this->storage_mutex_); return false; }
  out.resize((size_t) sz);
  if (sz > 0) fread(out.data(), 1, (size_t) sz, f);
  fclose(f);
  if (this->storage_mutex_) xSemaphoreGive(this->storage_mutex_);
  return true;
}

bool ProkopovAlarm::write_file_atomic_(const std::string &path, const std::string &data) {
  if (!this->sd_ok_) return false;
  if (this->storage_mutex_) xSemaphoreTake(this->storage_mutex_, portMAX_DELAY);

  // This installation runs FatFs in short-filename-compatible mode. Keep both the
  // persistent filenames and the atomic temporary filename within FAT 8.3 limits.
  const size_t slash = path.find_last_of('/');
  const std::string dir = slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
  const std::string tmp = dir + "write.tmp";

  bool ok = false;
  int saved_errno = 0;
  const char *failed_stage = "open";

  // Remove any stale temp file first. O_APPEND is intentionally used because it is
  // already proven to work on this ESP-IDF/FatFs target, while O_TRUNC returned EINVAL.
  remove(tmp.c_str());
  errno = 0;
  int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0664);
  if (fd >= 0) {
    failed_stage = "write";
    size_t total = 0;
    while (total < data.size()) {
      const ssize_t n = write(fd, data.data() + total, data.size() - total);
      if (n <= 0) {
        saved_errno = errno ? errno : EIO;
        break;
      }
      total += (size_t) n;
    }

    if (total == data.size()) {
      failed_stage = "close";
      if (close(fd) == 0) {
        fd = -1;
        failed_stage = "replace";
        errno = 0;
        if (remove(path.c_str()) != 0 && errno != ENOENT) {
          saved_errno = errno ? errno : EIO;
        } else if (rename(tmp.c_str(), path.c_str()) == 0) {
          ok = true;
        } else {
          saved_errno = errno ? errno : EIO;
        }
      } else {
        saved_errno = errno ? errno : EIO;
        fd = -1;
      }
    }

    if (fd >= 0) close(fd);
  } else {
    saved_errno = errno ? errno : EIO;
  }

  if (!ok) {
    remove(tmp.c_str());
    ESP_LOGW(TAG, "Atomic SD write failed: path=%s stage=%s errno=%d",
             path.c_str(), failed_stage, saved_errno);
  }

  if (this->storage_mutex_) xSemaphoreGive(this->storage_mutex_);
  return ok;
}

void ProkopovAlarm::load_cards_() {
  std::string body;
  if (this->read_file_(CARDS_FILE, body) && this->apply_cards_json_(body, false) && !this->cards_.empty()) {
    ESP_LOGI(TAG, "Loaded %u cards from SD", (unsigned) this->cards_.size());
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
  std::string body;
  if (!this->read_file_(STATE_FILE, body)) return;
  cJSON *root = cJSON_Parse(body.c_str());
  if (!root) return;
  cJSON *v = cJSON_GetObjectItem(root, "state");
  std::string saved = cJSON_IsString(v) && v->valuestring ? v->valuestring : "DISARMED";
  cJSON_Delete(root);

  if (saved == "ARMED_AWAY") this->state_ = AlarmState::ARMED_AWAY;
  else if (saved == "ARMED_HOME") this->state_ = AlarmState::ARMED_HOME;
  else if (saved == "ARMED_NIGHT") this->state_ = AlarmState::ARMED_NIGHT;
  else if (saved == "ALARM" || saved == "ENTRY_DELAY") this->state_ = AlarmState::ALARM;
  else if (saved == "PANIC") this->state_ = AlarmState::PANIC;
  else if (saved == "SILENT_PANIC") this->state_ = AlarmState::SILENT_PANIC;
  else this->state_ = AlarmState::DISARMED;  // EXIT_DELAY and unknown states recover disarmed.
  ESP_LOGI(TAG, "Runtime state restored: %s", state_to_cstr(this->state_));
}

void ProkopovAlarm::persist_runtime_state_() {
  if (!this->sd_ok_) { this->state_persist_pending_ = false; return; }
  AlarmState snapshot;
  {
    RecursiveLock lock(this->state_mutex_);
    snapshot = this->state_;
  }
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "state", state_to_cstr(snapshot));
  cJSON_AddNumberToObject(root, "epoch", (double) this->current_epoch_());
  char *p = cJSON_PrintUnformatted(root);
  std::string body = p ? p : "{}";
  if (p) cJSON_free(p);
  cJSON_Delete(root);
  if (this->write_file_atomic_(STATE_FILE, body)) this->state_persist_pending_ = false;
}

void ProkopovAlarm::load_config_() {
  std::string body;
  if (this->read_file_(CONFIG_FILE, body) && this->apply_config_json_(body, false) && !this->zones_.empty()) {
    ESP_LOGI(TAG, "Loaded configuration revision %lu from SD", static_cast<unsigned long>(this->config_revision_));
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
  ESP_LOGW(TAG, "No production config on SD; loaded safe legacy fallback with unmapped remote inputs");
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
  // synchronous API contract (HTTP 200 still means the SD write succeeded)
  // without stalling the real-time alarm state machine while FatFs is busy.
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
    this->pending_event_lines_.push_back(line);
    if (this->recent_event_lines_.size() >= 256) this->recent_event_lines_.pop_front();
    this->recent_event_lines_.push_back(line);
    cJSON_free(printed);
  }
  cJSON_Delete(root);
}

void ProkopovAlarm::flush_one_event_() {
  if (!this->sd_ok_) return;

  const uint32_t now_ms = millis();
  if (this->next_event_flush_ms_ && (int32_t) (now_ms - this->next_event_flush_ms_) < 0) return;

  std::string line;
  {
    RecursiveLock lock(this->state_mutex_);
    if (this->pending_event_lines_.empty()) return;
    line = this->pending_event_lines_.front();
    this->pending_event_lines_.pop_front();
  }

  if (this->storage_mutex_ && xSemaphoreTake(this->storage_mutex_, 0) != pdTRUE) {
    RecursiveLock lock(this->state_mutex_);
    this->pending_event_lines_.push_front(line);
    return;
  }

  bool ok = false;
  int saved_errno = 0;
  const char *failed_stage = "open";

  errno = 0;
  int fd = open(EVENTS_FILE, O_WRONLY | O_CREAT | O_APPEND, 0664);
  if (fd >= 0) {
    failed_stage = "write";
    const ssize_t written = write(fd, line.data(), line.size());
    if (written == (ssize_t) line.size()) {
      failed_stage = "close";
      if (close(fd) == 0) {
        ok = true;
      } else {
        saved_errno = errno ? errno : EIO;
      }
    } else {
      saved_errno = errno ? errno : EIO;
      close(fd);
    }
  } else {
    saved_errno = errno ? errno : EIO;
  }

  if (this->storage_mutex_) xSemaphoreGive(this->storage_mutex_);

  if (ok) {
    cJSON *root = cJSON_Parse(line.c_str());
    if (root) {
      cJSON *seq = cJSON_GetObjectItem(root, "seq");
      if (cJSON_IsNumber(seq)) {
        this->event_persisted_seq_ = std::max<uint64_t>(
            this->event_persisted_seq_, (uint64_t) seq->valuedouble);
      }
      cJSON_Delete(root);
    }
    this->event_write_errno_ = 0;
    this->next_event_flush_ms_ = 0;
    return;
  }

  this->event_write_failures_++;
  this->event_write_errno_ = saved_errno;
  this->next_event_flush_ms_ = millis() + 250;
  {
    RecursiveLock lock(this->state_mutex_);
    this->pending_event_lines_.push_front(line);
    if (this->pending_event_lines_.size() > 256) this->pending_event_lines_.pop_back();
  }
  if (this->event_write_failures_ == 1 || (this->event_write_failures_ % 20) == 0) {
    ESP_LOGW(TAG, "Audit log append failed: stage=%s errno=%d pending=%u failures=%lu",
             failed_stage,
             this->event_write_errno_,
             (unsigned) this->pending_event_lines_.size(),
             (unsigned long) this->event_write_failures_);
  }
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
  return this->control_key_ == buf.data();
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
  cJSON_AddBoolToObject(root, "sd_ok", this->sd_ok_);
  cJSON_AddBoolToObject(root, "ready_away", this->ready_for_(AlarmState::ARMED_AWAY));
  cJSON_AddBoolToObject(root, "ready_home", this->ready_for_(AlarmState::ARMED_HOME));
  cJSON_AddBoolToObject(root, "ready_night", this->ready_for_(AlarmState::ARMED_NIGHT));
  cJSON_AddNumberToObject(root, "config_revision", this->config_revision_);
  cJSON_AddNumberToObject(root, "cards_revision", this->cards_revision_);
  cJSON_AddNumberToObject(root, "cards", (double) this->cards_.size());
  cJSON_AddNumberToObject(root, "wiegand_filtered", this->filtered_total_);
  cJSON_AddNumberToObject(root, "wiegand_overflow", this->overflow_total_);
  cJSON_AddStringToObject(root, "last_event", this->last_event_.c_str());
  cJSON_AddNumberToObject(root, "event_seq", (double) this->event_seq_);
  cJSON_AddNumberToObject(root, "event_persisted_seq", (double) this->event_persisted_seq_);
  cJSON_AddNumberToObject(root, "event_pending", (double) this->pending_event_lines_.size());
  cJSON_AddNumberToObject(root, "event_write_failures", (double) this->event_write_failures_);
  cJSON_AddNumberToObject(root, "event_write_errno", this->event_write_errno_);
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
    cJSON_AddItemToArray(arr, o);
  }
  char *p = cJSON_PrintUnformatted(root);
  std::string out = p ? p : "{}";
  if (p) cJSON_free(p);
  cJSON_Delete(root);
  return out;
}

std::string ProkopovAlarm::build_events_json_(uint64_t after_seq) const {
  // The management API must never scan the SD audit file.
  // SD remains the persistent autonomous audit store, while the API serves
  // the bounded in-memory recent-event ring so /status cannot be blocked.
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
  if (!self->shelly_key_.empty() && query_value_(q, "key") != self->shelly_key_) { self->send_text_(req, 403, "DENY"); return ESP_OK; }
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
  if (!self->shelly_key_.empty() && query_value_(q, "key") != self->shelly_key_) { self->send_text_(req, 403, "DENY"); return ESP_OK; }
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

std::string ProkopovAlarm::json_escape_(const std::string &value) { return value; }

}  // namespace prokopov_alarm
}  // namespace esphome
