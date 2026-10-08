// NOTE: Must be compiled with -std=c++20

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

#define BIN_NAME "praplr"
#define PRAPLR_INTERVAL_MS_ENV_KEY "PRAPLR_INTERVAL_MS"
#define PRAPLR_INTERVAL_MS_DEFAULT 500

static constexpr const char *SOCKET_PREFIX = BIN_NAME;

// -----------------------------------------------------------------------------
// Global termination flag
// -----------------------------------------------------------------------------

static volatile std::sig_atomic_t g_stop = 0;

void handleSignal(int) {
  g_stop = 1;
}

// -----------------------------------------------------------------------------
// perf_event_open
// -----------------------------------------------------------------------------

static long perfEventOpen(struct perf_event_attr *attr, pid_t pid, int cpu, int groupFd,
                          unsigned long flags) {
  return syscall(__NR_perf_event_open, attr, pid, cpu, groupFd, flags);
}

// -----------------------------------------------------------------------------
// Utility functions
// -----------------------------------------------------------------------------

std::string getHostname() {
  char hostname[256]{};
  if (gethostname(hostname, sizeof(hostname)) != 0) {
    throw std::runtime_error(std::string("gethostname failed: ") + std::strerror(errno));
  }
  hostname[sizeof(hostname) - 1] = '\0';
  return hostname;
}

uint64_t hashFNV(const std::string &str) {
  uint64_t hash = 0xcbf29ce484222325ULL;
  constexpr uint64_t magicPrime = 0x00000100000001b3ULL;
  for (unsigned char c : str)
    hash = (hash ^ c) * magicPrime;
  return hash;
}

std::string hashPath(const std::string &path) {
  std::ostringstream oss;
  oss << std::hex << std::setw(16) << std::setfill('0') << hashFNV(path);
  return oss.str();
}

std::string absolutePath(const std::string &path) {
  namespace fs = std::filesystem;
  fs::path outputPath(path);
  if (fs::exists(outputPath)) {
    if (!fs::is_directory(outputPath)) {
      throw std::runtime_error("output path exists but is not a directory: " + path);
    }
  } else {
    fs::create_directories(outputPath);
  }
  return fs::canonical(outputPath).string();
}

std::string readFile(const std::string &path) {
  std::ifstream file(path);
  if (!file) {
    throw std::runtime_error("Could not open " + path);
  }
  std::string value;
  std::getline(file, value);
  if (!file && !file.eof()) {
    throw std::runtime_error("Could not read " + path);
  }
  return value;
}

uint64_t parseHex(const std::string &value) {
  size_t consumed = 0;
  uint64_t result = std::stoull(value, &consumed, 16);
  if (consumed != value.size()) {
    throw std::runtime_error("Invalid hexadecimal value: " + value);
  }
  return result;
}

double parseDouble(const std::string &value) {
  size_t consumed = 0;
  double result = std::stod(value, &consumed);
  if (consumed != value.size()) {
    throw std::runtime_error("Invalid floating-point value: " + value);
  }
  return result;
}

// -----------------------------------------------------------------------------
// JSON escaping
// -----------------------------------------------------------------------------

std::string jsonEscape(const std::string &value) {
  std::string result;
  result.reserve(value.size());
  for (unsigned char c : value) {
    switch (c) {
    case '\"':
      result += "\\\"";
      break;
    case '\\':
      result += "\\\\";
      break;
    case '\b':
      result += "\\b";
      break;
    case '\f':
      result += "\\f";
      break;
    case '\n':
      result += "\\n";
      break;
    case '\r':
      result += "\\r";
      break;
    case '\t':
      result += "\\t";
      break;
    default:
      if (c < 0x20) {
        char buffer[7];
        std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
        result += buffer;
      } else {
        result += static_cast<char>(c);
      }
    }
  }
  return result;
}

// -----------------------------------------------------------------------------
// CPU mask parsing
// -----------------------------------------------------------------------------

/*
 * Modern Linux sysfs CPU masks are generally exposed as CPU lists:
 *
 *     0
 *     0-7
 *     0,8
 *     0-7,16-23
 *
 * Some older/kernel-specific interfaces expose hexadecimal masks:
 *
 *     00000003
 *     ffffffff,ffffffff
 *
 * The important point is that a string consisting only of decimal
 * digits is ambiguous. "00000003" could be either CPU 3 or a
 * hexadecimal mask representing CPUs 0 and 1.
 *
 * For the power PMU cpumask, the normal modern representation is
 * a CPU list, so decimal-only strings are interpreted as CPU lists.
 *
 * A string containing a-f/A-F is unambiguously hexadecimal.
 */

std::vector<int> parseCpuList(const std::string &value) {
  std::vector<int> cpus;
  std::stringstream ss(value);
  std::string token;
  while (std::getline(ss, token, ',')) {
    if (token.empty())
      continue;
    const size_t dash = token.find('-');
    if (dash == std::string::npos) {
      size_t consumed = 0;
      long cpu = std::stol(token, &consumed, 10);
      if (consumed != token.size() || cpu < 0 || cpu > std::numeric_limits<int>::max()) {
        throw std::runtime_error("Invalid CPU in perf PMU cpumask: " + token);
      }
      cpus.push_back(static_cast<int>(cpu));
      continue;
    }
    const std::string first = token.substr(0, dash);
    const std::string last = token.substr(dash + 1);
    size_t consumedFirst = 0;
    size_t consumedLast = 0;
    long begin = std::stol(first, &consumedFirst, 10);
    long end = std::stol(last, &consumedLast, 10);
    if (consumedFirst != first.size() || consumedLast != last.size() || begin < 0 || end < begin ||
        end > std::numeric_limits<int>::max()) {
      throw std::runtime_error("Invalid CPU range in perf PMU cpumask: " + token);
    }
    /*
     * Avoid integer overflow in the loop condition.
     */
    for (long cpu = begin;; ++cpu) {
      cpus.push_back(static_cast<int>(cpu));
      if (cpu == end)
        break;
    }
  }
  return cpus;
}

std::vector<int> parseHexCpuMask(const std::string &value) {
  std::vector<int> cpus;
  std::vector<std::string> words;
  size_t start = 0;
  while (start < value.size()) {
    const size_t comma = value.find(',', start);
    if (comma == std::string::npos) {
      words.push_back(value.substr(start));
      break;
    }
    words.push_back(value.substr(start, comma - start));
    start = comma + 1;
  }
  for (size_t wordIndex = 0; wordIndex < words.size(); ++wordIndex) {
    const std::string &word = words[words.size() - 1 - wordIndex];
    uint64_t bits = 0;
    try {
      bits = parseHex(word);
    } catch (...) {
      throw std::runtime_error("Invalid perf PMU cpumask: " + value);
    }
    for (unsigned int bit = 0; bit < 64; ++bit) {
      if (bits & (uint64_t(1) << bit)) {
        const uint64_t cpu = uint64_t(wordIndex) * 64 + bit;
        if (cpu > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
          throw std::runtime_error("CPU number is too large");
        }
        cpus.push_back(static_cast<int>(cpu));
      }
    }
  }
  return cpus;
}

std::vector<int> parseCpuMask(const std::string &mask) {
  std::string value = mask;
  value.erase(
      std::remove_if(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c); }),
      value.end());
  if (value.empty()) {
    throw std::runtime_error("Empty perf PMU cpumask");
  }
  /*
   * If hexadecimal letters occur, it cannot be a normal
   * decimal CPU-list representation.
   */
  const bool hasHexLetters = value.find_first_of("abcdefABCDEF") != std::string::npos;
  std::vector<int> cpus;
  if (!hasHexLetters) {
    try {
      cpus = parseCpuList(value);
    } catch (...) {
      cpus.clear();
    }
  } else {
    cpus = parseHexCpuMask(value);
  }
  if (cpus.empty()) {
    throw std::runtime_error("No CPUs found in perf PMU cpumask: " + mask);
  }
  std::sort(cpus.begin(), cpus.end());
  cpus.erase(std::unique(cpus.begin(), cpus.end()), cpus.end());
  return cpus;
}

// -----------------------------------------------------------------------------
// MPI-rank election
// -----------------------------------------------------------------------------

int shouldSample(const std::string &outputPath) {
  const std::string socketName = std::string(SOCKET_PREFIX) + ":" + hashPath(outputPath);
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd == -1) {
    throw std::runtime_error(std::string("socket() failed: ") + std::strerror(errno));
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  /*
   * Abstract UNIX socket namespace.
   */
  if (socketName.size() + 1 > sizeof(address.sun_path)) {
    close(fd);
    throw std::runtime_error("Sampler election socket name is too long");
  }
  std::memcpy(address.sun_path + 1, socketName.data(), socketName.size());
  const socklen_t addressLength =
      static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + socketName.size());
  if (bind(fd, reinterpret_cast<sockaddr *>(&address), addressLength) == 0) {
    /*
     * This MPI rank won the election.
     */
    return fd;
  }
  if (errno == EADDRINUSE) {
    close(fd);
    return 0;
  }
  const int error = errno;
  close(fd);
  throw std::runtime_error(std::string("bind() failed: ") + std::strerror(error));
}

// -----------------------------------------------------------------------------
// RAPL data structures
// -----------------------------------------------------------------------------

struct RaplCounter {
  int cpu = -1;
  int fd = -1;
};

struct RaplDomain {
  std::string name;
  std::string eventPath;
  std::string unit;
  double scale = 1.0;
  uint64_t config = 0;
  /*
   * One perf event per package/socket.
   */
  std::vector<RaplCounter> counters;
};

void closeDomains(std::vector<RaplDomain> &domains) {
  for (auto &domain : domains) {
    for (auto &counter : domain.counters) {
      if (counter.fd >= 0) {
        close(counter.fd);
        counter.fd = -1;
      }
    }
  }
}

// -----------------------------------------------------------------------------
// Discover RAPL perf events
// -----------------------------------------------------------------------------

std::vector<RaplDomain> findRaplDomains() {
  namespace fs = std::filesystem;
  const fs::path base = "/sys/bus/event_source/devices/power";
  if (!fs::exists(base)) {
    throw std::runtime_error("perf power PMU not found: " + base.string());
  }
  const fs::path eventsPath = base / "events";
  if (!fs::exists(eventsPath)) {
    throw std::runtime_error("perf power PMU events directory not found: " + eventsPath.string());
  }
  const fs::path typePath = base / "type";
  const fs::path cpumaskPath = base / "cpumask";
  if (!fs::exists(typePath)) {
    throw std::runtime_error("perf power PMU type file not found: " + typePath.string());
  }
  if (!fs::exists(cpumaskPath)) {
    throw std::runtime_error("perf power PMU cpumask not found: " + cpumaskPath.string());
  }
  const uint32_t pmuType = static_cast<uint32_t>(std::stoul(readFile(typePath.string())));
  const std::vector<int> socketCpus = parseCpuMask(readFile(cpumaskPath.string()));
  std::vector<RaplDomain> domains;
  for (const auto &entry : fs::directory_iterator(eventsPath)) {
    if (!entry.is_regular_file())
      continue;
    const std::string eventName = entry.path().filename().string();
    /*
     * Only energy events:
     *
     *   energy-pkg
     *   energy-cores
     *   energy-gpu
     *   energy-ram
     *   ...
     */
    if (eventName.rfind("energy-", 0) != 0) {
      continue;
    }
    /*
     * Ignore scale/unit files.
     */
    if (eventName.ends_with(".scale") || eventName.ends_with(".unit")) {
      continue;
    }
    const std::string eventDescription = readFile(entry.path().string());
    const auto equalPos = eventDescription.find('=');
    if (equalPos == std::string::npos)
      continue;
    const std::string configString = eventDescription.substr(equalPos + 1);
    uint64_t config = 0;
    try {
      config = parseHex(configString);
    } catch (...) {
      continue;
    }
    RaplDomain domain;
    domain.name = eventName;
    domain.eventPath = entry.path().string();
    domain.config = config;
    const fs::path scalePath = eventsPath / (eventName + ".scale");
    const fs::path unitPath = eventsPath / (eventName + ".unit");
    if (fs::exists(scalePath)) {
      domain.scale = parseDouble(readFile(scalePath.string()));
    }
    if (fs::exists(unitPath)) {
      domain.unit = readFile(unitPath.string());
    } else {
      domain.unit = "J";
    }
    /*
     * Open the event on each representative CPU.
     */
    for (const int cpu : socketCpus) {
      struct perf_event_attr attr {};
      attr.type = pmuType;
      attr.size = sizeof(attr);
      attr.config = config;
      /*
       * Start disabled.
       */
      attr.disabled = 1;
      /*
       * We are measuring the PMU, not a process.
       */
      attr.exclude_kernel = 0;
      attr.exclude_hv = 0;
      const int fd = static_cast<int>(perfEventOpen(&attr, -1, cpu, -1, 0));
      if (fd == -1) {
        /*
         * A particular domain may not be available
         * on every package.
         */
        continue;
      }
      domain.counters.push_back({cpu, fd});
    }
    if (!domain.counters.empty()) {
      domains.push_back(std::move(domain));
    }
  }
  if (domains.empty()) {
    throw std::runtime_error("No usable perf RAPL energy events found under " +
                             eventsPath.string());
  }
  return domains;
}

// -----------------------------------------------------------------------------
// Metadata
// -----------------------------------------------------------------------------

void writeDomainMetadata(const std::string &path, const std::vector<RaplDomain> &domains) {
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("Could not open metadata file: " + path);
  }
  /*
   * TODO: Consider refactor to per-socket measurements.
   * (Preserve per-socket values rather than just their sum.)
   */
  output << "{\n"
         << "  \"hostname\": \"" << jsonEscape(getHostname()) << "\",\n"
         << "  \"counter_bits\": 64,\n"
         << "  \"domains\": [\n";
  for (size_t i = 0; i < domains.size(); ++i) {
    const auto &domain = domains[i];
    output << "    {\n"
           << "      \"name\": \"" << jsonEscape(domain.name) << "\",\n"
           << "      \"event_path\": \"" << jsonEscape(domain.eventPath) << "\",\n"
           << "      \"config\": " << domain.config << ",\n"
           << "      \"unit\": \"" << jsonEscape(domain.unit) << "\",\n"
           << "      \"scale\": " << std::setprecision(17) << domain.scale << ",\n"
           << "      \"sockets\": [";
    for (size_t j = 0; j < domain.counters.size(); ++j) {
      if (j != 0)
        output << ", ";
      output << domain.counters[j].cpu;
    }
    output << "]\n"
           << "    }";
    if (i + 1 != domains.size())
      output << ",";
    output << "\n";
  }
  output << "  ]\n"
         << "}\n";
}

// -----------------------------------------------------------------------------
// Counter reads
// -----------------------------------------------------------------------------

std::vector<uint64_t> readEnergy(const std::vector<RaplDomain> &domains) {
  std::vector<uint64_t> values;
  values.reserve(domains.size());
  for (const auto &domain : domains) {
    /*
     * TODO: Consider refactor to per-socket measurements.
     * (Preserve per-socket values rather than just their sum.)
     */
    uint64_t total = 0;
    for (const auto &counter : domain.counters) {
      uint64_t value = 0;
      const ssize_t result = ::read(counter.fd, &value, sizeof(value));
      if (result != static_cast<ssize_t>(sizeof(value))) {
        const int error = errno;
        throw std::runtime_error("Could not read perf counter for " + domain.name + " on CPU " +
                                 std::to_string(counter.cpu) + ": " + std::strerror(error));
      }
      /*
       * The C++ unsigned addition deliberately has
       * modulo-2^64 semantics.
       *
       * In practice the cumulative perf counts are nowhere
       * near overflowing uint64_t during a measurement run.
       */
      total += value;
    }
    values.push_back(total);
  }
  return values;
}

// -----------------------------------------------------------------------------
// Sampling interval
// -----------------------------------------------------------------------------

int getIntervalMs() {
  const char *value = std::getenv(PRAPLR_INTERVAL_MS_ENV_KEY);
  if (value == nullptr)
    return PRAPLR_INTERVAL_MS_DEFAULT;
  char *end = nullptr;
  errno = 0;
  const long interval = std::strtol(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || interval <= 0 ||
      interval > std::numeric_limits<int>::max()) {
    throw std::runtime_error(std::string("Invalid ") + PRAPLR_INTERVAL_MS_ENV_KEY + ": " + value);
  }
  return static_cast<int>(interval);
}

// -----------------------------------------------------------------------------
// Perf counter control
// -----------------------------------------------------------------------------

void resetAndEnable(std::vector<RaplDomain> &domains) {
  /*
   * Reset everything first.
   *
   * This makes the starting point of all counters as close
   * together as possible.
   */
  for (auto &domain : domains) {
    for (auto &counter : domain.counters) {
      if (ioctl(counter.fd, PERF_EVENT_IOC_RESET, 0) == -1) {
        const int error = errno;
        throw std::runtime_error("PERF_EVENT_IOC_RESET failed for " + domain.name + " on CPU " +
                                 std::to_string(counter.cpu) + ": " + std::strerror(error));
      }
    }
  }
  /*
   * Then enable everything.
   */
  for (auto &domain : domains) {
    for (auto &counter : domain.counters) {
      if (ioctl(counter.fd, PERF_EVENT_IOC_ENABLE, 0) == -1) {
        const int error = errno;
        throw std::runtime_error("PERF_EVENT_IOC_ENABLE failed for " + domain.name + " on CPU " +
                                 std::to_string(counter.cpu) + ": " + std::strerror(error));
      }
    }
  }
}

void disableCounters(std::vector<RaplDomain> &domains) {
  for (auto &domain : domains) {
    for (auto &counter : domain.counters) {
      if (counter.fd >= 0) {
        ioctl(counter.fd, PERF_EVENT_IOC_DISABLE, 0);
      }
    }
  }
}

// -----------------------------------------------------------------------------
// Sampler
// -----------------------------------------------------------------------------

void runSampler(const std::string &outputPath, int electionFd, int interval) {
  /*
   * Protect against the small fork()/parent-death race.
   */
  const pid_t parentPid = getppid();
  if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0) {
    throw std::runtime_error(std::string("prctl(PR_SET_PDEATHSIG) failed: ") +
                             std::strerror(errno));
  }
  if (getppid() != parentPid) {
    close(electionFd);
    std::_Exit(EXIT_SUCCESS);
  }
  /*
   * The election socket is no longer needed by the sampler.
   *
   * The important lifetime property is that the parent closes
   * its copy after fork, so the socket disappears when the
   * sampler dies.
   */
  close(electionFd);
  auto domains = findRaplDomains();
  std::filesystem::create_directories(outputPath);
  const std::string hostname = getHostname();
  const std::string metadataPath = outputPath + "/rapl-" + hostname + ".json";
  const std::string energyPath = outputPath + "/energy-" + hostname + ".csv";
  writeDomainMetadata(metadataPath, domains);
  resetAndEnable(domains);
  std::ofstream output(energyPath);
  if (!output) {
    disableCounters(domains);
    closeDomains(domains);
    throw std::runtime_error("Could not open output file: " + energyPath);
  }
  /*
   * Header.
   *
   * These are raw cumulative perf counter values.
   */
  output << "timestamp_ms";
  for (const auto &domain : domains) {
    output << "," << domain.name << "_energy";
  }
  output << '\n';
  output.flush();
  /*
   * Use an absolute monotonic schedule instead of:
   *
   *     read();
   *     sleep(interval);
   *
   * The latter causes the read/CSV overhead to accumulate as
   * sampling drift.
   */
  timespec nextWake{};
  if (clock_gettime(CLOCK_MONOTONIC, &nextWake) != 0) {
    disableCounters(domains);
    closeDomains(domains);
    throw std::runtime_error(std::string("clock_gettime failed: ") + std::strerror(errno));
  }
  const int64_t intervalNs = static_cast<int64_t>(interval) * 1000000LL;
  while (!g_stop) {
    timespec now{};
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
      const int error = errno;
      disableCounters(domains);
      closeDomains(domains);
      throw std::runtime_error(std::string("clock_gettime(CLOCK_REALTIME) failed: ") +
                               std::strerror(error));
    }
    const int64_t timestampMs =
        static_cast<int64_t>(now.tv_sec) * 1000LL + static_cast<int64_t>(now.tv_nsec / 1000000);
    const auto values = readEnergy(domains);
    /*
     * Avoid std::endl here.
     *
     * We explicitly flush once per sample.
     */
    output << timestampMs;
    for (const uint64_t value : values) {
      output << "," << value;
    }
    output << '\n';
    output.flush();
    /*
     * Schedule the next sample relative to the original
     * schedule, not relative to the completion of this sample.
     */
    nextWake.tv_sec += intervalNs / 1000000000LL;
    nextWake.tv_nsec += intervalNs % 1000000000LL;
    if (nextWake.tv_nsec >= 1000000000L) {
      ++nextWake.tv_sec;
      nextWake.tv_nsec -= 1000000000L;
    }
    while (!g_stop) {
      const int result = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &nextWake, nullptr);
      if (result == 0)
        break;
      if (result == EINTR)
        continue;
      disableCounters(domains);
      closeDomains(domains);
      throw std::runtime_error(std::string("clock_nanosleep failed: ") + std::strerror(result));
    }
  }
  disableCounters(domains);
  closeDomains(domains);
}

// -----------------------------------------------------------------------------
// Start sampler
// -----------------------------------------------------------------------------

void startSampler(const std::string &outputPath, int electionFd, int interval) {
  std::cerr << "Starting sampler for host " << getHostname() << " (PID " << getpid() << ")"
            << std::endl;
  const pid_t pid = fork();
  if (pid == -1) {
    throw std::runtime_error(std::string("fork() failed: ") + std::strerror(errno));
  }
  if (pid == 0) {
    try {
      runSampler(outputPath, electionFd, interval);
      std::_Exit(EXIT_SUCCESS);
    } catch (const std::exception &e) {
      std::cerr << BIN_NAME << " sampler: " << e.what() << '\n';
      /*
       * Do not call exit() here because this process has
       * inherited the parent's stdio state.
       */
      std::_Exit(EXIT_FAILURE);
    }
  }
  /*
   * Parent must close its copy of the election socket.
   */
  close(electionFd);
}

// -----------------------------------------------------------------------------
// Execute application
// -----------------------------------------------------------------------------

[[noreturn]]
void startApplication(int argc, char **argv) {
  execvp(argv[0], &argv[0]);
  std::cerr << "execvp(" << argv[0] << ") failed: " << std::strerror(errno) << std::endl;
  std::exit(127);
}

// -----------------------------------------------------------------------------
// Arguments
// -----------------------------------------------------------------------------

struct Arguments {
  std::string outputPath;
  int applicationIndex;
};

void printUsage(const char *program) {
  std::cerr << "Usage:\n"
            << "  [MPI_LAUNCHER] " << program << " -o PATH -- APPLICATION [ARGS...]\n"
            << "  [MPI_LAUNCHER] " << program << " --output PATH -- APPLICATION [ARGS...]\n"
            << "\n"
            << "Options:\n"
            << "  -o, --output PATH    "
               "Path for energy measurements\n"
            << "  --                   "
               "End "
            << program << " options\n"
            << "\n"
            << "Environment:\n"
            << "  " << PRAPLR_INTERVAL_MS_ENV_KEY << "   RAPL sampling interval in milliseconds\n"
            << "                       (default: " << PRAPLR_INTERVAL_MS_DEFAULT << ")\n";
}

Arguments parseArguments(int argc, char **argv) {
  if (argc < 5 || (std::string(argv[1]) != "-o" && std::string(argv[1]) != "--output") ||
      std::string(argv[3]) != "--") {
    printUsage(BIN_NAME);
    throw std::runtime_error("usage");
  }
  return {std::string(argv[2]), 4};
}

// -----------------------------------------------------------------------------
// main
// -----------------------------------------------------------------------------

int main(int argc, char **argv) {
  /*
   * SIGTERM is used by PR_SET_PDEATHSIG.
   *
   * SIGINT also makes manual termination clean.
   */
  std::signal(SIGTERM, handleSignal);
  std::signal(SIGINT, handleSignal);
  /*
   * Ignore SIGPIPE. The sampler doesn't normally use a pipe,
   * but this makes accidental pipe-related termination harmless.
   */
  std::signal(SIGPIPE, SIG_IGN);
  try {
    const Arguments args = parseArguments(argc, argv);
    const std::string outputPath = absolutePath(args.outputPath);
    const int electionFd = shouldSample(outputPath);
    if (electionFd) {
      /*
       * Do not sleep here.
       *
       * The abstract UNIX socket election is atomic: once bind()
       * succeeds, this rank is the sole sampler for this output
       * directory.
       */
      startSampler(outputPath, electionFd, getIntervalMs());
    }
    /*
     * Strip the wrapper from argv and execute the application.
     */
    startApplication(argc - args.applicationIndex, &argv[args.applicationIndex]);
  } catch (const std::exception &e) {
    std::cerr << "\n" << BIN_NAME << ": " << e.what() << std::endl;
    return EXIT_FAILURE;
  }
}