from pathlib import Path

CPP = Path('esphome/components/prokopov_alarm/prokopov_alarm.cpp')
HDR = Path('esphome/components/prokopov_alarm/prokopov_alarm.h')

if not CPP.exists() or not HDR.exists():
    raise SystemExit('Run this script from the repository root')

cpp = CPP.read_text(encoding='utf-8')
hdr = HDR.read_text(encoding='utf-8')


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f'{label}: expected exactly 1 match, found {count}')
    return text.replace(old, new, 1)


# 1) Event flush must not depend on Wiegand silence. Keep state persistence conservative,
# but let the queued audit log attempt one append on every loop iteration.
cpp = replace_once(
    cpp,
    '#include <algorithm>\n#include <cstdio>\n',
    '#include <algorithm>\n#include <cerrno>\n#include <cstdio>\n',
    'include cerrno',
)

cpp = replace_once(
    cpp,
    '''  const int64_t now_us = esp_timer_get_time();\n  if (this->wiegand_head_ == this->wiegand_tail_ &&\n      (this->last_irq_us_ == 0 || now_us - this->last_irq_us_ >= 350000)) {\n    if (this->state_persist_pending_) this->persist_runtime_state_();\n    this->flush_one_event_();\n  }\n''',
    '''  const int64_t now_us = esp_timer_get_time();\n  const bool wiegand_idle = this->wiegand_head_ == this->wiegand_tail_ &&\n      (this->last_irq_us_ == 0 || now_us - this->last_irq_us_ >= 350000);\n  if (wiegand_idle && this->state_persist_pending_) this->persist_runtime_state_();\n\n  // Audit logging is independent from Wiegand traffic. The ISR ring buffer protects\n  // reader pulses while a short FAT append is in progress.\n  this->flush_one_event_();\n''',
    'loop event flush',
)

# 2) Human-readable card actor. Unknown cards still retain their UID.
cpp = replace_once(
    cpp,
    '''  const CardPermissions &p = it->second;\n  this->queue_event_("NFC", "Reader", "AUTHORIZED", std::to_string(uid));\n''',
    '''  const CardPermissions &p = it->second;\n  const std::string card_actor = p.user_name.empty() ? ("card:" + std::to_string(uid)) : p.user_name;\n  this->queue_event_("NFC", "Reader", "AUTHORIZED", card_actor);\n''',
    'card actor setup',
)

for old, new, label in [
    ('this->queue_event_("NFC", "Reader", "DENIED_NO_DISARM", std::to_string(uid));',
     'this->queue_event_("NFC", "Reader", "DENIED_NO_DISARM", card_actor);',
     'card denied disarm actor'),
    ('this->disarm_("card:" + std::to_string(uid));',
     'this->disarm_(card_actor);',
     'card disarm actor'),
    ('this->set_door_locked_(true, "card:" + std::to_string(uid));',
     'this->set_door_locked_(true, card_actor);',
     'card lock actor'),
    ('this->arm_(AlarmState::ARMED_AWAY, "card:" + std::to_string(uid), false)',
     'this->arm_(AlarmState::ARMED_AWAY, card_actor, false)',
     'card arm actor'),
    ('this->queue_event_("ALARM", "Arming interlock", "ARM_BLOCKED", std::to_string(uid));',
     'this->queue_event_("ALARM", "Arming interlock", "ARM_BLOCKED", card_actor);',
     'card arm blocked actor'),
    ('this->set_door_locked_(false, "card:" + std::to_string(uid));',
     'this->set_door_locked_(false, card_actor);',
     'card unlock actor'),
]:
    cpp = replace_once(cpp, old, new, label)

# DENIED_NO_UNLOCK occurs twice for known cards.
old = 'this->queue_event_("NFC", "Reader", "DENIED_NO_UNLOCK", std::to_string(uid));'
if cpp.count(old) != 2:
    raise SystemExit(f'card denied unlock actor: expected 2 matches, found {cpp.count(old)}')
cpp = cpp.replace(old, 'this->queue_event_("NFC", "Reader", "DENIED_NO_UNLOCK", card_actor);')

# 3) Preserve the initiating actor through EXIT_DELAY -> ARMED_*.
cpp = replace_once(
    cpp,
    '''  if (use_exit_delay && this->exit_delay_s_ > 0) {\n    this->pending_arm_target_ = target;\n    this->state_deadline_ms_ = millis() + this->exit_delay_s_ * 1000ULL;\n    this->set_state_(AlarmState::EXIT_DELAY, actor);\n  } else {\n    this->state_deadline_ms_ = 0;\n    this->set_state_(target, actor);\n  }\n''',
    '''  if (use_exit_delay && this->exit_delay_s_ > 0) {\n    this->pending_arm_target_ = target;\n    this->pending_arm_actor_ = actor;\n    this->state_deadline_ms_ = millis() + this->exit_delay_s_ * 1000ULL;\n    this->set_state_(AlarmState::EXIT_DELAY, actor);\n  } else {\n    this->pending_arm_actor_.clear();\n    this->state_deadline_ms_ = 0;\n    this->set_state_(target, actor);\n  }\n''',
    'pending arm actor store',
)

cpp = replace_once(
    cpp,
    '''void ProkopovAlarm::disarm_(const std::string &actor) {\n  this->arm_confirm_card_ = 0;\n  this->arm_confirm_deadline_ms_ = 0;\n  this->state_deadline_ms_ = 0;\n''',
    '''void ProkopovAlarm::disarm_(const std::string &actor) {\n  this->arm_confirm_card_ = 0;\n  this->arm_confirm_deadline_ms_ = 0;\n  this->pending_arm_actor_.clear();\n  this->state_deadline_ms_ = 0;\n''',
    'clear pending actor on disarm',
)

cpp = replace_once(
    cpp,
    '''  if (this->state_ == AlarmState::EXIT_DELAY && this->state_deadline_ms_ && now >= this->state_deadline_ms_) {\n    this->state_deadline_ms_ = 0;\n    if (this->ready_for_(this->pending_arm_target_)) {\n      this->set_state_(this->pending_arm_target_, "exit-delay-complete");\n    } else {\n      this->set_state_(AlarmState::DISARMED, "exit-delay-blocked");\n      this->set_door_locked_(false, "exit-delay-blocked");\n    }\n  }\n''',
    '''  if (this->state_ == AlarmState::EXIT_DELAY && this->state_deadline_ms_ && now >= this->state_deadline_ms_) {\n    this->state_deadline_ms_ = 0;\n    const std::string actor = this->pending_arm_actor_.empty() ? "system" : this->pending_arm_actor_;\n    this->pending_arm_actor_.clear();\n    if (this->ready_for_(this->pending_arm_target_)) {\n      this->set_state_(this->pending_arm_target_, actor);\n    } else {\n      this->queue_event_("ALARM", "Arming interlock", "EXIT_DELAY_BLOCKED", actor);\n      this->set_state_(AlarmState::DISARMED, actor);\n      this->set_door_locked_(false, actor);\n    }\n  }\n''',
    'exit delay actor propagation',
)

# 4) Restore persisted sequence diagnostics while scanning the SD log at boot.
cpp = replace_once(
    cpp,
    '''        cJSON *seq = cJSON_GetObjectItem(root, "seq");\n        if (cJSON_IsNumber(seq)) this->event_seq_ = std::max<uint64_t>(this->event_seq_, (uint64_t) seq->valuedouble);\n''',
    '''        cJSON *seq = cJSON_GetObjectItem(root, "seq");\n        if (cJSON_IsNumber(seq)) {\n          const uint64_t value = (uint64_t) seq->valuedouble;\n          this->event_seq_ = std::max<uint64_t>(this->event_seq_, value);\n          this->event_persisted_seq_ = std::max<uint64_t>(this->event_persisted_seq_, value);\n        }\n''',
    'boot persisted sequence',
)

# 5) Keep both a persistence queue and an in-RAM recent-event ring. The latter lets
# /events work even if the SD append is temporarily unavailable.
cpp = replace_once(
    cpp,
    '''  char *printed = cJSON_PrintUnformatted(root);\n  if (printed) {\n    if (this->pending_event_lines_.size() >= 128) this->pending_event_lines_.pop_front();\n    this->pending_event_lines_.push_back(std::string(printed) + "\\n");\n    cJSON_free(printed);\n  }\n''',
    '''  char *printed = cJSON_PrintUnformatted(root);\n  if (printed) {\n    const std::string line = std::string(printed) + "\\n";\n    if (this->pending_event_lines_.size() >= 256) this->pending_event_lines_.pop_front();\n    this->pending_event_lines_.push_back(line);\n    if (this->recent_event_lines_.size() >= 256) this->recent_event_lines_.pop_front();\n    this->recent_event_lines_.push_back(line);\n    cJSON_free(printed);\n  }\n''',
    'event RAM ring',
)

cpp = replace_once(
    cpp,
    '''void ProkopovAlarm::flush_one_event_() {\n  if (!this->sd_ok_) return;\n  std::string line;\n  {\n    RecursiveLock lock(this->state_mutex_);\n    if (this->pending_event_lines_.empty()) return;\n    line = this->pending_event_lines_.front();\n    this->pending_event_lines_.pop_front();\n  }\n  if (this->storage_mutex_ && xSemaphoreTake(this->storage_mutex_, 0) != pdTRUE) {\n    RecursiveLock lock(this->state_mutex_);\n    this->pending_event_lines_.push_front(line);\n    return;\n  }\n  bool ok = false;\n  FILE *f = fopen(EVENTS_FILE, "ab");\n  if (f) {\n    ok = fwrite(line.data(), 1, line.size(), f) == line.size();\n    fflush(f);\n    fclose(f);\n  }\n  if (this->storage_mutex_) xSemaphoreGive(this->storage_mutex_);\n  if (!ok) {\n    RecursiveLock lock(this->state_mutex_);\n    this->pending_event_lines_.push_front(line);\n  }\n}\n''',
    '''void ProkopovAlarm::flush_one_event_() {\n  if (!this->sd_ok_) return;\n\n  const uint32_t now_ms = millis();\n  if (this->next_event_flush_ms_ && (int32_t) (now_ms - this->next_event_flush_ms_) < 0) return;\n\n  std::string line;\n  {\n    RecursiveLock lock(this->state_mutex_);\n    if (this->pending_event_lines_.empty()) return;\n    line = this->pending_event_lines_.front();\n    this->pending_event_lines_.pop_front();\n  }\n\n  if (this->storage_mutex_ && xSemaphoreTake(this->storage_mutex_, 0) != pdTRUE) {\n    RecursiveLock lock(this->state_mutex_);\n    this->pending_event_lines_.push_front(line);\n    return;\n  }\n\n  bool ok = false;\n  int saved_errno = 0;\n  errno = 0;\n  FILE *f = fopen(EVENTS_FILE, "ab");\n  if (f) {\n    const size_t written = fwrite(line.data(), 1, line.size(), f);\n    if (written == line.size() && fflush(f) == 0) ok = true;\n    else saved_errno = errno ? errno : EIO;\n    if (fclose(f) != 0) {\n      ok = false;\n      if (!saved_errno) saved_errno = errno ? errno : EIO;\n    }\n  } else {\n    saved_errno = errno ? errno : EIO;\n  }\n\n  if (this->storage_mutex_) xSemaphoreGive(this->storage_mutex_);\n\n  if (ok) {\n    cJSON *root = cJSON_Parse(line.c_str());\n    if (root) {\n      cJSON *seq = cJSON_GetObjectItem(root, "seq");\n      if (cJSON_IsNumber(seq)) {\n        this->event_persisted_seq_ = std::max<uint64_t>(\n            this->event_persisted_seq_, (uint64_t) seq->valuedouble);\n      }\n      cJSON_Delete(root);\n    }\n    this->event_write_errno_ = 0;\n    this->next_event_flush_ms_ = 0;\n    return;\n  }\n\n  this->event_write_failures_++;\n  this->event_write_errno_ = saved_errno;\n  this->next_event_flush_ms_ = millis() + 250;\n  {\n    RecursiveLock lock(this->state_mutex_);\n    this->pending_event_lines_.push_front(line);\n    if (this->pending_event_lines_.size() > 256) this->pending_event_lines_.pop_back();\n  }\n  if (this->event_write_failures_ == 1 || (this->event_write_failures_ % 20) == 0) {\n    ESP_LOGW(TAG, "Audit log append failed: errno=%d pending=%u failures=%lu",\n             this->event_write_errno_,\n             (unsigned) this->pending_event_lines_.size(),\n             (unsigned long) this->event_write_failures_);\n  }\n}\n''',
    'robust event flush',
)

# 6) Expose audit diagnostics in /status.
cpp = replace_once(
    cpp,
    '''  cJSON_AddStringToObject(root, "last_event", this->last_event_.c_str());\n  cJSON_AddNumberToObject(root, "event_seq", (double) this->event_seq_);\n''',
    '''  cJSON_AddStringToObject(root, "last_event", this->last_event_.c_str());\n  cJSON_AddNumberToObject(root, "event_seq", (double) this->event_seq_);\n  cJSON_AddNumberToObject(root, "event_persisted_seq", (double) this->event_persisted_seq_);\n  cJSON_AddNumberToObject(root, "event_pending", (double) this->pending_event_lines_.size());\n  cJSON_AddNumberToObject(root, "event_write_failures", (double) this->event_write_failures_);\n  cJSON_AddNumberToObject(root, "event_write_errno", this->event_write_errno_);\n''',
    'status audit diagnostics',
)

# 7) /events returns persisted events first, then recent RAM events not yet present on SD.
cpp = replace_once(
    cpp,
    '''std::string ProkopovAlarm::build_events_json_(uint64_t after_seq) const {\n  cJSON *root = cJSON_CreateObject();\n  cJSON_AddBoolToObject(root, "ok", true);\n  cJSON *arr = cJSON_AddArrayToObject(root, "events");\n  if (this->sd_ok_) {\n    if (this->storage_mutex_) xSemaphoreTake(this->storage_mutex_, portMAX_DELAY);\n    FILE *f = fopen(EVENTS_FILE, "r");\n    if (f) {\n      char line[1200];\n      int count = 0;\n      while (fgets(line, sizeof(line), f) && count < 200) {\n        cJSON *o = cJSON_Parse(line);\n        if (!o) continue;\n        cJSON *seq = cJSON_GetObjectItem(o, "seq");\n        if (cJSON_IsNumber(seq) && (uint64_t) seq->valuedouble > after_seq) { cJSON_AddItemToArray(arr, o); count++; }\n        else cJSON_Delete(o);\n      }\n      fclose(f);\n    }\n    if (this->storage_mutex_) xSemaphoreGive(this->storage_mutex_);\n  }\n  cJSON_AddNumberToObject(root, "last_seq", (double) this->event_seq_);\n  char *p = cJSON_PrintUnformatted(root);\n  std::string out = p ? p : "{}";\n  if (p) cJSON_free(p);\n  cJSON_Delete(root);\n  return out;\n}\n''',
    '''std::string ProkopovAlarm::build_events_json_(uint64_t after_seq) const {\n  cJSON *root = cJSON_CreateObject();\n  cJSON_AddBoolToObject(root, "ok", true);\n  cJSON *arr = cJSON_AddArrayToObject(root, "events");\n  int count = 0;\n  uint64_t highest_added = after_seq;\n\n  if (this->sd_ok_) {\n    if (this->storage_mutex_) xSemaphoreTake(this->storage_mutex_, portMAX_DELAY);\n    FILE *f = fopen(EVENTS_FILE, "r");\n    if (f) {\n      char line[1200];\n      while (fgets(line, sizeof(line), f) && count < 200) {\n        cJSON *o = cJSON_Parse(line);\n        if (!o) continue;\n        cJSON *seq = cJSON_GetObjectItem(o, "seq");\n        if (cJSON_IsNumber(seq) && (uint64_t) seq->valuedouble > after_seq) {\n          const uint64_t value = (uint64_t) seq->valuedouble;\n          highest_added = std::max<uint64_t>(highest_added, value);\n          cJSON_AddItemToArray(arr, o);\n          count++;\n        } else {\n          cJSON_Delete(o);\n        }\n      }\n      fclose(f);\n    }\n    if (this->storage_mutex_) xSemaphoreGive(this->storage_mutex_);\n  }\n\n  if (count < 200) {\n    RecursiveLock lock(this->state_mutex_);\n    for (const auto &line : this->recent_event_lines_) {\n      if (count >= 200) break;\n      cJSON *o = cJSON_Parse(line.c_str());\n      if (!o) continue;\n      cJSON *seq = cJSON_GetObjectItem(o, "seq");\n      if (cJSON_IsNumber(seq) && (uint64_t) seq->valuedouble > highest_added) {\n        highest_added = (uint64_t) seq->valuedouble;\n        cJSON_AddItemToArray(arr, o);\n        count++;\n      } else {\n        cJSON_Delete(o);\n      }\n    }\n  }\n\n  cJSON_AddNumberToObject(root, "last_seq", (double) this->event_seq_);\n  char *p = cJSON_PrintUnformatted(root);\n  std::string out = p ? p : "{}";\n  if (p) cJSON_free(p);\n  cJSON_Delete(root);\n  return out;\n}\n''',
    'events SD+RAM merge',
)

# Header additions.
hdr = replace_once(
    hdr,
    '''  AlarmState state_{AlarmState::DISARMED};\n  AlarmState armed_mode_before_alarm_{AlarmState::DISARMED};\n  AlarmState pending_arm_target_{AlarmState::ARMED_AWAY};\n  bool door_locked_{false};\n''',
    '''  AlarmState state_{AlarmState::DISARMED};\n  AlarmState armed_mode_before_alarm_{AlarmState::DISARMED};\n  AlarmState pending_arm_target_{AlarmState::ARMED_AWAY};\n  std::string pending_arm_actor_;\n  bool door_locked_{false};\n''',
    'header pending actor',
)

hdr = replace_once(
    hdr,
    '''  std::deque<std::string> pending_event_lines_;\n  uint64_t event_seq_{0};\n  std::string last_event_{"BOOT"};\n  bool state_persist_pending_{false};\n''',
    '''  std::deque<std::string> pending_event_lines_;\n  std::deque<std::string> recent_event_lines_;\n  uint64_t event_seq_{0};\n  uint64_t event_persisted_seq_{0};\n  uint32_t event_write_failures_{0};\n  int event_write_errno_{0};\n  uint32_t next_event_flush_ms_{0};\n  std::string last_event_{"BOOT"};\n  bool state_persist_pending_{false};\n''',
    'header audit diagnostics',
)

CPP.write_text(cpp, encoding='utf-8')
HDR.write_text(hdr, encoding='utf-8')

# Sanity markers.
checks = [
    'recent_event_lines_',
    'event_persisted_seq',
    'Audit log append failed',
    'pending_arm_actor_',
    'const std::string card_actor',
]
combined = cpp + '\n' + hdr
missing = [x for x in checks if x not in combined]
if missing:
    raise SystemExit(f'Patch verification failed: {missing}')

print('PROKOPOV firmware v1.0.4 audit patch applied successfully')
print('Modified:')
print(' -', CPP)
print(' -', HDR)
