from pathlib import Path

CPP = Path('esphome/components/prokopov_alarm/prokopov_alarm.cpp')

text = CPP.read_text()

old = '''  bool ok = false;
  int saved_errno = 0;
  errno = 0;
  FILE *f = fopen(EVENTS_FILE, "ab");
  if (f) {
    const size_t written = fwrite(line.data(), 1, line.size(), f);
    if (written == line.size() && fflush(f) == 0) ok = true;
    else saved_errno = errno ? errno : EIO;
    if (fclose(f) != 0) {
      ok = false;
      if (!saved_errno) saved_errno = errno ? errno : EIO;
    }
  } else {
    saved_errno = errno ? errno : EIO;
  }
'''

new = '''  bool ok = false;
  int saved_errno = 0;
  const char *failed_stage = "open";
  errno = 0;

  // ESP-IDF/FatFs on this target reports EINVAL for stdio append mode ("ab").
  // Open an existing file read/write, seek explicitly to EOF, and create it with
  // plain write mode on the first event. This keeps the append operation portable
  // while preserving the one-event-at-a-time durability model.
  FILE *f = fopen(EVENTS_FILE, "r+");
  if (!f && errno == ENOENT) {
    errno = 0;
    f = fopen(EVENTS_FILE, "w");
  }

  if (f) {
    failed_stage = "seek";
    if (fseek(f, 0, SEEK_END) == 0) {
      failed_stage = "write";
      const size_t written = fwrite(line.data(), 1, line.size(), f);
      if (written == line.size()) {
        failed_stage = "flush";
        if (fflush(f) == 0) ok = true;
        else saved_errno = errno ? errno : EIO;
      } else {
        saved_errno = errno ? errno : EIO;
      }
    } else {
      saved_errno = errno ? errno : EIO;
    }

    failed_stage = ok ? "close" : failed_stage;
    if (fclose(f) != 0) {
      ok = false;
      if (!saved_errno) saved_errno = errno ? errno : EIO;
      failed_stage = "close";
    }
  } else {
    saved_errno = errno ? errno : EIO;
  }
'''

if old not in text:
    raise SystemExit('Expected v1.0.4 audit append block not found; refusing to patch.')

text = text.replace(old, new, 1)

old_log = '''    ESP_LOGW(TAG, "Audit log append failed: errno=%d pending=%u failures=%lu",
             this->event_write_errno_,
             (unsigned) this->pending_event_lines_.size(),
             (unsigned long) this->event_write_failures_);
'''
new_log = '''    ESP_LOGW(TAG, "Audit log append failed: stage=%s errno=%d pending=%u failures=%lu",
             failed_stage,
             this->event_write_errno_,
             (unsigned) this->pending_event_lines_.size(),
             (unsigned long) this->event_write_failures_);
'''

if old_log not in text:
    raise SystemExit('Expected v1.0.4 audit warning block not found; refusing to patch.')

text = text.replace(old_log, new_log, 1)
CPP.write_text(text)
print('PROKOPOV firmware v1.0.5 SD append compatibility patch applied successfully')
print('Modified:')
print(' - esphome/components/prokopov_alarm/prokopov_alarm.cpp')
