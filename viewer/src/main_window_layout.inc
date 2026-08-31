void MainWindow::SetupUI() {
    setWindowTitle(QString("RemoteX - Versao %1").arg(REMOTEX_VIEWER_VERSION));
    resize(1380, 620);

    auto* central = new QWidget;
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(12);

    auto* controls_layout = new QHBoxLayout;
    controls_layout->setContentsMargins(0, 0, 0, 0);
    controls_layout->setSpacing(8);

    search_input_ = new QLineEdit;
    search_input_->setPlaceholderText(
        "Buscar por nome, razao social, fantasia, documento, grupo ou subgrupo");
    search_input_->setClearButtonEnabled(true);
    search_input_->addAction(
        style()->standardIcon(QStyle::SP_FileDialogContentsView),
        QLineEdit::LeadingPosition);
    controls_layout->addWidget(search_input_, 1);

    status_filter_ = new QComboBox;
    status_filter_->addItem("Todas");
    status_filter_->addItem("Online");
    status_filter_->addItem("Offline");
    controls_layout->addWidget(status_filter_);

    controls_layout->addStretch();

    summary_label_ = new QLabel("Total: 0 | Online: 0 | Novas: 0");
    controls_layout->addWidget(summary_label_);

    layout->addLayout(controls_layout);

    update_bar_ = new QFrame();
    update_bar_->setStyleSheet("QFrame { background-color: #0f609b; color: white; padding: 8px 16px; font-weight: bold; }");
    update_bar_->setVisible(false);
    auto* update_layout = new QHBoxLayout(update_bar_);
    update_layout->setContentsMargins(16, 4, 16, 4);
    update_label_ = new QLabel();
    update_label_->setObjectName("update_label");
    update_label_->setStyleSheet("color: white; font-weight: bold;");
    update_layout->addWidget(update_label_);
    update_btn_ = new QPushButton("Atualizar agora");
    update_btn_->setStyleSheet("QPushButton { background: white; color: #0f609b; border: none; padding: 6px 16px; border-radius: 4px; font-weight: bold; }");
    connect(update_btn_, &QPushButton::clicked, this, &MainWindow::OnUpdateNow);
    update_layout->addWidget(update_btn_);
    layout->addWidget(update_bar_);

    auto* splitter = new QSplitter(Qt::Vertical, central);

    table_ = new QTableWidget;
    table_->setColumnCount(9);
    table_->setHorizontalHeaderLabels({
        "Status",
        "Nome",
        "Razao social",
        "Nome fantasia",
        "Documento",
        "Grupo",
        "Subgrupo",
        "Versao",
        "Ultima vez online",
    });
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(7, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(8, QHeaderView::ResizeToContents);
    table_->setAlternatingRowColors(true);
    table_->verticalHeader()->setVisible(false);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);

    connect(table_, &QTableWidget::cellDoubleClicked, this, &MainWindow::OnTableDoubleClicked);
    connect(table_, &QTableWidget::customContextMenuRequested, this, &MainWindow::OnTableContextMenu);
    connect(search_input_, &QLineEdit::textChanged, this, &MainWindow::OnSearchTextChanged);
    connect(status_filter_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::OnStatusFilterChanged);
    connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &MainWindow::OnMachineSelectionChanged);

    splitter->addWidget(table_);

    auto* audit_panel = new QWidget;
    auto* audit_layout = new QVBoxLayout(audit_panel);
    audit_layout->setContentsMargins(0, 0, 0, 0);
    audit_layout->setSpacing(8);

    auto* audit_header = new QHBoxLayout;
    audit_title_label_ = new QLabel("Historico da maquina");
    audit_filter_ = new QComboBox;
    audit_filter_->addItem("Todos");
    audit_filter_->addItem("Acessos");
    audit_filter_->addItem("Arquivos");
    audit_header->addWidget(audit_title_label_);
    audit_header->addStretch();
    audit_header->addWidget(audit_filter_);
    audit_layout->addLayout(audit_header);

    audit_summary_label_ = new QLabel("Selecione uma maquina para ver acessos e transferencias recentes.");
    audit_layout->addWidget(audit_summary_label_);

    audit_empty_label_ = new QLabel("Nenhuma maquina selecionada.");
    audit_layout->addWidget(audit_empty_label_);

    audit_table_ = new QTableWidget;
    audit_table_->setColumnCount(7);
    audit_table_->setHorizontalHeaderLabels({
        "Data/Hora",
        "Operador",
        "Tipo",
        "Duracao",
        "Resumo",
        "Status",
        "Gravacao",
    });
    audit_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    audit_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    audit_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    audit_table_->setAlternatingRowColors(true);
    audit_table_->verticalHeader()->setVisible(false);
    audit_table_->setContextMenuPolicy(Qt::CustomContextMenu);
    audit_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    audit_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    audit_table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    audit_table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    audit_table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    audit_table_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    audit_table_->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    audit_layout->addWidget(audit_table_, 1);

    auto* audit_bottom = new QHBoxLayout;
    audit_download_btn_ = new QPushButton("Baixar gravacao");
    audit_download_btn_->setEnabled(false);
    audit_download_btn_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    audit_bottom->addWidget(audit_download_btn_);
    audit_bottom->addStretch();
    audit_load_more_btn_ = new QPushButton("Carregar mais");
    audit_load_more_btn_->setEnabled(false);
    audit_bottom->addWidget(audit_load_more_btn_);
    audit_layout->addLayout(audit_bottom);

    connect(audit_filter_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::OnAuditFilterChanged);
    connect(audit_load_more_btn_, &QPushButton::clicked,
            this, &MainWindow::OnAuditLoadMore);
    connect(audit_download_btn_, &QPushButton::clicked,
            this, &MainWindow::OnAuditDownloadRecording);
    connect(audit_table_, &QTableWidget::customContextMenuRequested,
            this, &MainWindow::OnAuditTableContextMenu);
    connect(audit_table_->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &MainWindow::OnAuditSelectionChanged);

    splitter->addWidget(audit_panel);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    splitter->setSizes({420, 220});

    layout->addWidget(splitter, 1);
    setCentralWidget(central);

    status_label_ = new QLabel("Pronto");
    statusBar()->addWidget(status_label_);
    ClearAuditPanel();
}

void MainWindow::SetupToolbar() {
    toolbar_ = addToolBar("Principal");
    toolbar_->setMovable(false);
    toolbar_->setIconSize(QSize(20, 20));
    toolbar_->setToolButtonStyle(Qt::ToolButtonIconOnly);

    auto* s = style();
    auto set_action_hints = [](QAction* action, const QString& tooltip, const QString& status_tip = QString()) {
        if (!action) {
            return;
        }

        action->setToolTip(tooltip);
        action->setStatusTip(status_tip.isEmpty() ? tooltip : status_tip);
    };

    auto* connect_menu = new QMenu(this);
    act_connect_ = connect_menu->addAction(
        s->standardIcon(QStyle::SP_MediaPlay), "Controlar", this, &MainWindow::OnConnect);
    set_action_hints(act_connect_,
                     "Controlar maquina selecionada",
                     "Inicia uma sessao interativa para controlar a maquina selecionada");
    act_watch_only_ = connect_menu->addAction(
        s->standardIcon(QStyle::SP_FileDialogContentsView), "Apenas assistir", this, &MainWindow::OnWatchOnly);
    set_action_hints(act_watch_only_,
                     "Assistir maquina selecionada",
                     "Inicia uma sessao somente leitura para acompanhar a maquina selecionada");
    connect_button_ = new QToolButton(toolbar_);
    connect_button_->setText("Conectar");
    connect_button_->setIcon(s->standardIcon(QStyle::SP_MediaPlay));
    connect_button_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    connect_button_->setPopupMode(QToolButton::MenuButtonPopup);
    connect_button_->setMenu(connect_menu);
    connect_button_->setToolTip("Conectar a maquina selecionada");
    connect_button_->setStatusTip(
        "Clique para controlar a maquina selecionada ou abra o menu para escolher entre controlar e apenas assistir");
    connect(connect_button_, &QToolButton::clicked, this, &MainWindow::OnConnect);
    toolbar_->addWidget(connect_button_);
    act_chat_ = toolbar_->addAction(
        s->standardIcon(QStyle::SP_MessageBoxInformation), "Chat", this, &MainWindow::OnChat);
    set_action_hints(act_chat_, "Abrir chat", "Abre um chat com a maquina selecionada");
    act_edit_ = toolbar_->addAction(
        s->standardIcon(QStyle::SP_FileDialogDetailedView), "Editar", this, &MainWindow::OnEditMachine);
    set_action_hints(act_edit_, "Editar maquina", "Edita os dados da maquina selecionada");
    act_delete_ = toolbar_->addAction(
        s->standardIcon(QStyle::SP_DialogDiscardButton), "Excluir", this, &MainWindow::OnDeleteMachine);
    set_action_hints(act_delete_, "Excluir maquina", "Exclui a maquina selecionada");
    toolbar_->addSeparator();
    act_groups_ = toolbar_->addAction(
        s->standardIcon(QStyle::SP_DirIcon), "Grupos", this, &MainWindow::OnManageGroups);
    set_action_hints(act_groups_, "Gerenciar grupos", "Abre o gerenciamento de grupos");
    {
        auto role = auth_->GetRole();
        if (role == "company_manager" || role == "super_admin") {
            act_users_ = toolbar_->addAction(
                s->standardIcon(QStyle::SP_DialogApplyButton), "Usuarios", this, &MainWindow::OnManageUsers);
            set_action_hints(act_users_, "Gerenciar usuarios", "Abre o gerenciamento de usuarios");
            act_reports_ = toolbar_->addAction(
                s->standardIcon(QStyle::SP_FileDialogInfoView), "Relatorios", this, &MainWindow::OnReports);
            set_action_hints(act_reports_, "Relatorios", "Abre a tela de relatorios");
        }
    }
    act_settings_ = toolbar_->addAction(
        s->standardIcon(QStyle::SP_FileDialogListView), "Configuracoes", this, &MainWindow::OnSettings);
    set_action_hints(act_settings_, "Configuracoes", "Abre as configuracoes do Viewer");
    act_refresh_ = toolbar_->addAction(
        s->standardIcon(QStyle::SP_BrowserReload), "Atualizar", this, &MainWindow::RefreshMachines);
    set_action_hints(act_refresh_, "Atualizar lista", "Atualiza a lista de maquinas");

    auto* spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar_->addWidget(spacer);
    act_logout_ = toolbar_->addAction(
        s->standardIcon(QStyle::SP_DialogCloseButton), "Sair", this, &MainWindow::OnLogout);
    set_action_hints(act_logout_, "Sair", "Encerra a sessao atual no Viewer");
    UpdateMachineActionState();
}
