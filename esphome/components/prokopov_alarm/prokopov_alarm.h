#pragma once

#include "esphome/core/component.h"
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

struct ZoneConfig {
  std::string id;
  std::string name;
  std::string device;
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
  bool enabled{true};
  bool can_unlock{true};
  bool can_arm{false};
  bool can_disarm{true};
};

class ProkopovAlarm : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override;

  void set_pins(int d0, int d1, int door, int siren, int reader_control);
  void set_status_light(light::LightState *state) { this->status_light_ = state; }
  void set_ports(uint16_t management_port, uint16_t shelly_port);
  void set_keys(const std::string &control_key, const std::string &shelly_key);
  void set_timings(uint32_t arm_confirm_window_ms, uint32_t valid_pulse_ms,
                   uint32_t arm_pulse_ms, uint32_t frame_gap_ms, uint32_t glitch_us);
  void add_bootstrap_card(uint32_t uid) { this->bootstrap_cards_.push_back(uid); }

  bool command(const std::string &command, const std::string &actor = "local");
  std::string alarm_state_string() const;
  std::string last_event_string() const { return this->last_event_; }
  std::string sd_status_string() const { return this->sd_ok_ ? "OK" : "ERROR"; }
  bool ready_for_current_mode() const;
  bool door_locked() const { return this->door_locked_; }
  bool siren_on() const { return this->siren_on_; }

 protected:
  static void IRAM_ATTR d0_isr_(void *arg);
  static void IRAM_ATTR d1_isr_(void *arg);
  void IRAM_ATTR push_wiegand_bit_(uint8_t bit);
  bool process_wiegand_frame_();
  static bool wiegand_parity_ok_(uint8_t bits, uint64_t raw);
  static uint32_t wiegand_card_(uint8_t bits, uint64_t raw);

  void set_door_locked_(bool locked, const std::string &reason);
  void set_siren_(bool on, const std::string &reason);
  void set_state_(AlarmState state, const std::string &reason);
  void render_led_();
  void start_reader_pulse_(uint32_t duration_ms);
  void stop_reader_pulse_if_due_();
  void process_card_(uint32_t uid);
  bool arm_(AlarmState target, const std::string &actor, bool use_exit_delay);
  void disarm_(const std::string &actor);
  void trigger_alarm_(const std::string &reason);
  void trigger_panic_(bool silent, const std::string &actor);
  void clear_alarm_(const std::string &actor);
  void tick_state_machine_();

  ZoneRule rule_for_(const ZoneConfig &zone, AlarmState mode) const;
  bool zone_should_block_(const ZoneConfig &zone, AlarmState target) const;
  bool ready_for_(AlarmState target) const;
  void set_zone_state_(const std::string &device, int channel, bool active, const std::string &source);
  void set_zone_state_by_id_(const std::string &id, bool active, const std::string &source);
  void evaluate_zone_(ZoneConfig &zone, bool became_active);

  bool init_sd_();
  void ensure_sd_dirs_();
  bool read_file_(const std::string &path, std::string &out);
  bool write_file_atomic_(const std::string &path, const std::string &data);
  void load_cards_();
  void load_config_();
  void load_runtime_state_();
  void persist_runtime_state_();
  bool apply_cards_json_(const std::string &body, bool persist);
  bool apply_config_json_(const std::string &body, bool persist);
  void queue_event_(const std::string &type, const std::string &source,
                    const std::string &event, const std::string &actor = "system");
  void flush_one_event_();
  uint64_t current_epoch_() const;

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
  static std::string json_escape_(const std::string &value);

  int d0_pin_{34};
  int d1_pin_{16};
  int door_relay_pin_{32};
  int siren_relay_pin_{33};
  int reader_control_pin_{17};
  light::LightState *status_light_{nullptr};

  uint16_t management_port_{8088};
  uint16_t shelly_port_{8081};
  std::string control_key_;
  std::string shelly_key_;

  uint32_t arm_confirm_window_ms_{5000};
  uint32_t reader_valid_pulse_ms_{120};
  uint32_t reader_arm_pulse_ms_{800};
  uint32_t frame_gap_us_{50000};
  uint32_t glitch_us_{250};

  static constexpr size_t WIEGAND_RING_SIZE = 256;
  volatile uint8_t wiegand_bits_[WIEGAND_RING_SIZE]{};
  volatile int64_t wiegand_times_[WIEGAND_RING_SIZE]{};
  volatile uint16_t wiegand_head_{0};
  volatile uint16_t wiegand_tail_{0};
  volatile int64_t last_irq_us_{0};
  volatile uint32_t filtered_total_{0};
  volatile uint32_t overflow_total_{0};

  AlarmState state_{AlarmState::DISARMED};
  AlarmState armed_mode_before_alarm_{AlarmState::DISARMED};
  AlarmState pending_arm_target_{AlarmState::ARMED_AWAY};
  std::string pending_arm_actor_;
  bool door_locked_{false};
  bool siren_on_{false};
  uint64_t state_deadline_ms_{0};
  uint64_t siren_deadline_ms_{0};

  uint32_t arm_confirm_card_{0};
  uint64_t arm_confirm_deadline_ms_{0};
  uint64_t reader_pulse_deadline_ms_{0};
  bool reader_pulse_active_{false};
  bool led_rendered_after_boot_{false};

  uint32_t exit_delay_s_{30};
  uint32_t entry_delay_s_{20};
  uint32_t siren_timeout_s_{300};
  uint32_t config_revision_{0};
  uint32_t cards_revision_{0};

  std::vector<ZoneConfig> zones_;
  std::unordered_map<std::string, uint64_t> device_heartbeat_ms_;
  uint32_t remote_device_timeout_s_{90};
  bool remote_fault_latched_{false};
  std::unordered_map<uint32_t, CardPermissions> cards_;
  std::vector<uint32_t> bootstrap_cards_;

  bool enroll_active_{false};
  uint32_t enroll_uid_{0};
  uint64_t enroll_deadline_ms_{0};

  bool sd_ok_{false};
  void *sd_card_{nullptr};
  SemaphoreHandle_t storage_mutex_{nullptr};
  mutable SemaphoreHandle_t state_mutex_{nullptr};
  std::deque<std::string> pending_event_lines_;
  std::deque<std::string> recent_event_lines_;
  uint64_t event_seq_{0};
  uint64_t event_persisted_seq_{0};
  uint32_t event_write_failures_{0};
  int event_write_errno_{0};
  uint32_t next_event_flush_ms_{0};
  std::string last_event_{"BOOT"};
  bool state_persist_pending_{false};

  httpd_handle_t management_http_{nullptr};
  httpd_handle_t shelly_http_{nullptr};
  bool http_servers_started_{false};
  uint32_t http_start_retry_ms_{0};
};

}  // namespace prokopov_alarm
}  // namespace esphome
