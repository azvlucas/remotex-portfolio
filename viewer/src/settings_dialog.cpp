// RemoteX — viewer/settings_dialog.cpp
// Dialogo de configuracoes: Geral + Exportar Host.

#include "viewer/settings_dialog.h"

#include "viewer/connection_settings.h"

#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

namespace remotex::viewer {

SettingsDialog::SettingsDialog(remotex::core::AuthClient* auth,
                               const QString& machine_id,
                               QWidget* parent)
    : QDialog(parent)
    , auth_(auth)
    , machine_id_(machine_id)
    , connection_settings_(ConnectionSettings::Load())
{
    SetupUI();
    LoadSettings();
}

void SettingsDialog::SetupUI() {
    setWindowTitle("Configuracoes");
    resize(400, 180);

    auto* layout = new QVBoxLayout(this);

    start_with_win_ = new QCheckBox("Iniciar com o Windows");
    layout->addWidget(start_with_win_);

    layout->addSpacing(12);

    export_btn_ = new QPushButton("Exportar Host");
    export_btn_->setToolTip("Gera o instalador do agente remoto para enviar a maquinas clientes.");
    connect(export_btn_, &QPushButton::clicked, this, &SettingsDialog::OnExportHost);
    layout->addWidget(export_btn_);

    layout->addStretch();

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::OnSave);
    connect(buttons, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);
    layout->addWidget(buttons);
}

void SettingsDialog::LoadSettings() {
    QSettings settings("RemoteX", "Viewer");
    start_with_win_->setChecked(settings.value("general/start_with_windows", false).toBool());
}

void SettingsDialog::OnSave() {
    QSettings settings("RemoteX", "Viewer");
    settings.setValue("general/start_with_windows", start_with_win_->isChecked());
    accept();
}

void SettingsDialog::OnExportHost() {
    auto dest = QFileDialog::getExistingDirectory(this, "Selecionar pasta destino para o instalador");
    if (dest.isEmpty()) return;

    const QString setup_path = dest + "/RemoteX_Host_Setup.exe";
    if (QFile::exists(setup_path)) {
        auto reply = QMessageBox::question(this, "Exportar Host",
            "Ja existe um instalador nesta pasta. Deseja sobrescrever?",
            QMessageBox::Yes | QMessageBox::No);
        if (reply != QMessageBox::Yes) return;
        QFile::remove(setup_path);
    }

    const std::string access_token = auth_ ? auth_->GetAccessToken() : std::string();
    if (access_token.empty()) {
        QMessageBox::warning(this, "Exportar Host",
            "Voce precisa estar autenticado para gerar o instalador.");
        return;
    }

    export_btn_->setEnabled(false);
    export_btn_->setText("Gerando instalador...");

    QNetworkAccessManager* manager = new QNetworkAccessManager(this);
    QNetworkRequest request{connection_settings_.BuildApiUrl("/host/installer")};
    request.setRawHeader("Authorization",
        QByteArray("Bearer ") + QByteArray::fromStdString(access_token));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setTransferTimeout(120000);

    const QByteArray request_body =
        QJsonDocument(connection_settings_.ToInstallerPayload()).toJson(QJsonDocument::Compact);
    QNetworkReply* reply = manager->post(request, request_body);
    connect(reply, &QNetworkReply::finished, this, [this, reply, setup_path, manager]() {
        reply->deleteLater();
        manager->deleteLater();
        export_btn_->setEnabled(true);
        export_btn_->setText("Exportar Host");

        if (reply->error() != QNetworkReply::NoError) {
            QString error_msg = reply->errorString();
            const QByteArray body = reply->readAll();
            if (!body.isEmpty()) {
                error_msg += "\n\n" + QString::fromUtf8(body);
            }
            QMessageBox::warning(this, "Exportar Host",
                "Falha ao gerar o instalador:\n" + error_msg);
            return;
        }

        const int status_code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status_code != 200) {
            const QByteArray body = reply->readAll();
            QMessageBox::warning(this, "Exportar Host",
                QString("Servidor retornou erro %1:\n%2")
                    .arg(status_code)
                    .arg(QString::fromUtf8(body)));
            return;
        }

        const QByteArray data = reply->readAll();
        if (data.isEmpty()) {
            QMessageBox::warning(this, "Exportar Host", "Resposta vazia do servidor.");
            return;
        }

        QFile file(setup_path);
        if (!file.open(QIODevice::WriteOnly)) {
            QMessageBox::warning(this, "Exportar Host",
                "Falha ao salvar o instalador em:\n" + setup_path);
            return;
        }
        file.write(data);
        file.close();

        QMessageBox::information(this, "Exportar Host",
            "Instalador gerado com sucesso!\n\n" + setup_path +
            "\n\nEnvie este arquivo para a maquina remota e execute-o.\n"
            "O servico RemoteX sera instalado automaticamente.");
    });
}

} // namespace remotex::viewer
