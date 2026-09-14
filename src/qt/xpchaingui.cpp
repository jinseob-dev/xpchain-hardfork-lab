#include <qt/xpchaingui.h>

#include <qt/xpchainunits.h>
#include <qt/clientmodel.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/createwalletdialog.h>
#include <qt/migratewalletdialog.h>
#include <qt/intro.h>
#include <qt/modaloverlay.h>
#include <qt/networkstyle.h>
#include <QFile>
#include <qt/notificator.h>
#include <qt/openuridialog.h>
#include <qt/optionsdialog.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/rpcconsole.h>
#include <qt/utilitydialog.h>

#ifdef ENABLE_WALLET
#include <qt/walletframe.h>
#include <qt/walletmodel.h>
#include <qt/walletview.h>
#include <wallet/db.h>
#include <wallet/sqlite.h>
#include <wallet/walletutil.h>
#include <qt/mnemonicimportdialog.h>
#include <qt/mnemonicbackupdialog.h>
#include <qt/walletsetupdialog.h>
#include <qt/importaddressdialog.h>
#include <qt/askpassphrasedialog.h>
#endif // ENABLE_WALLET

#ifdef Q_OS_MAC
#include <qt/macdockiconhandler.h>
#endif

#include <chainparams.h>
#include <interfaces/handler.h>
#include <interfaces/node.h>
#include <support/allocators/secure.h>
#include <support/cleanse.h>
#include <ui_interface.h>
#include <util.h>

#include <iostream>

#include <univalue.h>
#include <boost/bind.hpp>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QDragEnterEvent>
#include <QLabel>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QUrl>
#include <QMimeData>
#include <QProgressDialog>
#include <QRunnable>
#include <QSettings>
#include <QShortcut>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QThreadPool>
#include <QTimer>
#include <QToolBar>
#include <QUrlQuery>
#include <QVBoxLayout>

const std::string XPChainGUI::DEFAULT_UIPLATFORM =
#if defined(Q_OS_MAC)
        "macosx"
#elif defined(Q_OS_WIN)
        "windows"
#else
        "other"
#endif
        ;

XPChainGUI::XPChainGUI(interfaces::Node& node, const PlatformStyle *_platformStyle, const NetworkStyle *networkStyle, QWidget *parent) :
    QMainWindow(parent),
    m_node(node),
    platformStyle(_platformStyle),
    m_networkStyle(networkStyle)
{
    // Apply saved theme (dark / light / system) before building widgets
    GUIUtil::applyTheme();
    qApp->installEventFilter(this);
    QSettings settings;
    if (!restoreGeometry(settings.value("MainWindowGeometry").toByteArray())) {
        // Restore failed (perhaps missing setting), center the window
        move(QApplication::desktop()->availableGeometry().center() - frameGeometry().center());
    }

#ifdef ENABLE_WALLET
    enableWallet = WalletModel::isWalletEnabled();
#endif // ENABLE_WALLET
    updateWindowTitle();

    rpcConsole = new RPCConsole(node, _platformStyle, 0);
    helpMessageDialog = new HelpMessageDialog(node, this, false);
#ifdef ENABLE_WALLET
    if(enableWallet)
    {
        /** Create wallet frame and make it the central widget */
        walletFrame = new WalletFrame(_platformStyle, this);
        connect(walletFrame, &WalletFrame::setupWalletClicked, this, &XPChainGUI::walletSetup);
        setCentralWidget(walletFrame);
    } else
#endif // ENABLE_WALLET
    {
        /* When compiled without wallet or -disablewallet is provided,
         * the central widget is the rpc console.
         */
        setCentralWidget(rpcConsole);
    }

    // Accept D&D of URIs
    setAcceptDrops(true);

    // Create actions for the toolbar, menu bar and tray/dock icon
    // Needs walletFrame to be initialized
    createActions();

    // Create application menu bar
    createMenuBar();

    // Create the toolbars
    createToolBars();

    // Create system tray icon and notification
    createTrayIcon(networkStyle);

    // Create status bar
    statusBar();

    // Disable size grip because it looks ugly and nobody needs it
    statusBar()->setSizeGripEnabled(false);

    // Status bar notification icons
    QFrame *frameBlocks = new QFrame();
    frameBlocks->setContentsMargins(0,0,0,0);
    frameBlocks->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    QHBoxLayout *frameBlocksLayout = new QHBoxLayout(frameBlocks);
    frameBlocksLayout->setContentsMargins(3,0,3,0);
    frameBlocksLayout->setSpacing(3);
    unitDisplayControl = new UnitDisplayStatusBarControl(platformStyle);
    labelWalletEncryptionIcon = new GUIUtil::ClickableLabel();
    labelWalletHDStatusIcon = new QLabel();
    GUIUtil::ClickableLabel* clickableFormatStatus = new GUIUtil::ClickableLabel();
    labelWalletFormatStatus = clickableFormatStatus;
    labelWalletFormatStatus->setStyleSheet(
        "QLabel { font-size: 10px; font-weight: 600; color: #6cb6ff; "
        "background-color: transparent; border: 1px solid #1f6feb; "
        "border-radius: 3px; padding: 1px 6px; }");
    labelWalletFormatStatus->hide();
    labelStakingIcon = new GUIUtil::ClickableLabel();
    labelStakingIcon->hide();
    connect(clickableFormatStatus, &GUIUtil::ClickableLabel::clicked, [this]() {
        if (walletFrame && walletFrame->currentWalletView()) {
            WalletModel* model = walletFrame->currentWalletView()->getWalletModel();
            if (model && model->wallet().databaseFormat() != "sqlite") {
                migrateWallet();
            }
        }
    });
    connect(qobject_cast<GUIUtil::ClickableLabel*>(labelWalletEncryptionIcon), &GUIUtil::ClickableLabel::clicked, [this]() {
#ifdef ENABLE_WALLET
        if (!walletFrame) return;
        WalletView* view = walletFrame->currentWalletView();
        if (!view || !view->getWalletModel()) return;
        const int enc = view->getWalletModel()->getEncryptionStatus();
        if (enc == WalletModel::Locked) {
            unlockWallet();
        } else if (enc == WalletModel::Unlocked) {
            lockWallet();
        }
#endif
    });
    connect(qobject_cast<GUIUtil::ClickableLabel*>(labelStakingIcon), &GUIUtil::ClickableLabel::clicked, [this]() {
#ifdef ENABLE_WALLET
        if (!walletFrame) return;
        WalletView* view = walletFrame->currentWalletView();
        if (!view || !view->getWalletModel()) return;
        WalletModel* model = view->getWalletModel();
        if (model->getEncryptionStatus() == WalletModel::Locked) {
            // Staking-only unlock is the common click path from the staking icon.
            if (decryptForMintingAction) decryptForMintingAction->trigger();
        } else {
            gotoMintingPage();
        }
#endif
    });
    labelProxyIcon = new QLabel();
    connectionsControl = new GUIUtil::ClickableLabel();
    labelBlocksIcon = new GUIUtil::ClickableLabel();
    if(enableWallet)
    {
        frameBlocksLayout->addStretch();
        frameBlocksLayout->addWidget(unitDisplayControl);
        frameBlocksLayout->addStretch();
        frameBlocksLayout->addWidget(labelWalletEncryptionIcon);
        frameBlocksLayout->addWidget(labelWalletHDStatusIcon);
        frameBlocksLayout->addWidget(labelWalletFormatStatus);
        frameBlocksLayout->addWidget(labelStakingIcon);
    }
    frameBlocksLayout->addWidget(labelProxyIcon);
    frameBlocksLayout->addStretch();
    frameBlocksLayout->addWidget(connectionsControl);
    frameBlocksLayout->addStretch();
    frameBlocksLayout->addWidget(labelBlocksIcon);
    frameBlocksLayout->addStretch();

    // Progress bar and label for blocks download
    progressBarLabel = new QLabel();
    progressBarLabel->setVisible(false);
    progressBar = new GUIUtil::ProgressBar();
    progressBar->setAlignment(Qt::AlignCenter);
    progressBar->setVisible(false);

    // Override style sheet for progress bar for styles that have a segmented progress bar,
    // as they make the text unreadable (workaround for issue #1071)
    // See https://doc.qt.io/qt-5/gallery.html
    QString curStyle = QApplication::style()->metaObject()->className();
    if(curStyle == "QWindowsStyle" || curStyle == "QWindowsXPStyle")
    {
        progressBar->setStyleSheet("QProgressBar { background-color: #e8e8e8; border: 1px solid grey; border-radius: 7px; padding: 1px; text-align: center; } QProgressBar::chunk { background: QLinearGradient(x1: 0, y1: 0, x2: 1, y2: 0, stop: 0 #106ba3, stop: 1 #0d4f7a); border-radius: 7px; margin: 0px; }");
    }

    statusBar()->addWidget(progressBarLabel);
    statusBar()->addWidget(progressBar);
    statusBar()->addPermanentWidget(frameBlocks);

    // Install event filter to be able to catch status tip events (QEvent::StatusTip)
    this->installEventFilter(this);

    // Initially wallet actions should be disabled
    setWalletActionsEnabled(false);

    // Subscribe to notifications from core
    subscribeToCoreSignals();

    connect(connectionsControl, SIGNAL(clicked(QPoint)), this, SLOT(toggleNetworkActive()));

    modalOverlay = new ModalOverlay(this->centralWidget());
#ifdef ENABLE_WALLET
    if(enableWallet) {
        connect(walletFrame, SIGNAL(requestedSyncWarningInfo()), this, SLOT(showModalOverlay()));
        connect(labelBlocksIcon, SIGNAL(clicked(QPoint)), this, SLOT(showModalOverlay()));
        connect(progressBar, SIGNAL(clicked(QPoint)), this, SLOT(showModalOverlay()));
    }
#endif
}

XPChainGUI::~XPChainGUI()
{
    // Unsubscribe from notifications from core
    unsubscribeFromCoreSignals();

    QSettings settings;
    settings.setValue("MainWindowGeometry", saveGeometry());
    if(trayIcon) // Hide tray icon, as deleting will let it linger until quit (on Ubuntu)
        trayIcon->hide();
#ifdef Q_OS_MAC
    delete appMenuBar;
    MacDockIconHandler::cleanup();
#endif

    delete rpcConsole;
}

void XPChainGUI::createActions()
{
    QActionGroup *tabGroup = new QActionGroup(this);

    createWalletAction = new QAction(platformStyle->TextColorIcon(":/icons/add"), tr("&Create Wallet..."), this);
    createWalletAction->setStatusTip(tr("Create a new wallet (optionally encrypted on create)"));

    openWalletAction = new QAction(platformStyle->TextColorIcon(":/icons/open"), tr("&Open Wallet..."), this);
    openWalletAction->setStatusTip(tr("Open an existing wallet"));

    closeWalletAction = new QAction(tr("Close &Wallet..."), this);
    closeWalletAction->setStatusTip(tr("Unload the currently selected wallet"));

    walletSetupAction = new QAction(platformStyle->TextColorIcon(":/icons/key"), tr("Set &Up Wallet..."), this);
    walletSetupAction->setStatusTip(tr("Create, generate a mnemonic, restore, or open a wallet"));

    generateMnemonicAction = new QAction(platformStyle->TextColorIcon(":/icons/key"), tr("&Generate && Backup Mnemonic..."), this);
    generateMnemonicAction->setStatusTip(tr("Generate a new BIP39 mnemonic, confirm backup, and create a wallet seeded from it"));

    importMnemonicAction = new QAction(platformStyle->TextColorIcon(":/icons/key"), tr("&Restore Wallet from Mnemonic..."), this);
    importMnemonicAction->setStatusTip(tr("Restore keys from a BIP39 mnemonic into the current wallet (prefer a new empty wallet)"));

    importAddressAction = new QAction(tr("Import &Address..."), this);
    importAddressAction->setStatusTip(tr("Import a watch-only address into the current wallet"));

    rescanWalletAction = new QAction(tr("&Rescan Wallet..."), this);
    rescanWalletAction->setStatusTip(tr("Rescan the blockchain for transactions belonging to this wallet"));

    migrateWalletAction = new QAction(platformStyle->TextColorIcon(":/icons/filesave"), tr("&Migrate Wallet to SQLite..."), this);
    migrateWalletAction->setStatusTip(tr("Copy the current Berkeley DB wallet into a new SQLite wallet file"));

    backupAllWalletsAction = new QAction(platformStyle->TextColorIcon(":/icons/filesave"), tr("&Backup All Wallets..."), this);
    backupAllWalletsAction->setStatusTip(tr("Backup all loaded wallets to a specific directory"));

    overviewAction = new QAction(platformStyle->SingleColorIcon(":/icons/overview"), tr("&Overview"), this);
    overviewAction->setStatusTip(tr("Show general overview of wallet"));
    overviewAction->setToolTip(overviewAction->statusTip());
    overviewAction->setCheckable(true);
    overviewAction->setShortcut(QKeySequence(Qt::ALT + Qt::Key_1));
    tabGroup->addAction(overviewAction);

    sendCoinsAction = new QAction(platformStyle->SingleColorIcon(":/icons/send"), tr("&Send"), this);
    sendCoinsAction->setStatusTip(tr("Send coins to an XPChain address"));
    sendCoinsAction->setToolTip(sendCoinsAction->statusTip());
    sendCoinsAction->setCheckable(true);
    sendCoinsAction->setShortcut(QKeySequence(Qt::ALT + Qt::Key_2));
    tabGroup->addAction(sendCoinsAction);

    sendCoinsMenuAction = new QAction(platformStyle->TextColorIcon(":/icons/send"), sendCoinsAction->text(), this);
    sendCoinsMenuAction->setStatusTip(sendCoinsAction->statusTip());
    sendCoinsMenuAction->setToolTip(sendCoinsMenuAction->statusTip());

    receiveCoinsAction = new QAction(platformStyle->SingleColorIcon(":/icons/receiving_addresses"), tr("&Receive"), this);
    receiveCoinsAction->setStatusTip(tr("Request payments (generates QR codes and xpchain: URIs)"));
    receiveCoinsAction->setToolTip(receiveCoinsAction->statusTip());
    receiveCoinsAction->setCheckable(true);
    receiveCoinsAction->setShortcut(QKeySequence(Qt::ALT + Qt::Key_3));
    tabGroup->addAction(receiveCoinsAction);

    receiveCoinsMenuAction = new QAction(platformStyle->TextColorIcon(":/icons/receiving_addresses"), receiveCoinsAction->text(), this);
    receiveCoinsMenuAction->setStatusTip(receiveCoinsAction->statusTip());
    receiveCoinsMenuAction->setToolTip(receiveCoinsMenuAction->statusTip());

    historyAction = new QAction(platformStyle->SingleColorIcon(":/icons/history"), tr("&Transactions"), this);
    historyAction->setStatusTip(tr("Browse transaction history"));
    historyAction->setToolTip(historyAction->statusTip());
    historyAction->setCheckable(true);
    historyAction->setShortcut(QKeySequence(Qt::ALT + Qt::Key_4));
    tabGroup->addAction(historyAction);

    mintingAction = new QAction(platformStyle->SingleColorIcon(":/icons/tx_mined"), tr("&Staking"), this);
    mintingAction->setStatusTip(tr("Show stakeable coins and estimated staking odds"));
    mintingAction->setToolTip(mintingAction->statusTip());
    mintingAction->setCheckable(true);
    mintingAction->setShortcut(QKeySequence(Qt::ALT + Qt::Key_5));
    tabGroup->addAction(mintingAction);
#ifdef ENABLE_WALLET
    // These showNormalIfMinimized are needed because Send Coins and Receive Coins
    // can be triggered from the tray menu, and need to show the GUI to be useful.
    connect(overviewAction, SIGNAL(triggered()), this, SLOT(showNormalIfMinimized()));
    connect(overviewAction, SIGNAL(triggered()), this, SLOT(gotoOverviewPage()));
    connect(sendCoinsAction, SIGNAL(triggered()), this, SLOT(showNormalIfMinimized()));
    connect(sendCoinsAction, SIGNAL(triggered()), this, SLOT(gotoSendCoinsPage()));
    connect(sendCoinsMenuAction, SIGNAL(triggered()), this, SLOT(showNormalIfMinimized()));
    connect(sendCoinsMenuAction, SIGNAL(triggered()), this, SLOT(gotoSendCoinsPage()));
    connect(receiveCoinsAction, SIGNAL(triggered()), this, SLOT(showNormalIfMinimized()));
    connect(receiveCoinsAction, SIGNAL(triggered()), this, SLOT(gotoReceiveCoinsPage()));
    connect(receiveCoinsMenuAction, SIGNAL(triggered()), this, SLOT(showNormalIfMinimized()));
    connect(receiveCoinsMenuAction, SIGNAL(triggered()), this, SLOT(gotoReceiveCoinsPage()));
    connect(historyAction, SIGNAL(triggered()), this, SLOT(showNormalIfMinimized()));
    connect(historyAction, SIGNAL(triggered()), this, SLOT(gotoHistoryPage()));
    connect(mintingAction, SIGNAL(triggered()), this, SLOT(showNormalIfMinimized()));
    connect(mintingAction, SIGNAL(triggered()), this, SLOT(gotoMintingPage()));
#endif // ENABLE_WALLET

    quitAction = new QAction(platformStyle->TextColorIcon(":/icons/quit"), tr("E&xit"), this);
    quitAction->setStatusTip(tr("Quit application"));
    quitAction->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_Q));
    quitAction->setMenuRole(QAction::QuitRole);
    aboutAction = new QAction(platformStyle->TextColorIcon(":/icons/about"), tr("&About %1").arg(tr(PACKAGE_NAME)), this);
    aboutAction->setStatusTip(tr("Show information about %1").arg(tr(PACKAGE_NAME)));
    aboutAction->setMenuRole(QAction::AboutRole);
    aboutAction->setEnabled(false);
    aboutQtAction = new QAction(platformStyle->TextColorIcon(":/icons/about_qt"), tr("About &Qt"), this);
    aboutQtAction->setStatusTip(tr("Show information about Qt"));
    aboutQtAction->setMenuRole(QAction::AboutQtRole);
    optionsAction = new QAction(platformStyle->TextColorIcon(":/icons/options"), tr("&Options..."), this);
    optionsAction->setStatusTip(tr("Modify configuration options for %1").arg(tr(PACKAGE_NAME)));
    optionsAction->setMenuRole(QAction::PreferencesRole);
    optionsAction->setEnabled(false);
    toggleHideAction = new QAction(platformStyle->TextColorIcon(":/icons/about"), tr("&Show / Hide"), this);
    toggleHideAction->setStatusTip(tr("Show or hide the main Window"));

    encryptWalletAction = new QAction(platformStyle->TextColorIcon(":/icons/lock_closed"), tr("&Encrypt Wallet..."), this);
    encryptWalletAction->setStatusTip(tr("Encrypt spending keys and (on SQLite) the wallet file at rest with a passphrase"));
    encryptWalletAction->setCheckable(true);
    decryptForMintingAction = new QAction(platformStyle->TextColorIcon(":/icons/lock_open"), tr("&Unlock Wallet for Staking Only"), this);
    decryptForMintingAction->setStatusTip(tr("Unlock spending keys for staking only. Sending still requires a full unlock."));
    decryptForMintingAction->setCheckable(true);
    backupWalletAction = new QAction(platformStyle->TextColorIcon(":/icons/filesave"), tr("&Backup Wallet..."), this);
    backupWalletAction->setStatusTip(tr("Backup wallet to another location"));
    changePassphraseAction = new QAction(platformStyle->TextColorIcon(":/icons/key"), tr("&Change Passphrase..."), this);
    lockWalletAction = new QAction(platformStyle->TextColorIcon(":/icons/lock_closed"), tr("&Lock Wallet"), this);
    lockWalletAction->setStatusTip(tr("Lock spending keys. Staking and sending will require unlock."));
    lockWalletAction->setEnabled(false);
    unlockWalletAction = new QAction(platformStyle->TextColorIcon(":/icons/lock_open"), tr("&Unlock Wallet..."), this);
    unlockWalletAction->setStatusTip(tr("Unlock spending keys for sending and signing. Prefer Unlock for Staking Only if you only need to stake."));
    unlockWalletAction->setEnabled(false);
    changePassphraseAction->setStatusTip(tr("Change the passphrase for spending keys and encrypted wallet files"));
    signMessageAction = new QAction(platformStyle->TextColorIcon(":/icons/edit"), tr("Sign &Message..."), this);
    signMessageAction->setStatusTip(tr("Sign messages with your XPChain addresses to prove you own them"));
    verifyMessageAction = new QAction(platformStyle->TextColorIcon(":/icons/verify"), tr("&Verify Message..."), this);
    verifyMessageAction->setStatusTip(tr("Verify messages to ensure they were signed with specified XPChain addresses"));

    openRPCConsoleAction = new QAction(platformStyle->TextColorIcon(":/icons/debugwindow"), tr("&Debug window"), this);
    openRPCConsoleAction->setStatusTip(tr("Open debugging and diagnostic console"));
    // initially disable the debug window menu item
    openRPCConsoleAction->setEnabled(false);

    usedSendingAddressesAction = new QAction(platformStyle->TextColorIcon(":/icons/address-book"), tr("&Sending addresses..."), this);
    usedSendingAddressesAction->setStatusTip(tr("Show the list of used sending addresses and labels"));
    usedReceivingAddressesAction = new QAction(platformStyle->TextColorIcon(":/icons/address-book"), tr("&Receiving addresses..."), this);
    usedReceivingAddressesAction->setStatusTip(tr("Show the list of used receiving addresses and labels"));

    openAction = new QAction(platformStyle->TextColorIcon(":/icons/open"), tr("Open &URI..."), this);
    openAction->setStatusTip(tr("Open a xpchain: URI or payment request"));

    openStakingRewardSettingsAction = new QAction(platformStyle->TextColorIcon(":/icons/options"), tr("&Staking Reward Settings..."), this);
    openStakingRewardSettingsAction->setStatusTip(tr("Modify settings for staking reward"));

    showHelpMessageAction = new QAction(platformStyle->TextColorIcon(":/icons/info"), tr("&Command-line options"), this);
    showHelpMessageAction->setMenuRole(QAction::NoRole);
    showHelpMessageAction->setStatusTip(tr("Show the %1 help message to get a list with possible XPChain command-line options").arg(tr(PACKAGE_NAME)));

    connect(quitAction, SIGNAL(triggered()), qApp, SLOT(quit()));
    connect(aboutAction, SIGNAL(triggered()), this, SLOT(aboutClicked()));
    connect(aboutQtAction, SIGNAL(triggered()), qApp, SLOT(aboutQt()));
    connect(optionsAction, SIGNAL(triggered()), this, SLOT(optionsClicked()));
    connect(toggleHideAction, SIGNAL(triggered()), this, SLOT(toggleHidden()));
    connect(showHelpMessageAction, SIGNAL(triggered()), this, SLOT(showHelpMessageClicked()));
    connect(openRPCConsoleAction, SIGNAL(triggered()), this, SLOT(showDebugWindow()));
    connect(createWalletAction, SIGNAL(triggered()), this, SLOT(createWallet()));
    connect(openWalletAction, SIGNAL(triggered()), this, SLOT(openWallet()));
    connect(closeWalletAction, SIGNAL(triggered()), this, SLOT(closeWallet()));
    connect(walletSetupAction, SIGNAL(triggered()), this, SLOT(walletSetup()));
    connect(generateMnemonicAction, SIGNAL(triggered()), this, SLOT(generateMnemonicWallet()));
    connect(importMnemonicAction, SIGNAL(triggered()), this, SLOT(importMnemonic()));
    connect(importAddressAction, SIGNAL(triggered()), this, SLOT(importAddress()));
    connect(rescanWalletAction, SIGNAL(triggered()), this, SLOT(rescanWallet()));
    connect(migrateWalletAction, SIGNAL(triggered()), this, SLOT(migrateWallet()));
    connect(backupAllWalletsAction, SIGNAL(triggered()), this, SLOT(backupAllWallets()));
    // prevents an open debug window from becoming stuck/unusable on client shutdown
    connect(quitAction, SIGNAL(triggered()), rpcConsole, SLOT(hide()));

#ifdef ENABLE_WALLET
    if(walletFrame)
    {
        connect(encryptWalletAction, SIGNAL(triggered(bool)), walletFrame, SLOT(encryptWallet(bool)));
        connect(decryptForMintingAction, SIGNAL(triggered(bool)), walletFrame, SLOT(decryptForMinting(bool)));
        connect(backupWalletAction, SIGNAL(triggered()), walletFrame, SLOT(backupWallet()));
        connect(changePassphraseAction, SIGNAL(triggered()), walletFrame, SLOT(changePassphrase()));
        connect(lockWalletAction, SIGNAL(triggered()), this, SLOT(lockWallet()));
        connect(unlockWalletAction, SIGNAL(triggered()), this, SLOT(unlockWallet()));
        connect(signMessageAction, SIGNAL(triggered()), this, SLOT(gotoSignMessageTab()));
        connect(verifyMessageAction, SIGNAL(triggered()), this, SLOT(gotoVerifyMessageTab()));
        connect(usedSendingAddressesAction, SIGNAL(triggered()), walletFrame, SLOT(usedSendingAddresses()));
        connect(usedReceivingAddressesAction, SIGNAL(triggered()), walletFrame, SLOT(usedReceivingAddresses()));
        connect(openAction, SIGNAL(triggered()), this, SLOT(openClicked()));
        connect(openStakingRewardSettingsAction, SIGNAL(triggered()), walletFrame, SLOT(openStakingRewardSettings()));
    }
#endif // ENABLE_WALLET

    new QShortcut(QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_C), this, SLOT(showDebugWindowActivateConsole()));
    new QShortcut(QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_D), this, SLOT(showDebugWindow()));
}

void XPChainGUI::createMenuBar()
{
#ifdef Q_OS_MAC
    // Create a decoupled menu bar on Mac which stays even if the window is closed
    appMenuBar = new QMenuBar();
#else
    // Get the main window's menu bar on other platforms
    appMenuBar = menuBar();
#endif

    // Configure the menus
    QMenu *file = appMenuBar->addMenu(tr("&File"));
    if(walletFrame)
    {
        // Primary entry: Set Up Wallet covers create / mnemonic / restore / open.
        // Direct actions live under Advanced to avoid duplicating those choices.
        file->addAction(walletSetupAction);
        file->addSeparator();
        file->addAction(closeWalletAction);
        file->addSeparator();
        file->addAction(backupWalletAction);
        file->addAction(backupAllWalletsAction);
        file->addSeparator();
        file->addAction(migrateWalletAction);
        file->addAction(openAction);
        file->addSeparator();
        QMenu *fileAdvanced = file->addMenu(tr("&Advanced"));
        fileAdvanced->addAction(createWalletAction);
        fileAdvanced->addAction(openWalletAction);
        fileAdvanced->addAction(generateMnemonicAction);
        fileAdvanced->addAction(importMnemonicAction);
        file->addSeparator();
        file->addAction(signMessageAction);
        file->addAction(verifyMessageAction);
        file->addSeparator();
        file->addAction(usedSendingAddressesAction);
        file->addAction(usedReceivingAddressesAction);
        file->addSeparator();
    }
    file->addAction(quitAction);

    QMenu *settings = appMenuBar->addMenu(tr("&Settings"));
    if(walletFrame)
    {
        settings->addAction(encryptWalletAction);
        settings->addAction(decryptForMintingAction);
        settings->addAction(unlockWalletAction);
        settings->addAction(lockWalletAction);
        settings->addAction(changePassphraseAction);
        settings->addAction(openStakingRewardSettingsAction);
        settings->addSeparator();
    }
    settings->addAction(optionsAction);

    if (walletFrame) {
        QMenu *tools = appMenuBar->addMenu(tr("&Tools"));
        tools->addAction(importAddressAction);
        tools->addAction(rescanWalletAction);
        tools->addSeparator();
        tools->addAction(openRPCConsoleAction);
    }

    QMenu *help = appMenuBar->addMenu(tr("&Help"));
    // Debug window lives under Tools when the wallet UI is available.
    if(!walletFrame)
    {
        help->addAction(openRPCConsoleAction);
    }
    help->addAction(showHelpMessageAction);
    help->addSeparator();
    help->addAction(aboutAction);
    help->addAction(aboutQtAction);
}

void XPChainGUI::createToolBars()
{
    if(walletFrame)
    {
        QToolBar *toolbar = addToolBar(tr("Tabs toolbar"));
        appToolBar = toolbar;
        toolbar->setContextMenuPolicy(Qt::PreventContextMenu);
        toolbar->setMovable(false);
        toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        toolbar->addAction(overviewAction);
        toolbar->addAction(sendCoinsAction);
        toolbar->addAction(receiveCoinsAction);
        toolbar->addAction(historyAction);
        toolbar->addAction(mintingAction);
        overviewAction->setChecked(true);

#ifdef ENABLE_WALLET
        QWidget *spacer = new QWidget();
        spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        spacer->setAttribute(Qt::WA_StyledBackground, false);
        spacer->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
        toolbar->addWidget(spacer);

        /* Soft divider before the wallet selector (styled via QToolBar::separator in theme QSS) */
        m_wallet_selector_separator_action = toolbar->addSeparator();

        m_wallet_selector = new QComboBox();
        connect(m_wallet_selector, SIGNAL(currentIndexChanged(int)), this, SLOT(setCurrentWalletBySelectorIndex(int)));

        m_wallet_selector_label = new QLabel();
        m_wallet_selector_label->setText(tr("Wallet:") + " ");
        m_wallet_selector_label->setBuddy(m_wallet_selector);

        m_wallet_selector_label_action = appToolBar->addWidget(m_wallet_selector_label);
        m_wallet_selector_action = appToolBar->addWidget(m_wallet_selector);

        m_wallet_selector_separator_action->setVisible(false);
        m_wallet_selector_label_action->setVisible(false);
        m_wallet_selector_action->setVisible(false);
#endif
    }
}

void XPChainGUI::setClientModel(ClientModel *_clientModel)
{
    this->clientModel = _clientModel;
    if(_clientModel)
    {
        // Create system tray menu (or setup the dock menu) that late to prevent users from calling actions,
        // while the client has not yet fully loaded
        createTrayIconMenu();

        // Keep up to date with client
        updateNetworkState();
        connect(_clientModel, SIGNAL(numConnectionsChanged(int)), this, SLOT(setNumConnections(int)));
        connect(_clientModel, SIGNAL(networkActiveChanged(bool)), this, SLOT(setNetworkActive(bool)));

        modalOverlay->setKnownBestHeight(_clientModel->getHeaderTipHeight(), QDateTime::fromTime_t(_clientModel->getHeaderTipTime()));
        setNumBlocks(m_node.getNumBlocks(), QDateTime::fromTime_t(m_node.getLastBlockTime()), m_node.getVerificationProgress(), false);
        connect(_clientModel, SIGNAL(numBlocksChanged(int,QDateTime,double,bool)), this, SLOT(setNumBlocks(int,QDateTime,double,bool)));

        // Receive and report messages from client model
        connect(_clientModel, SIGNAL(message(QString,QString,unsigned int)), this, SLOT(message(QString,QString,unsigned int)));

        // Show progress dialog
        connect(_clientModel, SIGNAL(showProgress(QString,int)), this, SLOT(showProgress(QString,int)));

        rpcConsole->setClientModel(_clientModel);

        updateProxyIcon();

#ifdef ENABLE_WALLET
        if(walletFrame)
        {
            walletFrame->setClientModel(_clientModel);
        }
#endif // ENABLE_WALLET
        unitDisplayControl->setOptionsModel(_clientModel->getOptionsModel());

        OptionsModel* optionsModel = _clientModel->getOptionsModel();
        if(optionsModel)
        {
            // be aware of the tray icon disable state change reported by the OptionsModel object.
            connect(optionsModel,SIGNAL(hideTrayIconChanged(bool)),this,SLOT(setTrayIconVisible(bool)));

            // initialize the disable state of the tray icon with the current value in the model.
            setTrayIconVisible(optionsModel->getHideTrayIcon());

            connect(optionsModel, &OptionsModel::themeChanged, this, &XPChainGUI::updateTheme);
            updateTheme(optionsModel->getTheme());
        }
    } else {
        // Disable possibility to show main window via action
        toggleHideAction->setEnabled(false);
        if(trayIconMenu)
        {
            // Disable context menu on tray icon
            trayIconMenu->clear();
        }
        // Propagate cleared model to child objects
        rpcConsole->setClientModel(nullptr);
#ifdef ENABLE_WALLET
        if (walletFrame)
        {
            walletFrame->setClientModel(nullptr);
        }
#endif // ENABLE_WALLET
        unitDisplayControl->setOptionsModel(nullptr);
    }
}

#ifdef ENABLE_WALLET
bool XPChainGUI::addWallet(WalletModel *walletModel)
{
    if(!walletFrame)
        return false;
    const QString name = walletModel->getWalletName();
    QString display_name = name.isEmpty() ? "["+tr("default wallet")+"]" : name;
    display_name.remove("<br>"); // Potential fix for the observed UI glitch
    setWalletActionsEnabled(true);
    m_wallet_selector->addItem(display_name, name);
    if (m_wallet_selector->count() >= 1) {
        m_wallet_selector_separator_action->setVisible(true);
        m_wallet_selector_label_action->setVisible(true);
        m_wallet_selector_action->setVisible(true);
    }
    updateWindowTitle();
    rpcConsole->addWallet(walletModel);
    return walletFrame->addWallet(walletModel);
}

bool XPChainGUI::removeWallet(WalletModel* walletModel)
{
    if (!walletFrame) return false;
    QString name = walletModel->getWalletName();
    int index = m_wallet_selector->findData(name);
    m_wallet_selector->removeItem(index);
    if (m_wallet_selector->count() == 0) {
        setWalletActionsEnabled(false);
        m_wallet_selector_separator_action->setVisible(false);
        m_wallet_selector_label_action->setVisible(false);
        m_wallet_selector_action->setVisible(false);
    } else if (m_wallet_selector->count() >= 1) {
        m_wallet_selector_separator_action->setVisible(true);
        m_wallet_selector_label_action->setVisible(true);
        m_wallet_selector_action->setVisible(true);
    }
    updateWindowTitle();
    rpcConsole->removeWallet(walletModel);
    return walletFrame->removeWallet(name);
}

bool XPChainGUI::setCurrentWallet(const QString& name)
{
    if(!walletFrame)
        return false;
    m_current_wallet_name = name;
    if (m_current_wallet_name.isEmpty()) m_current_wallet_name = tr("default wallet");
    bool ret = walletFrame->setCurrentWallet(name);
    updateWindowTitle();
    return ret;
}

bool XPChainGUI::setCurrentWalletBySelectorIndex(int index)
{
    QString internal_name = m_wallet_selector->itemData(index).toString();
    return setCurrentWallet(internal_name);
}

void XPChainGUI::removeAllWallets()
{
    if(!walletFrame)
        return;
    setWalletActionsEnabled(false);
    walletFrame->removeAllWallets();
}
#endif // ENABLE_WALLET

void XPChainGUI::setWalletActionsEnabled(bool enabled)
{
    overviewAction->setEnabled(enabled);
    sendCoinsAction->setEnabled(enabled);
    sendCoinsMenuAction->setEnabled(enabled);
    receiveCoinsAction->setEnabled(enabled);
    receiveCoinsMenuAction->setEnabled(enabled);
    historyAction->setEnabled(enabled);
    encryptWalletAction->setEnabled(enabled);
    backupWalletAction->setEnabled(enabled);
    changePassphraseAction->setEnabled(enabled);
    signMessageAction->setEnabled(enabled);
    verifyMessageAction->setEnabled(enabled);
    usedSendingAddressesAction->setEnabled(enabled);
    usedReceivingAddressesAction->setEnabled(enabled);
    openAction->setEnabled(enabled);
    mintingAction->setEnabled(enabled);
    openStakingRewardSettingsAction->setEnabled(enabled);
    importMnemonicAction->setEnabled(enabled);
    // Enabled for Berkeley DB wallets only; setWalletFormatStatus() updates this.
    migrateWalletAction->setEnabled(false);
    if (backupAllWalletsAction) backupAllWalletsAction->setEnabled(enabled);
    if (closeWalletAction) closeWalletAction->setEnabled(enabled);
    if (importAddressAction) importAddressAction->setEnabled(enabled);
    if (rescanWalletAction) rescanWalletAction->setEnabled(enabled);
    if (lockWalletAction) lockWalletAction->setEnabled(false);
    if (unlockWalletAction) unlockWalletAction->setEnabled(false);
}

void XPChainGUI::createTrayIcon(const NetworkStyle *networkStyle)
{
#ifndef Q_OS_MAC
    trayIcon = new QSystemTrayIcon(this);
    QString toolTip = tr("%1 client").arg(tr(PACKAGE_NAME)) + " " + networkStyle->getTitleAddText();
    trayIcon->setToolTip(toolTip);
    trayIcon->setIcon(networkStyle->getTrayAndWindowIcon());
    trayIcon->hide();
#endif

    notificator = new Notificator(QApplication::applicationName(), trayIcon, this);
}

void XPChainGUI::createTrayIconMenu()
{
#ifndef Q_OS_MAC
    // return if trayIcon is unset (only on non-Mac OSes)
    if (!trayIcon)
        return;

    trayIconMenu = new QMenu(this);
    trayIcon->setContextMenu(trayIconMenu);

    connect(trayIcon, SIGNAL(activated(QSystemTrayIcon::ActivationReason)),
            this, SLOT(trayIconActivated(QSystemTrayIcon::ActivationReason)));
#else
    // Note: On Mac, the dock icon is used to provide the tray's functionality.
    MacDockIconHandler *dockIconHandler = MacDockIconHandler::instance();
    dockIconHandler->setMainWindow(static_cast<QMainWindow*>(this));
    trayIconMenu = dockIconHandler->dockMenu();
#endif

    // Configuration of the tray icon (or dock icon) icon menu
    trayIconMenu->addAction(toggleHideAction);
    trayIconMenu->addSeparator();
    trayIconMenu->addAction(sendCoinsMenuAction);
    trayIconMenu->addAction(receiveCoinsMenuAction);
    trayIconMenu->addSeparator();
    trayIconMenu->addAction(signMessageAction);
    trayIconMenu->addAction(verifyMessageAction);
    trayIconMenu->addSeparator();
    trayIconMenu->addAction(optionsAction);
    trayIconMenu->addAction(openRPCConsoleAction);
#ifndef Q_OS_MAC // This is built-in on Mac
    trayIconMenu->addSeparator();
    trayIconMenu->addAction(quitAction);
#endif
}

#ifndef Q_OS_MAC
void XPChainGUI::trayIconActivated(QSystemTrayIcon::ActivationReason reason)
{
    if(reason == QSystemTrayIcon::Trigger)
    {
        // Click on system tray icon triggers show/hide of the main window
        toggleHidden();
    }
}
#endif

void XPChainGUI::optionsClicked()
{
    if(!clientModel || !clientModel->getOptionsModel())
        return;

    OptionsDialog dlg(this, enableWallet);
    dlg.setModel(clientModel->getOptionsModel());
    dlg.exec();
}

void XPChainGUI::aboutClicked()
{
    if(!clientModel)
        return;

    HelpMessageDialog dlg(m_node, this, true);
    dlg.exec();
}

void XPChainGUI::showDebugWindow()
{
    rpcConsole->showNormal();
    rpcConsole->show();
    rpcConsole->raise();
    rpcConsole->activateWindow();
}

void XPChainGUI::showDebugWindowActivateConsole()
{
    rpcConsole->setTabFocus(RPCConsole::TAB_CONSOLE);
    showDebugWindow();
}

void XPChainGUI::showHelpMessageClicked()
{
    helpMessageDialog->show();
}

#ifdef ENABLE_WALLET
void XPChainGUI::openClicked()
{
    OpenURIDialog dlg(this);
    if(dlg.exec())
    {
        Q_EMIT receivedURI(dlg.getURI());
    }
}

void XPChainGUI::gotoOverviewPage()
{
    overviewAction->setChecked(true);
    if (walletFrame) walletFrame->gotoOverviewPage();
}

void XPChainGUI::gotoHistoryPage()
{
    historyAction->setChecked(true);
    if (walletFrame) walletFrame->gotoHistoryPage();
}

void XPChainGUI::gotoReceiveCoinsPage()
{
    receiveCoinsAction->setChecked(true);
    if (walletFrame) walletFrame->gotoReceiveCoinsPage();
}

void XPChainGUI::gotoSendCoinsPage(QString addr)
{
    sendCoinsAction->setChecked(true);
    if (walletFrame) walletFrame->gotoSendCoinsPage(addr);
}

void XPChainGUI::gotoMintingPage()
{
    mintingAction->setChecked(true);
    if (walletFrame) walletFrame->gotoMintingPage();
}

void XPChainGUI::gotoSignMessageTab(QString addr)
{
    if (walletFrame) walletFrame->gotoSignMessageTab(addr);
}

void XPChainGUI::gotoVerifyMessageTab(QString addr)
{
    if (walletFrame) walletFrame->gotoVerifyMessageTab(addr);
}
#endif // ENABLE_WALLET

void XPChainGUI::updateNetworkState()
{
    int count = clientModel->getNumConnections();
    QString icon;
    switch(count)
    {
    case 0: icon = ":/icons/connect_0"; break;
    case 1: case 2: case 3: icon = ":/icons/connect_1"; break;
    case 4: case 5: case 6: icon = ":/icons/connect_2"; break;
    case 7: case 8: case 9: icon = ":/icons/connect_3"; break;
    default: icon = ":/icons/connect_4"; break;
    }

    QString tooltip;

    if (m_node.getNetworkActive()) {
        tooltip = tr("%n active connection(s) to XPChain network", "", count) + QString(".<br>") + tr("Click to disable network activity.");
    } else {
        tooltip = tr("Network activity disabled.") + QString("<br>") + tr("Click to enable network activity again.");
        icon = ":/icons/network_disabled";
    }

    // Don't word-wrap this (fixed-width) tooltip
    tooltip = QString("<nobr>") + tooltip + QString("</nobr>");
    connectionsControl->setToolTip(tooltip);

    connectionsControl->setPixmap(platformStyle->SingleColorIcon(icon).pixmap(STATUSBAR_ICONSIZE,STATUSBAR_ICONSIZE));
}

void XPChainGUI::setNumConnections(int count)
{
    updateNetworkState();
}

void XPChainGUI::setNetworkActive(bool networkActive)
{
    updateNetworkState();
}

void XPChainGUI::updateHeadersSyncProgressLabel()
{
    int64_t headersTipTime = clientModel->getHeaderTipTime();
    int headersTipHeight = clientModel->getHeaderTipHeight();
    int estHeadersLeft = (GetTime() - headersTipTime) / Params().GetConsensus().nPowTargetSpacing;
    if (estHeadersLeft > HEADER_HEIGHT_DELTA_SYNC)
        progressBarLabel->setText(tr("Syncing Headers (%1%)...").arg(QString::number(100.0 / (headersTipHeight+estHeadersLeft)*headersTipHeight, 'f', 1)));
}

void XPChainGUI::setNumBlocks(int count, const QDateTime& blockDate, double nVerificationProgress, bool header)
{
    if (modalOverlay)
    {
        if (header)
            modalOverlay->setKnownBestHeight(count, blockDate);
        else
            modalOverlay->tipUpdate(count, blockDate, nVerificationProgress);
    }
    if (!clientModel)
        return;

    // Prevent orphan statusbar messages (e.g. hover Quit in main menu, wait until chain-sync starts -> garbled text)
    statusBar()->clearMessage();

    // Acquire current block source
    enum BlockSource blockSource = clientModel->getBlockSource();
    switch (blockSource) {
        case BlockSource::NETWORK:
            if (header) {
                updateHeadersSyncProgressLabel();
                return;
            }
            progressBarLabel->setText(tr("Synchronizing with network..."));
            updateHeadersSyncProgressLabel();
            break;
        case BlockSource::DISK:
            if (header) {
                progressBarLabel->setText(tr("Indexing blocks on disk..."));
            } else {
                progressBarLabel->setText(tr("Processing blocks on disk..."));
            }
            break;
        case BlockSource::REINDEX:
            progressBarLabel->setText(tr("Reindexing blocks on disk..."));
            break;
        case BlockSource::NONE:
            if (header) {
                return;
            }
            progressBarLabel->setText(tr("Connecting to peers..."));
            break;
    }

    QString tooltip;

    QDateTime currentDate = QDateTime::currentDateTime();
    qint64 secs = blockDate.secsTo(currentDate);

    tooltip = tr("Processed %n block(s) of transaction history.", "", count);

    // Set icon state: spinning if catching up, tick otherwise
    if(secs < 90*60)
    {
        tooltip = tr("Up to date") + QString(".<br>") + tooltip;
        labelBlocksIcon->setPixmap(platformStyle->SingleColorIcon(":/icons/synced").pixmap(STATUSBAR_ICONSIZE, STATUSBAR_ICONSIZE));

#ifdef ENABLE_WALLET
        if(walletFrame)
        {
            walletFrame->showOutOfSyncWarning(false);
            modalOverlay->showHide(true, true);
        }
#endif // ENABLE_WALLET

        progressBarLabel->setVisible(false);
        progressBar->setVisible(false);
    }
    else
    {
        QString timeBehindText = GUIUtil::formatNiceTimeOffset(secs);

        progressBarLabel->setVisible(true);
        progressBar->setFormat(tr("%1 behind").arg(timeBehindText));
        progressBar->setMaximum(1000000000);
        progressBar->setValue(nVerificationProgress * 1000000000.0 + 0.5);
        progressBar->setVisible(true);

        tooltip = tr("Catching up...") + QString("<br>") + tooltip;
        if(count != prevBlocks)
        {
            labelBlocksIcon->setPixmap(platformStyle->SingleColorIcon(QString(
                ":/movies/spinner-%1").arg(spinnerFrame, 3, 10, QChar('0')))
                .pixmap(STATUSBAR_ICONSIZE, STATUSBAR_ICONSIZE));
            spinnerFrame = (spinnerFrame + 1) % SPINNER_FRAMES;
        }
        prevBlocks = count;

#ifdef ENABLE_WALLET
        if(walletFrame)
        {
            walletFrame->showOutOfSyncWarning(true);
            modalOverlay->showHide();
        }
#endif // ENABLE_WALLET

        tooltip += QString("<br>");
        tooltip += tr("Last received block was generated %1 ago.").arg(timeBehindText);
        tooltip += QString("<br>");
        tooltip += tr("Transactions after this will not yet be visible.");
    }

    // Don't word-wrap this (fixed-width) tooltip
    tooltip = QString("<nobr>") + tooltip + QString("</nobr>");

    labelBlocksIcon->setToolTip(tooltip);
    progressBarLabel->setToolTip(tooltip);
    progressBar->setToolTip(tooltip);
}

void XPChainGUI::updateWindowTitle()
{
    QString windowTitle = tr("XPChain Core");
#ifdef ENABLE_WALLET
    if (enableWallet) {
        if (!m_current_wallet_name.isEmpty()) {
            windowTitle += " - " + m_current_wallet_name;
        } else {
            windowTitle += " - " + tr("Wallet");
        }
    } else {
        windowTitle += " - " + tr("Node");
    }
#else
    windowTitle += " - " + tr("Node");
#endif
    if (m_networkStyle) {
        windowTitle += " " + m_networkStyle->getTitleAddText();
    }
    setWindowTitle(windowTitle);
}

void XPChainGUI::message(const QString &title, const QString &message, unsigned int style, bool *ret)
{
    QString strTitle = tr("XPChain"); // default title
    // Default to information icon
    int nMBoxIcon = QMessageBox::Information;
    int nNotifyIcon = Notificator::Information;

    QString msgType;

    // Prefer supplied title over style based title
    if (!title.isEmpty()) {
        msgType = title;
    }
    else {
        switch (style) {
        case CClientUIInterface::MSG_ERROR:
            msgType = tr("Error");
            break;
        case CClientUIInterface::MSG_WARNING:
            msgType = tr("Warning");
            break;
        case CClientUIInterface::MSG_INFORMATION:
            msgType = tr("Information");
            break;
        default:
            break;
        }
    }
    // Append title to "XPChain - "
    if (!msgType.isEmpty())
        strTitle += " - " + msgType;

    // Check for error/warning icon
    if (style & CClientUIInterface::ICON_ERROR) {
        nMBoxIcon = QMessageBox::Critical;
        nNotifyIcon = Notificator::Critical;
    }
    else if (style & CClientUIInterface::ICON_WARNING) {
        nMBoxIcon = QMessageBox::Warning;
        nNotifyIcon = Notificator::Warning;
    }

    // Display message
    if (style & CClientUIInterface::MODAL) {
        // Check for buttons, use OK as default, if none was supplied
        QMessageBox::StandardButton buttons;
        if (!(buttons = (QMessageBox::StandardButton)(style & CClientUIInterface::BTN_MASK)))
            buttons = QMessageBox::Ok;

        showNormalIfMinimized();
        QMessageBox mBox(static_cast<QMessageBox::Icon>(nMBoxIcon), strTitle, message, buttons, this);
        mBox.setTextFormat(Qt::PlainText);
        int r = mBox.exec();
        if (ret != nullptr)
            *ret = r == QMessageBox::Ok;
    }
    else
        notificator->notify(static_cast<Notificator::Class>(nNotifyIcon), strTitle, message);
}

#ifdef ENABLE_WALLET
void XPChainGUI::askWalletDbPassphrase(const QString& wallet_name, const QString& message, QString* passphrase_out, bool* ok)
{
    if (ok) *ok = false;
    if (!passphrase_out) return;

    // Prefer a clear dual-layer explanation even when the core supplies a prompt.
    QString warning = message;
    if (warning.isEmpty()) {
        warning = tr("Enter the <b>database passphrase</b> to open the encrypted wallet <b>file</b>.<br/><br/>"
                     "Private keys stay locked after this step. You will be asked again when you "
                     "send, sign, or unlock for staking.");
    } else if (!warning.contains(QStringLiteral("locked"), Qt::CaseInsensitive)) {
        warning += tr("<br/><br/>Private keys stay locked after opening the file. "
                      "Unlock again before sending or signing.");
    }
    if (!wallet_name.isEmpty()) {
        warning = tr("<b>Wallet file:</b> %1").arg(wallet_name) + "<br><br>" + warning;
    }
    AskPassphraseDialog dlg(AskPassphraseDialog::DatabaseUnlock, this, warning);
    if (dlg.exec() == QDialog::Accepted) {
        const SecureString& pass = dlg.getPassphrase();
        *passphrase_out = QString::fromStdString(std::string(pass.begin(), pass.end()));
        if (ok) *ok = true;
    }
}
#endif // ENABLE_WALLET

void XPChainGUI::changeEvent(QEvent *e)
{
    QMainWindow::changeEvent(e);
#ifndef Q_OS_MAC // Ignored on Mac
    if(e->type() == QEvent::WindowStateChange)
    {
        if(clientModel && clientModel->getOptionsModel() && clientModel->getOptionsModel()->getMinimizeToTray())
        {
            QWindowStateChangeEvent *wsevt = static_cast<QWindowStateChangeEvent*>(e);
            if(!(wsevt->oldState() & Qt::WindowMinimized) && isMinimized())
            {
                QTimer::singleShot(0, this, SLOT(hide()));
                e->ignore();
            }
            else if((wsevt->oldState() & Qt::WindowMinimized) && !isMinimized())
            {
                QTimer::singleShot(0, this, SLOT(show()));
                e->ignore();
            }
        }
    }
#endif
}

void XPChainGUI::updateTheme(const QString& theme)
{
    GUIUtil::applyTheme(theme);
}

void XPChainGUI::closeEvent(QCloseEvent *event)
{
#ifndef Q_OS_MAC // Ignored on Mac
    if(clientModel && clientModel->getOptionsModel())
    {
        if(!clientModel->getOptionsModel()->getMinimizeOnClose())
        {
            // close rpcConsole in case it was open to make some space for the shutdown window
            rpcConsole->close();

            QApplication::quit();
        }
        else
        {
            QMainWindow::showMinimized();
            event->ignore();
        }
    }
#else
    QMainWindow::closeEvent(event);
#endif
}

void XPChainGUI::showEvent(QShowEvent *event)
{
    // enable the debug window when the main window shows up
    openRPCConsoleAction->setEnabled(true);
    aboutAction->setEnabled(true);
    optionsAction->setEnabled(true);

#ifdef ENABLE_WALLET
    // First show with no wallet loaded: open Set Up Wallet once.
    if (walletFrame && !walletFrame->currentWalletView() && !m_offered_wallet_setup) {
        m_offered_wallet_setup = true;
        QTimer::singleShot(0, this, SLOT(walletSetup()));
    }
#endif
}

#ifdef ENABLE_WALLET
void XPChainGUI::incomingTransaction(const QString& date, int unit, const CAmount& amount, const QString& type, const QString& address, const QString& label, const QString& walletName)
{
    // On new transaction, make an info balloon
    QString msg = tr("Date: %1\n").arg(date) +
                  tr("Amount: %1\n").arg(XPChainUnits::formatWithUnit(unit, amount, true));
    if (m_node.getWallets().size() > 1 && !walletName.isEmpty()) {
        msg += tr("Wallet: %1\n").arg(walletName);
    }
    msg += tr("Type: %1\n").arg(type);
    if (!label.isEmpty())
        msg += tr("Label: %1\n").arg(label);
    else if (!address.isEmpty())
        msg += tr("Address: %1\n").arg(address);
    message((amount)<0 ? tr("Sent transaction") : tr("Incoming transaction"),
             msg, CClientUIInterface::MSG_INFORMATION);
}
#endif // ENABLE_WALLET

void XPChainGUI::dragEnterEvent(QDragEnterEvent *event)
{
    // Accept only URIs
    if(event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void XPChainGUI::dropEvent(QDropEvent *event)
{
    if(event->mimeData()->hasUrls())
    {
        for (const QUrl &uri : event->mimeData()->urls())
        {
            Q_EMIT receivedURI(uri.toString());
        }
    }
    event->acceptProposedAction();
}

bool XPChainGUI::eventFilter(QObject *object, QEvent *event)
{
    // When following the system theme, re-apply if the OS palette changes.
    // Only handle the application-level event: Qt delivers ApplicationPaletteChange
    // to many widgets, and re-applying QSS for each one freezes the UI.
    if (object == qApp && event->type() == QEvent::ApplicationPaletteChange) {
        const QString pref = clientModel && clientModel->getOptionsModel()
            ? clientModel->getOptionsModel()->getTheme()
            : GUIUtil::getThemeSetting();
        if (pref == QLatin1String("system")) {
            GUIUtil::applyTheme(pref);
        }
    }
    // Catch status tip events
    if (event->type() == QEvent::StatusTip)
    {
        // Prevent adding text from setStatusTip(), if we currently use the status bar for displaying other stuff
        if (progressBarLabel->isVisible() || progressBar->isVisible())
            return true;
    }
    return QMainWindow::eventFilter(object, event);
}

#ifdef ENABLE_WALLET
bool XPChainGUI::handlePaymentRequest(const SendCoinsRecipient& recipient)
{
    // URI has to be valid
    if (walletFrame && walletFrame->handlePaymentRequest(recipient))
    {
        showNormalIfMinimized();
        gotoSendCoinsPage();
        return true;
    }
    return false;
}

void XPChainGUI::setHDStatus(int hdEnabled)
{
    labelWalletHDStatusIcon->setPixmap(platformStyle->SingleColorIcon(hdEnabled ? ":/icons/hd_enabled" : ":/icons/hd_disabled").pixmap(STATUSBAR_ICONSIZE,STATUSBAR_ICONSIZE));
    labelWalletHDStatusIcon->setToolTip(hdEnabled ? tr("HD key generation is <b>enabled</b>") : tr("HD key generation is <b>disabled</b>"));

    // eventually disable the QLabel to set its opacity to 50%
    labelWalletHDStatusIcon->setEnabled(hdEnabled);
}

void XPChainGUI::setEncryptionStatus(int status)
{
    switch(status)
    {
    case WalletModel::Unencrypted:
        labelWalletEncryptionIcon->hide();
        encryptWalletAction->setChecked(false);
        changePassphraseAction->setEnabled(false);
        encryptWalletAction->setEnabled(true);
        decryptForMintingAction->setEnabled(false);
        decryptForMintingAction->setChecked(false);
        if (lockWalletAction) lockWalletAction->setEnabled(false);
        if (unlockWalletAction) unlockWalletAction->setEnabled(false);
        break;
    case WalletModel::Unlocked:
        labelWalletEncryptionIcon->show();
        if (fWalletUnlockMintOnly) {
            labelWalletEncryptionIcon->setPixmap(platformStyle->SingleColorIcon(":/icons/lock_staking").pixmap(STATUSBAR_ICONSIZE,STATUSBAR_ICONSIZE));
            labelWalletEncryptionIcon->setToolTip(
                tr("Wallet file is open. Spending keys are <b>unlocked for staking only</b> — "
                   "sending still requires a full unlock.<br/>Click to lock the wallet."));
        } else {
            labelWalletEncryptionIcon->setPixmap(platformStyle->SingleColorIcon(":/icons/lock_open").pixmap(STATUSBAR_ICONSIZE,STATUSBAR_ICONSIZE));
            labelWalletEncryptionIcon->setToolTip(
                tr("Wallet file is open and spending keys are <b>unlocked</b> (sending and signing allowed).<br/>"
                   "Click to lock the wallet."));
        }
        encryptWalletAction->setChecked(true);
        changePassphraseAction->setEnabled(true);
        encryptWalletAction->setEnabled(false); // TODO: decrypt currently not supported
        decryptForMintingAction->setEnabled(fWalletUnlockMintOnly);
        decryptForMintingAction->setChecked(fWalletUnlockMintOnly);
        if (lockWalletAction) lockWalletAction->setEnabled(true);
        // Fully unlocked: no unlock needed. Staking-only unlock can still open full unlock via menu.
        if (unlockWalletAction) unlockWalletAction->setEnabled(fWalletUnlockMintOnly);
        break;
    case WalletModel::Locked:
        labelWalletEncryptionIcon->show();
        labelWalletEncryptionIcon->setPixmap(platformStyle->SingleColorIcon(":/icons/lock_closed").pixmap(STATUSBAR_ICONSIZE,STATUSBAR_ICONSIZE));
        labelWalletEncryptionIcon->setToolTip(
            tr("Wallet is <b>encrypted</b>. The wallet <b>file may already be open</b>, "
               "but spending keys are <b>locked</b> — unlock before sending or signing.<br/>"
               "Click to unlock."));
        encryptWalletAction->setChecked(true);
        changePassphraseAction->setEnabled(true);
        encryptWalletAction->setEnabled(false); // TODO: decrypt currently not supported
        decryptForMintingAction->setEnabled(true);
        decryptForMintingAction->setChecked(false);
        if (lockWalletAction) lockWalletAction->setEnabled(false);
        if (unlockWalletAction) unlockWalletAction->setEnabled(true);
        break;
    }
}

void XPChainGUI::updateWalletStatus()
{
    if (!walletFrame) {
        if (labelWalletFormatStatus) labelWalletFormatStatus->hide();
        if (labelStakingIcon) labelStakingIcon->hide();
        return;
    }
    WalletView * const walletView = walletFrame->currentWalletView();
    if (!walletView) {
        if (labelWalletFormatStatus) labelWalletFormatStatus->hide();
        if (labelStakingIcon) labelStakingIcon->hide();
        return;
    }
    WalletModel * const walletModel = walletView->getWalletModel();
    setEncryptionStatus(walletModel->getEncryptionStatus());
    setHDStatus(walletModel->wallet().hdEnabled());
    setWalletFormatStatus(walletModel);
    updateStakingIcon();
}

void XPChainGUI::setWalletFormatStatus(WalletModel* walletModel)
{
    if (!labelWalletFormatStatus) {
        return;
    }
    if (!walletModel) {
        labelWalletFormatStatus->hide();
        if (labelStakingIcon) labelStakingIcon->hide();
        if (migrateWalletAction) migrateWalletAction->setEnabled(false);
        return;
    }

    interfaces::Wallet& wallet = walletModel->wallet();
    const bool sqlite = (wallet.databaseFormat() == "sqlite");
    const bool at_rest = wallet.isEncryptedAtRest();
    const bool descriptor = wallet.isDescriptor();
    const bool watch_only = walletModel->privateKeysDisabled();
    const int enc = walletModel->getEncryptionStatus();

    QString format_text = sqlite ? tr("SQLite") : tr("BDB");
    QString color = sqlite ? QStringLiteral("#6cb6ff") : QStringLiteral("#ef6c00");
    QString border = sqlite ? QStringLiteral("#1f6feb") : QStringLiteral("#ef6c00");
    labelWalletFormatStatus->setText(format_text);
    labelWalletFormatStatus->setStyleSheet(QStringLiteral(
        "QLabel { font-size: 10px; font-weight: 600; color: %1; "
        "background-color: transparent; border: 1px solid %2; "
        "border-radius: 3px; padding: 1px 6px; }").arg(color, border));
    labelWalletFormatStatus->setCursor(sqlite ? Qt::ArrowCursor : Qt::PointingHandCursor);

    QString keys_line;
    if (watch_only) {
        keys_line = tr("Spending keys: none (watch-only)");
    } else if (enc == WalletModel::Unencrypted) {
        keys_line = tr("Spending keys: unencrypted");
    } else if (enc == WalletModel::Locked) {
        keys_line = tr("Spending keys: locked");
    } else if (fWalletUnlockMintOnly) {
        keys_line = tr("Spending keys: unlocked for staking only");
    } else {
        keys_line = tr("Spending keys: unlocked");
    }

    QString file_line;
    if (sqlite) {
        file_line = at_rest ? tr("File: SQLCipher (encrypted at rest)") : tr("File: unencrypted SQLite");
    } else {
        file_line = tr("File: Berkeley DB (use File → Migrate Wallet to SQLite…)");
    }

    QString click_hint = sqlite ? QString() : (QStringLiteral("<br/><b>") + tr("Click to migrate this wallet to SQLite…") + QStringLiteral("</b>"));

    labelWalletFormatStatus->setToolTip(
        tr("Wallet status") + QStringLiteral("<br/>") +
        tr("Database: %1").arg(sqlite ? tr("SQLite") : tr("Berkeley DB")) + QStringLiteral("<br/>") +
        file_line + QStringLiteral("<br/>") +
        tr("Type: %1").arg(watch_only ? tr("watch-only") : (descriptor ? tr("descriptor") : tr("legacy HD"))) + QStringLiteral("<br/>") +
        keys_line + click_hint);
    labelWalletFormatStatus->show();
    if (migrateWalletAction) {
        migrateWalletAction->setEnabled(!sqlite);
    }
}

void XPChainGUI::updateStakingIcon()
{
    if (!labelStakingIcon) {
        return;
    }
    WalletModel* model = nullptr;
    if (walletFrame && walletFrame->currentWalletView()) {
        model = walletFrame->currentWalletView()->getWalletModel();
    }
    if (!model) {
        labelStakingIcon->hide();
        return;
    }

    const bool fMinting = gArgs.GetBoolArg("-minting", true);
    const bool isIBD = clientModel ? clientModel->node().isInitialBlockDownload() : true;
    const int enc = model->getEncryptionStatus();
    const bool isLocked = (enc == WalletModel::Locked);
    const bool watch_only = model->privateKeysDisabled();
    interfaces::WalletBalances balances = model->wallet().getBalances();
    const bool hasBalance = balances.balance > 0;

    bool stakingActive = false;
    QString tooltip;

    if (!fMinting) {
        tooltip = tr("Staking is disabled (-minting=0).");
    } else if (isIBD) {
        tooltip = tr("Staking is paused: blockchain is synchronizing.");
    } else if (watch_only) {
        tooltip = tr("Staking is disabled: watch-only wallet has no private keys.");
    } else if (isLocked && !fWalletUnlockMintOnly) {
        tooltip = tr("Staking is paused: wallet is locked. Unlock for staking to enable.");
    } else if (!hasBalance) {
        tooltip = tr("Staking is waiting: no spendable balance available in this wallet.");
    } else {
        // Balance alone cannot prove stake maturity; avoid over-claiming "active".
        stakingActive = true;
        tooltip = tr("Staking may be active if this wallet has mature coins unlocked for staking.\n"
                     "Open the Staking tab to check coin age and estimated odds.");
    }

    if (stakingActive) {
        labelStakingIcon->setPixmap(QIcon(":/icons/staking_active").pixmap(STATUSBAR_ICONSIZE, STATUSBAR_ICONSIZE));
    } else {
        labelStakingIcon->setPixmap(QIcon(":/icons/staking_inactive").pixmap(STATUSBAR_ICONSIZE, STATUSBAR_ICONSIZE));
    }
    labelStakingIcon->setToolTip(tooltip);
    labelStakingIcon->show();
}
#endif // ENABLE_WALLET

void XPChainGUI::updateProxyIcon()
{
    std::string ip_port;
    bool proxy_enabled = clientModel->getProxyInfo(ip_port);

    if (proxy_enabled) {
        if (labelProxyIcon->pixmap() == 0) {
            QString ip_port_q = QString::fromStdString(ip_port);
            labelProxyIcon->setPixmap(platformStyle->SingleColorIcon(":/icons/proxy").pixmap(STATUSBAR_ICONSIZE, STATUSBAR_ICONSIZE));
            labelProxyIcon->setToolTip(tr("Proxy is <b>enabled</b>: %1").arg(ip_port_q));
        } else {
            labelProxyIcon->show();
        }
    } else {
        labelProxyIcon->hide();
    }
}

void XPChainGUI::showNormalIfMinimized(bool fToggleHidden)
{
    if(!clientModel)
        return;

    // activateWindow() (sometimes) helps with keyboard focus on Windows
    if (isHidden())
    {
        show();
        activateWindow();
    }
    else if (isMinimized())
    {
        showNormal();
        activateWindow();
    }
    else if (GUIUtil::isObscured(this))
    {
        raise();
        activateWindow();
    }
    else if(fToggleHidden)
        hide();
}

void XPChainGUI::toggleHidden()
{
    showNormalIfMinimized(true);
}

void XPChainGUI::detectShutdown()
{
    if (m_node.shutdownRequested())
    {
        if(rpcConsole)
            rpcConsole->hide();
        qApp->quit();
    }
}

void XPChainGUI::showProgress(const QString &title, int nProgress)
{
    if (nProgress == 0)
    {
        progressDialog = new QProgressDialog(title, "", 0, 100);
        progressDialog->setWindowModality(Qt::ApplicationModal);
        progressDialog->setMinimumDuration(0);
        progressDialog->setCancelButton(0);
        progressDialog->setAutoClose(false);
        progressDialog->setValue(0);
    }
    else if (nProgress == 100)
    {
        if (progressDialog)
        {
            progressDialog->close();
            progressDialog->deleteLater();
        }
    }
    else if (progressDialog)
        progressDialog->setValue(nProgress);
}

void XPChainGUI::setTrayIconVisible(bool fHideTrayIcon)
{
    if (trayIcon)
    {
        trayIcon->setVisible(!fHideTrayIcon);
    }
}

void XPChainGUI::showModalOverlay()
{
    if (modalOverlay && (progressBar->isVisible() || modalOverlay->isLayerVisible()))
        modalOverlay->toggleVisibility();
}

static bool ThreadSafeMessageBox(XPChainGUI *gui, const std::string& message, const std::string& caption, unsigned int style)
{
    bool modal = (style & CClientUIInterface::MODAL);
    // The SECURE flag has no effect in the Qt GUI.
    // bool secure = (style & CClientUIInterface::SECURE);
    style &= ~CClientUIInterface::SECURE;
    bool ret = false;
    // In case of modal message, use blocking connection to wait for user to click a button
    QMetaObject::invokeMethod(gui, "message",
                                modal ? GUIUtil::blockingGUIThreadConnection() : Qt::QueuedConnection,
                                Q_ARG(QString, QString::fromStdString(caption)),
                                Q_ARG(QString, QString::fromStdString(message)),
                                Q_ARG(unsigned int, style),
                                Q_ARG(bool*, &ret));
    return ret;
}

#ifdef ENABLE_WALLET
static bool ThreadSafeAskPassphrase(XPChainGUI *gui, const std::string& wallet_name, const std::string& message, SecureString& passphrase_out)
{
    QString passphrase;
    bool ok = false;
    QMetaObject::invokeMethod(gui, "askWalletDbPassphrase",
                              GUIUtil::blockingGUIThreadConnection(),
                              Q_ARG(QString, QString::fromStdString(wallet_name)),
                              Q_ARG(QString, QString::fromStdString(message)),
                              Q_ARG(QString*, &passphrase),
                              Q_ARG(bool*, &ok));
    if (!ok || passphrase.isEmpty()) {
        return false;
    }
    passphrase_out.assign(passphrase.toStdString().c_str());
    passphrase.fill(QChar(' '));
    passphrase.clear();
    return true;
}
#endif

void XPChainGUI::subscribeToCoreSignals()
{
    // Connect signals to client
    m_handler_message_box = m_node.handleMessageBox(boost::bind(ThreadSafeMessageBox, this, boost::placeholders::_1, boost::placeholders::_2, boost::placeholders::_3));
    m_handler_question = m_node.handleQuestion(boost::bind(ThreadSafeMessageBox, this, boost::placeholders::_1, boost::placeholders::_3, boost::placeholders::_4));
#ifdef ENABLE_WALLET
    m_handler_ask_passphrase = m_node.handleAskPassphrase(boost::bind(ThreadSafeAskPassphrase, this, boost::placeholders::_1, boost::placeholders::_2, boost::placeholders::_3));
#endif
}

void XPChainGUI::unsubscribeFromCoreSignals()
{
    // Disconnect signals from client
    m_handler_message_box->disconnect();
    m_handler_question->disconnect();
#ifdef ENABLE_WALLET
    if (m_handler_ask_passphrase) {
        m_handler_ask_passphrase->disconnect();
    }
#endif
}

void XPChainGUI::toggleNetworkActive()
{
    m_node.setNetworkActive(!m_node.getNetworkActive());
}

UnitDisplayStatusBarControl::UnitDisplayStatusBarControl(const PlatformStyle *platformStyle) :
    optionsModel(0),
    menu(0)
{
    createContextMenu();
    setToolTip(tr("Unit to show amounts in. Click to select another unit."));
    QList<XPChainUnits::Unit> units = XPChainUnits::availableUnits();
    int max_width = 0;
    const QFontMetrics fm(font());
    for (const XPChainUnits::Unit unit : units)
    {
        max_width = qMax(max_width, fm.width(XPChainUnits::longName(unit)));
    }
    setMinimumSize(max_width, 0);
    setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    setStyleSheet(QString("QLabel { color : %1 }").arg(platformStyle->SingleColor().name()));
}

/** So that it responds to button clicks */
void UnitDisplayStatusBarControl::mousePressEvent(QMouseEvent *event)
{
    onDisplayUnitsClicked(event->pos());
}

/** Creates context menu, its actions, and wires up all the relevant signals for mouse events. */
void UnitDisplayStatusBarControl::createContextMenu()
{
    menu = new QMenu(this);
    for (XPChainUnits::Unit u : XPChainUnits::availableUnits())
    {
        QAction *menuAction = new QAction(QString(XPChainUnits::longName(u)), this);
        menuAction->setData(QVariant(u));
        menu->addAction(menuAction);
    }
    connect(menu,SIGNAL(triggered(QAction*)),this,SLOT(onMenuSelection(QAction*)));
}

/** Lets the control know about the Options Model (and its signals) */
void UnitDisplayStatusBarControl::setOptionsModel(OptionsModel *_optionsModel)
{
    if (_optionsModel)
    {
        this->optionsModel = _optionsModel;

        // be aware of a display unit change reported by the OptionsModel object.
        connect(_optionsModel,SIGNAL(displayUnitChanged(int)),this,SLOT(updateDisplayUnit(int)));

        // initialize the display units label with the current value in the model.
        updateDisplayUnit(_optionsModel->getDisplayUnit());
    }
}

/** When Display Units are changed on OptionsModel it will refresh the display text of the control on the status bar */
void UnitDisplayStatusBarControl::updateDisplayUnit(int newUnits)
{
    setText(XPChainUnits::longName(newUnits));
}

/** Shows context menu with Display Unit options by the mouse coordinates */
void UnitDisplayStatusBarControl::onDisplayUnitsClicked(const QPoint& point)
{
    QPoint globalPos = mapToGlobal(point);
    menu->exec(globalPos);
}

/** Tells underlying optionsModel to update its current display unit. */
void UnitDisplayStatusBarControl::onMenuSelection(QAction* action)
{
    if (action)
    {
        optionsModel->setDisplayUnit(action->data());
    }
}

void XPChainGUI::walletSetup()
{
#ifdef ENABLE_WALLET
    if (!walletFrame) return;

    WalletSetupDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }

    switch (dlg.choice()) {
    case WalletSetupDialog::CreateEmpty:
        createWallet();
        break;
    case WalletSetupDialog::GenerateMnemonic:
        generateMnemonicWallet();
        break;
    case WalletSetupDialog::RestoreMnemonic: {
        WalletView* view = walletFrame->currentWalletView();
        if (!view || !view->getWalletModel()) {
            const auto reply = QMessageBox::question(
                this, tr("No wallet loaded"),
                tr("Restore needs a wallet to import keys into.\n\n"
                   "Create an empty wallet now, then continue with restore?"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
            if (reply != QMessageBox::Yes) {
                return;
            }
            createWalletInternal(true);
            view = walletFrame->currentWalletView();
            if (!view || !view->getWalletModel()) {
                return;
            }
        }
        importMnemonic();
        break;
    }
    case WalletSetupDialog::OpenExisting:
        openWallet();
        break;
    }
#endif
}

void XPChainGUI::lockWallet()
{
#ifdef ENABLE_WALLET
    if (!walletFrame) return;
    WalletView* view = walletFrame->currentWalletView();
    if (!view) return;
    WalletModel* model = view->getWalletModel();
    if (!model) return;
    if (model->getEncryptionStatus() != WalletModel::Unlocked) {
        return;
    }
    if (model->setWalletLocked(true)) {
        fWalletUnlockMintOnly = false;
        if (decryptForMintingAction) decryptForMintingAction->setChecked(false);
        updateWalletStatus();
    } else {
        QMessageBox::warning(this, tr("Lock Wallet"),
                             tr("Failed to lock the wallet."));
    }
#endif
}

void XPChainGUI::unlockWallet()
{
#ifdef ENABLE_WALLET
    if (!walletFrame) return;
    // Clear staking-only flag so AskPassphrase Unlock is a full spend unlock.
    fWalletUnlockMintOnly = false;
    if (decryptForMintingAction) decryptForMintingAction->setChecked(false);
    walletFrame->unlockWallet();
    updateWalletStatus();
#endif
}

void XPChainGUI::createWallet()
{
    createWalletInternal(false);
}

void XPChainGUI::createWalletInternal(bool for_mnemonic_restore)
{
    // Ensure wallets directory exists so new wallets are created there
    QDir().mkpath(QString::fromStdString(GetDataDir().string()) + "/wallets");

    CreateWalletDialog dlg(this);
    if (for_mnemonic_restore) {
        dlg.setForMnemonicRestore(true);
    }
    if(dlg.exec())
    {
        const QString name = dlg.walletName();
        const bool encrypt = dlg.encryptWallet();
        QString passphrase = dlg.passphrase();
        dlg.secureClearPassphrases();

        UniValue params(UniValue::VARR);
        params.push_back(name.toStdString());
        params.push_back(dlg.disablePrivateKeys());
        params.push_back(dlg.descriptors());
        params.push_back(encrypt ? passphrase.toStdString() : "");
        params.push_back(true); // load_on_startup

        passphrase.fill(QChar(' '));
        passphrase.clear();

        try {
            m_node.executeRpc("createwallet", params, "");
            if (encrypt) {
                QMessageBox::information(this, tr("Wallet created"),
                    tr("Wallet \"%1\" was created and encrypted.<br/><br/>"
                       "Spending keys are locked. Unlock before sending or signing.<br/><br/>"
                       "After restart: open the wallet file with this passphrase "
                       "(GUI prompt or <code>-walletdbpassphrase</code>), then unlock keys again.")
                        .arg(name));
            } else {
                QMessageBox::information(this, tr("Wallet created"),
                    tr("Wallet \"%1\" was created without encryption. "
                       "You can encrypt it later from Settings → Encrypt Wallet.")
                        .arg(name));
            }
        } catch (const UniValue& e) {
            QMessageBox::critical(this, tr("Wallet Creation Failed"), QString::fromStdString(e["message"].get_str()));
        } catch (const std::exception& e) {
            QMessageBox::critical(this, tr("Wallet Creation Failed"), QString::fromStdString(e.what()));
        }
    }
}

void XPChainGUI::generateMnemonicWallet()
{
#ifdef ENABLE_WALLET
    // Ensure wallets directory exists so new wallets are created there
    QDir().mkpath(QString::fromStdString(GetDataDir().string()) + "/wallets");

    MnemonicBackupDialog dlg(this);
    if (!dlg.exec()) {
        return;
    }

    const QString name = dlg.walletName();
    const bool encrypt = dlg.encryptWallet();
    QString wallet_pass = dlg.walletPassphrase();
    const bool use_bip44 = dlg.useBip44();
    const unsigned int bip44_coin_type = dlg.bip44CoinType();
    QString bip39_pass = dlg.bip39Passphrase();
    SecureString mnemonic = dlg.mnemonic();
    dlg.secureClearSecrets();

    QByteArray encodedName = QUrl::toPercentEncoding(name);
    const std::string uri = "/wallet/" + std::string(encodedName.constData(), encodedName.length());

    try {
        UniValue create_params(UniValue::VARR);
        create_params.push_back(name.toStdString());
        create_params.push_back(false); // disable_private_keys
        create_params.push_back(false); // descriptors — mnemonic import requires legacy HD
        create_params.push_back(encrypt ? wallet_pass.toStdString() : "");
        create_params.push_back(true); // load_on_startup
        m_node.executeRpc("createwallet", create_params, "");

        if (encrypt) {
            UniValue unlock_params(UniValue::VARR);
            unlock_params.push_back(wallet_pass.toStdString());
            unlock_params.push_back(120); // seconds
            m_node.executeRpc("walletpassphrase", unlock_params, uri);
        }

        UniValue options(UniValue::VOBJ);
        if (!bip39_pass.isEmpty()) {
            options.pushKV("passphrase", bip39_pass.toStdString());
        }
        options.pushKV("bip44", use_bip44);
        options.pushKV("bip44_coin_type", static_cast<int>(bip44_coin_type));
        options.pushKV("gap_limit", 1000);
        options.pushKV("rescan", false);

        UniValue import_params(UniValue::VARR);
        import_params.push_back(std::string(mnemonic.begin(), mnemonic.end()));
        import_params.push_back(options);
        m_node.executeRpc("importmnemonic", import_params, uri);

        QString path_note;
        if (use_bip44) {
            path_note = tr("BIP44 path m/44'/%1'/0'/…").arg(bip44_coin_type);
        } else {
            path_note = tr("XPChain Core HD path m/0'/…");
        }

        QMessageBox::information(this, tr("Mnemonic wallet created"),
            tr("Wallet \"%1\" was created from your confirmed BIP39 backup (%2).<br/><br/>"
               "Keep the written mnemonic offline. It cannot be recovered from the wallet file later.<br/><br/>"
               "%3")
                .arg(name)
                .arg(path_note)
                .arg(encrypt
                    ? tr("The wallet is encrypted. Unlock spending keys before sending.")
                    : tr("The wallet is not encrypted. You can encrypt it later from Settings → Encrypt Wallet.")));
    } catch (const UniValue& e) {
        QMessageBox::critical(this, tr("Mnemonic wallet creation failed"),
                              QString::fromStdString(e["message"].get_str()));
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Mnemonic wallet creation failed"),
                              QString::fromStdString(e.what()));
    }

    memory_cleanse(mnemonic.data(), mnemonic.size());
    mnemonic.clear();
    wallet_pass.fill(QChar(' '));
    wallet_pass.clear();
    bip39_pass.fill(QChar(' '));
    bip39_pass.clear();
#endif
}

void XPChainGUI::importMnemonic()
{
#ifdef ENABLE_WALLET
    if (!walletFrame)
        return;
    WalletView *walletView = walletFrame->currentWalletView();
    if (!walletView)
        return;
    WalletModel *walletModel = walletView->getWalletModel();
    if (!walletModel) {
        QMessageBox::warning(this, tr("No Wallet Selected"), tr("Please open or select a wallet first to restore."));
        return;
    }

    MnemonicImportDialog dlg(this, walletModel);
    bool create_empty = false;
    connect(&dlg, &MnemonicImportDialog::createEmptyWalletRequested, this, [&create_empty]() {
        create_empty = true;
    });
    dlg.exec();

    if (create_empty) {
        createWalletInternal(true);
        WalletView* view = walletFrame->currentWalletView();
        if (view && view->getWalletModel()) {
            MnemonicImportDialog again(this, view->getWalletModel());
            again.exec();
        }
    }
#endif
}

void XPChainGUI::closeWallet()
{
#ifdef ENABLE_WALLET
    if (!walletFrame) return;
    WalletView* walletView = walletFrame->currentWalletView();
    if (!walletView) return;
    WalletModel* walletModel = walletView->getWalletModel();
    if (!walletModel) {
        QMessageBox::warning(this, tr("No Wallet Selected"), tr("Please select a wallet to close."));
        return;
    }

    const QString name = walletModel->getWalletName();
    const QString display = name.isEmpty() ? tr("[default wallet]") : name;
    if (QMessageBox::question(this, tr("Close Wallet"),
            tr("Unload wallet \"%1\"?\n\n"
               "You can open it again later from File → Set Up Wallet or File → Advanced → Open Wallet.")
                .arg(display),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    try {
        UniValue params(UniValue::VARR);
        params.push_back(name.toStdString());
        m_node.executeRpc("unloadwallet", params, "");
    } catch (const UniValue& e) {
        QMessageBox::critical(this, tr("Close Wallet Failed"),
                              QString::fromStdString(e.exists("message") ? e["message"].get_str() : e.write()));
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Close Wallet Failed"), QString::fromStdString(e.what()));
    }
#endif
}

void XPChainGUI::importAddress()
{
#ifdef ENABLE_WALLET
    if (!walletFrame) return;
    WalletView* walletView = walletFrame->currentWalletView();
    if (!walletView) return;
    WalletModel* walletModel = walletView->getWalletModel();
    if (!walletModel) {
        QMessageBox::warning(this, tr("No Wallet Selected"),
                             tr("Please open or select a wallet first."));
        return;
    }

    ImportAddressDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }

    const QString address = dlg.address();
    if (address.isEmpty()) {
        QMessageBox::warning(this, tr("Import Address"), tr("Please enter an address."));
        return;
    }

    const std::string wallet_name = walletModel->wallet().getWalletName();
    QByteArray encodedName = QUrl::toPercentEncoding(QString::fromStdString(wallet_name));
    const std::string uri = "/wallet/" + std::string(encodedName.constData(), encodedName.length());

    try {
        UniValue params(UniValue::VARR);
        params.push_back(address.toStdString());
        params.push_back(dlg.label().toStdString());
        params.push_back(dlg.rescan());
        m_node.executeRpc("importaddress", params, uri);
        QMessageBox::information(this, tr("Address Imported"),
            dlg.rescan()
                ? tr("Address imported. A blockchain rescan may run in the background; watch the progress dialog.")
                : tr("Address imported without a rescan. Use Tools → Rescan Wallet if history is missing."));
    } catch (const UniValue& e) {
        QMessageBox::critical(this, tr("Import Failed"),
                              QString::fromStdString(e.exists("message") ? e["message"].get_str() : e.write()));
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Import Failed"), QString::fromStdString(e.what()));
    }
#endif
}

namespace {
class GuiRescanWorker : public QRunnable
{
public:
    GuiRescanWorker(WalletModel* model) : m_model(model) {}

    void run() override
    {
        bool success = false;
        if (m_model) {
            success = m_model->wallet().rescanBlockchain(0);
        }
        if (m_model) {
            QMetaObject::invokeMethod(m_model, "notifyMnemonicRescanFinished",
                                      Qt::QueuedConnection,
                                      Q_ARG(bool, success));
        }
    }

private:
    WalletModel* m_model;
};
} // namespace

void XPChainGUI::rescanWallet()
{
#ifdef ENABLE_WALLET
    if (!walletFrame) return;
    WalletView* walletView = walletFrame->currentWalletView();
    if (!walletView) return;
    WalletModel* walletModel = walletView->getWalletModel();
    if (!walletModel) {
        QMessageBox::warning(this, tr("No Wallet Selected"),
                             tr("Please open or select a wallet first."));
        return;
    }

    if (QMessageBox::question(this, tr("Rescan Wallet"),
            tr("Rescan the blockchain for wallet \"%1\"?\n\n"
               "This can take a long time. You can cancel from the progress dialog.")
                .arg(walletModel->getWalletName().isEmpty() ? tr("[default wallet]") : walletModel->getWalletName()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    GuiRescanWorker* worker = new GuiRescanWorker(walletModel);
    worker->setAutoDelete(true);
    QThreadPool::globalInstance()->start(worker);
#endif
}

void XPChainGUI::migrateWallet()
{
#ifdef ENABLE_WALLET
    if (!walletFrame)
        return;
    WalletView *walletView = walletFrame->currentWalletView();
    if (!walletView)
        return;
    WalletModel *walletModel = walletView->getWalletModel();
    if (!walletModel) {
        QMessageBox::warning(this, tr("No Wallet Selected"),
                             tr("Please open or select a Berkeley DB wallet first."));
        return;
    }

    MigrateWalletDialog dlg(this, walletModel);
    if (!dlg.exec()) {
        return;
    }

    UniValue options(UniValue::VOBJ);
    options.pushKV("backup", dlg.doBackup());
    options.pushKV("in_place", dlg.inPlace());
    options.pushKV("overwrite", dlg.overwrite());
    options.pushKV("load_new", false); // Do not swap live in-process to avoid BDB/PoS thread deadlock
    if (!dlg.inPlace()) {
        options.pushKV("destination", dlg.destination().toStdString());
    }

    UniValue params(UniValue::VARR);
    params.push_back(options);

    const std::string wallet_name = walletModel->wallet().getWalletName();
    QByteArray encodedName = QUrl::toPercentEncoding(QString::fromStdString(wallet_name));
    const std::string uri = "/wallet/" + std::string(encodedName.constData(), encodedName.length());

    try {
        UniValue result = m_node.executeRpc("migratewallet", params, uri);
        QString message = tr("Wallet migrated to SQLite successfully.");
        if (result.exists("destination")) {
            message += tr("\n\nSQLite wallet: %1").arg(QString::fromStdString(result["destination"].get_str()));
        }
        if (result.exists("backup")) {
            message += tr("\nBackup: %1").arg(QString::fromStdString(result["backup"].get_str()));
        }
        if (result.exists("records_copied")) {
            message += tr("\nRecords copied: %1").arg(result["records_copied"].get_int());
        }
        message += tr("\n\nXPChain will now close safely to complete the SQLite format upgrade.\n"
                      "Please restart XPChain to use your modern SQLite wallet.");
        QMessageBox::information(this, tr("Migration Complete"), message);
        qApp->quit();
    } catch (const UniValue& e) {
        QString err = e.exists("message") ? QString::fromStdString(e["message"].get_str())
                                          : QString::fromStdString(e.write());
        QMessageBox::critical(this, tr("Migration Failed"), err);
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Migration Failed"), QString::fromStdString(e.what()));
    }
#endif
}

void XPChainGUI::openWallet()
{
#ifdef ENABLE_WALLET
    QString wallets_dir = QString::fromStdString(GetWalletDir().string());
    QDir().mkpath(wallets_dir);

    QDir dir(wallets_dir);
    QStringList candidates;

    // Named wallet directories (BDB wallet.dat or SQLite wallet.sqlite)
    for (const QFileInfo& info : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const QString path = info.absoluteFilePath();
        if (QFile::exists(path + QStringLiteral("/wallet.dat")) ||
            QFile::exists(path + QStringLiteral("/wallet.sqlite"))) {
            candidates << info.fileName();
        }
    }
    // Standalone files directly under wallets/
    for (const QFileInfo& info : dir.entryInfoList(QStringList() << "*.dat" << "*.sqlite" << "wallet.dat", QDir::Files, QDir::Name)) {
        candidates << info.fileName();
    }
    candidates.removeDuplicates();
    candidates.sort(Qt::CaseInsensitive);

    QString wallet_name;
    if (!candidates.isEmpty()) {
        QDialog dlg(this);
        dlg.setWindowTitle(tr("Open Wallet"));
        QVBoxLayout* layout = new QVBoxLayout(&dlg);
        layout->addWidget(new QLabel(tr("Wallets found in:\n%1").arg(wallets_dir), &dlg));
        QListWidget* list = new QListWidget(&dlg);
        list->addItems(candidates);
        list->setCurrentRow(0);
        layout->addWidget(list);
        QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        QPushButton* browse = buttons->addButton(tr("Browse…"), QDialogButtonBox::ActionRole);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        connect(list, &QListWidget::itemDoubleClicked, &dlg, &QDialog::accept);
        bool browse_clicked = false;
        connect(browse, &QPushButton::clicked, &dlg, [&]() { browse_clicked = true; dlg.reject(); });
        if (dlg.exec() == QDialog::Accepted && list->currentItem()) {
            wallet_name = list->currentItem()->text();
        } else if (!browse_clicked) {
            return;
        }
    }

    if (wallet_name.isEmpty()) {
        QString path = QFileDialog::getOpenFileName(this, tr("Open Wallet"), wallets_dir,
            tr("Wallet files (wallet.dat *.dat *.sqlite);;All Files (*)"));
        if (path.isEmpty()) return;
        QFileInfo fileInfo(path);
        if (fileInfo.fileName() == "wallet.dat") {
            wallet_name = fileInfo.absoluteDir().dirName();
        } else {
            wallet_name = fileInfo.fileName();
        }
        // Prefer loading by name relative to wallets dir when possible
        const QString wallets_abs = QDir(wallets_dir).absolutePath();
        if (fileInfo.absoluteFilePath().startsWith(wallets_abs)) {
            // keep wallet_name as relative leaf
        }
    }

    UniValue params(UniValue::VARR);
    params.push_back(wallet_name.toStdString());

    const fs::path wallet_path = fs::absolute(wallet_name.toStdString(), GetWalletDir());
    if (IsSqlcipherEncryptedFile(WalletDatabaseFilePath(wallet_path))) {
        QString passphrase;
        bool ok = false;
        askWalletDbPassphrase(wallet_name, QString(), &passphrase, &ok);
        if (!ok || passphrase.isEmpty()) return;
        params.push_back(passphrase.toStdString());
        passphrase.fill(QChar(' '));
        passphrase.clear();
    }

    try {
        m_node.executeRpc("loadwallet", params, "");
    } catch (const UniValue& e) {
        QString err = e.exists("message") ? QString::fromStdString(e["message"].get_str()) : QString::fromStdString(e.write());
        QMessageBox::critical(this, tr("Open Wallet Failed"),
            tr("Could not open wallet \"%1\".\n\n%2\n\nWallets must live under:\n%3")
                .arg(wallet_name, err, wallets_dir));
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Open Wallet Failed"), QString::fromStdString(e.what()));
    }
#endif
}

void XPChainGUI::backupAllWallets()
{
    QString dir = QFileDialog::getExistingDirectory(this, tr("Backup All Wallets"), "", QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (dir.isEmpty()) return;

    int success_count = 0;
    int fail_count = 0;
    QString errors;

    for (auto& wallet : m_node.getWallets()) {
        QString name = QString::fromStdString(wallet->getWalletName());
        if (name.isEmpty()) name = "default_wallet";
        
        // Remove characters that are illegal in filenames
        name.remove(QRegExp("[\\\\/:*?\"<>|]"));
        
        const bool sqlite = wallet->databaseFormat() == "sqlite";
        QString filename = dir + "/" + name + (sqlite ? "_backup.sqlite" : "_backup.dat");
        if (wallet->backupWallet(filename.toStdString())) {
            success_count++;
        } else {
            fail_count++;
            errors += name + " ";
        }
    }

    if (fail_count == 0) {
        QMessageBox::information(this, tr("Backup Successful"), tr("All %1 wallets backed up successfully to %2").arg(success_count).arg(dir));
    } else {
        QMessageBox::critical(this, tr("Backup Partially Failed"), tr("Successfully backed up %1 wallets, but failed for %2: %3").arg(success_count).arg(fail_count).arg(errors));
    }
}
