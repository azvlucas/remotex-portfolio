#pragma once
// RemoteX — host/service_manager.h
// Windows Service wrapper: instala, desinstala e roda o host como serviço.

#include <windows.h>
#include <atomic>
#include <functional>
#include <string>

namespace remotex::host {

// Nome e display do serviço.
inline constexpr const wchar_t* kServiceName    = L"RemoteXHost";
inline constexpr const wchar_t* kServiceDisplay = L"RemoteX Host Agent";

// Callback que contém a lógica principal do host (chamada tanto pelo serviço
// quanto pelo modo console). Deve rodar até que stop_flag se torne true.
using HostEntryFn = std::function<int(std::atomic<bool>& stop_flag)>;

class ServiceManager {
public:
    // Registra o serviço no SCM (requer elevação).
    // exe_path: caminho completo do executável. Se vazio, usa o exe atual.
    static bool Install(const std::wstring& exe_path = L"");

    // Remove o serviço do SCM.
    static bool Uninstall();

    // Entry point quando o SCM inicia o serviço.
    // Chama StartServiceCtrlDispatcherW internamente.
    // host_fn é a lógica do host a ser executada.
    static bool RunAsService(HostEntryFn host_fn);

private:
    static void WINAPI ServiceMain(DWORD argc, LPWSTR* argv);
    static void WINAPI ServiceCtrlHandler(DWORD control);

    static HostEntryFn           s_host_fn;
    static SERVICE_STATUS        s_status;
    static SERVICE_STATUS_HANDLE s_status_handle;
    static std::atomic<bool>     s_stop_flag;
};

} // namespace remotex::host
