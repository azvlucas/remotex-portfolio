// RemoteX — viewer/main.cpp
// Entry point: QApplication, login flow, main window.

#include "viewer/login_dialog.h"
#include "viewer/main_window.h"
#include "viewer/connection_settings.h"
#include "core/auth_client.h"
#include "core/log_redaction.h"

#include <QApplication>
#include <QDateTime>
#include <QFutureWatcher>
#include <QIcon>
#include <QLabel>
#include <QSettings>
#include <QStyle>
#include <QStyleFactory>
#include <QtConcurrent>
#include <QtGlobal>
#include <functional>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace fs = std::filesystem;

namespace {

constexpr uintmax_t kMaxViewerLogFileSizeBytes = 5ull * 1024ull * 1024ull;
std::mutex g_viewer_log_mutex;

fs::path GetViewerLogPath() {
    try {
        return fs::temp_directory_path() / "remotex_viewer.log";
    } catch (...) {
        return fs::path("remotex_viewer.log");
    }
}

const char* ViewerLogLevelName(QtMsgType type) {
    switch (type) {
        case QtDebugMsg:
            return "DEBUG";
        case QtInfoMsg:
            return "INFO";
        case QtWarningMsg:
            return "WARN";
        case QtCriticalMsg:
            return "ERROR";
        case QtFatalMsg:
            return "FATAL";
        default:
            return "LOG";
    }
}

void TrimViewerLogIfNeeded(const fs::path& log_path) {
    std::error_code ec;
    const auto size = fs::file_size(log_path, ec);
    if (ec || size <= kMaxViewerLogFileSizeBytes) {
        return;
    }

    std::ofstream trunc(log_path, std::ios::trunc | std::ios::binary);
    (void)trunc;
}

void ViewerMessageHandler(QtMsgType type,
                          const QMessageLogContext&,
                          const QString& msg) {
    const QByteArray msg_utf8 = msg.toUtf8();
    const std::string sanitized_std = remotex::core::SanitizeLogMessage(
        std::string(msg_utf8.constData(), static_cast<size_t>(msg_utf8.size())));
    const QString sanitized_msg = QString::fromUtf8(
        sanitized_std.c_str(),
        static_cast<int>(sanitized_std.size()));
    const QString ts = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
    const QString line = QString("%1 [%2] %3")
        .arg(ts, ViewerLogLevelName(type), sanitized_msg);
    const QByteArray utf8 = line.toUtf8();

    {
        std::lock_guard lock(g_viewer_log_mutex);
        const fs::path log_path = GetViewerLogPath();
        TrimViewerLogIfNeeded(log_path);
        std::ofstream out(log_path, std::ios::app | std::ios::binary);
        if (out.is_open()) {
            out.write(utf8.constData(), utf8.size());
            out.put('\n');
            out.flush();
        }
    }

    std::fprintf(stderr, "%s\n", utf8.constData());
    std::fflush(stderr);

    if (type == QtFatalMsg) {
        std::abort();
    }
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("RemoteX");
    app.setOrganizationName("RemoteX");
    qInstallMessageHandler(ViewerMessageHandler);
    qInfo().noquote() << QString("[viewer] log file=%1")
                             .arg(QString::fromStdString(GetViewerLogPath().string()));
    app.setStyle(QStyleFactory::create("Fusion"));
    const QIcon app_icon(":/app/remotex.ico");
    app.setWindowIcon(app_icon.isNull() ? app.style()->standardIcon(QStyle::SP_ComputerIcon) : app_icon);

    // Configurar AuthClient.
    auto conn_settings = remotex::viewer::ConnectionSettings::Load();
    remotex::core::AuthClient auth;
    auth.Configure(conn_settings.ToAuthConfig());

    // Pré-carregar tokens salvos.
    auth.LoadStoredTokens();

    const QUrl health_url = conn_settings.BuildApiUrl("/health");

    // Callback para mostrar main window após login.
    std::function<void()> showMainWindow = [&]() {
        auto* main_window = new remotex::viewer::MainWindow(&auth);

        QObject::connect(main_window, &remotex::viewer::MainWindow::LoggedOut, [&]() {
            auth.Logout();
            auth.ClearStoredTokens();

            // Limpar remember-me ao fazer logout explícito.
            QSettings settings("RemoteX", "Viewer");
            settings.remove("login/remember_me");
            settings.remove("login/remembered_email");

            // Mostrar login de novo.
            auto* login = new remotex::viewer::LoginDialog(&auth, health_url);
            QObject::connect(login, &remotex::viewer::LoginDialog::Authenticated, [&, login]() {
                showMainWindow();
                login->close();
                login->deleteLater();
            });
            login->show();
        });

        main_window->show();
    };

    // Verificar se "Lembrar login" está ativo e temos tokens salvos.
    QSettings settings("RemoteX", "Viewer");
    const bool remember_me = settings.value("login/remember_me", false).toBool();

    if (remember_me && auth.IsAuthenticated()) {
        // Tentar auto-login via refresh token em background.
        auto* splash = new QLabel("Conectando...");
        splash->setAlignment(Qt::AlignCenter);
        splash->setFixedSize(280, 100);
        auto font = splash->font();
        font.setPointSize(14);
        splash->setFont(font);
        splash->setWindowFlags(Qt::SplashScreen | Qt::FramelessWindowHint);
        splash->show();

        auto* watcher = new QFutureWatcher<bool>(&app);
        QObject::connect(watcher, &QFutureWatcher<bool>::finished, [&, splash, watcher]() {
            const bool refreshed = watcher->result();
            watcher->deleteLater();
            splash->close();
            splash->deleteLater();

            if (refreshed) {
                showMainWindow();
            } else {
                // Refresh falhou — mostrar LoginDialog com email pré-preenchido.
                auto* login = new remotex::viewer::LoginDialog(&auth, health_url);
                QObject::connect(login, &remotex::viewer::LoginDialog::Authenticated, [&, login]() {
                    showMainWindow();
                    login->close();
                    login->deleteLater();
                });
                login->show();
            }
        });

        watcher->setFuture(QtConcurrent::run([&auth]() {
            auto result = auth.RefreshToken();
            return result.success;
        }));
    } else {
        // Sem remember-me — mostrar LoginDialog normalmente.
        auto* login = new remotex::viewer::LoginDialog(&auth, health_url);
        QObject::connect(login, &remotex::viewer::LoginDialog::Authenticated, [&, login]() {
            showMainWindow();
            login->close();
            login->deleteLater();
        });
        login->show();
    }

    return app.exec();
}
