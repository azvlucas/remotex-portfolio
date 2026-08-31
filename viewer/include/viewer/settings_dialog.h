#pragma once
// RemoteX — viewer/settings_dialog.h
// Dialogo de configuracoes: Geral + Exportar Host.

#include "viewer/connection_settings.h"
#include "core/auth_client.h"

#include <QCheckBox>
#include <QDialog>
#include <QPushButton>

namespace remotex::viewer {

class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(remotex::core::AuthClient* auth,
                            const QString& machine_id,
                            QWidget* parent = nullptr);

private slots:
    void OnSave();
    void OnExportHost();

private:
    void SetupUI();
    void LoadSettings();

    remotex::core::AuthClient* auth_;
    QString                    machine_id_;
    ConnectionSettings         connection_settings_;

    QCheckBox*   start_with_win_ = nullptr;
    QPushButton* export_btn_     = nullptr;
};

} // namespace remotex::viewer
