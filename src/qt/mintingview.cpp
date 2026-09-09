#include <qt/mintingview.h>
#include <qt/coldstakingdialog.h>

#include <qt/xpchainunits.h>
#include <qt/guiconstants.h>
#include <qt/csvmodelwriter.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/mintingfilterproxy.h>
#include <qt/transactionrecord.h>
#include <qt/mintingtablemodel.h>
#include <qt/walletmodel.h>
#include <interfaces/wallet.h>

#include <ui_interface.h>

#include <QComboBox>
#include <QEvent>
#include <QFrame>
#include <QPushButton>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPoint>
#include <QScrollBar>
#include <QTableView>
#include <QVBoxLayout>

MintingView::MintingView(const PlatformStyle *_platformStyle, QWidget *parent) :
    QWidget(parent), model(0), mintingView(0), mintingProxyModel(0),
    guidanceFrame(0), guidanceLabel(0), guidanceButton(0),
    youngColorSwatch(0), matureColorSwatch(0), oldColorSwatch(0),
    platformStyle(_platformStyle)
{
    QHBoxLayout *hlayout = new QHBoxLayout();
    hlayout->setContentsMargins(0,0,0,0);

    youngColorSwatch = new QLabel(" ");
    youngColorSwatch->setMaximumHeight(15);
    youngColorSwatch->setMaximumWidth(10);
    QLabel *youngLegend = new QLabel(tr("transaction is too young"));
    youngLegend->setContentsMargins(5,0,15,0);

    matureColorSwatch = new QLabel(" ");
    matureColorSwatch->setMaximumHeight(15);
    matureColorSwatch->setMaximumWidth(10);
    QLabel *matureLegend = new QLabel(tr("transaction is mature"));
    matureLegend->setContentsMargins(5,0,15,0);

    oldColorSwatch = new QLabel(" ");
    oldColorSwatch->setMaximumHeight(15);
    oldColorSwatch->setMaximumWidth(10);
    QLabel *oldLegend = new QLabel(tr("transaction has reached maximum probability"));
    oldLegend->setContentsMargins(5,0,15,0);

    updateThemeColors();

    QHBoxLayout *legendLayout = new QHBoxLayout();
    legendLayout->setContentsMargins(10,10,0,0);
    legendLayout->addWidget(youngColorSwatch);
    legendLayout->addWidget(youngLegend);
    legendLayout->addWidget(matureColorSwatch);
    legendLayout->addWidget(matureLegend);
    legendLayout->addWidget(oldColorSwatch);
    legendLayout->addWidget(oldLegend);
    legendLayout->insertStretch(-1);

    QPushButton *coldStakingBtn = new QPushButton(tr("Cold Staking / Delegate..."), this);
    coldStakingBtn->setToolTip(tr("Generate Cold Staking address or delegate staking coins to a node"));
    connect(coldStakingBtn, &QPushButton::clicked, this, &MintingView::onColdStakingClicked);

    QLabel *mintingLabel = new QLabel(tr("Display staking probability within : "));
    mintingCombo = new QComboBox();
    mintingCombo->addItem(tr("10 min"), Minting10min);
    mintingCombo->addItem(tr("24 hours"), Minting1day);
    mintingCombo->addItem(tr("7 days"), Minting7days);
    mintingCombo->addItem(tr("30 days"), Minting30days);
    mintingCombo->addItem(tr("60 days"), Minting60days);
    mintingCombo->setFixedWidth(120);

    hlayout->addWidget(coldStakingBtn);
    hlayout->insertStretch(1);
    hlayout->addWidget(mintingLabel);
    hlayout->addWidget(mintingCombo);

    QVBoxLayout *vlayout = new QVBoxLayout(this);
    vlayout->setContentsMargins(0,0,0,0);
    vlayout->setSpacing(0);

    guidanceFrame = new QFrame(this);
    guidanceFrame->setObjectName("stakingGuidanceFrame");
    guidanceFrame->setStyleSheet(
        "#stakingGuidanceFrame { background-color: rgba(30, 120, 200, 40); border: 1px solid #1f6feb; border-radius: 4px; }"
        "#stakingGuidanceFrame QLabel { padding: 6px; }"
        "#stakingGuidanceFrame QPushButton { margin: 6px; }");
    QHBoxLayout *guidanceLayout = new QHBoxLayout(guidanceFrame);
    guidanceLabel = new QLabel(guidanceFrame);
    guidanceLabel->setWordWrap(true);
    guidanceButton = new QPushButton(guidanceFrame);
    guidanceButton->setVisible(false);
    connect(guidanceButton, SIGNAL(clicked()), this, SLOT(guidanceButtonClicked()));
    guidanceLayout->addWidget(guidanceLabel, 1);
    guidanceLayout->addWidget(guidanceButton, 0, Qt::AlignTop);
    guidanceFrame->setVisible(false);
    vlayout->addWidget(guidanceFrame);

    QTableView *view = new QTableView(this);
    vlayout->addLayout(hlayout);
    vlayout->addWidget(view);
    vlayout->addLayout(legendLayout);

    vlayout->setSpacing(0);
    int width = view->verticalScrollBar()->sizeHint().width();
    // Cover scroll bar width with spacing
#ifdef Q_WS_MAC
    hlayout->addSpacing(width+2);
#else
    hlayout->addSpacing(width);
#endif
    // Always show scroll bar
    view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    view->setTabKeyNavigation(false);
    view->setContextMenuPolicy(Qt::CustomContextMenu);
    view->setAttribute(Qt::WA_MacShowFocusRect, false);
    view->setFocusPolicy(Qt::ClickFocus);

    mintingView = view;
    mintingView->setObjectName("mintingView");

    QAction *copyAddressAction = new QAction(tr("Copy address"), this);
    QAction *copyTransactionIdAction = new QAction(tr("Copy transaction id"), this);
    QAction *showHideAddressAction = new QAction(tr("Show/hide 'Address' column"), this);
    QAction *showHideTxIDAction = new QAction(tr("Show/hide 'Transaction' column"), this);

    contextMenu =new QMenu();
    contextMenu->addAction(copyAddressAction);
    contextMenu->addAction(copyTransactionIdAction);
    contextMenu->addAction(showHideAddressAction);
    contextMenu->addAction(showHideTxIDAction);

    connect(mintingCombo, SIGNAL(activated(int)), this, SLOT(chooseMintingInterval(int)));
    connect(view, SIGNAL(customContextMenuRequested(QPoint)), this, SLOT(contextualMenu(QPoint)));
    connect(copyAddressAction, SIGNAL(triggered()), this, SLOT(copyAddress()));
    connect(copyTransactionIdAction, SIGNAL(triggered()), this, SLOT(copyTransactionId()));
    connect(showHideAddressAction, SIGNAL(triggered()), this, SLOT(showHideAddress()));
    connect(showHideTxIDAction, SIGNAL(triggered()), this, SLOT(showHideTxID()));
}


void MintingView::setModel(WalletModel *_model)
{
    this->model = _model;

    if (mintingView) mintingView->setModel(nullptr);
    if (mintingProxyModel) {
        delete mintingProxyModel;
        mintingProxyModel = nullptr;
    }

    if(_model)
    {

        mintingProxyModel = new MintingFilterProxy(this);
        mintingProxyModel->setSourceModel(_model->getMintingTableModel());
        mintingProxyModel->setDynamicSortFilter(true);
        mintingProxyModel->setSortRole(Qt::EditRole);
        model->getMintingTableModel()->setMintingProxyModel(mintingProxyModel);

        mintingView->setModel(mintingProxyModel);
        mintingView->setAlternatingRowColors(true);
        mintingView->setSelectionBehavior(QAbstractItemView::SelectRows);
        mintingView->setSelectionMode(QAbstractItemView::ExtendedSelection);
        mintingView->setSortingEnabled(true);
        mintingView->sortByColumn(MintingTableModel::CoinDay, Qt::DescendingOrder);
        mintingView->verticalHeader()->hide();

        mintingView->horizontalHeader()->resizeSection(
                MintingTableModel::Address, 300);
#if QT_VERSION < 0x050000
        mintingView->horizontalHeader()->setResizeMode(
                MintingTableModel::TxHash, QHeaderView::Stretch);
#else
        mintingView->horizontalHeader()->setSectionResizeMode(
                MintingTableModel::TxHash, QHeaderView::Stretch);
        mintingView->horizontalHeader()->setSectionResizeMode(
                MintingTableModel::MintReward, QHeaderView::Stretch);
#endif

        mintingView->horizontalHeader()->resizeSection(
                MintingTableModel::Age, 60);
        mintingView->horizontalHeader()->resizeSection(
                MintingTableModel::Balance, 100);
        mintingView->horizontalHeader()->resizeSection(
                MintingTableModel::CoinDay,100);
        mintingView->horizontalHeader()->resizeSection(
                MintingTableModel::MintProbability, 120);

        connect(_model, &WalletModel::encryptionStatusChanged, this, &MintingView::updateGuidanceBanner);
        connect(_model, &WalletModel::balanceChanged, this, [this](const interfaces::WalletBalances&) {
            updateGuidanceBanner();
        });
    }
    updateGuidanceBanner();
}

void MintingView::updateGuidanceBanner()
{
    if (!guidanceFrame || !guidanceLabel || !guidanceButton) {
        return;
    }
    if (!model) {
        guidanceFrame->setVisible(false);
        return;
    }

    if (model->getEncryptionStatus() == WalletModel::Locked) {
        guidanceLabel->setText(tr("This wallet is locked. Unlock for staking to include your coins in Proof-of-Stake."));
        guidanceButton->setText(tr("Unlock for Staking…"));
        guidanceButton->setVisible(true);
        guidanceFrame->setVisible(true);
        return;
    }

    const interfaces::WalletBalances balances = model->wallet().getBalances();
    if (balances.balance <= 0) {
        guidanceLabel->setText(tr("No spendable balance yet. Receive coins, wait for confirmations, then return here to check staking odds."));
        guidanceButton->setVisible(false);
        guidanceFrame->setVisible(true);
        return;
    }

    guidanceFrame->setVisible(false);
}

void MintingView::guidanceButtonClicked()
{
    Q_EMIT unlockForStakingRequested();
}

void MintingView::chooseMintingInterval(int idx)
{
    int interval = 10;
    switch(mintingCombo->itemData(idx).toInt())
    {
        case Minting10min:
            interval = 10;
            break;
        case Minting1day:
            interval = 60*24;
            break;
        case Minting7days:
            interval = 60*24*7;
            break;
        case Minting30days:
            interval = 60*24*30;
            break;
        case Minting60days:
            interval = 60*24*60;
            break;
    }
    if (!model || !mintingProxyModel)
        return;

    model->getMintingTableModel()->setMintingInterval(interval);
    mintingProxyModel->invalidate();
}

void MintingView::exportClicked()
{
    // CSV is currently the only supported format
    QString filename = GUIUtil::getSaveFileName(
            this,
            tr("Export Staking Data"), QString(),
            tr("Comma separated file (*.csv)"),nullptr);

    if (filename.isNull()) return;

    CSVModelWriter writer(filename);

    // name, column, role
    writer.setModel(mintingProxyModel);
    writer.addColumn(tr("Address"), MintingTableModel::Address);
    writer.addColumn(tr("Transaction"), MintingTableModel::TxHash);
    writer.addColumn(tr("Age"), MintingTableModel::Age);
    writer.addColumn(tr("CoinDay"), MintingTableModel::CoinDay);
    writer.addColumn(tr("Balance"), MintingTableModel::Balance);
    writer.addColumn(tr("StakeProbability"), MintingTableModel::MintProbability);
    writer.addColumn(tr("StakeReward"), MintingTableModel::MintReward,0);

    if(!writer.write())
    {
        QMessageBox::critical(this, tr("Error exporting"), tr("Could not write to file %1.").arg(filename),
                              QMessageBox::Abort, QMessageBox::Abort);
    }
}

void MintingView::contextualMenu(const QPoint &point)
{
    QModelIndex index = mintingView->indexAt(point);
    if(index.isValid())
    {
        contextMenu->exec(QCursor::pos());
    }
}

void MintingView::copyAddress()
{
    GUIUtil::copyEntryData(mintingView, MintingTableModel::Address, Qt::DisplayRole);
}

void MintingView::copyTransactionId()
{
    GUIUtil::copyEntryData(mintingView, MintingTableModel::TxHash, Qt::DisplayRole);
}
void MintingView::showHideAddress()
{
    mintingView->horizontalHeader()->setSectionHidden(MintingTableModel::Address,
        !(mintingView->horizontalHeader()->isSectionHidden(MintingTableModel::Address)));
}

void MintingView::showHideTxID()
{
    mintingView->horizontalHeader()->setSectionHidden(MintingTableModel::TxHash,
        !(mintingView->horizontalHeader()->isSectionHidden(MintingTableModel::TxHash)));
}

void MintingView::updateThemeColors()
{
    const bool light = GUIUtil::isLightTheme();
    const QColor young = light ? COLOR_MINT_YOUNG_LIGHT : COLOR_MINT_YOUNG;
    const QColor mature = light ? COLOR_MINT_MATURE_LIGHT : COLOR_MINT_MATURE;
    const QColor old = light ? COLOR_MINT_OLD_LIGHT : COLOR_MINT_OLD;
    const QString box = QStringLiteral("background-color: rgb(%1,%2,%3); border: 1px solid %4;");
    const QString border = light ? QStringLiteral("#8b949e") : QStringLiteral("#000000");
    if (youngColorSwatch) {
        youngColorSwatch->setStyleSheet(box.arg(young.red()).arg(young.green()).arg(young.blue()).arg(border));
    }
    if (matureColorSwatch) {
        matureColorSwatch->setStyleSheet(box.arg(mature.red()).arg(mature.green()).arg(mature.blue()).arg(border));
    }
    if (oldColorSwatch) {
        oldColorSwatch->setStyleSheet(box.arg(old.red()).arg(old.green()).arg(old.blue()).arg(border));
    }
    if (mintingView) {
        mintingView->viewport()->update();
    }
}

void MintingView::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange) {
        updateThemeColors();
    }
    QWidget::changeEvent(event);
}

void MintingView::onColdStakingClicked()
{
    if (!model) return;
    ColdStakingDialog dlg(platformStyle, model, this);
    dlg.exec();
}
