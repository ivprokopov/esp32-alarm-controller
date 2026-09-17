#!/usr/bin/env python3
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
H = ROOT / "esphome/components/prokopov_alarm/prokopov_alarm.h"
CPP = ROOT / "esphome/components/prokopov_alarm/prokopov_alarm.cpp"

h = H.read_text(encoding="utf-8")
cpp = CPP.read_text(encoding="utf-8")

if "RemoteReaderResult" in h and "process_remote_card" in cpp:
    print("Remote reader core patch already applied.")
    raise SystemExit(0)

reader_feedback_enum = '''enum class ReaderFeedback : uint8_t {
  NONE = 0,
  UNLOCK,
  LOCK,
  ARMED,
  DISARMED,
  DENIED,
  ARM_BLOCKED,
  ENROLL,
};
'''

remote_enum = reader_feedback_enum + '''
// Result returned to a wireless Reader Node. Numeric values intentionally
// match prokopov_espnow::ResultCode.
enum class RemoteReaderResult : uint8_t {
  NONE = 0,
  ACCESS_GRANTED = 1,
  LOCKED_ARM_WAIT = 2,
  ARMED = 3,
  DISARMED_UNLOCKED = 4,
  DENIED = 5,
  ARM_BLOCKED = 6,
  ENROLL_CAPTURED = 7,
};
'''

if reader_feedback_enum not in h:
    raise SystemExit("ERROR: ReaderFeedback enum anchor not found in header")
h = h.replace(reader_feedback_enum, remote_enum, 1)

public_anchor = '''  bool door_locked() const { return this->door_locked_; }
  bool siren_on() const { return this->siren_on_; }
'''
public_repl = public_anchor + '''
  // Wireless Reader Node entry point. All authorization and alarm decisions
  // remain inside the Olimex alarm core.
  RemoteReaderResult process_remote_card(uint32_t uid);
  uint8_t remote_reader_state_code() const;
  uint32_t arm_confirm_remaining_ms() const;
'''
if public_anchor not in h:
    raise SystemExit("ERROR: public API anchor not found in header")
h = h.replace(public_anchor, public_repl, 1)

protected_anchor = '''  void tick_reader_feedback_();
  void process_card_(uint32_t uid);
  bool arm_(AlarmState target, const std::string &actor, bool use_exit_delay);
'''
protected_repl = '''  void tick_reader_feedback_();
  void process_card_(uint32_t uid);
  RemoteReaderResult process_card_action_(uint32_t uid, bool local_feedback);
  bool arm_(AlarmState target, const std::string &actor, bool use_exit_delay);
'''
if protected_anchor not in h:
    raise SystemExit("ERROR: protected API anchor not found in header")
h = h.replace(protected_anchor, protected_repl, 1)

new_card_logic = r'''RemoteReaderResult ProkopovAlarm::process_card_action_(uint32_t uid, bool local_feedback) {
  RecursiveLock lock(this->state_mutex_);
  const uint64_t now = millis();

  auto feedback = [&](ReaderFeedback value) {
    if (local_feedback) this->start_reader_feedback_(value);
  };

  if (this->enroll_active_) {
    this->enroll_uid_ = uid;
    this->enroll_active_ = false;
    this->queue_event_("NFC", "Reader", "ENROLL_CAPTURE", std::to_string(uid));
    feedback(ReaderFeedback::ENROLL);
    return RemoteReaderResult::ENROLL_CAPTURED;
  }

  auto it = this->cards_.find(uid);
  if (it == this->cards_.end() || !it->second.enabled) {
    this->queue_event_("NFC", "Reader", "DENIED", std::to_string(uid));
    feedback(ReaderFeedback::DENIED);
    return RemoteReaderResult::DENIED;
  }

  const CardPermissions &p = it->second;
  const std::string card_actor = p.user_name.empty() ? ("card:" + std::to_string(uid)) : p.user_name;
  this->queue_event_("NFC", "Reader", "AUTHORIZED", card_actor);

  // Any armed/alarm state: an authorized disarm card disarms and unlocks.
  if (this->state_ == AlarmState::ARMED_AWAY || this->state_ == AlarmState::ARMED_HOME ||
      this->state_ == AlarmState::ARMED_NIGHT || this->state_ == AlarmState::ENTRY_DELAY ||
      this->state_ == AlarmState::ALARM || this->state_ == AlarmState::PANIC ||
      this->state_ == AlarmState::SILENT_PANIC) {
    if (!p.can_disarm) {
      this->queue_event_("NFC", "Reader", "DENIED_NO_DISARM", card_actor);
      feedback(ReaderFeedback::DENIED);
      return RemoteReaderResult::DENIED;
    }

    this->disarm_(card_actor);
    feedback(ReaderFeedback::DISARMED);
    return RemoteReaderResult::DISARMED_UNLOCKED;
  }

  // Door currently unlocked: next presentation locks the door and opens the
  // short confirmation window for ARM AWAY.
  if (!this->door_locked_) {
    if (!p.can_unlock) {
      this->queue_event_("NFC", "Reader", "DENIED_NO_UNLOCK", card_actor);
      feedback(ReaderFeedback::DENIED);
      return RemoteReaderResult::DENIED;
    }

    this->set_door_locked_(true, card_actor);
    this->arm_confirm_card_ = uid;
    this->arm_confirm_deadline_ms_ = now + this->arm_confirm_window_ms_;
    feedback(ReaderFeedback::LOCK);
    return RemoteReaderResult::LOCKED_ARM_WAIT;
  }

  // Door is locked and the same card is presented again inside the confirm
  // window: ARM AWAY. The Reader Node duplicate filter guarantees that a card
  // held continuously cannot execute both LOCK and ARM.
  if (this->arm_confirm_card_ == uid && this->arm_confirm_deadline_ms_ > now) {
    this->arm_confirm_card_ = 0;
    this->arm_confirm_deadline_ms_ = 0;

    if (p.can_arm && this->arm_(AlarmState::ARMED_AWAY, card_actor, false)) {
      feedback(ReaderFeedback::ARMED);
      return RemoteReaderResult::ARMED;
    }

    this->queue_event_("ALARM", "Arming interlock", "ARM_BLOCKED", card_actor);
    feedback(ReaderFeedback::ARM_BLOCKED);
    return RemoteReaderResult::ARM_BLOCKED;
  }

  // Locked and not in a valid ARM confirmation: unlock.
  if (!p.can_unlock) {
    this->queue_event_("NFC", "Reader", "DENIED_NO_UNLOCK", card_actor);
    feedback(ReaderFeedback::DENIED);
    return RemoteReaderResult::DENIED;
  }

  this->arm_confirm_card_ = 0;
  this->arm_confirm_deadline_ms_ = 0;
  this->set_door_locked_(false, card_actor);
  feedback(ReaderFeedback::UNLOCK);
  return RemoteReaderResult::ACCESS_GRANTED;
}

void ProkopovAlarm::process_card_(uint32_t uid) {
  (void) this->process_card_action_(uid, true);
}

RemoteReaderResult ProkopovAlarm::process_remote_card(uint32_t uid) {
  return this->process_card_action_(uid, false);
}

uint8_t ProkopovAlarm::remote_reader_state_code() const {
  RecursiveLock lock(this->state_mutex_);
  const uint64_t now = millis();

  if (this->arm_confirm_card_ != 0 && this->arm_confirm_deadline_ms_ > now)
    return 2;  // ARM_WAIT

  if (this->state_ == AlarmState::EXIT_DELAY)
    return 2;  // ARM_WAIT / exit-delay indication

  if (this->state_ == AlarmState::ARMED_AWAY || this->state_ == AlarmState::ARMED_HOME ||
      this->state_ == AlarmState::ARMED_NIGHT || this->state_ == AlarmState::ENTRY_DELAY ||
      this->state_ == AlarmState::ALARM || this->state_ == AlarmState::PANIC ||
      this->state_ == AlarmState::SILENT_PANIC)
    return 3;  // ARMED / protected state

  return this->door_locked_ ? 0 : 1;  // LOCKED : UNLOCKED
}

uint32_t ProkopovAlarm::arm_confirm_remaining_ms() const {
  RecursiveLock lock(this->state_mutex_);
  if (this->arm_confirm_card_ == 0 || this->arm_confirm_deadline_ms_ == 0) return 0;

  const uint64_t now = millis();
  if (this->arm_confirm_deadline_ms_ <= now) return 0;

  const uint64_t remaining = this->arm_confirm_deadline_ms_ - now;
  return remaining > 0xFFFFFFFFULL ? 0xFFFFFFFFU : static_cast<uint32_t>(remaining);
}

'''

pattern = re.compile(
    r"void ProkopovAlarm::process_card_\(uint32_t uid\) \{.*?\n\}\n\nbool ProkopovAlarm::arm_\(",
    re.S,
)
match = pattern.search(cpp)
if not match:
    raise SystemExit("ERROR: process_card_ block not found in cpp")

cpp = cpp[:match.start()] + new_card_logic + "bool ProkopovAlarm::arm_(" + cpp[match.end():]

H.write_text(h, encoding="utf-8")
CPP.write_text(cpp, encoding="utf-8")

print("Applied remote Reader Node core patch:")
print(" - added RemoteReaderResult")
print(" - shared local/remote card decision logic")
print(" - added Reader Node state + ARM_WAIT remaining getters")
