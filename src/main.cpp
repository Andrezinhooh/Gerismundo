#include "nativecall/ui.hpp"

#include <nlohmann/json.hpp>
#include <shellapi.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <memory>

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
    bool smokeTest = false;
    std::filesystem::path smokeReport;
    auto writeReport = [&](bool passed, const std::string& detail) {
        if (smokeReport.empty()) return;
        std::ofstream output(smokeReport, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("Não foi possível escrever o relatório do teste da interface.");
        output << nlohmann::json{
            {"test", "native-win32-startup"},
            {"passed", passed},
            {"detail", detail},
            {"coverage", "Actual Win32 window, UI controls, COM, controller workers, SpeexDSP initialization and clean shutdown"},
            {"not_tested", "Microphone, physical audio devices, screen capture, hardware encoder, acoustic echo, internet connectivity"}
        }.dump(2) << '\n';
    };
    try {
        int count = 0;
        LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!arguments) throw std::runtime_error("Não foi possível ler a linha de comando.");
        const auto freeArguments = [](LPWSTR* pointer) { LocalFree(pointer); };
        std::unique_ptr<LPWSTR, decltype(freeArguments)> argumentOwner(arguments, freeArguments);
        // Detect smoke mode first so malformed arguments cannot open a modal
        // error dialog and block an unattended CI session.
        for (int index = 1; index < count; ++index)
            if (std::wstring_view(arguments[index]) == L"--smoke-test") smokeTest = true;
        std::filesystem::path config;
        bool explicitlyRequested = false;
        for (int index = 1; index < count; ++index) {
            const std::wstring_view option(arguments[index]);
            if (option == L"--smoke-test") continue;
            if (option == L"--config") {
                if (explicitlyRequested || index + 1 >= count)
                    throw std::runtime_error("--config exige um caminho e não pode ser repetido.");
                config = arguments[++index]; explicitlyRequested = true;
            } else if (option == L"--smoke-report") {
                if (!smokeReport.empty() || index + 1 >= count)
                    throw std::runtime_error("--smoke-report exige um caminho e não pode ser repetido.");
                smokeReport = arguments[++index];
            } else {
                throw std::runtime_error("Uso: native-call.exe [--config caminho/config.json] [--smoke-test [--smoke-report caminho/report.json]]");
            }
        }
        if (!smokeReport.empty() && !smokeTest)
            throw std::runtime_error("--smoke-report requer --smoke-test.");
        if (!explicitlyRequested && !smokeTest) {
            wchar_t executable[32768]{};
            const auto length = GetModuleFileNameW(nullptr, executable, 32768);
            if (!length || length >= 32768)
                throw std::runtime_error("Não foi possível localizar o diretório do aplicativo.");
            config = std::filesystem::path(executable).parent_path() / L"config.json";
        }
        const auto settings = smokeTest && !explicitlyRequested
            ? nativecall::ConnectionSettings{} : loadSettings(config, explicitlyRequested);
        if (smokeTest) writeReport(false, "Native UI startup test started; a final passing report requires successful window initialization and clean shutdown.");
        const int result = nativecall::runNativeApplication(instance, showCommand, settings, smokeTest);
        if (smokeTest) writeReport(result == 0,
            result == 0 ? "Native Win32 window and controller initialized and closed after two seconds."
                        : "Native Win32 startup or message processing failed.");
        return result;
    } catch (const std::exception& exception) {
        if (smokeTest) {
            try { writeReport(false, exception.what()); } catch (...) {}
            OutputDebugStringA(exception.what());
        } else {
            MessageBoxW(nullptr, widen(exception.what()).c_str(), L"Native Call — erro",
                        MB_OK | MB_ICONERROR);
        }
        return 1;
    }
}
