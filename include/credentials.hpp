#pragma once
#include <string>
#include <stdexcept>
#include <cstdlib>

namespace v2 {

struct Credentials {
    std::string api_key;
    std::string secret_key;
};

/// Load Binance credentials from environment variables.
/// Throws std::runtime_error if either variable is not set or is empty.
///
/// On Windows, set permanently via PowerShell (User scope):
///   [System.Environment]::SetEnvironmentVariable("BINANCE_API_KEY",    "...", "User")
///   [System.Environment]::SetEnvironmentVariable("BINANCE_SECRET_KEY", "...", "User")
///
/// Or for a single shell session only:
///   $env:BINANCE_API_KEY    = "your_key"
///   $env:BINANCE_SECRET_KEY = "your_secret"
///
/// Note: a .env file in the project root is also supported via load_dotenv()
/// (called at the top of main) as a developer convenience — it must NEVER be
/// committed to version control (.gitignore already excludes .env*).
inline Credentials load_credentials() {
    const char* key    = std::getenv("BINANCE_API_KEY");
    const char* secret = std::getenv("BINANCE_SECRET_KEY");

    if (!key || std::string(key).empty()) {
        throw std::runtime_error(
            "BINANCE_API_KEY environment variable is not set.\n"
            "Set it with: $env:BINANCE_API_KEY = \"your_key\""
        );
    }
    if (!secret || std::string(secret).empty()) {
        throw std::runtime_error(
            "BINANCE_SECRET_KEY environment variable is not set.\n"
            "Set it with: $env:BINANCE_SECRET_KEY = \"your_secret\""
        );
    }

    return { std::string(key), std::string(secret) };
}

} // namespace v2
