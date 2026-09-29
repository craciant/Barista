#pragma once

#include <functional>
#include <string>
#include <vector>

namespace barista::linux_service
{
struct CommandResult
{
    int exitCode = -1;
    std::string output;
};

using CommandRunner = std::function<CommandResult(const std::vector<std::string>&)>;

struct DedicatedAdapterState
{
    std::string permanentMac;
};

class DedicatedAdapterManager
{
public:
    DedicatedAdapterManager(CommandRunner runner = {},
        std::string statePath = "/var/lib/barista/dedicated-adapter.conf",
        std::string networkManagerPath = "/etc/NetworkManager/conf.d/90-barista-dedicated-adapter.conf");

    static std::string NormalizeMac(std::string mac);
    static std::string GenerateNetworkManagerConfig(const std::string& permanentMac);
    static bool ParseState(const std::string& text, DedicatedAdapterState& state);
    DedicatedAdapterState State() const;
    std::string PermanentMac(const std::string& interfaceName) const;
    bool Matches(const std::string& interfaceName) const;
    std::string Create(const std::string& interfaceName);
    std::string Undo();

private:
    bool WriteOwnedFile(const std::string& path, const std::string& contents, std::string& error) const;
    bool RemoveOwnedFile(const std::string& path, std::string& error) const;
    bool IsUnmanaged(const std::string& interfaceName) const;
    std::string InterfaceForMac(const std::string& mac) const;
    CommandResult Run(const std::vector<std::string>& args) const;

    CommandRunner m_runner;
    std::string m_statePath;
    std::string m_networkManagerPath;
};
}
