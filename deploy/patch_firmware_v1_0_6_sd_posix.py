from pathlib import Path

p = Path('esphome/components/prokopov_alarm/prokopov_alarm.cpp')
s = p.read_text()

old_include = '#include <cerrno>\n#include <cstdio>\n'
new_include = '#include <cerrno>\n#include <cstdio>\n#include <fcntl.h>\n#include <unistd.h>\n'
if old_include not in s:
    raise SystemExit('include marker not found')
s = s.replace(old_include, new_include, 1)

old_path = 'static const char *const EVENTS_FILE = "/sd/alarm/log/events.ndjson";'
new_path = 'static const char *const EVENTS_FILE = "/sd/alarm/log/events.log";'
if old_path not in s:
    raise SystemExit('EVENTS_FILE marker not found')
s = s.replace(old_path, new_path, 1)

start = s.find('void ProkopovAlarm::flush_one_event_() {')
end = s.find('\nbool ProkopovAlarm::start_http_servers_()', start)
if start < 0 or end < 0:
    raise SystemExit('flush_one_event_ block not found')

replacement = r'''void ProkopovAlarm::flush_one_event_() {
  if (!this->sd_ok_) return;

  const uint32_t now_ms = millis();
  if (this->next_event_flush_ms_ && (int32_t) (now_ms - this->next_event_flush_ms_) < 0) return;

  std::string line;
  {
    RecursiveLock lock(this->state_mutex_);
    if (this->pending_event_lines_.empty()) return;
    line = this->pending_event_lines_.front();
    this->pending_event_lines_.pop_front();
  }

  if (this->storage_mutex_ && xSemaphoreTake(this->storage_mutex_, 0) != pdTRUE) {
    RecursiveLock lock(this->state_mutex_);
    this->pending_event_lines_.push_front(line);
    return;
  }

  bool ok = false;
  int saved_errno = 0;
  const char *failed_stage = "open";

  errno = 0;
  int fd = open(EVENTS_FILE, O_WRONLY | O_CREAT | O_APPEND, 0664);
  if (fd >= 0) {
    failed_stage = "write";
    const ssize_t written = write(fd, line.data(), line.size());
    if (written == (ssize_t) line.size()) {
      failed_stage = "close";
      if (close(fd) == 0) {
        ok = true;
      } else {
        saved_errno = errno ? errno : EIO;
      }
    } else {
      saved_errno = errno ? errno : EIO;
      close(fd);
    }
  } else {
    saved_errno = errno ? errno : EIO;
  }

  if (this->storage_mutex_) xSemaphoreGive(this->storage_mutex_);

  if (ok) {
    cJSON *root = cJSON_Parse(line.c_str());
    if (root) {
      cJSON *seq = cJSON_GetObjectItem(root, "seq");
      if (cJSON_IsNumber(seq)) {
        this->event_persisted_seq_ = std::max<uint64_t>(
            this->event_persisted_seq_, (uint64_t) seq->valuedouble);
      }
      cJSON_Delete(root);
    }
    this->event_write_errno_ = 0;
    this->next_event_flush_ms_ = 0;
    return;
  }

  this->event_write_failures_++;
  this->event_write_errno_ = saved_errno;
  this->next_event_flush_ms_ = millis() + 250;
  {
    RecursiveLock lock(this->state_mutex_);
    this->pending_event_lines_.push_front(line);
    if (this->pending_event_lines_.size() > 256) this->pending_event_lines_.pop_back();
  }
  if (this->event_write_failures_ == 1 || (this->event_write_failures_ % 20) == 0) {
    ESP_LOGW(TAG, "Audit log append failed: stage=%s errno=%d pending=%u failures=%lu",
             failed_stage,
             this->event_write_errno_,
             (unsigned) this->pending_event_lines_.size(),
             (unsigned long) this->event_write_failures_);
  }
}
'''

s = s[:start] + replacement + s[end:]
p.write_text(s)
print('PROKOPOV firmware v1.0.6 POSIX SD audit patch applied successfully')
print('Modified:')
print(' -', p)
