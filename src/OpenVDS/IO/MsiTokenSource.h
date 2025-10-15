#pragma once

#include <string>

namespace OpenVDS
{
// Fetch an MSI access token from the Azure IMDS endpoint. Returns empty string
// on failure. This function performs a blocking HTTP call; keep callers
// synchronous.
std::string FetchMsiTokenFromImds();

}
