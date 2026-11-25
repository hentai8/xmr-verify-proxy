#pragma once

#include <cstdarg>
#include <string>
#include <mutex>
#include <cstdio>

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

class Logger {
 public:
  static Logger& instance();

  void setLevel(LogLevel lv) { level_ = lv; }
  LogLevel level() const { return level_; }
  bool openFile(const std::string& path);  // appends, returns false on failure

  void log(LogLevel lv, const char* fmt, ...);

  void debug(const char* fmt, ...);
  void info(const char* fmt, ...);
  void warn(const char* fmt, ...);
  void error(const char* fmt, ...);

 private:
  Logger() = default;
  void writeLine(LogLevel lv, const char* msg);

  LogLevel level_ = LogLevel::Info;
  std::mutex mu_;
  FILE* file_ = nullptr;
};

inline Logger& LOG() { return Logger::instance(); }
