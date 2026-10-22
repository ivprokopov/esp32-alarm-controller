#pragma once
// PROKOPOV Alarm core — v3 (main board Rev A, ESP32-S3 DevKit).
//
// What changed against v1/v2 (Olimex + SD card):
//   * two local Wiegand readers (outside / inside) through 6N137 optocouplers
//   * lock relay with three modes (auto / always locked / always unlocked) and lock type (maglock / strike)
//   * siren on a high-side MOSFET, status outputs for the Shelly Plus Uni (ARMED / ALARM)
//   * runtime state + audit journal in optional I2C FRAM (MB85RC256V, 0x50); without it: state in flash, journal in RAM + server
//   * cards and configuration in a FAT file system on the internal flash (wear-levelled partition "storage")
//   * external hardware watchdog (NE555, ~7 s) kicked from loop()
//   * brute-force protection: 5 rejected cards/PINs -> 5 min lockout of that input
//   * local zones / tamper / mains / siren-line come from ESPHome binary_sensors (see example YAML)

#include "esphome/core/component.h"
#include "esphome/components/i2c/i2c.h"
#include "esphome/components/light/light_state.h"

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_http_server.h"

namespace esphome {
namespace prokopov_alarm {

enum class AlarmState : uint8_t {
  DISARMED = 0,
  EXIT_DELAY,
  ARMED_AWAY,
  ARMED_HOME,
  ARMED_NIGHT,
  ENTRY_DELAY,
  ALARM,
  PANIC,
  SILENT_PANIC,
};

enum class ZoneRule : uint8_t {
  IGNORE = 0,
  INSTANT,
  DELAYED,
  FOLLOWER,
  ALWAYS,
};

enum class ReaderFeedback : uint8_t {
  NONE = 0,
  UNLOCK,
  LOCK,
  ARMED,
  DISARMED,
  DENIED,
  ARM_BLOCKED,
  ENROLL,
  LOCKED_OUT,
};

// Lock output policy. AUTO follows the alarm (locked when armed, card toggles when disarmed).
enum class LockMode : uint8_t { AUTO = 0, ALWAYS_LOCKED = 1, ALWAYS_UNLOCKED = 2 };
// MAGLOCK: relay energised = locked (fail-safe). STRIKE: relay energised = unlocked (fail-secure).
enum class LockType : uint8_t { MAGLOCK = 0, STRIKE = 1 };

// Numeric values intentionally match prokopov_espnow::ResultCode.
enum class RemoteReaderResult : uint8_t {
  NONE = 0,
  ACCESS_GRANTED = 1,
  LOCKED_ARM_WAIT = 2,
  ARMED = 3,
  DISARMED_UNLOCKED = 4,
  DENIED = 5,
  ARM_BLOCKED = 6,
  ENROLL_CAPTURED = 7,
  LOCKED_OUT = 8,
};

struct ZoneConfig {
  std::string id;
  std::string name;
  std::string device;  // "local" = board input (channel 1..5 = Z1..Z5, 6 = door contact)
  int channel{-1};
  bool enabled{true};
  bool inverted{false};
  bool active{false};
  ZoneRule away{ZoneRule::IGNORE};
  ZoneRule home{ZoneRule::IGNORE};
  ZoneRule night{ZoneRule::IGNORE};
};

struct CardPermissions {
  std::string user_name;
  std::string pin_hash;  // optional: lowercase hex SHA-256 of the user's PIN (keypad on the RS-485 panel)
  bool enabled{true};
  bool can_unlock{true};
  bool can_arm{false};
  bool can_disarm{true};
};

// One Wiegand port = one reader. ISR pushes bits into its own ring.
struct WiegandPort {
  static constexpr size_t RING = 256;
  int d0_pin{-1};
  int d1_pin{-1};
  const char *name{"reader"};
  volatile uint8_t bits[RING]{};
  volatile int64_t times[RING]{};
  volatile uint16_t head{0};
  volatile uint16_t tail{0};
  volatile int64_t last_irq_us{0};
  volatile uint32_t filtered{0};
  volatile uint32_t overflow{0};
  uint32_t glitch_us{250};
  // brute-force protection
  uint8_t fails{0};
  uint64_t fail_window_start_ms{0};
  uint64_t lockout_until_ms{0};
};

class ProkopovAlarm : public Component, public i2c::I2CDevice {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override;

  // ---- configuration (from __init__.py)
  void set_reader_pins(int index, int d0, int d1);
  void set_output_pins(int lock, int siren, int reader_led, int reader_beep, int st_armed, int st_alarm, int wdt_kick);
  void set_lock_type(int t) { this->lock_type_ = static_cast<LockType>(t); }
  void set_status_light(light::LightState *state) { this->status_light_ = state; }
  void set_ports(uint16_t management_port, uint16_t shelly_port);
  void set_keys(const std::string &control_key, const std::string &shelly_key);
  void set_timings(uint32_t arm_confirm_window_ms, uint32_t valid_pulse_ms, uint32_t arm_pulse_ms,
                   uint32_t frame_gap_ms, uint32_t glitch_us);
  void set_lockout(uint8_t attempts, uint32_t window_s, uint32_t lockout_s) {
    this->lockout_attempts_ = attempts; this->lockout_window_ms_ = window_s * 1000ULL; this->lockout_ms_ = lockout_s * 1000ULL;
  }
  void set_use_fram(bool v) { this->use_fram_ = v; }
  void add_bootstrap_card(uint32_t uid) { this->bootstrap_cards_.push_back(uid); }

  // ---- inputs from ESPHome binary_sensors / sensors (YAML lambdas)
  void set_local_input(int channel, bool active);      // Z1..Z5 = 1..5, door contact = 6
  void uni_command(bool arm);                          // pulse from Shelly Plus Uni OUT1 (arm) / OUT2 (disarm)
  void set_tamper(bool open);
  void set_mains(bool present);
  void set_siren_line_fault(bool fault);
  void set_battery(float volts, bool test_result_valid, bool test_passed);

  // ---- API used by the RS-485 panel link and the ESP-NOW reader link
  bool command(const std::string &command, const std::string &actor = "local");
  RemoteReaderResult process_remote_card(uint32_t uid);
  RemoteReaderResult process_pin(const std::string &pin, const std::string &action, const std::string &source);
  uint8_t remote_reader_state_code() const;
  uint32_t arm_confirm_remaining_ms() const;
  std::string alarm_state_string() const;
  std::string last_event_string() const { return this->last_event_; }
  std::string storage_status_string() const;
  std::string lock_mode_string() const;
  bool ready_for_current_mode() const;
  bool door_locked() const { return this->door_locked_; }
  bool lock_output_locked() const;
  bool siren_on() const { return this->siren_on_; }
  uint32_t state_remaining_ms() const;
  // compact status for the panel: state, ready flags, door, siren, zones bitmap
  void panel_snapshot(uint8_t &state, uint8_t &flags, uint16_t &zones_active, uint16_t &remaining_s) const;

 protected:
  static void IRAM_ATTR d0_isr_(void *arg);
  static void IRAM_ATTR d1_isr_(void *arg);
  static void IRAM_ATTR push_bit_(WiegandPort *p, uint8_t bit);
  bool process_wiegand_frame_(WiegandPort &p);
  static bool wiegand_parity_ok_(uint8_t bits, uint64_t raw);
  static uint32_t wiegand_card_(uint8_t bits, uint64_t raw);

  void apply_lock_output_();
  void set_door_locked_(bool locked, const std::string &reason);
  void set_lock_mode_(LockMode mode, const std::string &actor);
  void set_siren_(bool on, const std::string &reason);
  void set_state_(AlarmState state, const std::string &reason);
  void apply_status_outputs_();
  void render_led_();
  void set_reader_control_(bool active);
  void start_reader_feedback_(ReaderFeedback feedback);
  void tick_reader_feedback_();
  void process_card_(WiegandPort &port, uint32_t uid);
  RemoteReaderResult process_card_action_(uint32_t uid, bool local_feedback, const std::string &source);
  RemoteReaderResult authorize_(const CardPermissions &p, uint32_t uid, const std::string &actor, bool local_feedback,
                                const std::string &action);
  bool locked_out_(WiegandPort *p) const;
  void register_failure_(WiegandPort *p, const std::string &source);
  bool arm_(AlarmState target, const std::string &actor, bool use_exit_delay);
  void disarm_(const std::string &actor);
  void trigger_alarm_(const std::string &reason);
  void trigger_panic_(bool silent, const std::string &actor);
  void tick_state_machine_();
  void kick_watchdog_();

  ZoneRule rule_for_(const ZoneConfig &zone, AlarmState mode) const;
  bool zone_should_block_(const ZoneConfig &zone, AlarmState target) const;
  bool ready_for_(AlarmState target) const;
  void set_zone_state_(const std::string &device, int channel, bool active, const std::string &source);
  void set_zone_state_by_id_(const std::string &id, bool active, const std::string &source);
  void evaluate_zone_(ZoneConfig &zone, bool became_active);

  // ---- storage: flash FAT (cards, config) + FRAM (runtime state, journal)
  bool init_flash_();
  bool read_file_(const std::string &path, std::string &out);
  bool write_file_atomic_(const std::string &path, const std::string &data);
  bool fram_probe_();
  bool fram_read_(uint16_t addr, uint8_t *data, size_t len);
  bool fram_write_(uint16_t addr, const uint8_t *data, size_t len);
  void fram_load_journal_();
  bool fram_append_event_(uint64_t seq, const std::string &line);
  void load_cards_();
  void load_config_();
  void load_runtime_state_();
  void persist_runtime_state_();
  bool apply_cards_json_(const std::string &body, bool persist);
  bool apply_config_json_(const std::string &body, bool persist);
  void queue_event_(const std::string &type, const std::string &source, const std::string &event,
                    const std::string &actor = "system");
  void flush_one_event_();
  uint64_t current_epoch_() const;

  // ---- HTTP APIs (unchanged contract with alarm-server)
  bool start_http_servers_();
  bool start_http_server_(httpd_handle_t *handle, uint16_t port, bool shelly);
  static esp_err_t status_handler_(httpd_req_t *req);
  static esp_err_t command_handler_(httpd_req_t *req);
  static esp_err_t config_handler_(httpd_req_t *req);
  static esp_err_t cards_handler_(httpd_req_t *req);
  static esp_err_t reconcile_handler_(httpd_req_t *req);
  static esp_err_t card_scan_begin_handler_(httpd_req_t *req);
  static esp_err_t card_scan_status_handler_(httpd_req_t *req);
  static esp_err_t card_scan_stop_handler_(httpd_req_t *req);
  static esp_err_t events_handler_(httpd_req_t *req);
  static esp_err_t shelly_handler_(httpd_req_t *req);
  static esp_err_t heartbeat_handler_(httpd_req_t *req);
  static esp_err_t health_handler_(httpd_req_t *req);

  bool management_authorized_(httpd_req_t *req) const;
  bool read_request_body_(httpd_req_t *req, std::string &body) const;
  void send_json_(httpd_req_t *req, int code, const std::string &body) const;
  void send_text_(httpd_req_t *req, int code, const std::string &body) const;
  std::string build_status_json_() const;
  std::string build_cards_json_() const;
  std::string build_events_json_(uint64_t after_seq) const;
  static std::string query_value_(const std::string &query, const std::string &key);
  static std::string url_decode_(const std::string &value);
  static bool const_time_equal_(const std::string &a, const std::string &b);
  static std::string sha256_hex_(const std::string &data);

  // ---- hardware
  std::array<WiegandPort, 2> readers_{};
  int lock_pin_{40};
  int siren_pin_{41};
  int reader_led_pin_{14};
  int reader_beep_pin_{21};
  int st_armed_pin_{39};
  int st_alarm_pin_{48};
  int wdt_pin_{10};
  LockType lock_type_{LockType::MAGLOCK};
  light::LightState *status_light_{nullptr};
  bool wdt_level_{false};
  uint32_t wdt_last_ms_{0};
  void *wdt_boot_timer_{nullptr};      // esp_timer kicking the watchdog until the main loop has run for 15 s
  uint32_t first_loop_ms_{0};

  uint16_t management_port_{8088};
  uint16_t shelly_port_{8081};
  std::string control_key_;
  std::string shelly_key_;

  uint32_t arm_confirm_window_ms_{5000};
  uint32_t reader_valid_pulse_ms_{120};
  uint32_t reader_arm_pulse_ms_{800};
  uint32_t frame_gap_us_{50000};
  uint32_t glitch_us_{250};
  uint8_t lockout_attempts_{5};
  uint64_t lockout_window_ms_{300000};
  uint64_t lockout_ms_{300000};
  WiegandPort panel_port_{};   // brute-force counters for the keypad / remote readers

  AlarmState state_{AlarmState::DISARMED};
  AlarmState armed_mode_before_alarm_{AlarmState::DISARMED};
  AlarmState pending_arm_target_{AlarmState::ARMED_AWAY};
  std::string pending_arm_actor_;
  LockMode lock_mode_{LockMode::AUTO};
  bool door_locked_{false};
  bool siren_on_{false};
  uint64_t state_deadline_ms_{0};
  uint64_t siren_deadline_ms_{0};

  // board supervision
  bool tamper_open_{false};
  bool mains_present_{true};
  bool siren_line_fault_{false};
  float battery_v_{0};
  bool battery_low_latched_{false};
  uint64_t uni_last_ms_{0};

  uint32_t arm_confirm_card_{0};
  uint64_t arm_confirm_deadline_ms_{0};
  std::array<uint16_t, 8> reader_feedback_steps_{};
  uint8_t reader_feedback_len_{0};
  uint8_t reader_feedback_index_{0};
  uint32_t reader_feedback_deadline_ms_{0};
  bool reader_feedback_active_{false};
  bool reader_feedback_waiting_start_{false};
  bool led_rendered_after_boot_{false};

  uint32_t exit_delay_s_{30};
  uint32_t entry_delay_s_{20};
  uint32_t siren_timeout_s_{300};
  uint32_t config_revision_{0};
  uint32_t cards_revision_{0};

  std::vector<ZoneConfig> zones_;
  std::unordered_map<std::string, uint64_t> device_heartbeat_ms_;
  uint32_t remote_device_timeout_s_{15};
  bool remote_fault_latched_{false};
  std::unordered_map<uint32_t, CardPermissions> cards_;
  std::vector<uint32_t> bootstrap_cards_;

  bool enroll_active_{false};
  uint32_t enroll_uid_{0};
  uint64_t enroll_deadline_ms_{0};

  bool flash_ok_{false};
  bool use_fram_{true};
  bool fram_ok_{false};
  uint16_t fram_next_slot_{0};
  uint32_t fram_runtime_gen_{0};
  SemaphoreHandle_t storage_mutex_{nullptr};
  mutable SemaphoreHandle_t state_mutex_{nullptr};
  std::deque<std::pair<uint64_t, std::string>> pending_event_lines_;
  std::deque<std::string> recent_event_lines_;
  uint64_t event_seq_{0};
  uint64_t event_persisted_seq_{0};
  uint32_t event_write_failures_{0};
  std::string last_event_{"BOOT"};
  bool state_persist_pending_{false};

  httpd_handle_t management_http_{nullptr};
  httpd_handle_t shelly_http_{nullptr};
  bool http_servers_started_{false};
  uint32_t http_start_retry_ms_{0};
};

}  // namespace prokopov_alarm
}  // namespace esphome
