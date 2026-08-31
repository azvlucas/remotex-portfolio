// RemoteX - viewer/main_window.cpp
// Tela principal: lista de maquinas com CRUD, refresh a cada 30s.

#include "viewer/main_window.h"

#include "viewer/group_management_dialog.h"
#include "viewer/reports_dialog.h"
#include "viewer/user_management_dialog.h"
#include "viewer/connection_settings.h"
#include "viewer/machine_display_utils.h"
#include "viewer/machine_edit_dialog.h"
#include "viewer/presigned_url_utils.h"
#include "viewer/remote_session_window.h"
#include "viewer/settings_dialog.h"
#include "viewer/standalone_chat_dialog.h"

#include <QApplication>
#include <QtConcurrent>
#include <QFutureWatcher>
#include <algorithm>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QMenu>
#include <QSizePolicy>
#include <QSplitter>
#include <QStatusBar>
#include <QStringList>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <unordered_map>

namespace remotex::viewer {

#include "main_window_common.inc"
#include "main_window_layout.inc"
#include "main_window_machine.inc"
#include "main_window_audit.inc"
#include "main_window_actions.inc"

} // namespace remotex::viewer
