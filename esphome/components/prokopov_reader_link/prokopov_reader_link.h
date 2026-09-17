#pragma once

#include "esphome/core/component.h"
#include "esphome/components/prokopov_espnow/protocol.h"

#include <cstdint>
#include <string>

extern "C" {
#include "esp_now.h"
}

namespace esphome {
namespace prokopov_reader_link {

class ProkopovReaderLink : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  void set_crypto(const std::string &pmk_hex, const std::string &lmk_hex,
                  const std::string &auth_hex) {
    this->pmk_hex_ = pmk_hex;
    this->lmk_hex_ = lmk_hex;
    this->auth_hex_ = auth_hex;
  }
  void set_timings(uint32_t hello_ms, uint32_t retry_ms, uint8_t max_attempts,
                   uint32_t response_timeout_ms) {
    this->hello_interval_ms_ = hello_ms;
    this->retry_interval_ms_ = retry_ms;
    this->max_attempts_ = max_attempts;
    this->response_timeout_ms_ = response_timeout_ms;
  }

  bool send_card(uint32_t uid);
  bool take_result(uint8_t &result, uint8_t &state, uint32_t &wait_ms);
  bool take_state(uint8_t &state, uint32_t &wait_ms);

  bool paired() const { return this->paired_; }
  bool pending() const { return this->pending_; }
  uint8_t last_state() const { return this->last_state_; }

 protected:
  bool init_espnow_();
  bool add_broadcast_peer_();
  bool ensure_encrypted_peer_(const uint8_t mac[6]);
  bool send_packet_(const uint8_t dst[6], prokopov_espnow::Packet packet);
  void send_hello_();
  void send_pending_card_();
  void finish_no_response_();
  void handle_recv_(const uint8_t src[6], const uint8_t *data, int len);

  static void recv_cb_(const esp_now_recv_info_t *info, const uint8_t *data, int len);
  static ProkopovReaderLink *instance_;

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
  uint32_t hello_interval_ms_{3000};

  bool pending_{false};
  uint32_t pending_uid_{0};
  uint32_t pending_seq_{0};
  uint8_t pending_attempts_{0};
  uint32_t next_retry_at_{0};
  uint32_t pending_deadline_{0};
  uint32_t retry_interval_ms_{250};
  uint8_t max_attempts_{4};
  uint32_t response_timeout_ms_{1400};
  uint32_t seq_counter_{0};

  bool result_ready_{false};
  uint8_t result_code_{0};
  uint8_t result_state_{0};
  uint32_t result_wait_ms_{0};

  bool state_ready_{false};
  uint8_t last_state_{0};
  uint32_t last_wait_ms_{0};
};

}  // namespace prokopov_reader_link
}  // namespace esphome
