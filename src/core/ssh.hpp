#pragma once

#include "process.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ffmpegui {

enum class SshAuthMethod { PublicKey, Password };

struct SshServer {
    std::string alias;
    std::string hostName;
    std::string user;
    unsigned int port{22};
    std::string identityFile;
    std::string proxyJump;
    SshAuthMethod authMethod{SshAuthMethod::PublicKey};
};

class SshService {
public:
    SshService();
    std::string configPath() const;
    std::vector<SshServer> listServers() const;
    bool saveServer(const SshServer& server, const std::string& password, std::string& error);
    bool removeServer(const std::string& alias, std::string& error);
    bool hasPassword(const std::string& alias) const;
    bool setPassword(const std::string& alias, const std::string& password, std::string& error);
    bool clearPassword(const std::string& alias, std::string& error);
    std::optional<std::string> passwordForAskpass(const std::string& alias) const;
    bool testConnection(const std::string& alias, std::string& error) const;
    ProcessResult runRemote(const std::string& alias, const std::string& command,
                            const std::atomic_bool* cancelRequested = nullptr,
                            ProcessLineCallback onLine = {},
                            const std::atomic_bool* pauseRequested = nullptr) const;
    ProcessResult upload(const std::string& alias, const std::string& localPath,
                         const std::string& remotePath,
                         const std::atomic_bool* cancelRequested = nullptr) const;
    ProcessResult download(const std::string& alias, const std::string& remotePath,
                           const std::string& localPath,
                           const std::atomic_bool* cancelRequested = nullptr) const;
    std::string executablePath() const;
    static bool validAlias(const std::string& alias);
    static std::string shellQuote(std::string_view value);

private:
    std::optional<std::string> readPassword(const std::string& alias) const;
    bool writeConfig(const std::vector<std::string>& lines, std::string& error) const;
    ProcessEnvironment processEnvironment(const std::string& alias) const;
    std::vector<std::string> sshArguments(const std::string& alias, const std::string& command) const;
    std::vector<std::string> scpArguments(const std::string& alias, const std::string& source,
                                          const std::string& destination) const;
    std::string sshPath_;
    std::string scpPath_;
};

} // namespace ffmpegui
