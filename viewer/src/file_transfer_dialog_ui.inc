// RemoteX - viewer/file_transfer_dialog_ui.inc

namespace {

constexpr int kRoleAbsolutePath  = Qt::UserRole + 1;
constexpr int kRoleIsDirectory   = Qt::UserRole + 2;
constexpr int kRoleSortPrimary   = Qt::UserRole + 3;
constexpr int kRoleSortSecondary = Qt::UserRole + 4;

class RemoteTableItem final : public QTableWidgetItem {
public:
    explicit RemoteTableItem(const QString& text)
        : QTableWidgetItem(text) {
    }

    void SetSortKeys(const QVariant& primary, const QVariant& secondary = {}) {
        setData(kRoleSortPrimary, primary);
        setData(kRoleSortSecondary, secondary);
    }

    bool operator<(const QTableWidgetItem& other) const override {
        if (const int primary_compare = CompareSortValue(data(kRoleSortPrimary), other.data(kRoleSortPrimary));
            primary_compare != 0) {
            return primary_compare < 0;
        }

        if (const int secondary_compare = CompareSortValue(data(kRoleSortSecondary), other.data(kRoleSortSecondary));
            secondary_compare != 0) {
            return secondary_compare < 0;
        }

        return QTableWidgetItem::operator<(other);
    }

private:
    static int CompareSortValue(const QVariant& lhs, const QVariant& rhs) {
        if (!lhs.isValid() || !rhs.isValid()) {
            if (lhs.isValid() == rhs.isValid()) {
                return 0;
            }
            return lhs.isValid() ? 1 : -1;
        }

        bool lhs_numeric = false;
        const qlonglong lhs_number = lhs.toLongLong(&lhs_numeric);
        bool rhs_numeric = false;
        const qlonglong rhs_number = rhs.toLongLong(&rhs_numeric);
        if (lhs_numeric && rhs_numeric) {
            if (lhs_number < rhs_number) {
                return -1;
            }
            if (lhs_number > rhs_number) {
                return 1;
            }
            return 0;
        }

        const int text_compare = QString::localeAwareCompare(lhs.toString(), rhs.toString());
        if (text_compare < 0) {
            return -1;
        }
        if (text_compare > 0) {
            return 1;
        }
        return 0;
    }
};

RemoteTableItem* MakeRemoteTableItem(const QString& text,
                                     const QVariant& primary_sort_key,
                                     const QVariant& secondary_sort_key = {}) {
    auto* item = new RemoteTableItem(text);
    item->SetSortKeys(primary_sort_key, secondary_sort_key);
    return item;
}

QString RemoteEntryTypeLabel(bool is_directory) {
    return is_directory ? QStringLiteral("Pasta") : QStringLiteral("Arquivo");
}

void ConfigureActionButton(QPushButton* button,
                           const QString& object_name,
                           const QString& text,
                           const QString& tooltip) {
    button->setObjectName(object_name);
    button->setText(text);
    button->setToolTip(tooltip);
    button->setMinimumSize(56, 40);
}

bool IsClipboardStageDestination(const QString& path) {
    return path.contains(QStringLiteral("remotex_clipboard"), Qt::CaseInsensitive);
}

} // namespace

void FileTransferDialog::SetupUi() {
    setObjectName("file_transfer_dialog");
    setWindowTitle("Transferencia de Arquivos");
    resize(1180, 680);

    auto* main_layout = new QVBoxLayout(this);
    auto* panels_layout = new QHBoxLayout;
    panels_layout->setSpacing(18);

    auto* local_panel = new QWidget(this);
    local_panel->setObjectName("file_transfer_local_panel");
    auto* local_layout = new QVBoxLayout(local_panel);
    auto* local_header = new QHBoxLayout;

    local_up_btn_ = new QToolButton(local_panel);
    local_up_btn_->setObjectName("file_transfer_local_up");
    local_up_btn_->setText("Subir");
    local_refresh_btn_ = new QToolButton(local_panel);
    local_refresh_btn_->setObjectName("file_transfer_local_refresh");
    local_refresh_btn_->setText("Atualizar");
    local_path_edit_ = new QLineEdit(local_panel);
    local_path_edit_->setObjectName("file_transfer_local_path");
    local_path_edit_->setReadOnly(true);

    local_header->addWidget(new QLabel("Local", local_panel));
    local_header->addStretch(1);
    local_header->addWidget(local_up_btn_);
    local_header->addWidget(local_refresh_btn_);
    local_layout->addLayout(local_header);
    local_layout->addWidget(local_path_edit_);

    local_model_ = new QFileSystemModel(this);
    local_model_->setFilter(QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Drives);
    local_model_->setRootPath(QDir::homePath());
    connect(local_model_, &QFileSystemModel::directoryLoaded,
            this, [this](const QString&) { UpdateActionState(); });

    local_view_ = new QTreeView(local_panel);
    local_view_->setObjectName("file_transfer_local_view");
    local_view_->setModel(local_model_);
    local_view_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    local_view_->setSortingEnabled(true);
    local_view_->sortByColumn(0, Qt::AscendingOrder);
    local_view_->setRootIsDecorated(false);
    local_layout->addWidget(local_view_, 1);

    auto* action_panel = new QWidget(this);
    action_panel->setObjectName("file_transfer_action_column");
    auto* action_layout = new QVBoxLayout(action_panel);
    action_layout->setContentsMargins(0, 0, 0, 0);
    action_layout->setSpacing(12);
    action_layout->addStretch(1);

    send_btn_ = new QPushButton(action_panel);
    ConfigureActionButton(send_btn_,
                          "file_transfer_send_selection",
                          QStringLiteral("\u2192"),
                          "Enviar selecao para o painel remoto");
    action_layout->addWidget(send_btn_, 0, Qt::AlignHCenter);

    send_tree_btn_ = new QPushButton(action_panel);
    ConfigureActionButton(send_tree_btn_,
                          "file_transfer_send_tree",
                          QStringLiteral("\u21D2"),
                          "Enviar arvore atual para o painel remoto");
    action_layout->addWidget(send_tree_btn_, 0, Qt::AlignHCenter);

    receive_btn_ = new QPushButton(action_panel);
    ConfigureActionButton(receive_btn_,
                          "file_transfer_receive_selection",
                          QStringLiteral("\u2190"),
                          "Receber selecao para o painel local");
    action_layout->addWidget(receive_btn_, 0, Qt::AlignHCenter);

    receive_tree_btn_ = new QPushButton(action_panel);
    ConfigureActionButton(receive_tree_btn_,
                          "file_transfer_receive_tree",
                          QStringLiteral("\u21D0"),
                          "Receber arvore atual para o painel local");
    action_layout->addWidget(receive_tree_btn_, 0, Qt::AlignHCenter);
    action_layout->addStretch(1);

    auto* remote_panel = new QWidget(this);
    remote_panel->setObjectName("file_transfer_remote_panel");
    auto* remote_layout = new QVBoxLayout(remote_panel);
    auto* remote_header = new QHBoxLayout;

    remote_up_btn_ = new QToolButton(remote_panel);
    remote_up_btn_->setObjectName("file_transfer_remote_up");
    remote_up_btn_->setText("Subir");
    remote_refresh_btn_ = new QToolButton(remote_panel);
    remote_refresh_btn_->setObjectName("file_transfer_remote_refresh");
    remote_refresh_btn_->setText("Atualizar");
    remote_path_edit_ = new QLineEdit(remote_panel);
    remote_path_edit_->setObjectName("file_transfer_remote_path");
    remote_path_edit_->setReadOnly(true);

    remote_header->addWidget(new QLabel("Remoto", remote_panel));
    remote_header->addStretch(1);
    remote_header->addWidget(remote_up_btn_);
    remote_header->addWidget(remote_refresh_btn_);
    remote_layout->addLayout(remote_header);
    remote_layout->addWidget(remote_path_edit_);

    remote_table_ = new QTableWidget(remote_panel);
    remote_table_->setObjectName("file_transfer_remote_table");
    remote_table_->setColumnCount(4);
    remote_table_->setHorizontalHeaderLabels({"Nome", "Tipo", "Tamanho", "Modificado"});
    remote_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    remote_table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    remote_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    remote_table_->setAlternatingRowColors(true);
    remote_table_->setSortingEnabled(true);
    remote_table_->sortByColumn(0, Qt::AscendingOrder);
    remote_table_->verticalHeader()->setVisible(false);
    remote_table_->horizontalHeader()->setStretchLastSection(true);
    remote_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    remote_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    remote_table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    remote_table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    remote_layout->addWidget(remote_table_, 1);

    panels_layout->addWidget(local_panel, 1);
    panels_layout->addWidget(action_panel, 0, Qt::AlignCenter);
    panels_layout->addWidget(remote_panel, 1);
    main_layout->addLayout(panels_layout, 1);

    progress_bar_ = new QProgressBar(this);
    progress_bar_->setObjectName("file_transfer_progress_bar");
    progress_bar_->setRange(0, 100);
    progress_bar_->setValue(0);
    progress_label_ = new QLabel("Selecione arquivos ou pastas para transferir.", this);
    progress_label_->setObjectName("file_transfer_progress_label");
    main_layout->addWidget(progress_bar_);
    main_layout->addWidget(progress_label_);

    connect(local_view_, &QTreeView::doubleClicked,
            this, &FileTransferDialog::OnLocalActivated);
    connect(remote_table_, &QTableWidget::cellDoubleClicked,
            this, &FileTransferDialog::OnRemoteActivated);
    connect(local_up_btn_, &QToolButton::clicked,
            this, &FileTransferDialog::NavigateLocalUp);
    connect(local_refresh_btn_, &QToolButton::clicked,
            this, &FileTransferDialog::RefreshLocal);
    connect(remote_up_btn_, &QToolButton::clicked,
            this, &FileTransferDialog::NavigateRemoteUp);
    connect(remote_refresh_btn_, &QToolButton::clicked,
            this, &FileTransferDialog::RefreshRemote);
    connect(send_btn_, &QPushButton::clicked,
            this, &FileTransferDialog::SendSelection);
    connect(send_tree_btn_, &QPushButton::clicked,
            this, &FileTransferDialog::SendCurrentTree);
    connect(receive_btn_, &QPushButton::clicked,
            this, &FileTransferDialog::ReceiveSelection);
    connect(receive_tree_btn_, &QPushButton::clicked,
            this, &FileTransferDialog::ReceiveCurrentTree);
    connect(remote_table_, &QTableWidget::itemSelectionChanged,
            this, &FileTransferDialog::OnRemoteSelectionChanged);

    RebindLocalSelectionModel();
}

void FileTransferDialog::RebindLocalSelectionModel() {
    if (!local_view_ || !local_view_->selectionModel()) {
        return;
    }

    disconnect(local_view_->selectionModel(), &QItemSelectionModel::selectionChanged,
               this, &FileTransferDialog::OnLocalSelectionChanged);
    connect(local_view_->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &FileTransferDialog::OnLocalSelectionChanged);
}

QString FileTransferDialog::FormatSize(quint64 bytes) const {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit_index = 0;
    while (value >= 1024.0 && unit_index < 4) {
        value /= 1024.0;
        ++unit_index;
    }
    return QString::number(value, unit_index == 0 ? 'f' : 'f', unit_index == 0 ? 0 : 1) + " " + units[unit_index];
}

QString FileTransferDialog::FormatTimestamp(qint64 unix_sec) const {
    if (unix_sec <= 0) {
        return "-";
    }
    return QDateTime::fromSecsSinceEpoch(unix_sec).toString("dd/MM/yyyy HH:mm");
}

