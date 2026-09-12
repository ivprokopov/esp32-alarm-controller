from pathlib import Path

p = Path('esphome/components/prokopov_alarm/prokopov_alarm.cpp')
s = p.read_text()

old = r'''bool ProkopovAlarm::write_file_atomic_(const std::string &path, const std::string &data) {
  if (!this->sd_ok_) return false;
  if (this->storage_mutex_) xSemaphoreTake(this->storage_mutex_, portMAX_DELAY);
  std::string tmp = path + ".tmp";
  FILE *f = fopen(tmp.c_str(), "wb");
  bool ok = false;
  if (f) {
    ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    fflush(f);
    fclose(f);
    if (ok) {
      remove(path.c_str());
      ok = rename(tmp.c_str(), path.c_str()) == 0;
    }
  }
  if (!ok) remove(tmp.c_str());
  if (this->storage_mutex_) xSemaphoreGive(this->storage_mutex_);
  return ok;
}
'''

new = r'''bool ProkopovAlarm::write_file_atomic_(const std::string &path, const std::string &data) {
  if (!this->sd_ok_) return false;
  if (this->storage_mutex_) xSemaphoreTake(this->storage_mutex_, portMAX_DELAY);

  const std::string tmp = path + ".tmp";
  bool ok = false;
  int saved_errno = 0;
  const char *failed_stage = "open";

  errno = 0;
  int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0664);
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

if old not in s:
    raise SystemExit('write_file_atomic_ marker not found; source differs from expected v1.0.6')

s = s.replace(old, new, 1)
p.write_text(s)

print('PROKOPOV firmware v1.0.7 POSIX atomic persistence patch applied successfully')
print('Modified:')
print(' -', p)
