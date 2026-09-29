#include "dedicated_adapter.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>

namespace barista::linux_service
{
namespace
{
constexpr const char* OwnedMarker = "# Managed exclusively by Barista.\n";

std::string Trim(std::string value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    const auto begin = std::find_if(value.begin(), value.end(), [](unsigned char ch) { return !std::isspace(ch); });
    value.erase(value.begin(), begin);
    return value;
}

CommandResult DefaultRunner(const std::vector<std::string>& args)
{
    if (args.empty()) return {};
    int pipes[2];
    if (pipe(pipes) != 0) return {-1, std::strerror(errno)};
    const pid_t pid = fork();
    if (pid == 0)
    {
        dup2(pipes[1], STDOUT_FILENO);
        dup2(pipes[1], STDERR_FILENO);
        close(pipes[0]); close(pipes[1]);
        std::vector<char*> argv;
        for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    close(pipes[1]);
    std::string output;
    std::array<char, 512> buffer{};
    for (ssize_t count; (count = read(pipes[0], buffer.data(), buffer.size())) > 0;)
        output.append(buffer.data(), static_cast<size_t>(count));
    close(pipes[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return {WIFEXITED(status) ? WEXITSTATUS(status) : -1, Trim(output)};
}
}

DedicatedAdapterManager::DedicatedAdapterManager(CommandRunner runner, std::string statePath,
    std::string networkManagerPath)
    : m_runner(runner ? std::move(runner) : DefaultRunner), m_statePath(std::move(statePath)),
      m_networkManagerPath(std::move(networkManagerPath))
{
}

std::string DedicatedAdapterManager::NormalizeMac(std::string mac)
{
    mac = Trim(std::move(mac));
    std::transform(mac.begin(), mac.end(), mac.begin(), [](unsigned char ch) { return std::tolower(ch); });
    if (!std::regex_match(mac, std::regex("^[0-9a-f]{2}(:[0-9a-f]{2}){5}$"))) return {};
    if (mac == "00:00:00:00:00:00" || (std::stoi(mac.substr(0, 2), nullptr, 16) & 1)) return {};
    return mac;
}

std::string DedicatedAdapterManager::GenerateNetworkManagerConfig(const std::string& permanentMac)
{
    const auto mac = NormalizeMac(permanentMac);
    if (mac.empty()) return {};
    return std::string(OwnedMarker) + "[device-barista-dedicated]\nmatch-device=mac:" + mac + "\nmanaged=0\n";
}

bool DedicatedAdapterManager::ParseState(const std::string& text, DedicatedAdapterState& state)
{
    std::istringstream input(text);
    std::string line, version, mac;
    while (std::getline(input, line))
    {
        if (line.rfind("version=", 0) == 0) version = Trim(line.substr(8));
        if (line.rfind("permanent_mac=", 0) == 0) mac = NormalizeMac(line.substr(14));
    }
    if (version != "1" || mac.empty()) return false;
    state.permanentMac = mac;
    return true;
}

DedicatedAdapterState DedicatedAdapterManager::State() const
{
    std::ifstream input(m_statePath);
    std::ostringstream contents;
    contents << input.rdbuf();
    DedicatedAdapterState state;
    if (!input && contents.str().empty()) return {};
    if (!ParseState(contents.str(), state)) return {};
    return state;
}

CommandResult DedicatedAdapterManager::Run(const std::vector<std::string>& args) const { return m_runner(args); }

std::string DedicatedAdapterManager::PermanentMac(const std::string& interfaceName) const
{
    if (!std::regex_match(interfaceName, std::regex("^[A-Za-z0-9_.:-]{1,32}$"))) return {};
    const auto result = Run({"ethtool", "-P", interfaceName});
    if (result.exitCode != 0) return {};
    const auto colon = result.output.find(':');
    return colon == std::string::npos ? std::string{} : NormalizeMac(result.output.substr(colon + 1));
}

std::string DedicatedAdapterManager::InterfaceForMac(const std::string& mac) const
{
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/sys/class/net", ec))
        if (PermanentMac(entry.path().filename().string()) == mac) return entry.path().filename().string();
    return {};
}

bool DedicatedAdapterManager::Matches(const std::string& interfaceName) const
{
    const auto state = State();
    return !state.permanentMac.empty() && PermanentMac(interfaceName) == state.permanentMac;
}

bool DedicatedAdapterManager::WriteOwnedFile(const std::string& path, const std::string& contents, std::string& error) const
{
    std::ifstream existing(path);
    if (existing)
    {
        std::string firstLine;
        std::getline(existing, firstLine);
        if (firstLine != Trim(OwnedMarker)) { error = "Refusing to replace a file not owned by Barista: " + path; return false; }
    }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::string temporary = path + ".tmp.XXXXXX";
    std::vector<char> templateName(temporary.begin(), temporary.end());
    templateName.push_back('\0');
    const int fd = mkstemp(templateName.data());
    if (fd < 0) { error = "Could not create a safe temporary file for " + path; return false; }
    temporary = templateName.data();
    const char* data = contents.data();
    size_t remaining = contents.size();
    while (remaining > 0)
    {
        const ssize_t count = write(fd, data, remaining);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        data += count;
        remaining -= static_cast<size_t>(count);
    }
    const bool saved = remaining == 0 && fsync(fd) == 0 && fchmod(fd, 0644) == 0;
    close(fd);
    if (!saved) { std::filesystem::remove(temporary); error = "Could not write " + path; return false; }
    std::filesystem::rename(temporary, path, ec);
    if (ec) { std::filesystem::remove(temporary); error = "Could not install " + path + ": " + ec.message(); return false; }
    return true;
}

bool DedicatedAdapterManager::RemoveOwnedFile(const std::string& path, std::string& error) const
{
    std::ifstream input(path);
    if (!input) return true;
    std::string firstLine;
    std::getline(input, firstLine);
    if (firstLine != Trim(OwnedMarker)) { error = "Refusing to remove a file not owned by Barista: " + path; return false; }
    if (!std::filesystem::remove(path)) { error = "Could not remove " + path; return false; }
    return true;
}

bool DedicatedAdapterManager::IsUnmanaged(const std::string& interfaceName) const
{
    const auto result = Run({"nmcli", "-t", "-g", "GENERAL.STATE", "device", "show", interfaceName});
    return result.exitCode == 0 && (result.output.rfind("10", 0) == 0 || result.output.find("unmanaged") != std::string::npos);
}

std::string DedicatedAdapterManager::Create(const std::string& interfaceName)
{
    if (!State().permanentMac.empty()) return "Barista already has a dedicated adapter. Undo it before choosing another.";
    const auto mac = PermanentMac(interfaceName);
    if (mac.empty()) return "Could not determine the adapter's permanent hardware address.";
    if (Run({"ip", "route", "show", "default", "dev", interfaceName}).output.find("default") != std::string::npos)
        return "This adapter carries the computer's default network connection. Disconnect it or choose another adapter.";
    const auto connection = Run({"nmcli", "-t", "-g", "GENERAL.CONNECTION", "device", "show", interfaceName});
    if (connection.exitCode == 0 && !connection.output.empty() && connection.output != "--")
        return "This adapter has an active network connection. Disconnect it before making it dedicated.";

    std::string error;
    if (!WriteOwnedFile(m_networkManagerPath, GenerateNetworkManagerConfig(mac), error)) return error;
    const auto rollback = [this, &interfaceName] {
        std::string ignored;
        RemoveOwnedFile(m_networkManagerPath, ignored);
        Run({"nmcli", "general", "reload"});
        Run({"nmcli", "device", "set", interfaceName, "managed", "yes"});
    };
    if (Run({"nmcli", "general", "reload"}).exitCode != 0 ||
        Run({"nmcli", "device", "set", interfaceName, "managed", "no"}).exitCode != 0 || !IsUnmanaged(interfaceName))
    {
        rollback();
        return "NetworkManager did not release the adapter; no dedicated setting was kept.";
    }
    const std::string state = std::string(OwnedMarker) + "version=1\npermanent_mac=" + mac + "\n";
    if (!WriteOwnedFile(m_statePath, state, error)) { rollback(); return error; }
    return {};
}

std::string DedicatedAdapterManager::Undo()
{
    const auto state = State();
    if (state.permanentMac.empty()) return {};
    std::string error;
    if (!RemoveOwnedFile(m_networkManagerPath, error)) return error;
    const auto restoreReservation = [this, &state] {
        std::string ignored;
        WriteOwnedFile(m_networkManagerPath, GenerateNetworkManagerConfig(state.permanentMac), ignored);
        Run({"nmcli", "general", "reload"});
    };
    if (Run({"nmcli", "general", "reload"}).exitCode != 0)
    {
        restoreReservation();
        return "Could not reload NetworkManager after removing the dedicated setting.";
    }
    const auto interfaceName = InterfaceForMac(state.permanentMac);
    if (!interfaceName.empty())
    {
        if (Run({"nmcli", "device", "set", interfaceName, "managed", "yes"}).exitCode != 0 || IsUnmanaged(interfaceName))
        {
            restoreReservation();
            return "NetworkManager did not resume management of the adapter.";
        }
    }
    if (!RemoveOwnedFile(m_statePath, error)) return error;
    return {};
}
}
