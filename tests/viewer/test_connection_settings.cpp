#include "viewer/connection_settings.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QSettings>
#include <QTemporaryDir>

namespace {

class ConnectionSettingsTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(temp_dir_.isValid());
        previous_format_ = QSettings::defaultFormat();
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp_dir_.filePath("user-settings"));

        qputenv("REMOTEX_VIEWER_CONFIG_PATH", installer_config_path_.toUtf8());
        qputenv("REMOTEX_VIEWER_USER_SETTINGS_PATH", user_settings_path_.toUtf8());
    }

    void TearDown() override {
        qunsetenv("REMOTEX_VIEWER_CONFIG_PATH");
        qunsetenv("REMOTEX_VIEWER_USER_SETTINGS_PATH");
        QSettings::setDefaultFormat(previous_format_);
    }

    void WriteInstallerConfig(const QString& content) {
        QFile file(installer_config_path_);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(content.toUtf8());
        file.close();
    }

    QTemporaryDir temp_dir_;
    QString       installer_config_path_ = temp_dir_.filePath("connection.ini");
    QString       user_settings_path_ = temp_dir_.filePath("viewer-user.ini");

private:
    QSettings::Format previous_format_ = QSettings::NativeFormat;
};

TEST_F(ConnectionSettingsTest, LoadsInstallerIniBeforeUserOverrides) {
    WriteInstallerConfig(
        "[connection]\n"
        "api_url=https://api.example.com\n"
        "signaling_host=signaling.example.com\n"
        "signaling_port=8443\n"
        "signaling_use_tls=0\n"
        "relay_host=relay.example.com\n"
        "relay_port=9200\n");

    const auto settings = remotex::viewer::ConnectionSettings::Load();

    EXPECT_EQ(settings.api_host, QString("api.example.com"));
    EXPECT_EQ(settings.api_port, 443);
    EXPECT_TRUE(settings.api_use_tls);
    EXPECT_EQ(settings.signaling_host, QString("signaling.example.com"));
    EXPECT_EQ(settings.signaling_port, 8443);
    EXPECT_FALSE(settings.signaling_use_tls);
    EXPECT_EQ(settings.relay_host, QString("relay.example.com"));
    EXPECT_EQ(settings.relay_port, 9200);
}

TEST_F(ConnectionSettingsTest, UserSettingsOverrideInstallerIni) {
    WriteInstallerConfig(
        "[connection]\n"
        "api_host=installer-api.example.com\n"
        "api_port=443\n"
        "api_use_tls=1\n"
        "signaling_host=installer-signaling.example.com\n"
        "signaling_port=443\n"
        "signaling_use_tls=true\n");

    QSettings user_settings(user_settings_path_, QSettings::IniFormat);
    user_settings.setValue("connection/api_host", "user-api.example.com");
    user_settings.setValue("connection/api_port", 9443);
    user_settings.setValue("connection/api_use_tls", false);
    user_settings.sync();

    const auto settings = remotex::viewer::ConnectionSettings::Load();

    EXPECT_EQ(settings.api_host, QString("user-api.example.com"));
    EXPECT_EQ(settings.api_port, 9443);
    EXPECT_FALSE(settings.api_use_tls);
    EXPECT_EQ(settings.signaling_host, QString("installer-signaling.example.com"));
    EXPECT_EQ(settings.signaling_port, 443);
    EXPECT_TRUE(settings.signaling_use_tls);
}

} // namespace
