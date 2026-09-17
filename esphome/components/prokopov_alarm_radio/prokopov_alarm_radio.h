#pragma once

#include "esphome/core/component.h"
#include "esphome/components/prokopov_alarm/prokopov_alarm.h"
#include "esphome/components/prokopov_espnow/protocol.h"

#include <cstdint>
#include <string>

extern "C" {
#include "esp_now.h"
}

namespace esphome {
namespace prokopov_alarm_radio {

class ProkopovAlarmRadio : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  void set_alarm(prokopov_alarm::ProkopovAlarm *alarm) { this->alarm_ = alarm; }
  void set_crypto(const std::string &pmk_hex, const std::string &lmk_hex,
                  const std::string &auth_hex) {
    this->pmk_hex_ = pmk_hex;
    this->lmk_hex_ = lmk_hex;
    this->auth_hex_ = auth_hex;
  }
  void set_intervals(uint32_t hello_ms, uint32_t state_ms) {
    this->hello_interval_ms_ = hello_ms;
    this->state_interval_ms_ = state_ms;
  }

  bool paired() const { return this->paired_; }

 protected:
  bool init_espnow_();
  bool add_broadcast_peer_();
  bool ensure_encrypted_peer_(const uint8_t mac[6]);
  void send_hello_();
  void send_state_();
  void send_result_(uint32_t seq, uint8_t result_code, uint8_t state_code, uint32_t wait_ms);
  bool send_packet_(const uint8_t dst[6], prokopov_espnow::Packet packet);
  void handle_recv_(const uint8_t src[6], const uint8_t *data, int len);

  static void recv_cb_(const esp_now_recv_info_t *info, const uint8_t *data, int len);
  static ProkopovAlarmRadio *instance_;

  prokopov_alarm::ProkopovAlarm *alarm_{nullptr};

  std::string pmk_hex_;
  std::string lmk_hex_;
  std::string auth_hex_;
  uint8_t pmk_[prokopov_espnow::ESPNOW_KEY_SIZE]{};
  uint8_t lmk_[prokopov_espnow::ESPNOW_KEY_SIZE]{};
  uint8_t auth_key_[prokopov_espnow::AUTH_KEY_SIZE]{};

  bool keys_ok_{false};
  bool espnow_ready_{false};
  bool paired_{false};
  uint8_t local_mac_[6]{};
  uint8_t peer_mac_[6]{};

  uint32_t init_retry_at_{0};
  uint32_t next_hello_at_{0};
  uint32_t next_state_at_{0};
  uint32_t hello_interval_ms_{3000};
  uint32_t state_interval_ms_{1000};
  uint32_t state_seq_{0};

  bool have_cached_result_{false};
  uint32_t last_card_seq_{0};
  uint32_t last_card_uid_{0};
  uint8_t last_result_code_{0};
  uint8_t last_result_state_{0};
  uint32_t last_result_wait_ms_{0};
};

}  // namespace prokopov_alarm_radio
}  // namespace esphome
