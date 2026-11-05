#include "tcu/logging/console_sink.hpp"

#include <cstdio>

namespace tcu::logging {

bool ConsoleSink::write(std::string_view line) {
  // One fwrite for the payload and one for the separator, on a stream the C
  // runtime already locks per call. stdout is never considered unhealthy: if a
  // pipe is closed the records are lost, and that is a property of the pipe,
  // not a sink fault the application can act on.
  const std::size_t written =
      std::fwrite(line.data(), 1, line.size(), stdout);
  std::fputc('\n', stdout);
  return written == line.size();
}

void ConsoleSink::flush() { std::fflush(stdout); }

}  // namespace tcu::logging
