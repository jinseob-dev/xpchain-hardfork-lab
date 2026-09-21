// Copyright (c) 2026 The XPChain Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/coldstakingdialog.h>

#include <qt/addresstablemodel.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/walletmodel.h>
#include <qt/xpchainunits.h>

#include <wallet/coincontrol.h>
#include <chainparams.h>
#include <outputtype.h>
#include <key_io.h>
#include <pos/stake.h>

#include <QApplication>
#include <QAbstractItemView>
#include <QClipboard>
#include <QComboBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <map>

ColdStakingDialog::ColdStakingDialog(const PlatformStyle *_platformStyle, WalletModel *_model, QWidget *parent)
    : QDialog(parent),
      model(_model),
      platformStyle(_platformStyle),
      editOwnerAddress(nullptr),
      editStakerAddress(nullptr),
      editResultAddress(nullptr),
      btnGenerateAddress(nullptr),
      btnCopyAddress(nullptr),
      btnUseInDelegate(nullptr),
      labelStatus(nullptr),
      editDelegateAddress(nullptr),
      editDelegateAmount(nullptr),
      btnSendDelegation(nullptr),
      labelDelegateStatus(nullptr),
      tableContracts(nullptr),
      comboWithdrawContract(nullptr),
      editWithdrawDestination(nullptr),
      editWithdrawAmount(nullptr),
      btnRefreshContracts(nullptr),
      btnWithdrawAll(nullptr),
      btnWithdraw(nullptr),
      btnConsolidate(nullptr),
      labelContractSummary(nullptr),
      labelWithdrawStatus(nullptr)
{
    setupUI();
}

ColdStakingDialog::~ColdStakingDialog()
{
}

void ColdStakingDialog::setupUI()
{
    setWindowTitle(tr("Cold Staking & Delegation"));
    setMinimumWidth(760);
    resize(900, 650);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    QTabWidget *tabWidget = new QTabWidget(this);

    // ==========================================
    // Tab 1: Create Cold Staking Address
    // ==========================================
    QWidget *tabCreate = new QWidget();
    QVBoxLayout *createLayout = new QVBoxLayout(tabCreate);

    QLabel *descCreate = new QLabel(
        tr("Create a secure <b>Cold Staking P2WSH script</b> address.<br>"
           "Coins sent to this address can only be spent by the <b>Owner Key</b>, "
           "while Proof-of-Stake minting rights are granted to the <b>Staking Node Key</b>."), tabCreate);
    descCreate->setWordWrap(true);
    createLayout->addWidget(descCreate);

    QFormLayout *formCreate = new QFormLayout();
    editOwnerAddress = new QLineEdit(tabCreate);
    editOwnerAddress->setPlaceholderText(tr("Your spending address / Key ID"));
    formCreate->addRow(tr("&Owner Address:"), editOwnerAddress);

    editStakerAddress = new QLineEdit(tabCreate);
    editStakerAddress->setPlaceholderText(tr("Staking node address / Key ID"));
    formCreate->addRow(tr("&Staker Node Address:"), editStakerAddress);

    editResultAddress = new QLineEdit(tabCreate);
    editResultAddress->setReadOnly(true);
    editResultAddress->setPlaceholderText(tr("Generated P2WSH Cold Staking Address"));
    formCreate->addRow(tr("<b>Delegation Address:</b>"), editResultAddress);

    createLayout->addLayout(formCreate);

    QHBoxLayout *btnLayout1 = new QHBoxLayout();
    btnGenerateAddress = new QPushButton(tr("Generate Address"), tabCreate);
    btnCopyAddress = new QPushButton(tr("Copy Address"), tabCreate);
    btnUseInDelegate = new QPushButton(tr("Use in Send Tab ->"), tabCreate);
    btnCopyAddress->setEnabled(false);
    btnUseInDelegate->setEnabled(false);

    btnLayout1->addWidget(btnGenerateAddress);
    btnLayout1->addWidget(btnCopyAddress);
    btnLayout1->addWidget(btnUseInDelegate);
    createLayout->addLayout(btnLayout1);

    labelStatus = new QLabel(tabCreate);
    labelStatus->setStyleSheet("color: #6cb6ff;");
    createLayout->addWidget(labelStatus);
    createLayout->addStretch();

    tabWidget->addTab(tabCreate, tr("1. Create Address"));

    // ==========================================
    // Tab 2: Delegate Coins (Send)
    // ==========================================
    QWidget *tabDelegate = new QWidget();
    QVBoxLayout *delegateLayout = new QVBoxLayout(tabDelegate);

    QLabel *descDelegate = new QLabel(
        tr("Send coins to a <b>Cold Staking Address</b> to activate Proof-of-Stake delegation.<br>"
           "Your funds remain under your owner key. The wallet automatically chooses an amount-dependent UTXO split "
           "using current network difficulty.<br><br>"
           "<b>Policy:</b> preferred staking age 32 days; chain-stall fallback to the 3-day consensus minimum. "
           "Node commission is disabled during the testnet phase."), tabDelegate);
    descDelegate->setWordWrap(true);
    delegateLayout->addWidget(descDelegate);

    QFormLayout *formDelegate = new QFormLayout();
    editDelegateAddress = new QLineEdit(tabDelegate);
    editDelegateAddress->setPlaceholderText(tr("Cold Staking Address (P2WSH)"));
    formDelegate->addRow(tr("&Delegation Address:"), editDelegateAddress);

    editDelegateAmount = new QLineEdit(tabDelegate);
    editDelegateAmount->setPlaceholderText(tr("Amount to delegate (e.g. 50000)"));
    formDelegate->addRow(tr("&Amount (XPC):"), editDelegateAmount);

    delegateLayout->addLayout(formDelegate);

    QHBoxLayout *btnLayout2 = new QHBoxLayout();
    btnSendDelegation = new QPushButton(tr("Send Delegation Transaction"), tabDelegate);
    btnLayout2->addWidget(btnSendDelegation);
    delegateLayout->addLayout(btnLayout2);

    labelDelegateStatus = new QLabel(tabDelegate);
    labelDelegateStatus->setWordWrap(true);
    delegateLayout->addWidget(labelDelegateStatus);
    delegateLayout->addStretch();

    tabWidget->addTab(tabDelegate, tr("2. Delegate Coins"));

    // ==========================================
    // Tab 3: Monitor / Withdraw
    // ==========================================
    QWidget *tabManage = new QWidget();
    QVBoxLayout *manageLayout = new QVBoxLayout(tabManage);

    QLabel *descManage = new QLabel(
        tr("Review the cold-staking contracts known to this wallet. "
           "Only a wallet marked <b>Owner</b> can withdraw principal or consolidate UTXOs; "
           "a staking-only wallet cannot spend them."),
        tabManage);
    descManage->setWordWrap(true);
    manageLayout->addWidget(descManage);

    tableContracts = new QTableWidget(0, 5, tabManage);
    tableContracts->setHorizontalHeaderLabels({tr("Contract Address"), tr("Role"),
                                                tr("Balance"), tr("UTXOs"), tr("Min Conf.")});
    tableContracts->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tableContracts->setSelectionBehavior(QAbstractItemView::SelectRows);
    tableContracts->setSelectionMode(QAbstractItemView::SingleSelection);
    tableContracts->verticalHeader()->setVisible(false);
    tableContracts->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 5; ++column) {
        tableContracts->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    }
    manageLayout->addWidget(tableContracts);

    QHBoxLayout *summaryLayout = new QHBoxLayout();
    labelContractSummary = new QLabel(tabManage);
    btnRefreshContracts = new QPushButton(tr("Refresh"), tabManage);
    summaryLayout->addWidget(labelContractSummary, 1);
    summaryLayout->addWidget(btnRefreshContracts);
    manageLayout->addLayout(summaryLayout);

    QFormLayout *formWithdraw = new QFormLayout();
    comboWithdrawContract = new QComboBox(tabManage);
    formWithdraw->addRow(tr("&Owner Contract:"), comboWithdrawContract);

    editWithdrawDestination = new QLineEdit(tabManage);
    editWithdrawDestination->setPlaceholderText(tr("Normal address controlled by the owner"));
    formWithdraw->addRow(tr("&Destination Address:"), editWithdrawDestination);

    editWithdrawAmount = new QLineEdit(tabManage);
    editWithdrawAmount->setPlaceholderText(
        tr("Amount to withdraw (use Max to deduct the fee from this amount)"));
    btnWithdrawAll = new QPushButton(tr("Max"), tabManage);
    QWidget *amountRow = new QWidget(tabManage);
    QHBoxLayout *amountLayout = new QHBoxLayout(amountRow);
    amountLayout->setContentsMargins(0, 0, 0, 0);
    amountLayout->addWidget(editWithdrawAmount, 1);
    amountLayout->addWidget(btnWithdrawAll);
    formWithdraw->addRow(tr("&Amount (XPC):"), amountRow);
    manageLayout->addLayout(formWithdraw);

    btnWithdraw = new QPushButton(tr("Withdraw Delegated Coins"), tabManage);
    btnConsolidate = new QPushButton(tr("Consolidate Small UTXOs"), tabManage);
    btnWithdraw->setEnabled(false);
    btnConsolidate->setEnabled(false);
    QHBoxLayout *manageButtons = new QHBoxLayout();
    manageButtons->addWidget(btnWithdraw);
    manageButtons->addWidget(btnConsolidate);
    manageLayout->addLayout(manageButtons);

    labelWithdrawStatus = new QLabel(tabManage);
    labelWithdrawStatus->setWordWrap(true);
    manageLayout->addWidget(labelWithdrawStatus);

    tabWidget->addTab(tabManage, tr("3. Manage / Withdraw"));

    mainLayout->addWidget(tabWidget);

    // Default: if model available, populate owner address with a receiving address
    if (model && model->getAddressTableModel()) {
        AddressTableModel *addrModel = model->getAddressTableModel();
        for (int row = 0; row < addrModel->rowCount(QModelIndex()); ++row) {
            QModelIndex idxType = addrModel->index(row, 0, QModelIndex());
            if (addrModel->data(idxType, AddressTableModel::TypeRole).toString() == AddressTableModel::Receive) {
                QModelIndex idxAddr = addrModel->index(row, AddressTableModel::Address, QModelIndex());
                const QString receiveAddress = addrModel->data(idxAddr, Qt::DisplayRole).toString();
                editOwnerAddress->setText(receiveAddress);
                editWithdrawDestination->setText(receiveAddress);
                break;
            }
        }
    }

    // Connect signals
    connect(btnGenerateAddress, &QPushButton::clicked, this, &ColdStakingDialog::onGenerateAddressClicked);
    connect(btnCopyAddress, &QPushButton::clicked, this, &ColdStakingDialog::onCopyAddressClicked);
    connect(btnUseInDelegate, &QPushButton::clicked, [this, tabWidget]() {
        editDelegateAddress->setText(editResultAddress->text());
        tabWidget->setCurrentIndex(1);
    });
    connect(btnSendDelegation, &QPushButton::clicked, this, &ColdStakingDialog::onDelegateClicked);
    connect(btnRefreshContracts, &QPushButton::clicked, this, &ColdStakingDialog::refreshColdStaking);
    connect(btnWithdrawAll, &QPushButton::clicked, [this]() {
        const int index = comboWithdrawContract->currentIndex();
        if (index < 0) return;
        const CAmount balance = comboWithdrawContract->itemData(index, Qt::UserRole + 1).toLongLong();
        editWithdrawAmount->setText(XPChainUnits::format(XPChainUnits::XPC, balance, false,
                                                         XPChainUnits::separatorNever));
        labelWithdrawStatus->setStyleSheet("color: #8b949e;");
        labelWithdrawStatus->setText(
            tr("Full withdrawal selected. The network fee will be deducted from the amount received."));
    });
    connect(btnWithdraw, &QPushButton::clicked, this, &ColdStakingDialog::onWithdrawClicked);
    connect(btnConsolidate, &QPushButton::clicked, this, &ColdStakingDialog::onConsolidateClicked);
    connect(comboWithdrawContract, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int index) {
        const int outputs = index < 0 ? 0 : comboWithdrawContract->itemData(index, Qt::UserRole + 2).toInt();
        btnConsolidate->setEnabled(
            outputs >= WalletModel::COLD_STAKING_UTXOS_TO_PRESERVE + 2);
    });
    connect(tabWidget, &QTabWidget::currentChanged, [this](int index) {
        if (index == 2) refreshColdStaking();
    });
    connect(tableContracts, &QTableWidget::itemSelectionChanged, [this]() {
        const int row = tableContracts->currentRow();
        if (row < 0) return;
        const QString address = tableContracts->item(row, 0)->data(Qt::UserRole).toString();
        const int index = comboWithdrawContract->findData(address);
        if (index >= 0) comboWithdrawContract->setCurrentIndex(index);
    });

    refreshColdStaking();
}

void ColdStakingDialog::onGenerateAddressClicked()
{
    if (!model || model->getNumBlocks() + 1 < Params().GetConsensus().ColdStakingHeight) {
        labelStatus->setStyleSheet("color: #f85149;");
        labelStatus->setText(tr("Cold staking activates at block %1.").arg(Params().GetConsensus().ColdStakingHeight));
        return;
    }
    QString ownerStr = editOwnerAddress->text().trimmed();
    QString stakerStr = editStakerAddress->text().trimmed();

    if (ownerStr.isEmpty() || stakerStr.isEmpty()) {
        labelStatus->setStyleSheet("color: #f85149;");
        labelStatus->setText(tr("Please enter both Owner Address and Staker Node Address."));
        return;
    }

    CTxDestination ownerDest = DecodeDestination(ownerStr.toStdString());
    CTxDestination stakerDest = DecodeDestination(stakerStr.toStdString());

    if (!IsValidDestination(ownerDest) || !IsValidDestination(stakerDest)) {
        labelStatus->setStyleSheet("color: #f85149;");
        labelStatus->setText(tr("Invalid Owner or Staker Address format."));
        return;
    }

    if (boost::get<WitnessV1Taproot>(&ownerDest)) {
        const QString warning = tr(
            "Taproot owner addresses (txpc1p...) are not supported by this cold-staking contract version. "
            "Create a SegWit v0 Bech32 address (txpc1q...) in the owner wallet and use that address instead.");
        labelStatus->setStyleSheet("color: #f85149;");
        labelStatus->setText(warning);
        QMessageBox::warning(this, tr("Unsupported Taproot Owner Address"), warning);
        editOwnerAddress->setFocus();
        editOwnerAddress->selectAll();
        return;
    }
    if (boost::get<WitnessV1Taproot>(&stakerDest)) {
        const QString warning = tr(
            "Taproot staker addresses (txpc1p...) are not supported by this cold-staking contract version. "
            "Create a SegWit v0 Bech32 address (txpc1q...) in the staking wallet and use that address instead.");
        labelStatus->setStyleSheet("color: #f85149;");
        labelStatus->setText(warning);
        QMessageBox::warning(this, tr("Unsupported Taproot Staker Address"), warning);
        editStakerAddress->setFocus();
        editStakerAddress->selectAll();
        return;
    }

    CKeyID ownerKeyId;
    if (const CKeyID *k = boost::get<CKeyID>(&ownerDest)) {
        ownerKeyId = *k;
    } else if (const WitnessV0KeyHash *w = boost::get<WitnessV0KeyHash>(&ownerDest)) {
        ownerKeyId = CKeyID(*w);
    } else {
        labelStatus->setStyleSheet("color: #f85149;");
        labelStatus->setText(tr("Owner address must be P2PKH or SegWit v0 P2WPKH (txpc1q...)."));
        return;
    }

    CKeyID stakerKeyId;
    if (const CKeyID *k = boost::get<CKeyID>(&stakerDest)) {
        stakerKeyId = *k;
    } else if (const WitnessV0KeyHash *w = boost::get<WitnessV0KeyHash>(&stakerDest)) {
        stakerKeyId = CKeyID(*w);
    } else {
        labelStatus->setStyleSheet("color: #f85149;");
        labelStatus->setText(tr("Staker node address must be P2PKH or SegWit v0 P2WPKH (txpc1q...)."));
        return;
    }

    CScript script = pos::CreateColdStakingScript(stakerKeyId, ownerKeyId);
    const CScript witnessProgram = GetScriptForDestination(WitnessV0ScriptHash(script));
    if (!model || !model->wallet().addScript(script) || !model->wallet().addScript(witnessProgram)) {
        labelStatus->setStyleSheet("color: #f85149;");
        labelStatus->setText(tr("Failed to save the cold staking contract in this wallet."));
        return;
    }
    std::string coldAddr = EncodeDestination(WitnessV0ScriptHash(script));

    QString qAddr = QString::fromStdString(coldAddr);
    editResultAddress->setText(qAddr);
    btnCopyAddress->setEnabled(true);
    btnUseInDelegate->setEnabled(true);

    labelStatus->setStyleSheet("color: #56d364;");
    labelStatus->setText(tr("Cold Staking Address generated successfully!"));

    // Add to address table model as Send label
    if (model && model->getAddressTableModel()) {
        model->getAddressTableModel()->addRow(AddressTableModel::Send, tr("Cold Staking Delegation"), qAddr, OutputType::BECH32);
    }
}

void ColdStakingDialog::onCopyAddressClicked()
{
    QString addr = editResultAddress->text();
    if (!addr.isEmpty()) {
        GUIUtil::setClipboard(addr);
        QMessageBox::information(this, tr("Address Copied"), tr("Delegation address copied to clipboard:\n%1").arg(addr));
    }
}

void ColdStakingDialog::onDelegateClicked()
{
    if (!model) return;

    if (model->getNumBlocks() + 1 < Params().GetConsensus().ColdStakingHeight) {
        labelDelegateStatus->setStyleSheet("color: #f85149;");
        labelDelegateStatus->setText(tr("Cold staking activates at block %1.").arg(Params().GetConsensus().ColdStakingHeight));
        return;
    }

    QString addr = editDelegateAddress->text().trimmed();
    QString amountStr = editDelegateAmount->text().trimmed();

    if (addr.isEmpty() || amountStr.isEmpty()) {
        labelDelegateStatus->setStyleSheet("color: #f85149;");
        labelDelegateStatus->setText(tr("Please enter delegation address and amount."));
        return;
    }

    CAmount amount = 0;
    if (!XPChainUnits::parse(XPChainUnits::XPC, amountStr, &amount) || amount <= 0) {
        labelDelegateStatus->setStyleSheet("color: #f85149;");
        labelDelegateStatus->setText(tr("Invalid amount."));
        return;
    }

    CTxDestination dest = DecodeDestination(addr.toStdString());
    if (!IsValidDestination(dest)) {
        labelDelegateStatus->setStyleSheet("color: #f85149;");
        labelDelegateStatus->setText(tr("Invalid delegation address."));
        return;
    }
    if (!model->wallet().isColdStakingDestination(dest)) {
        labelDelegateStatus->setStyleSheet("color: #f85149;");
        labelDelegateStatus->setText(tr("This wallet does not contain the redeem script for that cold staking address."));
        return;
    }

    WalletModel::UnlockContext ctx(model->requestUnlock());
    if (!ctx.isValid()) {
        labelDelegateStatus->setStyleSheet("color: #f85149;");
        labelDelegateStatus->setText(tr("Wallet unlock cancelled."));
        return;
    }

    QList<SendCoinsRecipient> recipients;
    SendCoinsRecipient rcp(addr, tr("Cold Staking Delegation"), amount, "");
    recipients.append(rcp);

    WalletModelTransaction tx(recipients);
    CCoinControl ctrl;
    int outputCount = 0;
    WalletModel::SendCoinsReturn prepareStatus = model->prepareColdStakingTransaction(tx, ctrl, outputCount);
    if (prepareStatus.status != WalletModel::OK) {
        labelDelegateStatus->setStyleSheet("color: #f85149;");
        labelDelegateStatus->setText(tr("Failed to prepare transaction (insufficient funds or fee error)."));
        return;
    }


    const QString fee = XPChainUnits::formatWithUnit(XPChainUnits::XPC, tx.getTransactionFee());
    const QMessageBox::StandardButton confirmation = QMessageBox::question(
        this, tr("Confirm Delegation"),
        tr("Delegate %1 XPC as %2 staking UTXO(s)?\n\nTransaction fee: %3\n"
           "Rewards return to the same cold-staking contract; operator commission is disabled.")
            .arg(amountStr).arg(outputCount).arg(fee),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (confirmation != QMessageBox::Yes) return;

    WalletModel::SendCoinsReturn sendStatus = model->sendCoins(tx);
    if (sendStatus.status == WalletModel::OK) {
        labelDelegateStatus->setStyleSheet("color: #56d364;");
        labelDelegateStatus->setText(tr("Delegation transaction broadcasted successfully!"));
        QMessageBox::information(this, tr("Delegation Sent"),
            tr("Successfully sent %1 XPC in %2 staking UTXO(s)!\nFunds are now delegating Proof-of-Stake.")
                .arg(amountStr).arg(outputCount));
        editDelegateAmount->clear();
    } else {
        labelDelegateStatus->setStyleSheet("color: #f85149;");
        labelDelegateStatus->setText(tr("Transaction broadcast failed."));
    }
}

void ColdStakingDialog::onUseGeneratedAddressClicked()
{
}

void ColdStakingDialog::refreshColdStaking()
{
    tableContracts->setRowCount(0);
    comboWithdrawContract->clear();
    btnWithdraw->setEnabled(false);
    btnConsolidate->setEnabled(false);

    if (!model) return;

    struct ContractSummary {
        CAmount balance = 0;
        int outputs = 0;
        int minConfirmations = 0;
        bool owner = false;
        bool staker = false;
    };
    std::map<std::string, ContractSummary> contracts;
    for (const auto& output : model->getColdStakingOutputs()) {
        const std::string address = EncodeDestination(output.address);
        ContractSummary& summary = contracts[address];
        summary.balance += output.amount;
        summary.outputs++;
        if (summary.outputs == 1 || output.confirmations < summary.minConfirmations) {
            summary.minConfirmations = output.confirmations;
        }
        summary.owner = summary.owner || output.owner;
        summary.staker = summary.staker || output.staker;
    }

    CAmount total = 0;
    CAmount ownerTotal = 0;
    CAmount stakerTotal = 0;
    for (const auto& entry : contracts) {
        const QString address = QString::fromStdString(entry.first);
        const ContractSummary& summary = entry.second;
        const int row = tableContracts->rowCount();
        tableContracts->insertRow(row);

        QString role = tr("Observed");
        if (summary.owner && summary.staker) role = tr("Owner + Staker");
        else if (summary.owner) role = tr("Owner");
        else if (summary.staker) role = tr("Staker");

        QTableWidgetItem *addressItem = new QTableWidgetItem(address);
        addressItem->setData(Qt::UserRole, address);
        tableContracts->setItem(row, 0, addressItem);
        tableContracts->setItem(row, 1, new QTableWidgetItem(role));
        tableContracts->setItem(row, 2, new QTableWidgetItem(
            XPChainUnits::format(XPChainUnits::XPC, summary.balance, false, XPChainUnits::separatorAlways)));
        tableContracts->setItem(row, 3, new QTableWidgetItem(QString::number(summary.outputs)));
        tableContracts->setItem(row, 4, new QTableWidgetItem(QString::number(summary.minConfirmations)));

        total += summary.balance;
        if (summary.owner) {
            ownerTotal += summary.balance;
            comboWithdrawContract->addItem(
                tr("%1 — %2 XPC").arg(address, XPChainUnits::format(XPChainUnits::XPC, summary.balance)),
                address);
            comboWithdrawContract->setItemData(comboWithdrawContract->count() - 1,
                                               QVariant::fromValue<qlonglong>(summary.balance),
                                               Qt::UserRole + 1);
            comboWithdrawContract->setItemData(comboWithdrawContract->count() - 1,
                                               summary.outputs, Qt::UserRole + 2);
        }
        if (summary.staker) stakerTotal += summary.balance;
    }

    const int compoundHeight = Params().GetConsensus().ColdStakingCompoundHeight;
    const QString compoundStatus = model->getNumBlocks() + 1 >= compoundHeight
        ? tr("Automatic reward compounding is active. New staking rewards no longer add contract UTXOs.")
        : tr("Automatic reward compounding activates at block %1.").arg(compoundHeight);
    labelContractSummary->setText(
        tr("Total: %1 XPC  |  Owner: %2 XPC  |  Staking: %3 XPC\n%4")
            .arg(XPChainUnits::format(XPChainUnits::XPC, total),
                 XPChainUnits::format(XPChainUnits::XPC, ownerTotal),
                 XPChainUnits::format(XPChainUnits::XPC, stakerTotal),
                 compoundStatus));
    btnWithdraw->setEnabled(comboWithdrawContract->count() > 0);
    const int selectedContract = comboWithdrawContract->currentIndex();
    const int selectedOutputs = selectedContract < 0 ? 0
        : comboWithdrawContract->itemData(selectedContract, Qt::UserRole + 2).toInt();
    btnConsolidate->setEnabled(
        selectedOutputs >= WalletModel::COLD_STAKING_UTXOS_TO_PRESERVE + 2);
    if (contracts.empty()) {
        labelWithdrawStatus->setStyleSheet("color: #8b949e;");
        labelWithdrawStatus->setText(tr("No unspent cold-staking contracts were found in this wallet."));
    } else {
        labelWithdrawStatus->clear();
    }
}

void ColdStakingDialog::onWithdrawClicked()
{
    if (!model || comboWithdrawContract->currentIndex() < 0) return;

    const QString contract = comboWithdrawContract->currentData().toString();
    const QString destination = editWithdrawDestination->text().trimmed();
    const QString amountString = editWithdrawAmount->text().trimmed();
    CAmount amount = 0;
    if (!model->validateAddress(destination)) {
        labelWithdrawStatus->setStyleSheet("color: #f85149;");
        labelWithdrawStatus->setText(tr("Enter a valid destination address."));
        return;
    }
    if (!XPChainUnits::parse(XPChainUnits::XPC, amountString, &amount) || amount <= 0) {
        labelWithdrawStatus->setStyleSheet("color: #f85149;");
        labelWithdrawStatus->setText(tr("Enter a valid withdrawal amount."));
        return;
    }

    WalletModel::UnlockContext unlock(model->requestUnlock());
    if (!unlock.isValid()) {
        labelWithdrawStatus->setStyleSheet("color: #f85149;");
        labelWithdrawStatus->setText(tr("Wallet unlock cancelled."));
        return;
    }

    QList<SendCoinsRecipient> recipients;
    recipients.append(SendCoinsRecipient(destination, tr("Cold Staking Withdrawal"), amount, ""));
    WalletModelTransaction transaction(recipients);
    int inputCount = 0;
    const WalletModel::SendCoinsReturn prepared =
        model->prepareColdStakingWithdrawal(transaction, contract, inputCount);
    if (prepared.status != WalletModel::OK) {
        labelWithdrawStatus->setStyleSheet("color: #f85149;");
        if (prepared.status == WalletModel::AmountExceedsBalance ||
            prepared.status == WalletModel::AmountWithFeeExceedsBalance) {
            labelWithdrawStatus->setText(tr("The selected contract does not have enough owner-controlled funds for the amount and fee."));
        } else {
            labelWithdrawStatus->setText(tr("Could not prepare the cold-staking withdrawal."));
        }
        return;
    }

    const CAmount receiveAmount = transaction.getRecipients().first().amount;
    const bool feeDeducted = receiveAmount < amount;
    const QString formattedAmount = XPChainUnits::formatWithUnit(XPChainUnits::XPC, amount);
    const QString formattedReceiveAmount = XPChainUnits::formatWithUnit(XPChainUnits::XPC, receiveAmount);
    const QString fee = XPChainUnits::formatWithUnit(XPChainUnits::XPC, transaction.getTransactionFee());
    const QMessageBox::StandardButton confirmation = QMessageBox::question(
        this, tr("Confirm Cold Staking Withdrawal"),
        (feeDeducted
            ? tr("Withdraw the full contract balance of %1?\n\nDestination: %2\nAmount received: %3\nInputs: %4\n"
                 "Transaction fee deducted from received amount: %5\n\nOnly the owner key can authorize this transaction.")
                  .arg(formattedAmount, destination, formattedReceiveAmount).arg(inputCount).arg(fee)
            : tr("Withdraw %1 from the selected contract?\n\nDestination: %2\nInputs: %3\nTransaction fee: %4\n\n"
                 "Only the owner key can authorize this transaction.")
                  .arg(formattedAmount, destination).arg(inputCount).arg(fee)),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (confirmation != QMessageBox::Yes) return;

    const WalletModel::SendCoinsReturn sent = model->sendCoins(transaction);
    if (sent.status != WalletModel::OK) {
        labelWithdrawStatus->setStyleSheet("color: #f85149;");
        labelWithdrawStatus->setText(sent.reasonCommitFailed.isEmpty()
            ? tr("Withdrawal transaction broadcast failed.")
            : tr("Withdrawal rejected: %1").arg(sent.reasonCommitFailed));
        return;
    }

    const QString txid = QString::fromStdString(transaction.getWtx()->get().GetHash().GetHex());
    labelWithdrawStatus->setStyleSheet("color: #56d364;");
    labelWithdrawStatus->setText(
        tr("Withdrawal transaction broadcast successfully. Transaction ID: %1").arg(txid));
    editWithdrawAmount->clear();
    refreshColdStaking();
}

void ColdStakingDialog::onConsolidateClicked()
{
    if (!model || comboWithdrawContract->currentIndex() < 0) return;

    const QString contractAddress = comboWithdrawContract->currentData().toString();
    const CTxDestination contract = DecodeDestination(contractAddress.toStdString());
    std::vector<interfaces::ColdStakingOutput> candidates;
    for (const auto& output : model->getColdStakingOutputs()) {
        if (output.address == contract && output.owner && output.confirmations > 0) {
            candidates.push_back(output);
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return a.amount < b.amount;
    });

    const int excess = static_cast<int>(candidates.size()) -
        WalletModel::COLD_STAKING_UTXOS_TO_PRESERVE;
    const int smallInputCount = std::min(
        excess, WalletModel::COLD_STAKING_MAX_CONSOLIDATION_INPUTS - 1);
    if (smallInputCount < 2) {
        labelWithdrawStatus->setStyleSheet("color: #8b949e;");
        labelWithdrawStatus->setText(
            tr("Nothing to consolidate. At least %1 confirmed UTXOs are kept available for staking.")
                .arg(WalletModel::COLD_STAKING_UTXOS_TO_PRESERVE));
        return;
    }

    CAmount requestedAmount = 0;
    for (int i = 0; i < smallInputCount; ++i) requestedAmount += candidates[i].amount;
    const CAmount requestedSponsorAmount = candidates[excess].amount;
    requestedAmount += requestedSponsorAmount;

    WalletModel::UnlockContext unlock(model->requestUnlock());
    if (!unlock.isValid()) {
        labelWithdrawStatus->setStyleSheet("color: #f85149;");
        labelWithdrawStatus->setText(tr("Wallet unlock cancelled."));
        return;
    }

    QList<SendCoinsRecipient> recipients;
    recipients.append(SendCoinsRecipient(
        contractAddress, tr("Cold Staking UTXO Consolidation"), requestedAmount, ""));
    WalletModelTransaction transaction(recipients);
    int inputCount = 0;
    CAmount inputAmount = 0;
    CAmount sponsorAmount = 0;
    const WalletModel::SendCoinsReturn prepared = model->prepareColdStakingConsolidation(
        transaction, contractAddress, inputCount, inputAmount, sponsorAmount);
    if (prepared.status != WalletModel::OK) {
        labelWithdrawStatus->setStyleSheet("color: #f85149;");
        labelWithdrawStatus->setText(
            prepared.status == WalletModel::InvalidAmount
                ? tr("There are not enough confirmed small UTXOs to consolidate safely.")
                : tr("Could not prepare the cold-staking consolidation transaction."));
        return;
    }

    const CAmount consolidatedAmount = transaction.getRecipients().first().amount;
    const CAmount smallInputAmount = inputAmount - sponsorAmount;
    const QString smallTotal = XPChainUnits::formatWithUnit(XPChainUnits::XPC, smallInputAmount);
    const QString sponsor = XPChainUnits::formatWithUnit(XPChainUnits::XPC, sponsorAmount);
    const QString total = XPChainUnits::formatWithUnit(XPChainUnits::XPC, inputAmount);
    const QString result = XPChainUnits::formatWithUnit(XPChainUnits::XPC, consolidatedAmount);
    const QString fee = XPChainUnits::formatWithUnit(
        XPChainUnits::XPC, transaction.getTransactionFee());
    const QMessageBox::StandardButton confirmation = QMessageBox::question(
        this, tr("Confirm UTXO Consolidation"),
        tr("Combine %1 small confirmed UTXOs using one established staking UTXO to cover the fee?\n\n"
           "Small UTXOs: %2\nFee sponsor UTXO: %3\nTotal selected: %4\n"
           "New contract output: %5\nTransaction fee: %6\n\n"
           "The other %7 established UTXOs remain untouched. Only the replacement output must mature again before staking.")
            .arg(inputCount - 1).arg(smallTotal, sponsor, total)
            .arg(result, fee)
            .arg(WalletModel::COLD_STAKING_UTXOS_TO_PRESERVE - 1),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (confirmation != QMessageBox::Yes) return;

    const WalletModel::SendCoinsReturn sent = model->sendCoins(transaction);
    if (sent.status != WalletModel::OK) {
        labelWithdrawStatus->setStyleSheet("color: #f85149;");
        labelWithdrawStatus->setText(sent.reasonCommitFailed.isEmpty()
            ? tr("UTXO consolidation transaction broadcast failed.")
            : tr("Consolidation rejected: %1").arg(sent.reasonCommitFailed));
        return;
    }

    const QString txid = QString::fromStdString(transaction.getWtx()->get().GetHash().GetHex());
    labelWithdrawStatus->setStyleSheet("color: #56d364;");
    labelWithdrawStatus->setText(
        tr("Consolidated %1 UTXOs successfully. Transaction ID: %2").arg(inputCount).arg(txid));
    refreshColdStaking();
}
