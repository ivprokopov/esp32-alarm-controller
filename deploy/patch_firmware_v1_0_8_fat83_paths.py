from pathlib import Path

p = Path('esphome/components/prokopov_alarm/prokopov_alarm.cpp')
s = p.read_text()

repls = {
    'static const char *const CARDS_FILE = "/sd/alarm/auth/cards.json";':
        'static const char *const CARDS_FILE = "/sd/alarm/auth/cards.dat";',
    'static const char *const CONFIG_FILE = "/sd/alarm/config.json";':
        'static const char *const CONFIG_FILE = "/sd/alarm/config.cfg";',
    'static const char *const STATE_FILE = "/sd/alarm/runtime/state.json";':
        'static const char *const STATE_FILE = "/sd/alarm/runtime/state.dat";',
}
for old, new in repls.items():
    if old not in s:
        raise SystemExit(f'path marker not found: {old}')
    s = s.replace(old, new, 1)

start = s.find('bool ProkopovAlarm::write_file_atomic_(const std::string &path, const std::string &data) {')
end = s.find('\nvoid ProkopovAlarm::load_cards_()', start)
if start < 0 or end < 0:
    raise SystemExit('write_file_atomic_ block not found')

replacement = r'''bool ProkopovAlarm::write_file_atomic_(const std::string &path, const std::string &data) {
  if (!this->sd_ok_) return false;
  if (this->storage_mutex_) xSemaphoreTake(this->storage_mutex_, portMAX_DELAY);

  // This installation runs FatFs in short-filename-compatible mode. Keep both the
  // persistent filenames and the atomic temporary filename within FAT 8.3 limits.
  const size_t slash = path.find_last_of('/');
  const std::string dir = slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
  const std::string tmp = dir + "write.tmp";

  bool ok = false;
  int saved_errno = 0;
  const char *failed_stage = "open";

  // Remove any stale temp file first. O_APPEND is intentionally used because it is
  // already proven to work on this ESP-IDF/FatFs target, while O_TRUNC returned EINVAL.
  remove(tmp.c_str());
  errno = 0;
  int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0664);
  if (fd >= 0) {
    failed_stage = "write";
    size_t total = 0;
    while (total < data.size()) {
      const ssize_t n = write(fd, data.data() + total, data.size() - total);
      if (n <= 0) {
        saved_errno = errno ? errno : EIO;
        break;
      }
      total += (size_t) n;
    }

    if (total == data.size()) {
      failed_stage = "close";
      if (close(fd) == 0) {
        fd = -1;
        failed_stage = "replace";
        errno = 0;
        if (remove(path.c_str()) != 0 && errno != ENOENT) {
          saved_errno = errno ? errno : EIO;
        } else if (rename(tmp.c_str(), path.c_str()) == 0) {
          ok = true;
        } else {
          saved_errno = errno ? errno : EIO;
        }
      } else {
        saved_errno = errno ? errno : EIO;
        fd = -1;
      }
    }

    if (fd >= 0) close(fd);
  } else {
    saved_errno = errno ? errno : EIO;
  }

  if (!ok) {
    remove(tmp.c_str());
    ESP_LOGW(TAG, "Atomic SD write failed: path=%s stage=%s errno=%d",
             path.c_str(), failed_stage, saved_errno);
  }

  if (this->storage_mutex_) xSemaphoreGive(this->storage_mutex_);
  return ok;
}
'''

s = s[:start] + replacement + s[end:]
p.write_text(s)

print('PROKOPOV firmware v1.0.8 FAT 8.3 persistence patch applied successfully')
print('Modified:')
print(' -', p)
print('Persistent files: config.cfg, cards.dat, state.dat; temp file: write.tmp')
