#pragma once
// RS-485 link between the alarm board (role: controller) and the 2.8" touch panel ESP32-2432S028 (role: panel).
//
// Frame (little endian):
//   0x7E | ver=1 | type | seq u32 | len u8 | payload[len] | mac[8] | crc16
//   mac = first 8 bytes of HMAC-SHA256(key, ver..payload)   crc16 = CRC-16/CCITT-FALSE over ver..mac
// The controller is the bus master: it sends STATUS every 500 ms (and at once on change); the panel may answer
// only inside the 150 ms after a STATUS, so the half-duplex bus never has two talkers.
// Replay protection: STATUS carries a random 32-bit session id chosen at controller boot; every panel request
// must carry that session id, the panel's per-boot random nonce and a sequence number higher than the last one
// accepted for that nonce. A nonce that has been replaced (panel reboot) is retired for the rest of the session.
#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/components/uart/uart.h"

#include <array>
#include <functional>
#include <string>
#include <vector>

namespace esphome {
namespace prokopov_alarm { class ProkopovAlarm; }
namespace prokopov_panel_link {

enum FrameType : uint8_t {
  STATUS = 0x01,       // controller -> panel: session u32, state u8, flags u8, zones u16, remaining_s u16, result u8, result_seq u32
  PIN_ACTION = 0x10,   // panel -> controller: session u32, nonce u32, action u8, pin_len u8, pin[pin_len]
  PANIC = 0x11,        // panel -> controller: session u32, nonce u32, silent u8
  PING = 0x12,         // panel -> controller: session u32, nonce u32
};

enum PanelAction : uint8_t { ARM_AWAY = 1, ARM_HOME = 2, ARM_NIGHT = 3, DISARM = 4, UNLOCK = 5, LOCK = 6 };

struct PanelStatus {
  uint8_t state{0};
  uint8_t flags{0};
  uint16_t zones{0};
  uint16_t remaining_s{0};
  uint8_t result{0};
  uint32_t result_seq{0};
  bool online{false};
};

class PanelLink : public Component, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_key(const std::string &hex_key);
  void set_controller(prokopov_alarm::ProkopovAlarm *alarm) { this->alarm_ = alarm; this->is_controller_ = true; }
  void set_status_interval(uint32_t ms) { this->status_interval_ms_ = ms; }

  // panel role
  void send_pin(uint8_t action, const std::string &pin);
  void send_panic(bool silent);
  const PanelStatus &status() const { return this->status_; }
  void add_on_status_callback(std::function<void(const PanelStatus &)> cb) { this->status_cb_.add(std::move(cb)); }

  uint32_t frames_ok() const { return this->frames_ok_; }
  uint32_t frames_bad() const { return this->frames_bad_; }

 protected:
  void send_frame_(uint8_t type, const uint8_t *payload, uint8_t len);
  void handle_frame_(uint8_t type, uint32_t seq, const uint8_t *p, uint8_t len);
  void controller_send_status_();
  bool parse_();
  void mac_(const uint8_t *data, size_t len, uint8_t out[8]) const;
  static uint16_t crc16_(const uint8_t *data, size_t len);

  prokopov_alarm::ProkopovAlarm *alarm_{nullptr};
  bool is_controller_{false};
  std::array<uint8_t, 32> key_{};
  bool key_ok_{false};
  uint32_t status_interval_ms_{500};
  uint32_t last_status_ms_{0};
  uint32_t last_rx_ms_{0};
  uint32_t session_{0};
  uint32_t tx_seq_{0};
  uint32_t rx_last_seq_{0};
  uint32_t panel_nonce_{0};               // panel: random per boot; controller: current peer nonce
  std::vector<uint32_t> retired_nonces_;  // controller: nonces seen earlier in this session are never accepted again
  uint32_t status_last_seq_{0};           // panel: STATUS frames must be fresh too
  uint8_t last_result_{0};
  uint32_t last_result_seq_{0};
  uint64_t last_snapshot_{0};
  std::vector<uint8_t> rx_;
  PanelStatus status_{};
  CallbackManager<void(const PanelStatus &)> status_cb_;
  // panel side: a request waits for the next STATUS (bus slot)
  std::vector<uint8_t> pending_payload_;
  uint8_t pending_type_{0};
  uint32_t frames_ok_{0};
  uint32_t frames_bad_{0};
};

class StatusTrigger : public Trigger<PanelStatus> {
 public:
  explicit StatusTrigger(PanelLink *parent) {
    parent->add_on_status_callback([this](const PanelStatus &s) { this->trigger(s); });
  }
};

}  // namespace prokopov_panel_link
}  // namespace esphome
