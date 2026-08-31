// RemoteX - viewer/file_transfer_dialog_paths.inc

void FileTransferDialog::SetLocalRoots() {
    if (!local_model_ || !local_view_ || !local_path_edit_) {
        return;
    }

    local_model_->setRootPath(QString{});
    local_view_->setRootIndex(QModelIndex{});
    RebindLocalSelectionModel();
    local_view_->clearSelection();

    local_at_roots_ = true;
    local_current_path_.clear();
    local_path_edit_->setText(QStringLiteral("Raizes"));

    UpdateActionState();
}

void FileTransferDialog::SetLocalRoot(const QString& path) {
    if (path.trimmed().isEmpty()) {
        SetLocalRoots();
        return;
    }

    const QString normalized_path = QDir::cleanPath(path);
    const QModelIndex root_index = local_model_->setRootPath(normalized_path);
    local_view_->setRootIndex(root_index);
    RebindLocalSelectionModel();
    local_view_->clearSelection();

    local_at_roots_ = false;
    local_current_path_ = normalized_path;
    local_path_edit_->setText(normalized_path);

    UpdateActionState();
}

void FileTransferDialog::ApplyRemotePathState(bool at_roots, const QString& path) {
    const QString normalized_path = at_roots ? QString{} : QDir::cleanPath(path);
    remote_at_roots_ = at_roots || normalized_path.isEmpty();
    remote_current_path_ = remote_at_roots_ ? QString{} : normalized_path;
    remote_path_edit_->setText(remote_at_roots_ ? QStringLiteral("Raizes") : remote_current_path_);
    remote_table_->clearSelection();
}

void FileTransferDialog::RequestRemoteRoots() {
    ApplyRemotePathState(true, QString{});
    if (dispatch_hooks_.request_remote_roots) {
        dispatch_hooks_.request_remote_roots();
    }
    UpdateActionState();
}

void FileTransferDialog::RequestRemoteListing(const QString& path) {
    const QString normalized_path = QDir::cleanPath(path);
    if (path.trimmed().isEmpty() || normalized_path.isEmpty()) {
        RequestRemoteRoots();
        return;
    }

    ApplyRemotePathState(false, normalized_path);
    if (dispatch_hooks_.request_remote_listing) {
        dispatch_hooks_.request_remote_listing(normalized_path);
    }
    UpdateActionState();
}

void FileTransferDialog::PopulateRemoteTable(const remotex::viewer::RemoteListingResult& listing) {
    const int current_sort_column = remote_table_->horizontalHeader()->sortIndicatorSection();
    const Qt::SortOrder current_sort_order = remote_table_->horizontalHeader()->sortIndicatorOrder();
    const bool sorting_enabled = remote_table_->isSortingEnabled();

    remote_table_->setSortingEnabled(false);
    remote_table_->setRowCount(0);
    ApplyRemotePathState(listing.is_root_listing || listing.path.trimmed().isEmpty(), listing.path);

    for (const auto& entry : listing.entries) {
        const int row = remote_table_->rowCount();
        remote_table_->insertRow(row);

        const QString name_sort_key = entry.name.toCaseFolded();
        const qlonglong size_sort_key = entry.is_directory ? -1 : static_cast<qlonglong>(entry.size_bytes);

        auto* name_item = MakeRemoteTableItem(entry.name,
                                              name_sort_key,
                                              QDir::cleanPath(entry.absolute_path).toCaseFolded());
        name_item->setData(kRoleAbsolutePath, entry.absolute_path);
        name_item->setData(kRoleIsDirectory, entry.is_directory);
        auto* type_item = MakeRemoteTableItem(RemoteEntryTypeLabel(entry.is_directory),
                                              entry.is_directory ? 0 : 1,
                                              name_sort_key);
        auto* size_item = MakeRemoteTableItem(entry.is_directory ? QStringLiteral("-") : FormatSize(entry.size_bytes),
                                              size_sort_key,
                                              name_sort_key);
        auto* modified_item = MakeRemoteTableItem(FormatTimestamp(entry.modified_unix_sec),
                                                  entry.modified_unix_sec,
                                                  name_sort_key);

        remote_table_->setItem(row, 0, name_item);
        remote_table_->setItem(row, 1, type_item);
        remote_table_->setItem(row, 2, size_item);
        remote_table_->setItem(row, 3, modified_item);
    }

    if (sorting_enabled) {
        remote_table_->setSortingEnabled(true);
        if (current_sort_column >= 0 && current_sort_column < remote_table_->columnCount()) {
            remote_table_->sortItems(current_sort_column, current_sort_order);
            remote_table_->horizontalHeader()->setSortIndicator(current_sort_column, current_sort_order);
        }
    }

    UpdateActionState();
}

QString FileTransferDialog::CurrentLocalDirectory() const {
    return local_at_roots_ ? QString{} : local_current_path_;
}

QStringList FileTransferDialog::SelectedLocalPaths() const {
    QStringList paths;
    if (!local_view_ || !local_view_->selectionModel()) {
        return paths;
    }

    const auto rows = local_view_->selectionModel()->selectedRows(0);
    for (const auto& index : rows) {
        const QString path = local_model_->filePath(index);
        if (!path.isEmpty() && !paths.contains(path)) {
            paths.push_back(path);
        }
    }
    return paths;
}

QStringList FileTransferDialog::SelectedRemotePaths() const {
    QStringList paths;
    const auto items = remote_table_->selectedItems();
    for (auto* item : items) {
        if (item->column() != 0) {
            continue;
        }
        const QString path = item->data(kRoleAbsolutePath).toString();
        if (!path.isEmpty() && !paths.contains(path)) {
            paths.push_back(path);
        }
    }
    return paths;
}

QStringList FileTransferDialog::ListedLocalChildPaths() const {
    QStringList paths;
    if (!local_model_ || !local_view_) {
        return paths;
    }

    const QModelIndex root_index = local_view_->rootIndex();
    int fetch_guard = 0;
    while (local_model_->canFetchMore(root_index) && fetch_guard++ < 64) {
        local_model_->fetchMore(root_index);
    }

    const int row_count = local_model_->rowCount(root_index);
    if (row_count <= 0) {
        return paths;
    }

    for (int row = 0; row < row_count; ++row) {
        const QModelIndex child_index = local_model_->index(row, 0, root_index);
        if (!child_index.isValid()) {
            return {};
        }

        const QString child_path = QDir::cleanPath(local_model_->filePath(child_index));
        if (child_path.trimmed().isEmpty()) {
            return {};
        }

        if (!paths.contains(child_path)) {
            paths.push_back(child_path);
        }
    }

    return paths;
}

QStringList FileTransferDialog::ListedRemoteChildPaths() const {
    QStringList paths;
    if (!remote_table_) {
        return paths;
    }

    const int row_count = remote_table_->rowCount();
    if (row_count <= 0) {
        return paths;
    }

    for (int row = 0; row < row_count; ++row) {
        auto* item = remote_table_->item(row, 0);
        if (!item) {
            return {};
        }

        const QString child_path = QDir::cleanPath(item->data(kRoleAbsolutePath).toString());
        if (child_path.trimmed().isEmpty()) {
            return {};
        }

        if (!paths.contains(child_path)) {
            paths.push_back(child_path);
        }
    }

    return paths;
}

QStringList FileTransferDialog::CurrentLocalTreeTransferPaths() const {
    const QString current_directory = CurrentLocalDirectory();
    if (current_directory.isEmpty()) {
        return {};
    }

    const QStringList listed_children = ListedLocalChildPaths();
    if (IsDriveRootPath(current_directory)) {
        return listed_children;
    }

    if (!listed_children.isEmpty()) {
        return {current_directory};
    }

    const QDir directory(current_directory);
    const QFileInfoList entry_info = directory.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                                                             QDir::NoSort);
    if (entry_info.isEmpty()) {
        return {};
    }

    return {current_directory};
}

QStringList FileTransferDialog::CurrentRemoteTreeTransferPaths() const {
    if (remote_at_roots_ || remote_current_path_.trimmed().isEmpty()) {
        return {};
    }

    const QStringList listed_children = ListedRemoteChildPaths();
    if (listed_children.isEmpty()) {
        return {};
    }

    if (IsDriveRootPath(remote_current_path_)) {
        return listed_children;
    }

    return {remote_current_path_};
}

bool FileTransferDialog::IsDriveRootPath(const QString& path) {
    const QString normalized_path = QDir::cleanPath(path).trimmed();
    if (normalized_path.isEmpty()) {
        return false;
    }

    if (normalized_path == QStringLiteral("/")) {
        return true;
    }

    if (normalized_path.size() >= 2 &&
        normalized_path.at(0).isLetter() &&
        normalized_path.at(1) == QLatin1Char(':')) {
        const QString tail = normalized_path.mid(2);
        if (tail.isEmpty() || tail == QStringLiteral("/")) {
            return true;
        }
    }

    return QFileInfo(normalized_path).isRoot();
}

void FileTransferDialog::OnLocalActivated(const QModelIndex& index) {
    if (!index.isValid()) {
        return;
    }

    if (local_model_->isDir(index)) {
        const QString path = local_model_->filePath(index);
        if (!path.isEmpty()) {
            SetLocalRoot(path);
        }
    }
}

void FileTransferDialog::OnRemoteActivated(int row, int /*column*/) {
    auto* item = remote_table_->item(row, 0);
    if (!item) {
        return;
    }

    if (item->data(kRoleIsDirectory).toBool()) {
        RequestRemoteListing(item->data(kRoleAbsolutePath).toString());
    }
}

void FileTransferDialog::OnLocalSelectionChanged() {
    UpdateActionState();
}

void FileTransferDialog::OnRemoteSelectionChanged() {
    UpdateActionState();
}

void FileTransferDialog::NavigateLocalUp() {
    if (local_at_roots_) {
        return;
    }

    const QString current_path = CurrentLocalDirectory();
    if (current_path.isEmpty()) {
        SetLocalRoots();
        return;
    }

    QDir directory(current_path);
    if (directory.isRoot()) {
        SetLocalRoots();
        return;
    }

    if (!directory.cdUp()) {
        SetLocalRoots();
        return;
    }

    SetLocalRoot(directory.absolutePath());
}

void FileTransferDialog::RefreshLocal() {
    if (local_at_roots_ || CurrentLocalDirectory().isEmpty()) {
        SetLocalRoots();
        return;
    }

    SetLocalRoot(CurrentLocalDirectory());
}

void FileTransferDialog::NavigateRemoteUp() {
    if (remote_at_roots_) {
        return;
    }

    if (remote_current_path_.trimmed().isEmpty()) {
        RequestRemoteRoots();
        return;
    }

    QDir directory(remote_current_path_);
    if (directory.isRoot()) {
        RequestRemoteRoots();
        return;
    }

    if (!directory.cdUp()) {
        RequestRemoteRoots();
        return;
    }

    RequestRemoteListing(directory.absolutePath());
}

void FileTransferDialog::RefreshRemote() {
    if (remote_at_roots_ || remote_current_path_.trimmed().isEmpty()) {
        RequestRemoteRoots();
    } else {
        RequestRemoteListing(remote_current_path_);
    }
}

void FileTransferDialog::ApplyRemoteListing(const remotex::viewer::RemoteListingResult& listing) {
    if (!listing.success) {
        progress_label_->setText("Falha ao listar remoto: " + listing.error);
        return;
    }
    PopulateRemoteTable(listing);
}

