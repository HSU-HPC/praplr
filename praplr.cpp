#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <cstdint>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

#include "praplr.h"
#if defined(RAPL_METHOD_SYSFS) == defined(RAPL_METHOD_PERF)
#error "Either RAPL_METHOD_SYSFS or RAPL_METHOD_PERF must be defined"
#endif

#ifdef RAPL_METHOD_PERF
#include <linux/perf_event.h>
#endif

#define BIN_NAME "praplr"
#define PRAPLR_INTERVAL_MS_ENV_KEY "PRAPLR_INTERVAL_MS"
#define PRAPLR_INTERVAL_MS_DEFAULT 500

namespace fs = std::filesystem;

static volatile std::sig_atomic_t isStopped = 0;

void handleSignal(int) {
  isStopped = 1;
}

std::string getHostname() {
  char hostname[256];
  if (gethostname(hostname, sizeof(hostname)) != 0)
    throw std::runtime_error(std::string("gethostname failed: ") + std::strerror(errno));
  hostname[sizeof(hostname) - 1] = '\0';
  return hostname;
}

uint64_t hashFNV(const std::string &str) {
  uint64_t hash = 0xcbf29ce484222325;
  constexpr uint64_t magicPrime = 0x00000100000001b3;
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
    if (!fs::is_directory(outputPath))
      throw std::runtime_error("output path exists but is not a directory: " + path);
  } else {
    fs::create_directories(outputPath);
  }
  return fs::canonical(outputPath).string();
}

inline std::string getInstanceName(const std::string &outputPath) {
    return std::string(BIN_NAME) + ":" + hashPath(outputPath);
}

inline std::string getLockPath(const std::string &outputPath) {
    return "/dev/shm/" + getInstanceName(outputPath) + ".lock";
}

int shouldSample(const std::string &outputPath) {
  std::string socketName = getInstanceName(outputPath);
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd == -1)
    throw std::runtime_error(std::string("socket() failed: ") + std::strerror(errno));
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  // Abstract namespace: first byte is '\0'.
  std::memcpy(address.sun_path + 1, socketName.data(), socketName.size());
  const socklen_t addressLength = offsetof(sockaddr_un, sun_path) + 1 + socketName.size();
  if (bind(fd, reinterpret_cast<sockaddr *>(&address), addressLength) == 0) {
    // Ranks selected as sampler.
    // Keep fd open for now.
    // Its lifetime will eventually be tied to the sampler/application.
    return fd;
  }
  if (errno == EADDRINUSE) {
    close(fd);
    return 0;
  }
  int error = errno;
  close(fd);
  throw std::runtime_error(std::string("bind() failed: ") + std::strerror(error));
}

std::string readFile(const fs::path &path) {
  std::ifstream file(path);
  if (!file)
    throw std::runtime_error("Could not open " + path.string());
  std::string value;
  std::getline(file, value);
  if (!file && !file.eof())
    throw std::runtime_error("Could not read " + path.string());
  return value;
}

#ifdef RAPL_METHOD_PERF
struct RaplCounter {
  int cpu = -1;
  int fd = -1;
};
#endif

struct RaplDomain {
  std::string name;
#ifdef RAPL_METHOD_SYSFS
  fs::path energyPath;
  uint64_t maxEnergy;
#else
  fs::path eventPath;
  std::string unit = "J";
  std::string scale = "1.0";
  uint64_t config;
  std::vector<RaplCounter> counters;
#endif
};

#ifdef RAPL_METHOD_SYSFS
std::vector<RaplDomain> findRaplDomains() {
  std::vector<RaplDomain> domains;
  const fs::path base = "/sys/class/powercap/intel-rapl";
  if (!fs::exists(base))
    throw std::runtime_error("RAPL powercap directory not found: " + base.string());
  for (const auto &entry : fs::recursive_directory_iterator(base)) {
    if (entry.is_symlink() || !entry.is_directory())
        continue;
    const fs::path domain = entry.path();
    const fs::path energyPath = domain / "energy_uj";
    const fs::path maxEnergyPath = domain / "max_energy_range_uj";
    const fs::path namePath = domain / "name";
    // Detect RAPL domain by exposed objects
    if (!fs::is_regular_file(energyPath) || !fs::is_regular_file(maxEnergyPath) ||
        !fs::is_regular_file(namePath)) {
      continue;
    }
    const auto namePrefix = domain.filename().string();
    const auto name = namePrefix + "_" + readFile(namePath);
    domains.push_back({name, energyPath, std::stoull(readFile(maxEnergyPath))});
  }
  if (domains.empty())
    throw std::runtime_error("No Intel RAPL domains found under " + base.string());
  return domains;
}
#endif

#ifdef RAPL_METHOD_PERF
void resetAndEnableDomains(std::vector<RaplDomain> &domains) {
  // Reset all
  for (auto &domain : domains)
    for (auto &counter : domain.counters) {
      if (ioctl(counter.fd, PERF_EVENT_IOC_RESET, 0) == -1) {
        const int error = errno;
        throw std::runtime_error("PERF_EVENT_IOC_RESET failed for " + domain.name + " on CPU " +
                                 std::to_string(counter.cpu) + ": " + std::strerror(error));
      }
    }
  // Enable all
  for (auto &domain : domains)
    for (auto &counter : domain.counters) {
      if (ioctl(counter.fd, PERF_EVENT_IOC_ENABLE, 0) == -1) {
        const int error = errno;
        throw std::runtime_error("PERF_EVENT_IOC_ENABLE failed for " + domain.name + " on CPU " +
                                 std::to_string(counter.cpu) + ": " + std::strerror(error));
      }
    }
}

template <typename F> struct ScopeExit {
  F fn;
  ~ScopeExit() noexcept {
    fn();
  }
};

void disableCountersAndCloseDomains(std::vector<RaplDomain> &domains) {
  // Disable counters
  for (auto &domain : domains)
    for (auto &counter : domain.counters) {
      if (counter.fd >= 0)
        ioctl(counter.fd, PERF_EVENT_IOC_DISABLE, 0);
    }
  // Close domains
  for (auto &domain : domains)
    for (auto &counter : domain.counters) {
      if (counter.fd < 0)
        continue;
      close(counter.fd);
      counter.fd = -1;
    }
}

std::vector<int> readCpuMask(const fs::path cpumask) {
  std::vector<int> cpus;
  std::stringstream ss(readFile(cpumask));
  std::string part;
  while (std::getline(ss, part, ',')) {
    auto dash = part.find('-');
    if (dash == std::string::npos) {
      cpus.push_back(std::stoi(part));
    } else {
      int start = std::stoi(part.substr(0, dash));
      int end = std::stoi(part.substr(dash + 1));
      for (int i = start; i <= end; ++i)
        cpus.push_back(i);
    }
  }
  return cpus;
}

std::vector<RaplDomain> findRaplDomains() {
  std::vector<RaplDomain> domains;
  const fs::path base = "/sys/bus/event_source/devices/power";
  if (!fs::exists(base))
    throw std::runtime_error("perf power PMU not found: " + base.string());
  const fs::path eventsPath = base / "events";
  if (!fs::exists(eventsPath)) {
    throw std::runtime_error("perf power PMU events directory not found: " + eventsPath.string());
  }
  const fs::path typePath = base / "type";
  if (!fs::exists(typePath)) {
    throw std::runtime_error("perf power PMU type file not found: " + typePath.string());
  }
  const uint32_t pmuType = std::stoul(readFile(typePath));
  const fs::path cpumaskPath = base / "cpumask";
  if (!fs::exists(cpumaskPath)) {
    throw std::runtime_error("perf power PMU cpumask not found: " + cpumaskPath.string());
  }
  const std::vector<int> socketCpus = readCpuMask(cpumaskPath);
  for (const auto &entry : fs::directory_iterator(eventsPath)) {
    if (!entry.is_regular_file())
      continue;
    const std::string eventName = entry.path().filename().string();
    if (eventName.rfind("energy-", 0) != 0 || eventName.ends_with(".scale") ||
        eventName.ends_with(".unit")) {
      continue;
    }
    const std::string eventDescription = readFile(entry.path());
    const std::string configString = eventDescription.substr(6);
    uint64_t config = std::stoull(configString, nullptr, 0); // Hex format
    RaplDomain domain;
    domain.name = eventName;
    domain.eventPath = entry.path();
    domain.config = config;
    const fs::path scalePath = eventsPath / (eventName + ".scale");
    if (fs::exists(scalePath))
      domain.scale = readFile(scalePath);
    const fs::path unitPath = eventsPath / (eventName + ".unit");
    if (fs::exists(unitPath))
      domain.unit = readFile(unitPath);
    for (const int cpu : socketCpus) {
      struct perf_event_attr attr {};
      attr.type = pmuType;
      attr.size = sizeof(attr);
      attr.config = config;
      attr.disabled = 1; // Jointly enable later
      // Measure system (not just process) for comparability to sysfs method:
      // (s. https://man7.org/linux/man-pages/man2/perf_event_open.2.html)
      attr.exclude_kernel = 0;
      attr.exclude_hv = 0;
      const int fd = syscall(__NR_perf_event_open, &attr,
                             -1, // pid
                             cpu,
                             -1, // groupFd
                             0   // flags
      );
      if (fd == -1) {
        std::cerr << "perf_event_open(" << eventName << ", cpu " << cpu
                  << "): " << std::strerror(errno) << std::endl;
        continue;
      }
      domain.counters.push_back({cpu, fd});
    }
    if (!domain.counters.empty())
      domains.push_back(domain);
  }
  if (domains.empty())
    throw std::runtime_error("No usable perf RAPL energy events found under " + base.string());
  return domains;
}
#endif

void writeDomainMetadata(const std::string &path, const std::vector<RaplDomain> &domains) {
  std::ofstream output(path);
  if (!output)
    throw std::runtime_error("Could not open metadata file: " + path);
  output << "{\n";
  output << "  \"hostname\": \"" << getHostname() << "\",\n";
  output << "  \"method\": \"" <<
#ifdef RAPL_METHOD_SYSFS
      "sysfs"
#else
      "perf"
#endif
         << "\",\n";
  output << "  \"domains\": [\n";
  for (size_t i = 0; i < domains.size(); ++i) {
    const auto &domain = domains[i];
    output << "    {\n";
    output << "      \"name\": \"" << domain.name << "\",\n";
#ifdef RAPL_METHOD_SYSFS
    output << "      \"energy_path\": \"" << domain.energyPath.string() << "\",\n";
    output << "      \"max_energy_range_uj\": " << domain.maxEnergy << "\n";
#else
    output << "      \"event_path\": \"" << domain.eventPath.string() << "\",\n";
    output << "      \"config\": " << domain.config << ",\n";
    output << "      \"unit\": \"" << domain.unit << "\",\n";
    output << "      \"scale\": " << domain.scale << ",\n";
    output << "      \"cpus\": [";
    for (size_t j = 0; j < domain.counters.size(); ++j) {
      if (j != 0)
        output << ", ";
      output << domain.counters[j].cpu;
    }
    output << "]\n";
#endif
    output << "    }";
    if (i + 1 != domains.size())
      output << ",";
    output << "\n";
  }
  output << "  ]\n";
  output << "}\n";
}

std::vector<uint64_t> readEnergy(const std::vector<RaplDomain> &domains) {
  std::vector<uint64_t> values;
  values.reserve(domains.size());
  for (const auto &domain : domains) {
#ifdef RAPL_METHOD_SYSFS
    values.push_back(std::stoull(readFile(domain.energyPath)));
#else
    uint64_t total = 0;
    for (const auto &counter : domain.counters) {
      uint64_t value = 0;
      const ssize_t result = read(counter.fd, &value, sizeof(value));
      if (result != static_cast<ssize_t>(sizeof(value))) {
        const int error = errno;
        throw std::runtime_error("Could not read perf counter for " + domain.name + " on CPU " +
                                 std::to_string(counter.cpu) + ": " + std::strerror(error));
      }
      total += value;
    }
    values.push_back(total);
#endif
  }
  return values;
}

int getIntervalMs() {
  const char *value = std::getenv(PRAPLR_INTERVAL_MS_ENV_KEY);
  if (value == nullptr)
    return PRAPLR_INTERVAL_MS_DEFAULT;
  char *end = nullptr;
  errno = 0;
  long interval = std::strtol(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || interval <= 0)
    throw std::runtime_error(std::string("Invalid ") + PRAPLR_INTERVAL_MS_ENV_KEY + ": " + value);
  return static_cast<int>(interval);
}

constexpr uint64_t NsPerSec = 1000000000ULL;
constexpr uint64_t NsPerMs = NsPerSec / 1000;

uint64_t getTimestampNs(clockid_t clock) {
  timespec ts;
  if (clock_gettime(clock, &ts) != 0) {
    throw std::runtime_error(std::string("clock_gettime failed: ") + std::strerror(errno));
  }
  return static_cast<uint64_t>(ts.tv_sec) * NsPerSec + ts.tv_nsec;
}

void sleepUntil(uint64_t timestampMonotonicNs) {
  timespec ts{static_cast<time_t>(timestampMonotonicNs / NsPerSec),
              static_cast<long>(timestampMonotonicNs % NsPerSec)};
  int result;
  do {
    if (isStopped)
      return;
    result = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
  } while (result == EINTR);
  if (result != 0)
    throw std::runtime_error(std::string("clock_nanosleep failed: ") + std::strerror(result));
}

int acquireSharedLock(const std::string &outputPath) {
  const std::string path = getLockPath(outputPath);
  int fd = open(path.c_str(), O_RDONLY | O_CREAT, 0666);   // no O_CLOEXEC: must survive exec
  if (fd == -1 || flock(fd, LOCK_SH) != 0)
    throw std::runtime_error(std::string("shared lock failed: ") + std::strerror(errno));
  return fd;
}

void runSampler(const std::string &outputPath, int intervalMs) {
  const pid_t parentPid = getppid();
  if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0) {
    throw std::runtime_error(std::string("prctl(PR_SET_PDEATHSIG) failed: ") +
                             std::strerror(errno));
  }
  if (getppid() != parentPid) std::_Exit(EXIT_SUCCESS);
  auto domains = findRaplDomains();
  std::sort(domains.begin(), domains.end(),
            [](const RaplDomain &a, const RaplDomain &b) { return a.name < b.name; });
  for (size_t i = 1; i < domains.size(); ++i)
    if (domains[i].name == domains[i - 1].name)
      throw std::runtime_error("Duplicate RAPL domain name: " + domains[i].name);
#ifdef RAPL_METHOD_PERF
  ScopeExit cleanup([&]() { disableCountersAndCloseDomains(domains); });
  resetAndEnableDomains(domains);
#endif
  std::filesystem::create_directories(outputPath);
  const auto hostname = getHostname();
  writeDomainMetadata(outputPath + "/rapl-" + hostname + ".json", domains);
  std::ofstream output(outputPath + "/energy-" + hostname + ".csv");
  if (!output) {
    throw std::runtime_error("Could not open output file");
  }
  // Header.
  output << "timestamp_ms";
  for (const auto &domain : domains)
    output << "," << domain.name << "_energy";
  output << '\n';
  output.flush();
  const std::string lockPath = getLockPath(outputPath);
  const int probeFd = open(lockPath.c_str(), O_RDONLY);
  if (probeFd == -1)
    throw std::runtime_error("Could not open lock file: " + std::string(std::strerror(errno)));
  // Measurements.
  const uint64_t intervalNs = intervalMs * NsPerMs;
  uint64_t nextWakeNs = getTimestampNs(CLOCK_MONOTONIC);
  for (size_t i = 0; !isStopped; i++) {
    if (
        i > 0 // At least one measurement was taken
        &&
        flock(probeFd, LOCK_EX | LOCK_NB) == 0 // No other processes are found
    ){
        break;
    }
    std::ostringstream row;
    uint64_t measurementNs = getTimestampNs(CLOCK_REALTIME);
    row << measurementNs / NsPerMs;
    const auto values = readEnergy(domains);
    for (const auto value : values)
      row << "," << value;
    row << '\n';
    output << row.str();
    output.flush();
    nextWakeNs += intervalNs;
    sleepUntil(nextWakeNs);
  }
  // Re-lock just to be sure
  if (flock(probeFd, LOCK_EX | LOCK_NB) == 0)
    unlink(lockPath.c_str());
  close(probeFd);
}

void startSampler(const std::string &outputPath, int electionFd, int lockFd, int intervalMs) {
  std::cerr << "Starting sampler for host " << getHostname() << " (PID " << getpid() << ")"
            << std::endl;
  pid_t pid = fork();
  if (pid == -1) {
    throw std::runtime_error(std::string("fork() failed: ") + std::strerror(errno));
  }
  if (pid == 0) {
      close(lockFd);
    int status = EXIT_SUCCESS;
    try {
      runSampler(outputPath, intervalMs);
    } catch (const std::exception &e) {
      std::cerr << BIN_NAME << " sampler: " << e.what() << '\n';
      status = EXIT_FAILURE;
    }
    std::_Exit(status);
  }
  // IMPORTANT:
  // The parent must close its copy of electionFd here.
  close(electionFd);
}

[[noreturn]]
void startApplication(char **argv) {
std::signal(SIGPIPE, SIG_DFL);
  execvp(argv[0], &argv[0]);
  // Only reached if execvp failed.
  std::cerr << "execvp(" << argv[0] << ") failed: " << std::strerror(errno) << std::endl;
  std::exit(127);
}

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
            << "  -o, --output PATH    Path for energy measurements\n"
            << "  --                   End " << program << " options\n"
            << "\n"
            << "Environment:\n"
            << "  " << PRAPLR_INTERVAL_MS_ENV_KEY << "   RAPL sampling interval in milliseconds\n"
            << "                       (default: " << PRAPLR_INTERVAL_MS_DEFAULT << ")\n";
}

Arguments parseArguments(int argc, char **argv) {
  if (argc < 5 or (std::string(argv[1]) != "-o" and std::string(argv[1]) != "--output") or
      std::string(argv[3]) != "--") {
    printUsage(BIN_NAME); // argv[0] may be a very long path
    throw std::runtime_error("usage");
  }
  return {std::string(argv[2]), 4};
}

int main(int argc, char **argv) {
  std::signal(SIGTERM, handleSignal);
  std::signal(SIGINT, handleSignal);
  std::signal(SIGPIPE, SIG_IGN); // Avoid pipe-related termination
  constexpr int EXIT_STOPPED = 130;
  try {
    Arguments args = parseArguments(argc, argv);
    std::string outputPath = absolutePath(args.outputPath);
    const int lockFd = acquireSharedLock(outputPath);
    int electionFd = shouldSample(outputPath);
    if (electionFd) {
      if (isStopped) {
        close(electionFd);
        return EXIT_STOPPED;
      }
      startSampler(outputPath, electionFd, lockFd, getIntervalMs());
    }
    if (isStopped)
      return EXIT_STOPPED;
    // Strip wrapper from args<w
    startApplication(&argv[args.applicationIndex]);
  } catch (const std::exception &e) {
    std::cerr << "\n" << BIN_NAME << ": " << e.what() << std::endl;
    return EXIT_FAILURE;
  }
}
