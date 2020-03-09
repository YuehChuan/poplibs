// Copyright (c) 2019 Graphcore Ltd, All rights reserved.
#include "poplibs_support/logging.hpp"
#include "poputil/exceptions.hpp"

#include <spdlog/fmt/fmt.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <iostream>
#include <string>

namespace poplibs_support {
namespace logging {

namespace {

// Check our enums match (incase spdlog changes under us)
static_assert(static_cast<spdlog::level::level_enum>(Level::Trace) ==
                  spdlog::level::trace,
              "Logging enum mismatch");
static_assert(static_cast<spdlog::level::level_enum>(Level::Off) ==
                  spdlog::level::off,
              "Logging enum mismatch");

// Translate to a speedlog log level.
spdlog::level::level_enum translate(Level l) {
  return static_cast<spdlog::level::level_enum>(l);
}

// Stores the logging object needed by spdlog.
struct LoggingContext {
  LoggingContext();
  std::shared_ptr<spdlog::logger> logger;
};

LoggingContext &context() {
  // This avoids the static initialisation order fiasco, but doesn't solve the
  // deinitialisation order. Who logs in destructors anyway?
  static LoggingContext loggingContext;
  return loggingContext;
}

Level logLevelFromString(const std::string &level) {

  if (level == "TRACE")
    return Level::Trace;
  if (level == "DEBUG")
    return Level::Debug;
  if (level == "INFO")
    return Level::Info;
  if (level == "WARN")
    return Level::Warn;
  if (level == "ERR")
    return Level::Err;
  if (level == "OFF" || level == "")
    return Level::Off;

  throw poputil::poplibs_error(
      "Unknown POPLIBS_LOG_LEVEL '" + level +
      "'. Valid values are TRACE, DEBUG, INFO, WARN, ERR and OFF.");
}

template <typename Mutex>
void setColours(spdlog::sinks::ansicolor_sink<Mutex> &sink) {
  // See https://en.wikipedia.org/wiki/ANSI_escape_code#Colors
  static const std::string brightBlack = "\033[90m";

  sink.set_color(spdlog::level::trace, brightBlack);
  sink.set_color(spdlog::level::debug, sink.cyan);
  sink.set_color(spdlog::level::info, sink.white);
  sink.set_color(spdlog::level::warn, sink.yellow + sink.bold);
  sink.set_color(spdlog::level::err, sink.red + sink.bold);
}

LoggingContext::LoggingContext() {
  auto POPLIBS_LOG_DEST = std::getenv("POPLIBS_LOG_DEST");
  auto POPLIBS_LOG_LEVEL = std::getenv("POPLIBS_LOG_LEVEL");

  // Get logging output from the POPLIBS_LOG_DEST environment variable.
  // The valid options are "stdout", "stderr", or if it is neither
  // of those it is treated as a filename. The default is stderr.
  std::string logDest = POPLIBS_LOG_DEST ? POPLIBS_LOG_DEST : "stderr";

  // Get logging level from OS ENV. The default level is off.
  Level defaultLevel =
      logLevelFromString(POPLIBS_LOG_LEVEL ? POPLIBS_LOG_LEVEL : "OFF");

  if (logDest == "stdout") {
    auto sink = std::make_shared<spdlog::sinks::ansicolor_stdout_sink_mt>();
    setColours(*sink);
    logger = std::make_shared<spdlog::logger>("graphcore", sink);
  } else if (logDest == "stderr") {
    auto sink = std::make_shared<spdlog::sinks::ansicolor_stderr_sink_mt>();
    setColours(*sink);
    logger = std::make_shared<spdlog::logger>("graphcore", sink);
  } else {
    try {
      logger = spdlog::basic_logger_mt("graphcore", logDest, true);
    } catch (const spdlog::spdlog_ex &e) {
      std::cerr << "Error opening log file: " << e.what() << std::endl;
      throw;
    }
  }

  logger->set_pattern("%T.%e %t PL [%L] %v");
  logger->set_level(translate(defaultLevel));
}

} // namespace

void log(Level l, std::string &&msg) {
  context().logger->log(translate(l), msg);
}

bool shouldLog(Level l) { return context().logger->should_log(translate(l)); }

void setLogLevel(Level l) { context().logger->set_level(translate(l)); }

void flush() { context().logger->flush(); }

} // namespace logging
} // namespace poplibs_support
