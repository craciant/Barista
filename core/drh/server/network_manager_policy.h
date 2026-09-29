#pragma once

namespace barista::drh
{
constexpr bool ShouldManageAdapterWithNetworkManager(bool dedicatedAdapter)
{
    return !dedicatedAdapter;
}
}
