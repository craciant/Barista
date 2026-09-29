#include "dedicated_adapter.h"
#include "drh/server/network_manager_policy.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

using barista::linux_service::CommandResult;
using barista::linux_service::DedicatedAdapterManager;

namespace
{
bool Check(bool condition, const char* message)
{
    if (!condition) std::cerr << message << '\n';
    return condition;
}
}

int main()
{
    bool ok = true;
    barista::linux_service::DedicatedAdapterState state;
    ok &= Check(DedicatedAdapterManager::ParseState(
        "# Managed exclusively by Barista.\nversion=1\npermanent_mac=AA:BB:CC:DD:EE:02\n", state) &&
        state.permanentMac == "aa:bb:cc:dd:ee:02", "valid state parses and normalizes");
    ok &= Check(!DedicatedAdapterManager::ParseState("version=2\npermanent_mac=aa:bb:cc:dd:ee:02\n", state),
        "unknown state version is rejected");
    ok &= Check(DedicatedAdapterManager::GenerateNetworkManagerConfig("AA:BB:CC:DD:EE:02") ==
        "# Managed exclusively by Barista.\n[device-barista-dedicated]\nmatch-device=mac:aa:bb:cc:dd:ee:02\nmanaged=0\n",
        "NetworkManager config is stable and permanent-MAC keyed");
    ok &= Check(barista::drh::ShouldManageAdapterWithNetworkManager(false) &&
        !barista::drh::ShouldManageAdapterWithNetworkManager(true),
        "restore decision distinguishes ordinary and dedicated adapters");

    const auto directory = std::filesystem::temp_directory_path() /
        ("barista-dedicated-test-" + std::to_string(getpid()));
    std::filesystem::create_directories(directory);
    const auto statePath = (directory / "state").string();
    const auto nmPath = (directory / "nm.conf").string();
    int reloads = 0;
    auto failingRunner = [&reloads](const std::vector<std::string>& args) -> CommandResult {
        if (args.size() >= 3 && args[0] == "ethtool") return {0, "Permanent address: aa:bb:cc:dd:ee:02"};
        if (args.size() >= 3 && args[0] == "ip") return {0, ""};
        if (args.size() >= 5 && args[1] == "-t") return {0, "--"};
        if (args.size() >= 3 && args[1] == "general") { ++reloads; return {0, ""}; }
        if (args.size() >= 6 && args[1] == "device" && args.back() == "no") return {1, "failure"};
        return {0, ""};
    };
    DedicatedAdapterManager manager(failingRunner, statePath, nmPath);
    ok &= Check(!manager.Create("wlan0").empty(), "create reports a late failure");
    ok &= Check(!std::filesystem::exists(nmPath) && !std::filesystem::exists(statePath) && reloads == 2,
        "late create failure removes owned config and reloads NetworkManager");

    std::ofstream foreign(nmPath);
    foreign << "# administrator file\n";
    foreign.close();
    ok &= Check(manager.Create("wlan0").find("not owned") != std::string::npos,
        "administrator configuration is never overwritten");
    std::filesystem::remove_all(directory);
    return ok ? 0 : 1;
}
