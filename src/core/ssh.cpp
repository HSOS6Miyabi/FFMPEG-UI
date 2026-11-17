#include "ssh.hpp"

#include "process.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincred.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ffmpegui {
namespace {
std::mutex passwordMutex;
std::map<std::string,std::string> sessionPasswords;

std::string lower(std::string value) {
    std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    return value;
}
std::string trim(std::string value) {
    const auto first=value.find_first_not_of(" \t\r\n");
    if(first==std::string::npos)return {};
    return value.substr(first,value.find_last_not_of(" \t\r\n")-first+1);
}
std::vector<std::string> words(const std::string& line) {
    std::vector<std::string> result;std::string current;char quote=0;bool escaped=false;
    for(char c:line){
        if(escaped){current.push_back(c);escaped=false;continue;}
        if(c=='\\'&&quote!='\''){escaped=true;continue;}
        if(quote){if(c==quote)quote=0;else current.push_back(c);continue;}
        if(c=='\''||c=='"'){quote=c;continue;}
        if(c=='#')break;
        if(std::isspace(static_cast<unsigned char>(c))){if(!current.empty()){result.push_back(current);current.clear();}}
        else current.push_back(c);
    }
    if(!current.empty())result.push_back(current);
    return result;
}
std::filesystem::path fromUtf8(std::string_view value) {
    std::u8string encoded;encoded.reserve(value.size());
    for(unsigned char c:value)encoded.push_back(static_cast<char8_t>(c));
    return std::filesystem::path(encoded);
}
std::string toUtf8(const std::filesystem::path& path) {
    const auto value=path.u8string();return std::string(reinterpret_cast<const char*>(value.data()),value.size());
}
std::string credentialTarget(const std::string& alias){return "FFmpegStudio:SSH:"+alias;}
std::string quoteConfig(const std::string& value){
    std::string result="\"";for(char c:value){if(c=='\\'||c=='"')result.push_back('\\');result.push_back(c);}return result+"\"";
}
bool managedDirective(const std::string& key){
    const auto k=lower(key);return k=="hostname"||k=="user"||k=="port"||k=="identityfile"||
        k=="proxyjump"||k=="preferredauthentications"||k=="passwordauthentication"||
        k=="kbdinteractiveauthentication"||k=="identitiesonly";
}
std::vector<std::string> readLines(const std::string& path){
    std::ifstream input(fromUtf8(path),std::ios::binary);std::vector<std::string> lines;std::string line;
    while(std::getline(input,line)){if(!line.empty()&&line.back()=='\r')line.pop_back();lines.push_back(line);}return lines;
}
std::vector<std::string> removeAlias(const std::vector<std::string>& lines,const std::string& alias){
    std::vector<std::string> out;bool skip=false;
    for(const auto& line:lines){
        const auto tokens=words(line);
        if(!tokens.empty()&&lower(tokens[0])=="host"){
            skip=false;std::vector<std::string> aliases;
            for(std::size_t i=1;i<tokens.size();++i)if(tokens[i]!=alias)aliases.push_back(tokens[i]);
            if(aliases.empty()){skip=true;continue;}
            std::string header="Host";for(const auto& item:aliases)header+=" "+item;out.push_back(header);continue;
        }
        if(!skip)out.push_back(line);
    }
    return out;
}
std::vector<std::string> inheritedOptions(const std::vector<std::string>& lines,const std::string& alias){
    std::vector<std::string> out;bool active=false;
    for(const auto& line:lines){
        const auto tokens=words(line);
        if(!tokens.empty()&&lower(tokens[0])=="host"){
            active=std::find(tokens.begin()+1,tokens.end(),alias)!=tokens.end();continue;
        }
        if(!active)continue;
        if(tokens.empty()){if(trim(line).starts_with("#"))out.push_back(line);continue;}
        if(!managedDirective(tokens[0]))out.push_back(line);
    }
    return out;
}
} // namespace

SshService::SshService(){
    const auto sshPath = environmentVariable("FFMPEG_STUDIO_SSH_PATH");
    const auto scpPath = environmentVariable("FFMPEG_STUDIO_SCP_PATH");
    sshPath_ = sshPath && !sshPath->empty() ? *sshPath : "ssh";
    scpPath_ = scpPath && !scpPath->empty() ? *scpPath : "scp";
}
std::string SshService::configPath() const{
    const auto overridePath = environmentVariable("FFMPEG_STUDIO_SSH_CONFIG");
    if (overridePath && !overridePath->empty()) {
        return *overridePath;
    }
#ifdef _WIN32
    const auto home = environmentVariable("USERPROFILE");
#else
    const auto home = environmentVariable("HOME");
#endif
    if (home && !home->empty()) {
        return toUtf8(fromUtf8(*home) / ".ssh" / "config");
    }
    return {};
}
bool SshService::validAlias(const std::string& alias){
    if(alias.empty()||alias.size()>64||alias.front()=='-')return false;
    return std::all_of(alias.begin(),alias.end(),[](unsigned char c){return std::isalnum(c)||c=='.'||c=='_'||c=='-';});
}
std::string SshService::shellQuote(std::string_view value){
    std::string result="'";for(char c:value){if(c=='\'')result+="'\\''";else result.push_back(c);}return result+"'";
}
std::vector<SshServer> SshService::listServers() const{
    std::map<std::string,SshServer> servers;std::vector<std::string> active;
    for(const auto& line:readLines(configPath())){
        const auto tokens=words(line);if(tokens.empty())continue;
        const auto key=lower(tokens[0]);
        if(key=="host"){
            active.clear();
            for(std::size_t i=1;i<tokens.size();++i){
                const auto& alias=tokens[i];if(alias.find_first_of("*!?")!=std::string::npos||!validAlias(alias))continue;
                active.push_back(alias);auto [it,inserted]=servers.try_emplace(alias);
                if(inserted){it->second.alias=alias;it->second.hostName=alias;}
            }
            continue;
        }
        if(tokens.size()<2||active.empty())continue;
        for(const auto& alias:active){
            auto& server=servers[alias];const auto& value=tokens[1];
            if(key=="hostname"&&server.hostName==alias)server.hostName=value;
            else if(key=="user"&&server.user.empty())server.user=value;
            else if(key=="port"&&server.port==22){try{auto p=std::stoul(value);if(p>0&&p<=65535)server.port=static_cast<unsigned int>(p);}catch(...){}}
            else if(key=="identityfile"&&server.identityFile.empty())server.identityFile=value;
            else if(key=="proxyjump"&&server.proxyJump.empty())server.proxyJump=value;
            else if(key=="preferredauthentications"&&value.find("password")!=std::string::npos&&value.find("publickey")==std::string::npos)server.authMethod=SshAuthMethod::Password;
        }
    }
    std::vector<SshServer> result;for(auto& [key,value]:servers){(void)key;result.push_back(std::move(value));}return result;
}
bool SshService::writeConfig(const std::vector<std::string>& lines,std::string& error) const{
    const auto destination=fromUtf8(configPath());if(destination.empty()){error="无法确定 SSH 配置文件位置。";return false;}
    std::error_code ec;std::filesystem::create_directories(destination.parent_path(),ec);
    if(ec){error="无法创建 SSH 配置目录："+ec.message();return false;}
    const auto temporary=fromUtf8(toUtf8(destination)+".ffmpeg-studio.tmp");
    {std::ofstream out(temporary,std::ios::binary|std::ios::trunc);if(!out){error="无法写入 SSH 配置文件。";return false;}
     for(const auto& line:lines)out<<line<<'\n';out.flush();if(!out){error="写入 SSH 配置文件时发生错误。";std::filesystem::remove(temporary,ec);return false;}}
#ifdef _WIN32
    if(!MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){error="无法更新 SSH 配置文件："+std::to_string(GetLastError());std::filesystem::remove(temporary,ec);return false;}
#else
    chmod(temporary.c_str(),0600);std::filesystem::rename(temporary,destination,ec);
    if(ec){error="无法更新 SSH 配置文件："+ec.message();std::filesystem::remove(temporary,ec);return false;}
    chmod(destination.c_str(),0600);
#endif
    error.clear();return true;
}
bool SshService::saveServer(const SshServer& server,const std::string& password,std::string& error){
    if(!validAlias(server.alias)){error="服务器别名只能包含英文字母、数字、点、下划线和连字符。";return false;}
    if(server.hostName.empty()||server.hostName.find_first_of(" \t\r\n")!=std::string::npos){error="请填写有效的主机名或 IP 地址。";return false;}
    if(server.user.find_first_of(" \t\r\n")!=std::string::npos||server.identityFile.find_first_of("\r\n")!=std::string::npos||server.proxyJump.find_first_of("\r\n")!=std::string::npos){error="SSH 设置包含无效字符。";return false;}
    if(server.port==0||server.port>65535){error="SSH 端口必须介于 1 到 65535 之间。";return false;}
    if(server.authMethod==SshAuthMethod::Password&&password.empty()&&!hasPassword(server.alias)){error="密码认证需要填写密码。";return false;}
    const auto original=readLines(configPath());auto extras=inheritedOptions(original,server.alias);auto rest=removeAlias(original,server.alias);
    std::vector<std::string> block{"# FFmpeg Studio managed SSH host","Host "+server.alias};
    block.insert(block.end(),extras.begin(),extras.end());
    block.push_back("  HostName "+quoteConfig(server.hostName));if(!server.user.empty())block.push_back("  User "+quoteConfig(server.user));
    block.push_back("  Port "+std::to_string(server.port));if(!server.identityFile.empty())block.push_back("  IdentityFile "+quoteConfig(server.identityFile));
    if(!server.proxyJump.empty())block.push_back("  ProxyJump "+quoteConfig(server.proxyJump));
    block.push_back(server.authMethod==SshAuthMethod::Password?"  PreferredAuthentications password,keyboard-interactive":"  PreferredAuthentications publickey");
    block.push_back(server.authMethod==SshAuthMethod::Password?"  PasswordAuthentication yes":"  PasswordAuthentication no");
    block.push_back(server.authMethod==SshAuthMethod::Password?"  KbdInteractiveAuthentication yes":"  KbdInteractiveAuthentication no");
    block.push_back(server.authMethod==SshAuthMethod::Password?"  IdentitiesOnly no":"  IdentitiesOnly yes");block.push_back("");
    block.insert(block.end(),rest.begin(),rest.end());if(!writeConfig(block,error))return false;
    if(server.authMethod==SshAuthMethod::Password&&!password.empty()){if(!setPassword(server.alias,password,error)){std::string ignored;writeConfig(original,ignored);return false;}}
    else if(server.authMethod==SshAuthMethod::PublicKey){std::string ignored;clearPassword(server.alias,ignored);}
    error.clear();return true;
}
bool SshService::removeServer(const std::string& alias,std::string& error){
    if(!validAlias(alias)){error="服务器别名无效。";return false;}
    if(!writeConfig(removeAlias(readLines(configPath()),alias),error))return false;
    std::string ignored;clearPassword(alias,ignored);error.clear();return true;
}
bool SshService::hasPassword(const std::string& alias) const{return readPassword(alias).has_value();}
bool SshService::setPassword(const std::string& alias,const std::string& password,std::string& error){
    if(!validAlias(alias)||password.empty()||password.find_first_of("\r\n")!=std::string::npos){error="服务器别名或密码无效。";return false;}
#ifdef _WIN32
    const auto target=wideFromUtf8(credentialTarget(alias));CREDENTIALW c{};c.Type=CRED_TYPE_GENERIC;c.TargetName=const_cast<LPWSTR>(target.c_str());
    c.CredentialBlob=reinterpret_cast<LPBYTE>(const_cast<char*>(password.data()));c.CredentialBlobSize=static_cast<DWORD>(password.size());
    c.Persist=CRED_PERSIST_LOCAL_MACHINE;c.UserName=const_cast<LPWSTR>(L"FFmpeg Studio SSH");
    if(!CredWriteW(&c,0)){error="无法保存 SSH 密码："+std::to_string(GetLastError());return false;}
#else
    std::lock_guard lock(passwordMutex);sessionPasswords[credentialTarget(alias)]=password;
#endif
    error.clear();return true;
}
bool SshService::clearPassword(const std::string& alias,std::string& error){
#ifdef _WIN32
    const auto target=wideFromUtf8(credentialTarget(alias));if(!CredDeleteW(target.c_str(),CRED_TYPE_GENERIC,0)){const auto e=GetLastError();if(e!=ERROR_NOT_FOUND){error="无法删除 SSH 密码："+std::to_string(e);return false;}}
#else
    std::lock_guard lock(passwordMutex);sessionPasswords.erase(credentialTarget(alias));
#endif
    error.clear();return true;
}
std::optional<std::string> SshService::readPassword(const std::string& alias) const{
#ifdef _WIN32
    const auto target=wideFromUtf8(credentialTarget(alias));PCREDENTIALW c=nullptr;if(!CredReadW(target.c_str(),CRED_TYPE_GENERIC,0,&c)||!c)return std::nullopt;
    std::string value(reinterpret_cast<const char*>(c->CredentialBlob),c->CredentialBlobSize);CredFree(c);return value;
#else
    std::lock_guard lock(passwordMutex);const auto it=sessionPasswords.find(credentialTarget(alias));if(it==sessionPasswords.end())return std::nullopt;return it->second;
#endif
}
std::optional<std::string> SshService::passwordForAskpass(const std::string& alias) const{
#ifdef _WIN32
    return readPassword(alias);
#else
    (void)alias;
    return environmentVariable("FFMPEG_STUDIO_SSH_PASSWORD");
#endif
}
std::string SshService::executablePath() const{
#ifdef _WIN32
    std::wstring path(32768,L'\0');const DWORD n=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));if(!n||n>=path.size())return {};path.resize(n);return utf8FromWide(path.c_str());
#else
    std::vector<char> path(4096);const auto n=readlink("/proc/self/exe",path.data(),path.size()-1);if(n<=0)return {};path[static_cast<std::size_t>(n)]='\0';return path.data();
#endif
}
ProcessEnvironment SshService::processEnvironment(const std::string& alias) const{
    ProcessEnvironment env;const auto servers=listServers();const auto it=std::find_if(servers.begin(),servers.end(),[&](const auto& s){return s.alias==alias;});
    if(it!=servers.end()&&it->authMethod==SshAuthMethod::Password){if(auto password=readPassword(alias)){
        const auto helper=executablePath();if(!helper.empty())env["SSH_ASKPASS"]=helper;
        env["SSH_ASKPASS_REQUIRE"]="force";env["DISPLAY"]="ffmpeg-studio";env["FFMPEG_STUDIO_SSH_ASKPASS_ALIAS"]=alias;
#ifndef _WIN32
        env["FFMPEG_STUDIO_SSH_PASSWORD"]=*password;
#endif
    }}
    return env;
}
std::vector<std::string> SshService::sshArguments(const std::string& alias,const std::string& command) const{
    const auto servers=listServers();const auto it=std::find_if(servers.begin(),servers.end(),[&](const auto& s){return s.alias==alias;});
    const bool password=it!=servers.end()&&it->authMethod==SshAuthMethod::Password;
    std::vector<std::string> args{sshPath_,"-T","-o","ConnectTimeout=15","-o","ServerAliveInterval=20","-o","ServerAliveCountMax=3","-o","StrictHostKeyChecking=accept-new","-o",password?"BatchMode=no":"BatchMode=yes"};
    if(password)args.insert(args.end(),{"-o","PubkeyAuthentication=no","-o","PreferredAuthentications=password,keyboard-interactive","-o","NumberOfPasswordPrompts=1"});
    args.push_back(alias);args.push_back(command);return args;
}
std::vector<std::string> SshService::scpArguments(const std::string& alias,const std::string& source,const std::string& destination) const{
    const auto servers=listServers();const auto it=std::find_if(servers.begin(),servers.end(),[&](const auto& s){return s.alias==alias;});
    const bool password=it!=servers.end()&&it->authMethod==SshAuthMethod::Password;
    std::vector<std::string> args{scpPath_,"-q","-o","ConnectTimeout=15","-o","StrictHostKeyChecking=accept-new","-o",password?"BatchMode=no":"BatchMode=yes"};
    if(password)args.insert(args.end(),{"-o","PubkeyAuthentication=no","-o","PreferredAuthentications=password,keyboard-interactive","-o","NumberOfPasswordPrompts=1"});
    args.push_back(source);args.push_back(destination);return args;
}
bool SshService::testConnection(const std::string& alias,std::string& error) const{
    const auto servers=listServers();const auto it=std::find_if(servers.begin(),servers.end(),[&](const auto& s){return s.alias==alias;});
    if(it==servers.end()){error="SSH 配置中不存在该服务器。";return false;}
    if(it->authMethod==SshAuthMethod::Password&&!hasPassword(alias)){error="尚未保存该服务器的密码。";return false;}
    const auto env=processEnvironment(alias);const auto result=runProcess(sshArguments(alias,"printf 'FFMPEG_STUDIO_SSH_OK'"),nullptr,{},nullptr,&env);
    if(result.exitCode||result.output.find("FFMPEG_STUDIO_SSH_OK")==std::string::npos){error=trim(result.output);if(error.empty())error="SSH 连接失败，请检查服务器地址、认证方式和网络连接。";return false;}
    error.clear();return true;
}
ProcessResult SshService::runRemote(const std::string& alias,const std::string& command,const std::atomic_bool* cancel,ProcessLineCallback callback,const std::atomic_bool* pause) const{
    const auto env=processEnvironment(alias);return runProcess(sshArguments(alias,command),cancel,std::move(callback),pause,&env);
}
ProcessResult SshService::upload(const std::string& alias,const std::string& local,const std::string& remote,const std::atomic_bool* cancel) const{
    const auto env=processEnvironment(alias);return runProcess(scpArguments(alias,local,alias+":"+remote),cancel,{},nullptr,&env);
}
ProcessResult SshService::download(const std::string& alias,const std::string& remote,const std::string& local,const std::atomic_bool* cancel) const{
    const auto env=processEnvironment(alias);return runProcess(scpArguments(alias,alias+":"+remote,local),cancel,{},nullptr,&env);
}
} // namespace ffmpegui
