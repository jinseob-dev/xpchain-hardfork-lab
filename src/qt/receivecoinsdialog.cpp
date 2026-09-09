// Copyright (c) 2011-2018 The XPChain Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <wallet/wallet.h>
#include <outputtype.h>

#include <qt/receivecoinsdialog.h>
#include <qt/forms/ui_receivecoinsdialog.h>

#include <qt/addressbookpage.h>
#include <qt/addresstablemodel.h>
#include <qt/xpchainunits.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/receiverequestdialog.h>
#include <qt/recentrequeststablemodel.h>
#include <qt/walletmodel.h>

#include <QAction>
#include <QComboBox>
#include <QCursor>
#include <QMessageBox>
#include <QScrollBar>
#include <QTextDocument>

ReceiveCoinsDialog::ReceiveCoinsDialog(const PlatformStyle *_platformStyle, QWidget *parent) :
    QDialog(parent),
    ui(new Ui::ReceiveCoinsDialog),
    columnResizingFixer(0),
    model(0),
    platformStyle(_platformStyle)
{
    ui->setupUi(this);

    if (!_platformStyle->getImagesOnButtons()) {
        ui->clearButton->setIcon(QIcon());
        ui->receiveButton->setIcon(QIcon());
        ui->showRequestButton->setIcon(QIcon());
        ui->removeRequestButton->setIcon(QIcon());
    } else {
        ui->clearButton->setIcon(_platformStyle->SingleColorIcon(":/icons/remove"));
        ui->receiveButton->setIcon(_platformStyle->SingleColorIcon(":/icons/receiving_addresses"));
        ui->showRequestButton->setIcon(_platformStyle->SingleColorIcon(":/icons/edit"));
        ui->removeRequestButton->setIcon(_platformStyle->SingleColorIcon(":/icons/remove"));
    }

    // context menu actions
    QAction *copyURIAction = new QAction(tr("Copy URI"), this);
    QAction *copyLabelAction = new QAction(tr("Copy label"), this);
    QAction *copyMessageAction = new QAction(tr("Copy message"), this);
    QAction *copyAmountAction = new QAction(tr("Copy amount"), this);

    // context menu
    contextMenu = new QMenu(this);
    contextMenu->addAction(copyURIAction);
    contextMenu->addAction(copyLabelAction);
    contextMenu->addAction(copyMessageAction);
    contextMenu->addAction(copyAmountAction);

    // context menu signals
    connect(ui->recentRequestsView, SIGNAL(customContextMenuRequested(QPoint)), this, SLOT(showMenu(QPoint)));
    connect(copyURIAction, SIGNAL(triggered()), this, SLOT(copyURI()));
    connect(copyLabelAction, SIGNAL(triggered()), this, SLOT(copyLabel()));
    connect(copyMessageAction, SIGNAL(triggered()), this, SLOT(copyMessage()));
    connect(copyAmountAction, SIGNAL(triggered()), this, SLOT(copyAmount()));

    // Address Type ComboBox will be populated in setModel()

    connect(ui->clearButton, SIGNAL(clicked()), this, SLOT(clear()));
}

void ReceiveCoinsDialog::setModel(WalletModel *_model)
{
    this->model = _model;

    if(_model && _model->getOptionsModel())
    {
        _model->getRecentRequestsTableModel()->sort(RecentRequestsTableModel::Date, Qt::DescendingOrder);
        connect(_model->getOptionsModel(), SIGNAL(displayUnitChanged(int)), this, SLOT(updateDisplayUnit()));
        updateDisplayUnit();

        QTableView* tableView = ui->recentRequestsView;

        tableView->verticalHeader()->hide();
        tableView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        tableView->setModel(_model->getRecentRequestsTableModel());
        tableView->setAlternatingRowColors(true);
        tableView->setSelectionBehavior(QAbstractItemView::SelectRows);
        tableView->setSelectionMode(QAbstractItemView::ContiguousSelection);
        tableView->setColumnWidth(RecentRequestsTableModel::Date, DATE_COLUMN_WIDTH);
        tableView->setColumnWidth(RecentRequestsTableModel::Label, LABEL_COLUMN_WIDTH);
        tableView->setColumnWidth(RecentRequestsTableModel::Address, 250);
        tableView->setColumnWidth(RecentRequestsTableModel::Amount, AMOUNT_MINIMUM_COLUMN_WIDTH);

        connect(tableView->selectionModel(),
            SIGNAL(selectionChanged(QItemSelection, QItemSelection)), this,
            SLOT(recentRequestsView_selectionChanged(QItemSelection, QItemSelection)));
        // Last columns are set by the columnResizingFixer, when the table geometry is ready.
        columnResizingFixer = new GUIUtil::TableViewLastColumnResizingFixer(tableView, AMOUNT_MINIMUM_COLUMN_WIDTH, DATE_COLUMN_WIDTH, this);

        // Dynamically populate Address Type ComboBox
        int currentIndex = ui->addressType->currentIndex();
        OutputType currentType = currentIndex != -1 ? (OutputType)ui->addressType->currentData().toInt() : model->wallet().getDefaultAddressType();
        
        ui->addressType->clear();
        ui->addressType->addItem(tr("Base58 (Legacy)"), (int)OutputType::LEGACY);
        ui->addressType->addItem(tr("Base58 (P2SH-SegWit)"), (int)OutputType::P2SH_SEGWIT);
        ui->addressType->addItem(tr("Bech32 (SegWit)"), (int)OutputType::BECH32);
        
        // Taproot outputs are spendable by anyone until the activation height, so
        // the option only appears once consensus enforces the Taproot rules.
        // Taproot requires descriptor wallets; isLegacy() only means Berkeley DB.
        if (model->wallet().isDescriptor() && TaprootOutputsProtected()) {
            ui->addressType->addItem(tr("Bech32m (Taproot)"), (int)OutputType::BECH32M);
        }

        // Restore previous selection if valid, otherwise fallback
        int newIndex = ui->addressType->findData((int)currentType);
        if (newIndex != -1) {
            ui->addressType->setCurrentIndex(newIndex);
        } else if (currentType == OutputType::BECH32M) {
            ui->addressType->setCurrentIndex(ui->addressType->findData((int)OutputType::BECH32)); // Fallback
        } else {
            ui->addressType->setCurrentIndex(0);
        }

        // eventually disable the main receive button if private key operations are disabled
        ui->receiveButton->setEnabled(!model->privateKeysDisabled());
    }
}

ReceiveCoinsDialog::~ReceiveCoinsDialog()
{
    delete ui;
}

void ReceiveCoinsDialog::clear()
{
    ui->reqAmount->clear();
    ui->reqLabel->setText("");
    ui->reqMessage->setText("");
    // Default to wallet's default address type
    if (model) {
        OutputType default_type = ProtectedOutputType(model->wallet().getDefaultAddressType());
        if (!model->wallet().isDescriptor() && default_type == OutputType::BECH32M) {
            default_type = OutputType::BECH32; // Fallback for non-descriptor wallets
        }

        int index = ui->addressType->findData((int)default_type);
        ui->addressType->setCurrentIndex(index != -1 ? index : 0);
    }
    updateDisplayUnit();
}

void ReceiveCoinsDialog::reject()
{
    clear();
}

void ReceiveCoinsDialog::accept()
{
    clear();
}

void ReceiveCoinsDialog::updateDisplayUnit()
{
    if(model && model->getOptionsModel())
    {
        ui->reqAmount->setDisplayUnit(model->getOptionsModel()->getDisplayUnit());
    }
}


void ReceiveCoinsDialog::on_quickCopyButton_clicked()
{
    if (!model || !model->getOptionsModel() || !model->getAddressTableModel())
        return;

    OutputType address_type = (OutputType)ui->addressType->currentData().toInt();
    if (address_type == OutputType::BECH32M && !model->wallet().isDescriptor()) {
        QMessageBox::warning(this, tr("Address generation failure"),
            tr("Taproot (Bech32m) addresses require a descriptor wallet. "
               "Create a new descriptor wallet to use Taproot features."));
        return;
    }
    if (address_type == OutputType::BECH32M && !TaprootOutputsProtected()) {
        QMessageBox::warning(this, tr("Address generation failure"),
            tr("Taproot (Bech32m) addresses are not available until Taproot activates at block %1.")
                .arg(Params().GetConsensus().TaprootHeight));
        return;
    }

    const QString address = model->getAddressTableModel()->addRow(AddressTableModel::Receive, "", "", address_type);
    if (address.isEmpty()) {
        QMessageBox::warning(this, tr("Address generation failure"),
            tr("Could not generate a new receiving address."));
        return;
    }
    ui->lineGeneratedAddress->setText(address);
    GUIUtil::setClipboard(address);

    if (model->getRecentRequestsTableModel()) {
        SendCoinsRecipient info(address, "", 0, "");
        model->getRecentRequestsTableModel()->addNewRequest(info);
    }

    QMessageBox::information(this, tr("Address generated & copied"),
        tr("New receiving address generated and copied to clipboard:\n%1").arg(address));
}

void ReceiveCoinsDialog::on_btnCopyGenerated_clicked()
{
    QString addr = ui->lineGeneratedAddress->text();
    if (!addr.isEmpty()) {
        GUIUtil::setClipboard(addr);
        QMessageBox::information(this, tr("Address copied"),
            tr("Address copied to clipboard:\n%1").arg(addr));
    }
}

void ReceiveCoinsDialog::on_receiveButton_clicked()
{
    if(!model || !model->getOptionsModel() || !model->getAddressTableModel() || !model->getRecentRequestsTableModel())
        return;

    QString address;
    QString label = ui->reqLabel->text();
    /* Generate new receiving address */
    OutputType address_type = (OutputType)ui->addressType->currentData().toInt();
    if (address_type == OutputType::BECH32M && !model->wallet().isDescriptor()) {
        QMessageBox::warning(this, tr("Address generation failure"),
            tr("Taproot (Bech32m) addresses require a descriptor wallet. "
               "Create a new descriptor wallet to use Taproot features."));
        return;
    }
    if (address_type == OutputType::BECH32M && !TaprootOutputsProtected()) {
        QMessageBox::warning(this, tr("Address generation failure"),
            tr("Taproot (Bech32m) addresses are not available until Taproot activates at block %1. "
               "Coins sent to a Bech32m address before then are spendable by anyone.")
                .arg(Params().GetConsensus().TaprootHeight));
        return;
    }
    address = model->getAddressTableModel()->addRow(AddressTableModel::Receive, label, "", address_type);
    SendCoinsRecipient info(address, label,
        ui->reqAmount->value(), ui->reqMessage->text());
    ReceiveRequestDialog *dialog = new ReceiveRequestDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setModel(model);
    dialog->setInfo(info);
    dialog->show();
    clear();

    /* Store request for later reference */
    model->getRecentRequestsTableModel()->addNewRequest(info);
}

void ReceiveCoinsDialog::on_recentRequestsView_doubleClicked(const QModelIndex &index)
{
    const RecentRequestsTableModel *submodel = model->getRecentRequestsTableModel();
    ReceiveRequestDialog *dialog = new ReceiveRequestDialog(this);
    dialog->setModel(model);
    dialog->setInfo(submodel->entry(index.row()).recipient);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void ReceiveCoinsDialog::recentRequestsView_selectionChanged(const QItemSelection &selected, const QItemSelection &deselected)
{
    // Enable Show/Remove buttons only if anything is selected.
    bool enable = !ui->recentRequestsView->selectionModel()->selectedRows().isEmpty();
    ui->showRequestButton->setEnabled(enable);
    ui->removeRequestButton->setEnabled(enable);
}

void ReceiveCoinsDialog::on_showRequestButton_clicked()
{
    if(!model || !model->getRecentRequestsTableModel() || !ui->recentRequestsView->selectionModel())
        return;
    QModelIndexList selection = ui->recentRequestsView->selectionModel()->selectedRows();

    for (const QModelIndex& index : selection) {
        on_recentRequestsView_doubleClicked(index);
    }
}

void ReceiveCoinsDialog::on_removeRequestButton_clicked()
{
    if(!model || !model->getRecentRequestsTableModel() || !ui->recentRequestsView->selectionModel())
        return;
    QModelIndexList selection = ui->recentRequestsView->selectionModel()->selectedRows();
    if(selection.empty())
        return;
    // correct for selection mode ContiguousSelection
    QModelIndex firstIndex = selection.at(0);
    model->getRecentRequestsTableModel()->removeRows(firstIndex.row(), selection.length(), firstIndex.parent());
}

// We override the virtual resizeEvent of the QWidget to adjust tables column
// sizes as the tables width is proportional to the dialogs width.
void ReceiveCoinsDialog::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    columnResizingFixer->stretchColumnWidth(RecentRequestsTableModel::Message);
}

void ReceiveCoinsDialog::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Return)
    {
        // press return -> submit form
        if (ui->reqLabel->hasFocus() || ui->reqAmount->hasFocus() || ui->reqMessage->hasFocus())
        {
            event->ignore();
            on_receiveButton_clicked();
            return;
        }
    }

    this->QDialog::keyPressEvent(event);
}

QModelIndex ReceiveCoinsDialog::selectedRow()
{
    if(!model || !model->getRecentRequestsTableModel() || !ui->recentRequestsView->selectionModel())
        return QModelIndex();
    QModelIndexList selection = ui->recentRequestsView->selectionModel()->selectedRows();
    if(selection.empty())
        return QModelIndex();
    // correct for selection mode ContiguousSelection
    QModelIndex firstIndex = selection.at(0);
    return firstIndex;
}

// copy column of selected row to clipboard
void ReceiveCoinsDialog::copyColumnToClipboard(int column)
{
    QModelIndex firstIndex = selectedRow();
    if (!firstIndex.isValid()) {
        return;
    }
    GUIUtil::setClipboard(model->getRecentRequestsTableModel()->data(firstIndex.child(firstIndex.row(), column), Qt::EditRole).toString());
}

// context menu
void ReceiveCoinsDialog::showMenu(const QPoint &point)
{
    if (!selectedRow().isValid()) {
        return;
    }
    contextMenu->exec(QCursor::pos());
}

// context menu action: copy URI
void ReceiveCoinsDialog::copyURI()
{
    QModelIndex sel = selectedRow();
    if (!sel.isValid()) {
        return;
    }

    const RecentRequestsTableModel * const submodel = model->getRecentRequestsTableModel();
    const QString uri = GUIUtil::formatXPChainURI(submodel->entry(sel.row()).recipient);
    GUIUtil::setClipboard(uri);
}

// context menu action: copy label
void ReceiveCoinsDialog::copyLabel()
{
    copyColumnToClipboard(RecentRequestsTableModel::Label);
}

// context menu action: copy message
void ReceiveCoinsDialog::copyMessage()
{
    copyColumnToClipboard(RecentRequestsTableModel::Message);
}

// context menu action: copy amount
void ReceiveCoinsDialog::copyAmount()
{
    copyColumnToClipboard(RecentRequestsTableModel::Amount);
}
