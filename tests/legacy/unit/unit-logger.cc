#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "core/logger.hh"
#include "unit-logger.h"

void logger_test(void) {
  using lightusd::logging::LogLevel;
  using lightusd::logging::Logger;

  TEST_CHECK(static_cast<int>(LogLevel::Debug) == 0);
  TEST_CHECK(static_cast<int>(LogLevel::Info) == 1);
  TEST_CHECK(static_cast<int>(LogLevel::Warn) == 2);
  TEST_CHECK(static_cast<int>(LogLevel::Error) == 3);
  TEST_CHECK(static_cast<int>(LogLevel::Critical) == 4);
  TEST_CHECK(static_cast<int>(LogLevel::Off) == 5);

  Logger &logger = Logger::getInstance();

  logger.setLogLevel(LogLevel::Warn);
  TEST_CHECK(!logger.shouldLog(LogLevel::Debug));
  TEST_CHECK(!logger.shouldLog(LogLevel::Info));
  TEST_CHECK(logger.shouldLog(LogLevel::Warn));
  TEST_CHECK(logger.shouldLog(LogLevel::Error));
  TEST_CHECK(logger.shouldLog(LogLevel::Critical));

  logger.setLogLevel(LogLevel::Info);
  TEST_CHECK(!logger.shouldLog(LogLevel::Debug));
  TEST_CHECK(logger.shouldLog(LogLevel::Info));
  TEST_CHECK(logger.shouldLog(LogLevel::Warn));

  logger.setLogLevel(LogLevel::Debug);
  TEST_CHECK(logger.shouldLog(LogLevel::Debug));

  logger.setLogLevel(LogLevel::Off);
  TEST_CHECK(!logger.shouldLog(LogLevel::Debug));
  TEST_CHECK(!logger.shouldLog(LogLevel::Info));
  TEST_CHECK(!logger.shouldLog(LogLevel::Warn));
  TEST_CHECK(!logger.shouldLog(LogLevel::Error));
  TEST_CHECK(!logger.shouldLog(LogLevel::Critical));

  // Restore the process-wide singleton default for subsequent tests.
  logger.setLogLevel(LogLevel::Warn);
}
