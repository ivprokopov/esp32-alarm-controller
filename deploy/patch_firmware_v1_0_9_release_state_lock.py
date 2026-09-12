from pathlib import Path

path = Path("esphome/components/prokopov_alarm/prokopov_alarm.cpp")
s = path.read_text()

old_cards_start = '''bool ProkopovAlarm::apply_cards_json_(const std::string &body, bool persist) {
  RecursiveLock lock(this->state_mutex_);
'''
new_cards_start = '''bool ProkopovAlarm::apply_cards_json_(const std::string &body, bool persist) {
  {
    RecursiveLock lock(this->state_mutex_);
'''

old_cards_end = '''  this->cards_.swap(next);
  cJSON_Delete(root);
  if (persist) return this->write_file_atomic_(CARDS_FILE, body);
  return true;
}
'''
new_cards_end = '''  this->cards_.swap(next);
  cJSON_Delete(root);
  }

  // FAT metadata updates on this installation can take more than one second.
  // The HTTP server task may wait for persistence, but never keep state_mutex_
  // held while doing SD I/O: the autonomous alarm loop and ESPHome template
  // sensors must remain responsive during config/card synchronization.
  if (persist) return this->write_file_atomic_(CARDS_FILE, body);
  return true;
}
'''

old_config_start = '''bool ProkopovAlarm::apply_config_json_(const std::string &body, bool persist) {
  RecursiveLock lock(this->state_mutex_);
'''
new_config_start = '''bool ProkopovAlarm::apply_config_json_(const std::string &body, bool persist) {
  {
    RecursiveLock lock(this->state_mutex_);
'''

old_config_end = '''  this->zones_.swap(next);
  cJSON_Delete(root);
  if (persist) return this->write_file_atomic_(CONFIG_FILE, body);
  return true;
}
'''
new_config_end = '''  this->zones_.swap(next);
  cJSON_Delete(root);
  }

  // Persist only after releasing state_mutex_.  This preserves the existing
  // synchronous API contract (HTTP 200 still means the SD write succeeded)
  // without stalling the real-time alarm state machine while FatFs is busy.
  if (persist) return this->write_file_atomic_(CONFIG_FILE, body);
  return true;
}
'''

for old, new, label in (
    (old_cards_start, new_cards_start, "cards start"),
    (old_cards_end, new_cards_end, "cards end"),
    (old_config_start, new_config_start, "config start"),
    (old_config_end, new_config_end, "config end"),
):
    if old not in s:
        raise SystemExit(f"marker not found: {label}")
    s = s.replace(old, new, 1)

path.write_text(s)

print("PROKOPOV firmware v1.0.9 state-lock/SD decoupling patch applied successfully")
print("Modified:")
print(" -", path)
print()
print("Effect:")
print(" - config/card JSON is still applied atomically under state_mutex_")
print(" - config/card SD persistence remains synchronous for API correctness")
print(" - slow FAT writes no longer hold state_mutex_ and cannot stall the alarm loop")
