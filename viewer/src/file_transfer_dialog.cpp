// RemoteX - viewer/file_transfer_dialog.cpp

#include "viewer/file_transfer_dialog.h"

#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QTableWidget>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>
#include <QWidget>

#include <utility>

namespace remotex::viewer {

#include "file_transfer_dialog_ui.inc"
#include "file_transfer_dialog_paths.inc"
#include "file_transfer_dialog_transfer.inc"

} // namespace remotex::viewer
