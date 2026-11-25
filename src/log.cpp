#include "log.h"

#include <ctime>
#include <windows.h>

namespace {
const char* levelName(LogLevel lv) {
  switch (lv) {
    case LogLevel::Debug: return "DBG";
    case LogLevel::Info:  return "INF";
    case LogLevel::Warn:  return "WRN";
    case LogLevel::Error: return "ERR";
  }
  return "???";
}
}  // namespace

Logger& Logger::instance() {
  static Logger inst;
  return inst;
}

bool Logger::openFile(const std::string& path) {
  std::lock_guard<std::mutex> lock(mu_);
  if (file_) {
    std::fclose(file_);
    file_ = nullptr;
  }
  if (path.empty()) return true;
  file_ = std::fopen(path.c_str(), "a");
  return file_ != nullptr;
}

void Logger::log(LogLevel lv, const char* fmt, ...) {
  if (lv < level_) return;
  char buf[4096];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  writeLine(lv, buf);
}

void Logger::writeLine(LogLevel lv, const char* msg) {
  SYSTEMTIME st;
  GetLocalTime(&st);
  char stamp[32];
  std::snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03d", st.wHour, st.wMinute, st.wSecond,
                st.wMilliseconds);

  std::lock_guard<std::mutex> lock(mu_);
  std::printf("%s [%s] %s\n", stamp, levelName(lv), msg);
  std::fflush(stdout);
  if (file_) {
    std::fprintf(file_, "%04d-%02d-%02d %s [%s] %s\n", st.wYear, st.wMonth, st.wDay, stamp,
                 levelName(lv), msg);
    std::fflush(file_);
  }
}

void Logger::debug(const char* fmt, ...) {
  if (LogLevel::Debug < level_) return;
  char buf[4096];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  writeLine(LogLevel::Debug, buf);
}

void Logger::info(const char* fmt, ...) {
  if (LogLevel::Info < level_) return;
  char buf[4096];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  writeLine(LogLevel::Info, buf);
}

void Logger::warn(const char* fmt, ...) {
  if (LogLevel::Warn < level_) return;
  char buf[4096];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  writeLine(LogLevel::Warn, buf);
}

void Logger::error(const char* fmt, ...) {
  if (LogLevel::Error < level_) return;
  char buf[4096];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  writeLine(LogLevel::Error, buf);
}
