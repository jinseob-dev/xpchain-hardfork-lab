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
#include <QClipboard>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>

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
      labelDelegateStatus(nullptr)
{
    setupUI();
}

ColdStakingDialog::~ColdStakingDialog()
{
}

void ColdStakingDialog::setupUI()
{
    setWindowTitle(tr("Cold Staking & Delegation"));
    setMinimumWidth(680);

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

    mainLayout->addWidget(tabWidget);

    // Default: if model available, populate owner address with a receiving address
    if (model && model->getAddressTableModel()) {
        AddressTableModel *addrModel = model->getAddressTableModel();
        for (int row = 0; row < addrModel->rowCount(QModelIndex()); ++row) {
            QModelIndex idxType = addrModel->index(row, 0, QModelIndex());
            if (addrModel->data(idxType, AddressTableModel::TypeRole).toString() == AddressTableModel::Receive) {
                QModelIndex idxAddr = addrModel->index(row, AddressTableModel::Address, QModelIndex());
                editOwnerAddress->setText(addrModel->data(idxAddr, Qt::DisplayRole).toString());
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

    CKeyID ownerKeyId;
    if (const CKeyID *k = boost::get<CKeyID>(&ownerDest)) {
        ownerKeyId = *k;
    } else if (const WitnessV0KeyHash *w = boost::get<WitnessV0KeyHash>(&ownerDest)) {
        ownerKeyId = CKeyID(*w);
    } else {
        labelStatus->setStyleSheet("color: #f85149;");
        labelStatus->setText(tr("Owner address must be a standard P2PKH or SegWit (Bech32) address."));
        return;
    }

    CKeyID stakerKeyId;
    if (const CKeyID *k = boost::get<CKeyID>(&stakerDest)) {
        stakerKeyId = *k;
    } else if (const WitnessV0KeyHash *w = boost::get<WitnessV0KeyHash>(&stakerDest)) {
        stakerKeyId = CKeyID(*w);
    } else {
        labelStatus->setStyleSheet("color: #f85149;");
        labelStatus->setText(tr("Staker node address must be a standard P2PKH or SegWit (Bech32) address."));
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
