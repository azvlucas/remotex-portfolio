// RemoteX - viewer/remote_session_window.cpp

#include "viewer/remote_session_window.h"

#include "viewer/chat_panel.h"
#include "viewer/connection_settings.h"
#include "core/log_redaction.h"
#include "core/windows_shell_utils.h"
#include "viewer/diagnostics_window.h"
#include "viewer/file_transfer_dialog.h"

#include <QClipboard>
#include <QCloseEvent>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QMetaObject>
#include <QMimeData>
#include <QProgressBar>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace remotex::viewer {

#include "remote_session_window_common.inc"
#include "remote_session_window_transport.inc"
#include "remote_session_window_network.inc"
#include "remote_session_window_ctor.inc"
#include "remote_session_window_ui.inc"
#include "remote_session_window_presentation.inc"

} // namespace remotex::viewer
