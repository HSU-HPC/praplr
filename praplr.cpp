#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#include <cstdint>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#define BIN_NAME "praplr"
#define PRAPLR_INTERVAL_MS_ENV_KEY "PRAPLR_INTERVAL_MS"
#define PRAPLR_INTERVAL_MS_DEFAULT 500

static constexpr const char *SOCKET_PREFIX = BIN_NAME;

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

int shouldSample(const std::string &outputPath) {
  std::string socketName = std::string(BIN_NAME) + ":" + hashPath(outputPath.c_str());
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

struct RaplZone {
  std::string name;
  std::string energyPath;
  uint64_t maxEnergy;
};

std::string readFile(const std::string &path) {
  std::ifstream file(path);
  if (!file)
    throw std::runtime_error("Could not open " + path);
  std::string value;
  std::getline(file, value);
  if (!file && !file.eof())
    throw std::runtime_error("Could not read " + path);
  return value;
}

std::vector<RaplZone> findRaplZones() {
  const std::string base = "/sys/class/powercap/intel-rapl";
  std::vector<RaplZone> zones;
  for (int package = 0;; ++package) {
    const std::string zone = base + "/intel-rapl:" + std::to_string(package);
    const std::string energyPath = zone + "/energy_uj";
    const std::string maxEnergyPath = zone + "/max_energy_range_uj";
    if (access(energyPath.c_str(), R_OK) != 0)
      break;
    zones.push_back({readFile(zone + "/name"), energyPath, std::stoull(readFile(maxEnergyPath))});
  }
  if (zones.empty())
    throw std::runtime_error("No Intel RAPL zones found under " + base);
  return zones;
}

void writeZoneMetadata(const std::string &path, const std::vector<RaplZone> &zones) {
  std::ofstream output(path);
  if (!output)
    throw std::runtime_error("Could not open metadata file: " + path);
  output << "{\n";
  output << "  \"hostname\": \"" << getHostname() << "\",\n";
  output << "  \"zones\": [\n";
  for (size_t i = 0; i < zones.size(); ++i) {
    const auto &zone = zones[i];
    output << "    {\n";
    output << "      \"name\": \"" << zone.name << "\",\n";
    output << "      \"energy_path\": \"" << zone.energyPath << "\",\n";
    output << "      \"max_energy_range_uj\": " << zone.maxEnergy << "\n";
    output << "    }";
    if (i + 1 != zones.size())
      output << ",";
    output << "\n";
  }
  output << "  ]\n";
  output << "}\n";
}

std::vector<uint64_t> readEnergy(const std::vector<RaplZone> &zones) {
  std::vector<uint64_t> values;
  values.reserve(zones.size());
  for (const auto &zone : zones)
    values.push_back(std::stoull(readFile(zone.energyPath)));
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

void runSampler(const std::string &outputPath, int electionFd, int interval) {
  // Remember the parent so we can close the small fork()/prctl() race.
  const pid_t parentPid = getppid();
  if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0) {
    throw std::runtime_error(std::string("prctl(PR_SET_PDEATHSIG) failed: ") +
                             std::strerror(errno));
  }
  if (getppid() != parentPid) {
    // Parent died before we installed the death signal.
    close(electionFd);
    std::_Exit(EXIT_SUCCESS);
  }
  const auto zones = findRaplZones();
  std::filesystem::create_directories(outputPath);
  const auto hostname = getHostname();
  writeZoneMetadata(outputPath + "/rapl-" + hostname + ".json", zones);
  std::ofstream output(outputPath + "/energy-" + hostname + ".csv");
  if (!output) {
    throw std::runtime_error("Could not open output file");
  }
  // Header.
  output << "timestamp_ms";
  for (const auto &zone : zones)
    output << "," << zone.name << "_energy_uj";
  output << '\n';
  output.flush();
  // Measurements.
  while (true) {
    const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
    const auto values = readEnergy(zones);
    std::ostringstream row;
    row << timestamp;
    for (const auto value : values)
      row << "," << value;
    row << '\n';
    output << row.str();
    output.flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(interval));
  }
}

void startSampler(const std::string &outputPath, int electionFd, int interval) {
  std::cerr << "Starting sampler for host " << getHostname() << " (PID " << getpid() << ")"
            << std::endl;
  pid_t pid = fork();
  if (pid == -1) {
    throw std::runtime_error(std::string("fork() failed: ") + std::strerror(errno));
  }
  if (pid == 0) {
    try {
      runSampler(outputPath, electionFd, interval);
    } catch (const std::exception &e) {
      std::cerr << BIN_NAME << " sampler: " << e.what() << '\n';
    }
    std::_Exit(EXIT_FAILURE);
  }
  // IMPORTANT:
  // The parent must close its copy of electionFd here.
  close(electionFd);
}

[[noreturn]]
void startApplication(int argc, char **argv) {
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
  try {
    Arguments args = parseArguments(argc, argv);
    std::string outputPath = absolutePath(args.outputPath);
    int electionFd = shouldSample(outputPath);
    if (electionFd) {
      // Give the other MPI ranks time to reach the election.
      // (Required in case the application exists immediately.)
      std::this_thread::sleep_for(std::chrono::seconds(1));
      startSampler(outputPath, electionFd, getIntervalMs());
    }
    // Strip wrapper from args
    startApplication(argc - args.applicationIndex, &argv[args.applicationIndex]);
  } catch (const std::exception &e) {
    std::cerr << "\n" << BIN_NAME << ": " << e.what() << std::endl;
    return EXIT_FAILURE;
  }
}
