#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "mbedtls/md.h"

namespace esphome {
namespace prokopov_espnow {

static constexpr uint32_t PROTOCOL_MAGIC = 0x50414C4DU;  // "PALM"
static constexpr uint8_t PROTOCOL_VERSION = 1;
static constexpr size_t AUTH_TAG_SIZE = 16;
static constexpr size_t AUTH_KEY_SIZE = 32;
static constexpr size_t ESPNOW_KEY_SIZE = 16;

enum class MessageType : uint8_t {
  HELLO = 1,
  CARD = 2,
  RESULT = 3,
  STATE = 4,
};

enum class NodeRole : uint8_t {
  HUB = 1,
  READER = 2,
};

enum class ResultCode : uint8_t {
  NONE = 0,
  ACCESS_GRANTED = 1,
  LOCKED_ARM_WAIT = 2,
  ARMED = 3,
  DISARMED_UNLOCKED = 4,
  DENIED = 5,
  ARM_BLOCKED = 6,
  ENROLL_CAPTURED = 7,
  NO_RESPONSE = 250,
};

enum class ReaderStateCode : uint8_t {
  LOCKED = 0,
  UNLOCKED = 1,
  ARM_WAIT = 2,
  ARMED = 3,
};

struct __attribute__((packed)) Packet {
  uint32_t magic{PROTOCOL_MAGIC};
  uint8_t version{PROTOCOL_VERSION};
  uint8_t type{0};
  uint8_t role{0};
  uint8_t state{0};
  uint32_t seq{0};
  uint32_t value{0};
  uint32_t aux{0};
  uint8_t sender_mac[6]{};
  uint8_t auth[AUTH_TAG_SIZE]{};
};

static_assert(sizeof(Packet) <= 250, "ESP-NOW packet must stay below 250 bytes");

inline int hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + c - 'a';
  if (c >= 'A' && c <= 'F') return 10 + c - 'A';
  return -1;
}

inline bool parse_hex_exact(const std::string &hex, uint8_t *out, size_t out_len) {
  if (hex.size() != out_len * 2U) return false;
  for (size_t i = 0; i < out_len; i++) {
    int hi = hex_nibble(hex[i * 2U]);
    int lo = hex_nibble(hex[i * 2U + 1U]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

inline bool constant_time_equal(const uint8_t *a, const uint8_t *b, size_t len) {
  uint8_t diff = 0;
  for (size_t i = 0; i < len; i++) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
  return diff == 0;
}

inline bool calculate_auth(const Packet &packet, const uint8_t key[AUTH_KEY_SIZE],
                           uint8_t out[AUTH_TAG_SIZE]) {
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (md == nullptr) return false;

  uint8_t full[32]{};
  const int rc = mbedtls_md_hmac(
      md, key, AUTH_KEY_SIZE,
      reinterpret_cast<const uint8_t *>(&packet), offsetof(Packet, auth),
      full);
  if (rc != 0) return false;

  std::memcpy(out, full, AUTH_TAG_SIZE);
  return true;
}

inline bool sign_packet(Packet &packet, const uint8_t key[AUTH_KEY_SIZE]) {
  std::memset(packet.auth, 0, sizeof(packet.auth));
  uint8_t tag[AUTH_TAG_SIZE]{};
  if (!calculate_auth(packet, key, tag)) return false;
  std::memcpy(packet.auth, tag, AUTH_TAG_SIZE);
  return true;
}

inline bool verify_packet(const Packet &packet, const uint8_t key[AUTH_KEY_SIZE]) {
  uint8_t expected[AUTH_TAG_SIZE]{};
  if (!calculate_auth(packet, key, expected)) return false;
  return constant_time_equal(packet.auth, expected, AUTH_TAG_SIZE);
}

inline bool packet_header_valid(const Packet &packet) {
  return packet.magic == PROTOCOL_MAGIC && packet.version == PROTOCOL_VERSION;
}

}  // namespace prokopov_espnow
}  // namespace esphome
