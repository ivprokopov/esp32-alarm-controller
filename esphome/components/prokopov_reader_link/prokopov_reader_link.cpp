#include "prokopov_reader_link.h"

#include "esphome/core/log.h"
#include "esphome/core/hal.h"

#include <cstring>

extern "C" {
#include "esp_err.h"
#include "esp_system.h"
#include "esp_wifi.h"
}

namespace esphome {
namespace prokopov_reader_link {

using prokopov_espnow::MessageType;
using prokopov_espnow::NodeRole;
using prokopov_espnow::Packet;
using prokopov_espnow::ResultCode;

static const char *const TAG = "prokopov_reader_link";
static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

ProkopovReaderLink *ProkopovReaderLink::instance_ = nullptr;

void ProkopovReaderLink::setup() {
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

  this->seq_counter_ = esp_random();
  if (this->seq_counter_ == 0) this->seq_counter_ = 1;
  this->init_retry_at_ = millis();
}

void ProkopovReaderLink::dump_config() {
  ESP_LOGCONFIG(TAG, "PROKOPOV ESP-NOW Reader Link:");
  ESP_LOGCONFIG(TAG, "  ESP-NOW: %s", this->espnow_ready_ ? "READY" : "WAITING");
  ESP_LOGCONFIG(TAG, "  Alarm hub: %s", this->paired_ ? "PAIRED" : "NOT PAIRED");
  ESP_LOGCONFIG(TAG, "  Retry: %u ms x %u, timeout %u ms",
                (unsigned) this->retry_interval_ms_, (unsigned) this->max_attempts_,
                (unsigned) this->response_timeout_ms_);
}

void ProkopovReaderLink::loop() {
  const uint32_t now = millis();

  if (!this->espnow_ready_) {
    if ((int32_t) (now - this->init_retry_at_) >= 0) {
      if (this->init_espnow_()) {
        this->next_hello_at_ = now;
      } else {
        this->init_retry_at_ = now + 2000;
      }
    }
  } else if ((int32_t) (now - this->next_hello_at_) >= 0) {
    this->send_hello_();
    this->next_hello_at_ = now + this->hello_interval_ms_;
  }

  if (!this->pending_) return;

  if ((int32_t) (now - this->pending_deadline_) >= 0) {
    this->finish_no_response_();
    return;
  }

  if (this->paired_ && this->pending_attempts_ < this->max_attempts_ &&
      (int32_t) (now - this->next_retry_at_) >= 0) {
    this->send_pending_card_();
  }
}

bool ProkopovReaderLink::init_espnow_() {
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

  err = esp_now_register_recv_cb(&ProkopovReaderLink::recv_cb_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_now_register_recv_cb failed: %s", esp_err_to_name(err));
    return false;
  }

  if (!this->add_broadcast_peer_()) return false;

  this->espnow_ready_ = true;

  ESP_LOGI(TAG, "ESP-NOW ready. Reader MAC %02X:%02X:%02X:%02X:%02X:%02X",
           this->local_mac_[0], this->local_mac_[1], this->local_mac_[2],
           this->local_mac_[3], this->local_mac_[4], this->local_mac_[5]);
  return true;
}

bool ProkopovReaderLink::add_broadcast_peer_() {
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

bool ProkopovReaderLink::ensure_encrypted_peer_(const uint8_t mac[6]) {
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
    ESP_LOGW(TAG, "Cannot add encrypted alarm peer: %s", esp_err_to_name(err));
    return false;
  }

  std::memcpy(this->peer_mac_, mac, 6);
  this->paired_ = true;

  ESP_LOGI(TAG, "Alarm hub paired: %02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  // If a card was already presented while pairing was not ready, send it now.
  if (this->pending_) this->next_retry_at_ = millis();
  return true;
}

bool ProkopovReaderLink::send_packet_(const uint8_t dst[6], Packet packet) {
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

void ProkopovReaderLink::send_hello_() {
  Packet p{};
  p.type = static_cast<uint8_t>(MessageType::HELLO);
  p.role = static_cast<uint8_t>(NodeRole::READER);
  p.seq = ++this->seq_counter_;
  this->send_packet_(BROADCAST_MAC, p);
}

bool ProkopovReaderLink::send_card(uint32_t uid) {
  if (uid == 0 || this->pending_) return false;

  this->pending_ = true;
  this->pending_uid_ = uid;
  this->pending_seq_ = ++this->seq_counter_;
  if (this->pending_seq_ == 0) this->pending_seq_ = ++this->seq_counter_;
  this->pending_attempts_ = 0;
  this->next_retry_at_ = millis();
  this->pending_deadline_ = millis() + this->response_timeout_ms_;
  this->result_ready_ = false;

  ESP_LOGI(TAG, "Card queued uid=%lu seq=%lu paired=%s",
           (unsigned long) uid, (unsigned long) this->pending_seq_, YESNO(this->paired_));

  if (this->paired_) this->send_pending_card_();
  return true;
}

void ProkopovReaderLink::send_pending_card_() {
  if (!this->pending_ || !this->paired_ || this->pending_attempts_ >= this->max_attempts_) return;

  Packet p{};
  p.type = static_cast<uint8_t>(MessageType::CARD);
  p.role = static_cast<uint8_t>(NodeRole::READER);
  p.seq = this->pending_seq_;
  p.value = this->pending_uid_;
  p.state = this->last_state_;

  this->pending_attempts_++;
  this->next_retry_at_ = millis() + this->retry_interval_ms_;

  ESP_LOGD(TAG, "CARD tx uid=%lu seq=%lu attempt=%u",
           (unsigned long) this->pending_uid_, (unsigned long) this->pending_seq_,
           (unsigned) this->pending_attempts_);

  this->send_packet_(this->peer_mac_, p);
}

void ProkopovReaderLink::finish_no_response_() {
  if (!this->pending_) return;

  ESP_LOGW(TAG, "No Olimex response for uid=%lu seq=%lu",
           (unsigned long) this->pending_uid_, (unsigned long) this->pending_seq_);

  this->pending_ = false;
  this->result_code_ = static_cast<uint8_t>(ResultCode::NO_RESPONSE);
  this->result_state_ = this->last_state_;
  this->result_wait_ms_ = 0;
  this->result_ready_ = true;
}

bool ProkopovReaderLink::take_result(uint8_t &result, uint8_t &state, uint32_t &wait_ms) {
  if (!this->result_ready_) return false;
  result = this->result_code_;
  state = this->result_state_;
  wait_ms = this->result_wait_ms_;
  this->result_ready_ = false;
  return true;
}

bool ProkopovReaderLink::take_state(uint8_t &state, uint32_t &wait_ms) {
  if (!this->state_ready_) return false;
  state = this->last_state_;
  wait_ms = this->last_wait_ms_;
  this->state_ready_ = false;
  return true;
}

void ProkopovReaderLink::recv_cb_(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (instance_ == nullptr || info == nullptr) return;
  instance_->handle_recv_(info->src_addr, data, len);
}

void ProkopovReaderLink::handle_recv_(const uint8_t src[6], const uint8_t *data, int len) {
  if (data == nullptr || len != static_cast<int>(sizeof(Packet))) return;

  Packet p{};
  std::memcpy(&p, data, sizeof(p));

  if (!prokopov_espnow::packet_header_valid(p)) return;
  if (std::memcmp(p.sender_mac, src, 6) != 0) return;
  if (!prokopov_espnow::verify_packet(p, this->auth_key_)) {
    ESP_LOGW(TAG, "Rejected ESP-NOW packet with invalid authentication tag");
    return;
  }
  if (p.role != static_cast<uint8_t>(NodeRole::HUB)) return;

  const auto type = static_cast<MessageType>(p.type);

  if (type == MessageType::HELLO) {
    this->ensure_encrypted_peer_(src);
    return;
  }

  if (!this->paired_ || std::memcmp(src, this->peer_mac_, 6) != 0) return;

  if (type == MessageType::RESULT) {
    if (!this->pending_ || p.seq != this->pending_seq_) return;

    this->pending_ = false;
    this->result_code_ = static_cast<uint8_t>(p.value & 0xFFU);
    this->result_state_ = p.state;
    this->result_wait_ms_ = p.aux;
    this->last_state_ = p.state;
    this->last_wait_ms_ = p.aux;
    this->result_ready_ = true;

    ESP_LOGI(TAG, "RESULT seq=%lu result=%u state=%u wait=%lu ms",
             (unsigned long) p.seq, (unsigned) this->result_code_,
             (unsigned) this->result_state_, (unsigned long) this->result_wait_ms_);
    return;
  }

  if (type == MessageType::STATE) {
    this->last_state_ = p.state;
    this->last_wait_ms_ = p.aux;
    this->state_ready_ = true;
    return;
  }
}

}  // namespace prokopov_reader_link
}  // namespace esphome
