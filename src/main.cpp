#include "nativecall/ui.hpp"

#include <nlohmann/json.hpp>
#include <shellapi.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>

namespace {

std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                    static_cast<int>(text.size()), nullptr, 0);
    if (!length) return L"Erro de configuração";
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), result.data(), length);
    return result;
}

nativecall::ConnectionSettings loadSettings(const std::filesystem::path& file,
                                           bool explicitlyRequested) {
    nativecall::ConnectionSettings settings;
    std::ifstream input(file);
    if (!input) {
        if (explicitlyRequested) throw std::runtime_error("Não foi possível abrir o arquivo de configuração.");
        return settings;
    }
    const auto json = nlohmann::json::parse(input);
    settings.signalingUrl = json.value("signalingUrl", settings.signalingUrl);
    settings.room = json.value("room", settings.room);
    settings.displayName = json.value("displayName", settings.displayName);
    settings.accessToken = json.value("accessToken", settings.accessToken);
    if (json.contains("iceServers")) {
        if (!json["iceServers"].is_array()) throw std::runtime_error("iceServers precisa ser uma lista.");
        for (const auto& ice : json["iceServers"]) {
            settings.iceServers.push_back({ice.at("url").get<std::string>(),
                                           ice.value("username", std::string{}),
                                           ice.value("password", std::string{})});
        }
    }
    return settings;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    try {
        int count = 0;
        LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!arguments) throw std::runtime_error("Não foi possível ler a linha de comando.");
        std::filesystem::path config;
        bool explicitlyRequested = false;
        if (count > 1) {
            if (count != 3 || std::wstring_view(arguments[1]) != L"--config") {
                LocalFree(arguments);
                MessageBoxW(nullptr, L"Uso: native-call.exe [--config caminho\\config.json]",
                            L"Native Call", MB_OK | MB_ICONINFORMATION);
                return 1;
            }
            config = arguments[2];
            explicitlyRequested = true;
        } else {
            wchar_t executable[32768]{};
            const auto length = GetModuleFileNameW(nullptr, executable, 32768);
            if (!length || length >= 32768) {
                LocalFree(arguments);
                throw std::runtime_error("Não foi possível localizar o diretório do aplicativo.");
            }
            config = std::filesystem::path(executable).parent_path() / L"config.json";
        }
        LocalFree(arguments);
        return nativecall::runNativeApplication(instance, showCommand,
                                               loadSettings(config, explicitlyRequested));
    } catch (const std::exception& exception) {
        MessageBoxW(nullptr, widen(exception.what()).c_str(), L"Native Call — erro",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
}
