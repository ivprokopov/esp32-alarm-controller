#include "prokopov_alarm_radio.h"

#include "esphome/core/log.h"
#include "esphome/core/hal.h"

#include <cstring>

extern "C" {
#include "esp_err.h"
#include "esp_wifi.h"
}

namespace esphome {
namespace prokopov_alarm_radio {

using prokopov_espnow::MessageType;
using prokopov_espnow::NodeRole;
using prokopov_espnow::Packet;

static const char *const TAG = "prokopov_alarm_radio";
static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

ProkopovAlarmRadio *ProkopovAlarmRadio::instance_ = nullptr;

void ProkopovAlarmRadio::setup() {
  instance_ = this;

  this->keys_ok_ =
      prokopov_espnow::parse_hex_exact(this->pmk_hex_, this->pmk_, sizeof(this->pmk_)) &&
      prokopov_espnow::parse_hex_exact(this->lmk_hex_, this->lmk_, sizeof(this->lmk_)) &&
      prokopov_espnow::parse_hex_exact(this->auth_hex_, this->auth_key_, sizeof(this->auth_key_));

  if (!this->keys_ok_) {
    ESP_LOGE(TAG, "Invalid crypto key length/hex. PMK=32 hex, LMK=32 hex, AUTH=64 hex required.");
    this->mark_failed();
    return;
  }

  this->init_retry_at_ = millis();
}

void ProkopovAlarmRadio::dump_config() {
  ESP_LOGCONFIG(TAG, "PROKOPOV ESP-NOW Alarm Hub:");
  ESP_LOGCONFIG(TAG, "  ESP-NOW: %s", this->espnow_ready_ ? "READY" : "WAITING");
  ESP_LOGCONFIG(TAG, "  Reader peer: %s", this->paired_ ? "PAIRED" : "NOT PAIRED");
  ESP_LOGCONFIG(TAG, "  HELLO interval: %u ms", (unsigned) this->hello_interval_ms_);
  ESP_LOGCONFIG(TAG, "  STATE interval: %u ms", (unsigned) this->state_interval_ms_);
}

void ProkopovAlarmRadio::loop() {
  const uint32_t now = millis();

  if (!this->espnow_ready_) {
    if ((int32_t) (now - this->init_retry_at_) >= 0) {
      if (this->init_espnow_()) {
        this->next_hello_at_ = now;
        this->next_state_at_ = now;
      } else {
        this->init_retry_at_ = now + 2000;
      }
    }
    return;
  }

  if ((int32_t) (now - this->next_hello_at_) >= 0) {
    this->send_hello_();
    this->next_hello_at_ = now + this->hello_interval_ms_;
  }

  if (this->paired_ && (int32_t) (now - this->next_state_at_) >= 0) {
    this->send_state_();
    this->next_state_at_ = now + this->state_interval_ms_;
  }
}

bool ProkopovAlarmRadio::init_espnow_() {
  if (esp_wifi_get_mac(WIFI_IF_STA, this->local_mac_) != ESP_OK) {
    ESP_LOGW(TAG, "Wi-Fi STA not ready yet; ESP-NOW init postponed");
    return false;
  }

  esp_err_t err = esp_now_init();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "esp_now_init failed: %s", esp_err_to_name(err));
    return false;
  }

  err = esp_now_set_pmk(this->pmk_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_now_set_pmk failed: %s", esp_err_to_name(err));
    return false;
  }

  err = esp_now_register_recv_cb(&ProkopovAlarmRadio::recv_cb_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_now_register_recv_cb failed: %s", esp_err_to_name(err));
    return false;
  }

  if (!this->add_broadcast_peer_()) return false;

  this->espnow_ready_ = true;

  ESP_LOGI(TAG, "ESP-NOW ready. Hub MAC %02X:%02X:%02X:%02X:%02X:%02X",
           this->local_mac_[0], this->local_mac_[1], this->local_mac_[2],
           this->local_mac_[3], this->local_mac_[4], this->local_mac_[5]);
  return true;
}

bool ProkopovAlarmRadio::add_broadcast_peer_() {
  if (esp_now_is_peer_exist(BROADCAST_MAC)) return true;

  esp_now_peer_info_t peer{};
  std::memcpy(peer.peer_addr, BROADCAST_MAC, 6);
  peer.channel = 0;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;

  esp_err_t err = esp_now_add_peer(&peer);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Cannot add ESP-NOW broadcast peer: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

bool ProkopovAlarmRadio::ensure_encrypted_peer_(const uint8_t mac[6]) {
  if (this->paired_ && std::memcmp(this->peer_mac_, mac, 6) == 0 && esp_now_is_peer_exist(mac)) {
    return true;
  }

  if (this->paired_ && std::memcmp(this->peer_mac_, mac, 6) != 0 && esp_now_is_peer_exist(this->peer_mac_)) {
    esp_now_del_peer(this->peer_mac_);
  }

  if (esp_now_is_peer_exist(mac)) esp_now_del_peer(mac);

  esp_now_peer_info_t peer{};
  std::memcpy(peer.peer_addr, mac, 6);
  peer.channel = 0;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = true;
  std::memcpy(peer.lmk, this->lmk_, sizeof(peer.lmk));

  esp_err_t err = esp_now_add_peer(&peer);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Cannot add encrypted reader peer: %s", esp_err_to_name(err));
    return false;
  }

  std::memcpy(this->peer_mac_, mac, 6);
  this->paired_ = true;

  ESP_LOGI(TAG, "Reader paired: %02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return true;
}

bool ProkopovAlarmRadio::send_packet_(const uint8_t dst[6], Packet packet) {
  if (!this->espnow_ready_) return false;

  std::memcpy(packet.sender_mac, this->local_mac_, 6);
  if (!prokopov_espnow::sign_packet(packet, this->auth_key_)) return false;

  esp_err_t err = esp_now_send(dst, reinterpret_cast<const uint8_t *>(&packet), sizeof(packet));
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "esp_now_send failed: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

void ProkopovAlarmRadio::send_hello_() {
  Packet p{};
  p.type = static_cast<uint8_t>(MessageType::HELLO);
  p.role = static_cast<uint8_t>(NodeRole::HUB);
  p.seq = ++this->state_seq_;
  this->send_packet_(BROADCAST_MAC, p);
}

void ProkopovAlarmRadio::send_state_() {
  if (!this->paired_ || this->alarm_ == nullptr) return;

  Packet p{};
  p.type = static_cast<uint8_t>(MessageType::STATE);
  p.role = static_cast<uint8_t>(NodeRole::HUB);
  p.seq = ++this->state_seq_;
  p.state = this->alarm_->remote_reader_state_code();
  p.aux = this->alarm_->arm_confirm_remaining_ms();
  this->send_packet_(this->peer_mac_, p);
}

void ProkopovAlarmRadio::send_result_(uint32_t seq, uint8_t result_code,
                                      uint8_t state_code, uint32_t wait_ms) {
  if (!this->paired_) return;

  Packet p{};
  p.type = static_cast<uint8_t>(MessageType::RESULT);
  p.role = static_cast<uint8_t>(NodeRole::HUB);
  p.seq = seq;
  p.value = result_code;
  p.state = state_code;
  p.aux = wait_ms;
  this->send_packet_(this->peer_mac_, p);
}

void ProkopovAlarmRadio::recv_cb_(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (instance_ == nullptr || info == nullptr) return;
  instance_->handle_recv_(info->src_addr, data, len);
}

void ProkopovAlarmRadio::handle_recv_(const uint8_t src[6], const uint8_t *data, int len) {
  if (data == nullptr || len != static_cast<int>(sizeof(Packet))) return;

  Packet p{};
  std::memcpy(&p, data, sizeof(p));

  if (!prokopov_espnow::packet_header_valid(p)) return;
  if (std::memcmp(p.sender_mac, src, 6) != 0) return;
  if (!prokopov_espnow::verify_packet(p, this->auth_key_)) {
    ESP_LOGW(TAG, "Rejected ESP-NOW packet with invalid authentication tag");
    return;
  }
  if (p.role != static_cast<uint8_t>(NodeRole::READER)) return;

  const auto type = static_cast<MessageType>(p.type);

  if (type == MessageType::HELLO) {
    if (this->ensure_encrypted_peer_(src)) this->send_state_();
    return;
  }

  if (!this->paired_ || std::memcmp(src, this->peer_mac_, 6) != 0) return;

  if (type != MessageType::CARD || this->alarm_ == nullptr) return;

  const uint32_t uid = p.value;
  if (uid == 0) return;

  // Reader retries the same request when the RESULT packet is lost. Re-send the
  // cached result without executing the card action a second time.
  if (this->have_cached_result_ && p.seq == this->last_card_seq_ && uid == this->last_card_uid_) {
    this->send_result_(p.seq, this->last_result_code_, this->last_result_state_, this->last_result_wait_ms_);
    return;
  }

  const auto result = this->alarm_->process_remote_card(uid);
  const uint8_t result_code = static_cast<uint8_t>(result);
  const uint8_t state_code = this->alarm_->remote_reader_state_code();
  const uint32_t wait_ms = this->alarm_->arm_confirm_remaining_ms();

  this->last_card_seq_ = p.seq;
  this->last_card_uid_ = uid;
  this->last_result_code_ = result_code;
  this->last_result_state_ = state_code;
  this->last_result_wait_ms_ = wait_ms;
  this->have_cached_result_ = true;

  ESP_LOGI(TAG, "Remote card uid=%lu seq=%lu result=%u state=%u wait=%lu ms",
           (unsigned long) uid, (unsigned long) p.seq, (unsigned) result_code,
           (unsigned) state_code, (unsigned long) wait_ms);

  this->send_result_(p.seq, result_code, state_code, wait_ms);
}

}  // namespace prokopov_alarm_radio
}  // namespace esphome
