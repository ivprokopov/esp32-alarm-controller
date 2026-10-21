#include "prokopov_panel_link.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#ifdef USE_PROKOPOV_PANEL_CONTROLLER
#include "esphome/components/prokopov_alarm/prokopov_alarm.h"
#endif

extern "C" {
#include "mbedtls/md.h"
}

#include <cstring>

namespace esphome {
namespace prokopov_panel_link {

static const char *const TAG = "prokopov_panel_link";
static constexpr uint8_t SYNC = 0x7E;
static constexpr uint8_t VER = 1;
static constexpr uint8_t MAX_PAYLOAD = 48;
static constexpr uint32_t PANEL_REPLY_WINDOW_MS = 150;

static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static uint32_t get32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t) p[3] << 24); }

void PanelLink::set_key(const std::string &hex_key) {
  this->key_ok_ = hex_key.size() == 64;
  for (size_t i = 0; i < 32 && this->key_ok_; i++) {
    char b[3] = {hex_key[2 * i], hex_key[2 * i + 1], 0};
    this->key_[i] = (uint8_t) strtoul(b, nullptr, 16);
  }
}

void PanelLink::setup() {
  this->session_ = random_uint32();
  this->panel_nonce_ = this->is_controller_ ? 0 : random_uint32() | 1;
  this->tx_seq_ = 1;
  if (!this->key_ok_) ESP_LOGE(TAG, "key must be 64 hex characters; link disabled");
}

void PanelLink::dump_config() {
  ESP_LOGCONFIG(TAG, "PROKOPOV panel link (RS-485): role %s, status every %u ms", this->is_controller_ ? "controller" : "panel",
                (unsigned) this->status_interval_ms_);
}

uint16_t PanelLink::crc16_(const uint8_t *d, size_t n) {
  uint16_t crc = 0xFFFF;
  while (n--) {
    crc ^= (uint16_t) (*d++) << 8;
    for (int i = 0; i < 8; i++) crc = crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1;
  }
  return crc;
}

void PanelLink::mac_(const uint8_t *data, size_t len, uint8_t out[8]) const {
  uint8_t full[32];
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), this->key_.data(), this->key_.size(), data, len, full);
  memcpy(out, full, 8);
}

void PanelLink::send_frame_(uint8_t type, const uint8_t *payload, uint8_t len) {
  if (!this->key_ok_ || len > MAX_PAYLOAD) return;
  uint8_t buf[1 + 7 + MAX_PAYLOAD + 8 + 2];
  size_t n = 0;
  buf[n++] = SYNC;
  buf[n++] = VER;
  buf[n++] = type;
  put32(buf + n, this->tx_seq_++); n += 4;
  buf[n++] = len;
  memcpy(buf + n, payload, len); n += len;
  this->mac_(buf + 1, n - 1, buf + n); n += 8;
  uint16_t crc = crc16_(buf + 1, n - 1);
  buf[n++] = crc & 0xFF;
  buf[n++] = crc >> 8;
  this->write_array(buf, n);
}

// Returns true when a complete frame was consumed (valid or not).
bool PanelLink::parse_() {
  while (!this->rx_.empty() && this->rx_[0] != SYNC) this->rx_.erase(this->rx_.begin());
  if (this->rx_.size() < 9) return false;
  const uint8_t len = this->rx_[7];
  if (this->rx_[1] != VER || len > MAX_PAYLOAD) {
    this->rx_.erase(this->rx_.begin());
    this->frames_bad_++;
    return true;
  }
  const size_t total = 1 + 7 + len + 8 + 2;
  if (this->rx_.size() < total) return false;
  const uint8_t *f = this->rx_.data();
  const uint16_t crc = f[total - 2] | (f[total - 1] << 8);
  uint8_t mac[8];
  bool ok = crc16_(f + 1, total - 3) == crc;
  if (ok) {
    this->mac_(f + 1, 7 + len, mac);
    uint8_t d = 0;
    for (int i = 0; i < 8; i++) d |= mac[i] ^ f[8 + len + i];
    ok = d == 0;
  }
  if (!ok) {
    this->rx_.erase(this->rx_.begin());   // resync on the next 0x7E
    this->frames_bad_++;
    return true;
  }
  const uint8_t type = f[2];
  const uint32_t seq = get32(f + 3);
  std::vector<uint8_t> payload(f + 8, f + 8 + len);
  this->rx_.erase(this->rx_.begin(), this->rx_.begin() + total);
  this->frames_ok_++;
  this->last_rx_ms_ = millis();
  this->handle_frame_(type, seq, payload.data(), len);
  return true;
}

void PanelLink::loop() {
  uint8_t b;
  int guard = 256;
  while (this->available() && guard--) {
    if (!this->read_byte(&b)) break;
    this->rx_.push_back(b);
    if (this->rx_.size() > 256) this->rx_.erase(this->rx_.begin(), this->rx_.begin() + 128);
  }
  while (this->parse_()) {}

  if (this->is_controller_) {
    if (millis() - this->last_status_ms_ >= this->status_interval_ms_) this->controller_send_status_();
#ifdef USE_PROKOPOV_PANEL_CONTROLLER
    else if (this->alarm_ != nullptr) {
      // push a STATUS immediately when something visible changed
      uint8_t st, fl; uint16_t z, rem;
      this->alarm_->panel_snapshot(st, fl, z, rem);
      uint64_t snap = ((uint64_t) st << 40) | ((uint64_t) fl << 32) | ((uint64_t) z << 16);
      if (snap != this->last_snapshot_ && millis() - this->last_status_ms_ > 50) this->controller_send_status_();
    }
#endif
  } else {
    const bool was = this->status_.online;
    this->status_.online = this->last_rx_ms_ && millis() - this->last_rx_ms_ < 3000;
    if (was != this->status_.online) this->status_cb_.call(this->status_);
  }
}

void PanelLink::controller_send_status_() {
  this->last_status_ms_ = millis();
  uint8_t p[15];
  uint8_t st = 0, fl = 0; uint16_t z = 0, rem = 0;
#ifdef USE_PROKOPOV_PANEL_CONTROLLER
  if (this->alarm_ != nullptr) this->alarm_->panel_snapshot(st, fl, z, rem);
#endif
  this->last_snapshot_ = ((uint64_t) st << 40) | ((uint64_t) fl << 32) | ((uint64_t) z << 16);
  put32(p, this->session_);
  p[4] = st; p[5] = fl;
  p[6] = z & 0xFF; p[7] = z >> 8;
  p[8] = rem & 0xFF; p[9] = rem >> 8;
  p[10] = this->last_result_;
  put32(p + 11, this->last_result_seq_);
  this->send_frame_(STATUS, p, sizeof(p));
}

void PanelLink::handle_frame_(uint8_t type, uint32_t seq, const uint8_t *p, uint8_t len) {
  if (!this->is_controller_) {
    if (type != STATUS || len < 15) return;
    const uint32_t session = get32(p);
    if (session != this->session_) {          // controller rebooted: adopt the new session
      this->session_ = session;
      this->status_last_seq_ = 0;
    } else if (seq <= this->status_last_seq_) {
      return;                                  // stale / replayed STATUS
    }
    this->status_last_seq_ = seq;
    this->status_.state = p[4];
    this->status_.flags = p[5];
    this->status_.zones = p[6] | (p[7] << 8);
    this->status_.remaining_s = p[8] | (p[9] << 8);
    this->status_.result = p[10];
    this->status_.result_seq = get32(p + 11);
    this->status_.online = true;
    this->status_cb_.call(this->status_);
    // our bus slot: send a queued request now
    if (this->pending_type_) {
      std::vector<uint8_t> pl(8 + this->pending_payload_.size());
      put32(pl.data(), this->session_);
      put32(pl.data() + 4, this->panel_nonce_);
      memcpy(pl.data() + 8, this->pending_payload_.data(), this->pending_payload_.size());
      this->send_frame_(this->pending_type_, pl.data(), pl.size());
      this->pending_type_ = 0;
      std::fill(this->pending_payload_.begin(), this->pending_payload_.end(), 0);   // wipe the PIN
      std::fill(pl.begin(), pl.end(), 0);
      this->pending_payload_.clear();
    }
    return;
  }

#ifdef USE_PROKOPOV_PANEL_CONTROLLER
  // controller: only accept requests inside the reply window, for this session, with a fresh sequence number
  if (len < 8 || get32(p) != this->session_) return;
  const uint32_t nonce = get32(p + 4);
  if (nonce != this->panel_nonce_) {
    for (uint32_t r : this->retired_nonces_) if (r == nonce) { ESP_LOGW(TAG, "retired panel nonce: replay dropped"); return; }
    if (this->panel_nonce_) {
      this->retired_nonces_.push_back(this->panel_nonce_);
      if (this->retired_nonces_.size() > 32) this->retired_nonces_.erase(this->retired_nonces_.begin());
    }
    this->panel_nonce_ = nonce;          // panel rebooted
    this->rx_last_seq_ = 0;
  }
  if (seq <= this->rx_last_seq_) {
    ESP_LOGW(TAG, "replayed/old frame seq %u <= %u dropped", (unsigned) seq, (unsigned) this->rx_last_seq_);
    return;
  }
  if (millis() - this->last_status_ms_ > PANEL_REPLY_WINDOW_MS + 50) return;
  this->rx_last_seq_ = seq;
  p += 8; len -= 8;
  if (this->alarm_ == nullptr) return;
  uint8_t result = 0;
  if (type == PIN_ACTION && len >= 2) {
    const uint8_t action = p[0], pl = p[1];
    if (pl > len - 2) return;
    std::string pin(reinterpret_cast<const char *>(p + 2), pl);
    static const char *names[] = {"", "arm_away", "arm_home", "arm_night", "disarm", "unlock", "lock"};
    if (action >= 1 && action <= 6)
      result = (uint8_t) this->alarm_->process_pin(pin, names[action], "panel");
    std::fill(pin.begin(), pin.end(), '\0');
  } else if (type == PANIC && len >= 1) {
    result = this->alarm_->command(p[0] ? "silent_panic" : "panic", "panel") ? 1 : 0;
  } else if (type == PING) {
    result = 0;
  } else {
    return;
  }
  this->last_result_ = result;
  this->last_result_seq_ = seq;
  this->controller_send_status_();   // answer at once (next bus slot)
#endif
}

void PanelLink::send_pin(uint8_t action, const std::string &pin) {
  if (pin.size() > 12) return;
  this->pending_payload_.assign({action, (uint8_t) pin.size()});
  this->pending_payload_.insert(this->pending_payload_.end(), pin.begin(), pin.end());
  this->pending_type_ = PIN_ACTION;
}

void PanelLink::send_panic(bool silent) {
  this->pending_payload_.assign({(uint8_t) (silent ? 1 : 0)});
  this->pending_type_ = PANIC;
}

}  // namespace prokopov_panel_link
}  // namespace esphome
