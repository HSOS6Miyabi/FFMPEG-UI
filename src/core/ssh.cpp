#include "ssh.hpp"

#include "process.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincred.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace ffmpegui {
namespace {

std::mutex passwordMutex;
std::map<std::string, std::string> sessionPasswords;
std::atomic_uint64_t temporaryFileSequence{0};

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }

    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

// Parse the quoting and escaping rules used by common OpenSSH config directives.
std::vector<std::string> splitConfigWords(const std::string& line) {
    std::vector<std::string> result;
    std::string current;
    char quote = '\0';
    bool escaped = false;

    for (const char character : line) {
        if (escaped) {
            current.push_back(character);
            escaped = false;
            continue;
        }

        if (character == '\\' && quote != '\'') {
            escaped = true;
            continue;
        }

        if (quote != '\0') {
            if (character == quote) {
                quote = '\0';
            } else {
                current.push_back(character);
            }
            continue;
        }

        if (character == '\'' || character == '"') {
            quote = character;
            continue;
        }

        if (character == '#') {
            break;
        }

        if (std::isspace(static_cast<unsigned char>(character))) {
            if (!current.empty()) {
                result.push_back(std::move(current));
                current.clear();
            }
            continue;
        }

        current.push_back(character);
    }

    if (!current.empty()) {
        result.push_back(std::move(current));
    }

    return result;
}

std::filesystem::path pathFromUtf8(std::string_view value) {
    std::u8string encoded;
    encoded.reserve(value.size());
    for (const unsigned char character : value) {
        encoded.push_back(static_cast<char8_t>(character));
    }
    return std::filesystem::path(encoded);
}

std::string pathToUtf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

std::string credentialTarget(const std::string& alias) {
    return "FFmpegStudio:SSH:" + alias;
}

std::string quoteConfigValue(const std::string& value) {
    std::string result = "\"";
    for (const char character : value) {
        if (character == '\\' || character == '"') {
            result.push_back('\\');
        }
        result.push_back(character);
    }
    result.push_back('"');
    return result;
}

bool isManagedDirective(const std::string& key) {
    const std::string normalized = lower(key);
    return normalized == "hostname" ||
           normalized == "user" ||
           normalized == "port" ||
           normalized == "identityfile" ||
           normalized == "proxyjump" ||
           normalized == "preferredauthentications" ||
           normalized == "passwordauthentication" ||
           normalized == "kbdinteractiveauthentication" ||
           normalized == "identitiesonly";
}

std::vector<std::string> readConfigLines(const std::string& path) {
    std::ifstream input(pathFromUtf8(path), std::ios::binary);
    std::vector<std::string> lines;
    std::string line;

    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
    }

    return lines;
}

std::vector<std::string> removeAliasFromConfig(const std::vector<std::string>& lines,
                                               const std::string& alias) {
    std::vector<std::string> result;
    bool skipCurrentBlock = false;

    for (const auto& line : lines) {
        const auto tokens = splitConfigWords(line);
        if (!tokens.empty() && lower(tokens.front()) == "host") {
            skipCurrentBlock = false;

            std::vector<std::string> remainingAliases;
            for (std::size_t index = 1; index < tokens.size(); ++index) {
                if (tokens[index] != alias) {
                    remainingAliases.push_back(tokens[index]);
                }
            }

            if (remainingAliases.empty()) {
                skipCurrentBlock = true;
                continue;
            }

            std::string header = "Host";
            for (const auto& remainingAlias : remainingAliases) {
                header += " " + remainingAlias;
            }
            result.push_back(std::move(header));
            continue;
        }

        if (!skipCurrentBlock) {
            result.push_back(line);
        }
    }

    return result;
}

std::vector<std::string> inheritedOptionsForAlias(const std::vector<std::string>& lines,
                                                  const std::string& alias) {
    std::vector<std::string> result;
    bool active = false;

    for (const auto& line : lines) {
        const auto tokens = splitConfigWords(line);
        if (!tokens.empty() && lower(tokens.front()) == "host") {
            active = std::find(tokens.begin() + 1, tokens.end(), alias) != tokens.end();
            continue;
        }

        if (!active) {
            continue;
        }

        if (tokens.empty()) {
            if (trim(line).starts_with("#")) {
                result.push_back(line);
            }
            continue;
        }

        if (!isManagedDirective(tokens.front())) {
            result.push_back(line);
        }
    }

    return result;
}

std::string temporaryConfigSuffix() {
#ifdef _WIN32
    const auto processId = static_cast<unsigned long long>(GetCurrentProcessId());
#else
    const auto processId = static_cast<unsigned long long>(getpid());
#endif
    const auto sequence = temporaryFileSequence.fetch_add(1, std::memory_order_relaxed);
    return ".ffmpeg-studio-" + std::to_string(processId) + "-" +
           std::to_string(sequence) + ".tmp";
}

#ifndef _WIN32
bool writeAll(int descriptor, std::string_view content) {
    std::size_t offset = 0;
    while (offset < content.size()) {
        const ssize_t written = ::write(descriptor, content.data() + offset, content.size() - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (written == 0) {
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}
#endif

void eraseSecret(std::string& secret) {
#ifdef _WIN32
    if (!secret.empty()) {
        SecureZeroMemory(secret.data(), secret.size());
    }
#else
    volatile char* bytes = secret.data();
    for (std::size_t index = 0; index < secret.size(); ++index) {
        bytes[index] = 0;
    }
#endif
    secret.clear();
}

} // namespace

SshService::SshService() {
    const auto configuredSshPath = environmentVariable("FFMPEG_STUDIO_SSH_PATH");
    const auto configuredScpPath = environmentVariable("FFMPEG_STUDIO_SCP_PATH");

    sshPath_ = configuredSshPath && !configuredSshPath->empty() ? *configuredSshPath : "ssh";
    scpPath_ = configuredScpPath && !configuredScpPath->empty() ? *configuredScpPath : "scp";
}

std::string SshService::configPath() const {
    const auto overridePath = environmentVariable("FFMPEG_STUDIO_SSH_CONFIG");
    if (overridePath && !overridePath->empty()) {
        return *overridePath;
    }

#ifdef _WIN32
    const auto homeDirectory = environmentVariable("USERPROFILE");
#else
    const auto homeDirectory = environmentVariable("HOME");
#endif

    if (!homeDirectory || homeDirectory->empty()) {
        return {};
    }

    return pathToUtf8(pathFromUtf8(*homeDirectory) / ".ssh" / "config");
}

bool SshService::validAlias(const std::string& alias) {
    if (alias.empty() || alias.size() > 64 || alias.front() == '-') {
        return false;
    }

    return std::all_of(alias.begin(), alias.end(), [](unsigned char character) {
        return std::isalnum(character) || character == '.' || character == '_' || character == '-';
    });
}

std::string SshService::shellQuote(std::string_view value) {
    std::string result = "'";
    for (const char character : value) {
        if (character == '\'') {
            result += "'\\''";
        } else {
            result.push_back(character);
        }
    }
    result.push_back('\'');
    return result;
}

std::vector<SshServer> SshService::listServers() const {
    std::map<std::string, SshServer> servers;
    std::vector<std::string> activeAliases;

    for (const auto& line : readConfigLines(configPath())) {
        const auto tokens = splitConfigWords(line);
        if (tokens.empty()) {
            continue;
        }

        const std::string key = lower(tokens.front());
        if (key == "host") {
            activeAliases.clear();

            for (std::size_t index = 1; index < tokens.size(); ++index) {
                const auto& alias = tokens[index];
                if (alias.find_first_of("*!?") != std::string::npos || !validAlias(alias)) {
                    continue;
                }

                activeAliases.push_back(alias);
                auto [server, inserted] = servers.try_emplace(alias);
                if (inserted) {
                    server->second.alias = alias;
                    server->second.hostName = alias;
                }
            }
            continue;
        }

        if (tokens.size() < 2 || activeAliases.empty()) {
            continue;
        }

        const std::string& value = tokens[1];
        for (const auto& alias : activeAliases) {
            auto& server = servers[alias];

            if (key == "hostname" && server.hostName == alias) {
                server.hostName = value;
            } else if (key == "user" && server.user.empty()) {
                server.user = value;
            } else if (key == "port" && server.port == 22) {
                try {
                    const auto port = std::stoul(value);
                    if (port > 0 && port <= 65535) {
                        server.port = static_cast<unsigned int>(port);
                    }
                } catch (const std::exception&) {
                    // Keep the default port when the config value is invalid.
                }
            } else if (key == "identityfile" && server.identityFile.empty()) {
                server.identityFile = value;
            } else if (key == "proxyjump" && server.proxyJump.empty()) {
                server.proxyJump = value;
            } else if (key == "preferredauthentications" &&
                       value.find("password") != std::string::npos &&
                       value.find("publickey") == std::string::npos) {
                server.authMethod = SshAuthMethod::Password;
            } else if ((key == "passwordauthentication" ||
                        key == "kbdinteractiveauthentication") &&
                       lower(value) == "yes") {
                server.authMethod = SshAuthMethod::Password;
            }
        }
    }

    std::vector<SshServer> result;
    result.reserve(servers.size());
    for (auto& [alias, server] : servers) {
        (void)alias;
        result.push_back(std::move(server));
    }
    return result;
}

bool SshService::writeConfig(const std::vector<std::string>& lines, std::string& error) const {
    const std::filesystem::path destination = pathFromUtf8(configPath());
    if (destination.empty()) {
        error = "无法确定 SSH 配置文件位置。";
        return false;
    }

    std::error_code filesystemError;
    const auto parentDirectory = destination.parent_path();
    if (!parentDirectory.empty()) {
        std::filesystem::create_directories(parentDirectory, filesystemError);
    }
    if (filesystemError) {
        error = "无法创建 SSH 配置目录：" + filesystemError.message();
        return false;
    }

    const std::filesystem::path temporary =
        pathFromUtf8(pathToUtf8(destination) + temporaryConfigSuffix());

#ifdef _WIN32
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "无法创建 SSH 配置临时文件。Windows 错误代码：" +
                std::to_string(GetLastError());
        return false;
    }

    bool writeSucceeded = true;
    for (const auto& line : lines) {
        const std::string entry = line + "\n";
        DWORD written = 0;
        if (!WriteFile(file, entry.data(), static_cast<DWORD>(entry.size()), &written, nullptr) ||
            written != static_cast<DWORD>(entry.size())) {
            writeSucceeded = false;
            break;
        }
    }

    if (writeSucceeded) {
        writeSucceeded = FlushFileBuffers(file) != FALSE;
    }
    CloseHandle(file);

    if (!writeSucceeded) {
        error = "写入 SSH 配置文件时发生错误。";
        DeleteFileW(temporary.c_str());
        return false;
    }

    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "无法更新 SSH 配置文件。Windows 错误代码：" +
                std::to_string(GetLastError());
        DeleteFileW(temporary.c_str());
        return false;
    }
#else
    const int descriptor = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
    if (descriptor < 0) {
        error = "无法创建 SSH 配置临时文件。";
        return false;
    }

    std::string content;
    for (const auto& line : lines) {
        content += line;
        content.push_back('\n');
    }

    const bool writeSucceeded = writeAll(descriptor, content) && ::fsync(descriptor) == 0;
    const int closeResult = ::close(descriptor);
    if (!writeSucceeded || closeResult != 0) {
        error = "写入 SSH 配置文件时发生错误。";
        std::filesystem::remove(temporary, filesystemError);
        return false;
    }

    std::filesystem::rename(temporary, destination, filesystemError);
    if (filesystemError) {
        error = "无法更新 SSH 配置文件：" + filesystemError.message();
        std::filesystem::remove(temporary, filesystemError);
        return false;
    }
    (void)::chmod(destination.c_str(), S_IRUSR | S_IWUSR);
#endif

    error.clear();
    return true;
}

bool SshService::saveServer(const SshServer& server, const std::string& password,
                            std::string& error) {
    if (!validAlias(server.alias)) {
        error = "服务器别名只能包含英文字母、数字、点、下划线和连字符。";
        return false;
    }
    if (server.hostName.empty() ||
        server.hostName.find_first_of(" \t\r\n") != std::string::npos) {
        error = "请填写有效的主机名或 IP 地址。";
        return false;
    }
    if (server.user.find_first_of(" \t\r\n") != std::string::npos ||
        server.identityFile.find_first_of("\r\n") != std::string::npos ||
        server.proxyJump.find_first_of("\r\n") != std::string::npos) {
        error = "SSH 设置包含无效字符。";
        return false;
    }
    if (server.port == 0 || server.port > 65535) {
        error = "SSH 端口必须介于 1 到 65535 之间。";
        return false;
    }
    if (server.authMethod == SshAuthMethod::Password && password.empty() &&
        !hasPassword(server.alias)) {
        error = "密码认证需要填写密码。";
        return false;
    }

    const auto originalLines = readConfigLines(configPath());
    const auto inheritedOptions = inheritedOptionsForAlias(originalLines, server.alias);
    auto remainingLines = removeAliasFromConfig(originalLines, server.alias);

    std::vector<std::string> managedBlock{
        "# FFmpeg Studio managed SSH host",
        "Host " + server.alias
    };
    managedBlock.insert(managedBlock.end(), inheritedOptions.begin(), inheritedOptions.end());
    managedBlock.push_back("  HostName " + quoteConfigValue(server.hostName));

    if (!server.user.empty()) {
        managedBlock.push_back("  User " + quoteConfigValue(server.user));
    }

    managedBlock.push_back("  Port " + std::to_string(server.port));
    if (!server.identityFile.empty()) {
        managedBlock.push_back("  IdentityFile " + quoteConfigValue(server.identityFile));
    }
    if (!server.proxyJump.empty()) {
        managedBlock.push_back("  ProxyJump " + quoteConfigValue(server.proxyJump));
    }

    const bool passwordAuthentication = server.authMethod == SshAuthMethod::Password;
    managedBlock.push_back(passwordAuthentication
        ? "  PreferredAuthentications password,keyboard-interactive"
        : "  PreferredAuthentications publickey");
    managedBlock.push_back(passwordAuthentication
        ? "  PasswordAuthentication yes"
        : "  PasswordAuthentication no");
    managedBlock.push_back(passwordAuthentication
        ? "  KbdInteractiveAuthentication yes"
        : "  KbdInteractiveAuthentication no");
    managedBlock.push_back(passwordAuthentication ? "  IdentitiesOnly no" : "  IdentitiesOnly yes");
    managedBlock.emplace_back();

    managedBlock.insert(managedBlock.end(), remainingLines.begin(), remainingLines.end());
    if (!writeConfig(managedBlock, error)) {
        return false;
    }

    if (passwordAuthentication && !password.empty()) {
        if (!setPassword(server.alias, password, error)) {
            std::string restoreError;
            (void)writeConfig(originalLines, restoreError);
            return false;
        }
    } else if (!passwordAuthentication) {
        std::string credentialError;
        if (!clearPassword(server.alias, credentialError)) {
            error = credentialError;
            return false;
        }
    }

    error.clear();
    return true;
}

bool SshService::removeServer(const std::string& alias, std::string& error) {
    if (!validAlias(alias)) {
        error = "服务器别名无效。";
        return false;
    }

    if (!writeConfig(removeAliasFromConfig(readConfigLines(configPath()), alias), error)) {
        return false;
    }

    if (!clearPassword(alias, error)) {
        return false;
    }

    error.clear();
    return true;
}

bool SshService::hasPassword(const std::string& alias) const {
    auto password = readPassword(alias);
    const bool exists = password.has_value();
    if (password) {
        eraseSecret(*password);
    }
    return exists;
}

bool SshService::setPassword(const std::string& alias, const std::string& password,
                             std::string& error) {
    if (!validAlias(alias) || password.empty() ||
        password.find_first_of("\r\n") != std::string::npos) {
        error = "服务器别名或密码无效。";
        return false;
    }

#ifdef _WIN32
    if (password.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) {
        error = "SSH 密码长度超过 Windows 凭据管理器允许的范围。";
        return false;
    }

    const std::wstring target = wideFromUtf8(credentialTarget(alias));
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<LPWSTR>(target.c_str());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(
        const_cast<char*>(password.data()));
    credential.CredentialBlobSize = static_cast<DWORD>(password.size());
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.UserName = const_cast<LPWSTR>(L"FFmpeg Studio SSH");

    if (!CredWriteW(&credential, 0)) {
        error = "无法保存 SSH 密码：" + std::to_string(GetLastError());
        return false;
    }
#else
    std::lock_guard lock(passwordMutex);
    auto& storedPassword = sessionPasswords[credentialTarget(alias)];
    eraseSecret(storedPassword);
    storedPassword = password;
#endif

    error.clear();
    return true;
}

bool SshService::clearPassword(const std::string& alias, std::string& error) {
#ifdef _WIN32
    const std::wstring target = wideFromUtf8(credentialTarget(alias));
    if (!CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0)) {
        const DWORD errorCode = GetLastError();
        if (errorCode != ERROR_NOT_FOUND) {
            error = "无法删除 SSH 密码：" + std::to_string(errorCode);
            return false;
        }
    }
#else
    std::lock_guard lock(passwordMutex);
    const auto it = sessionPasswords.find(credentialTarget(alias));
    if (it != sessionPasswords.end()) {
        eraseSecret(it->second);
        sessionPasswords.erase(it);
    }
#endif

    error.clear();
    return true;
}

std::optional<std::string> SshService::readPassword(const std::string& alias) const {
#ifdef _WIN32
    const std::wstring target = wideFromUtf8(credentialTarget(alias));
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential) || credential == nullptr) {
        return std::nullopt;
    }

    std::string password(reinterpret_cast<const char*>(credential->CredentialBlob),
                         credential->CredentialBlobSize);
    CredFree(credential);
    return password;
#else
    std::lock_guard lock(passwordMutex);
    const auto it = sessionPasswords.find(credentialTarget(alias));
    if (it == sessionPasswords.end()) {
        return std::nullopt;
    }
    return it->second;
#endif
}

std::optional<std::string> SshService::passwordForAskpass(const std::string& alias) const {
#ifdef _WIN32
    return readPassword(alias);
#else
    (void)alias;
    return environmentVariable("FFMPEG_STUDIO_SSH_PASSWORD");
#endif
}

std::string SshService::executablePath() const {
#ifdef _WIN32
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= static_cast<DWORD>(path.size())) {
        return {};
    }
    path.resize(length);
    return utf8FromWide(path.c_str());
#else
    std::vector<char> path(4096);
    const auto length = readlink("/proc/self/exe", path.data(), path.size() - 1);
    if (length <= 0) {
        return {};
    }
    path[static_cast<std::size_t>(length)] = '\0';
    return path.data();
#endif
}

ProcessEnvironment SshService::processEnvironment(const std::string& alias) const {
    ProcessEnvironment environment;
    const auto servers = listServers();
    const auto server = std::find_if(servers.begin(), servers.end(), [&](const SshServer& item) {
        return item.alias == alias;
    });

    if (server == servers.end() || server->authMethod != SshAuthMethod::Password) {
        return environment;
    }

#ifdef _WIN32
    if (!hasPassword(alias)) {
        return environment;
    }
#else
    auto password = readPassword(alias);
    if (!password || password->empty()) {
        return environment;
    }
    environment["FFMPEG_STUDIO_SSH_PASSWORD"] = *password;
    eraseSecret(*password);
#endif

    const std::string helper = executablePath();
    if (!helper.empty()) {
        environment["SSH_ASKPASS"] = helper;
    }
    environment["SSH_ASKPASS_REQUIRE"] = "force";
    environment["DISPLAY"] = "ffmpeg-studio";
    environment["FFMPEG_STUDIO_SSH_ASKPASS_ALIAS"] = alias;
    return environment;
}

std::vector<std::string> SshService::sshArguments(const std::string& alias,
                                                  const std::string& command) const {
    const auto servers = listServers();
    const auto server = std::find_if(servers.begin(), servers.end(), [&](const SshServer& item) {
        return item.alias == alias;
    });
    const bool passwordAuthentication =
        server != servers.end() && server->authMethod == SshAuthMethod::Password;

    std::vector<std::string> arguments{
        sshPath_,
        "-T",
        "-o", "ConnectTimeout=15",
        "-o", "ServerAliveInterval=20",
        "-o", "ServerAliveCountMax=3",
        "-o", "StrictHostKeyChecking=accept-new",
        "-o", passwordAuthentication ? "BatchMode=no" : "BatchMode=yes"
    };

    if (passwordAuthentication) {
        arguments.insert(arguments.end(), {
            "-o", "PubkeyAuthentication=no",
            "-o", "PreferredAuthentications=password,keyboard-interactive",
            "-o", "NumberOfPasswordPrompts=1"
        });
    }

    arguments.push_back(alias);
    arguments.push_back(command);
    return arguments;
}

std::vector<std::string> SshService::scpArguments(const std::string& alias,
                                                  const std::string& source,
                                                  const std::string& destination) const {
    const auto servers = listServers();
    const auto server = std::find_if(servers.begin(), servers.end(), [&](const SshServer& item) {
        return item.alias == alias;
    });
    const bool passwordAuthentication =
        server != servers.end() && server->authMethod == SshAuthMethod::Password;

    std::vector<std::string> arguments{
        scpPath_,
        "-q",
        "-o", "ConnectTimeout=15",
        "-o", "StrictHostKeyChecking=accept-new",
        "-o", passwordAuthentication ? "BatchMode=no" : "BatchMode=yes"
    };

    if (passwordAuthentication) {
        arguments.insert(arguments.end(), {
            "-o", "PubkeyAuthentication=no",
            "-o", "PreferredAuthentications=password,keyboard-interactive",
            "-o", "NumberOfPasswordPrompts=1"
        });
    }

    arguments.push_back(source);
    arguments.push_back(destination);
    return arguments;
}

bool SshService::testConnection(const std::string& alias, std::string& error) const {
    const auto servers = listServers();
    const auto server = std::find_if(servers.begin(), servers.end(), [&](const SshServer& item) {
        return item.alias == alias;
    });
    if (server == servers.end()) {
        error = "SSH 配置中不存在该服务器。";
        return false;
    }
    if (server->authMethod == SshAuthMethod::Password && !hasPassword(alias)) {
        error = "尚未保存该服务器的密码。";
        return false;
    }

    auto environment = processEnvironment(alias);
    const auto result = runProcess(
        sshArguments(alias, "printf 'FFMPEG_STUDIO_SSH_OK'"),
        nullptr, {}, nullptr, &environment);
#ifndef _WIN32
    if (auto password = environment.find("FFMPEG_STUDIO_SSH_PASSWORD");
        password != environment.end()) {
        eraseSecret(password->second);
    }
#endif

    if (result.exitCode != 0 ||
        result.output.find("FFMPEG_STUDIO_SSH_OK") == std::string::npos) {
        error = trim(result.output);
        if (error.empty()) {
            error = "SSH 连接失败，请检查服务器地址、认证方式和网络连接。";
        }
        return false;
    }

    error.clear();
    return true;
}

ProcessResult SshService::runRemote(const std::string& alias, const std::string& command,
                                    const std::atomic_bool* cancelRequested,
                                    ProcessLineCallback onLine,
                                    const std::atomic_bool* pauseRequested) const {
    auto environment = processEnvironment(alias);
    ProcessResult result = runProcess(sshArguments(alias, command), cancelRequested,
                                      std::move(onLine), pauseRequested, &environment);
#ifndef _WIN32
    if (auto password = environment.find("FFMPEG_STUDIO_SSH_PASSWORD");
        password != environment.end()) {
        eraseSecret(password->second);
    }
#endif
    return result;
}

ProcessResult SshService::upload(const std::string& alias, const std::string& localPath,
                                 const std::string& remotePath,
                                 const std::atomic_bool* cancelRequested) const {
    auto environment = processEnvironment(alias);
    ProcessResult result = runProcess(
        scpArguments(alias, localPath, alias + ":" + remotePath),
        cancelRequested, {}, nullptr, &environment);
#ifndef _WIN32
    if (auto password = environment.find("FFMPEG_STUDIO_SSH_PASSWORD");
        password != environment.end()) {
        eraseSecret(password->second);
    }
#endif
    return result;
}

ProcessResult SshService::download(const std::string& alias, const std::string& remotePath,
                                   const std::string& localPath,
                                   const std::atomic_bool* cancelRequested) const {
    auto environment = processEnvironment(alias);
    ProcessResult result = runProcess(
        scpArguments(alias, alias + ":" + remotePath, localPath),
        cancelRequested, {}, nullptr, &environment);
#ifndef _WIN32
    if (auto password = environment.find("FFMPEG_STUDIO_SSH_PASSWORD");
        password != environment.end()) {
        eraseSecret(password->second);
    }
#endif
    return result;
}

} // namespace ffmpegui
