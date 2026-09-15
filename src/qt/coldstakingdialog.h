// Copyright (c) 2026 The XPChain Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef XPCHAIN_QT_COLDSTAKINGDIALOG_H
#define XPCHAIN_QT_COLDSTAKINGDIALOG_H

#include <QDialog>

class WalletModel;
class PlatformStyle;
class QLineEdit;
class QPushButton;
class QLabel;
class QTabWidget;
class QComboBox;
class QTableWidget;

class ColdStakingDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ColdStakingDialog(const PlatformStyle *platformStyle, WalletModel *model, QWidget *parent = nullptr);
    ~ColdStakingDialog();

private Q_SLOTS:
    void onGenerateAddressClicked();
    void onCopyAddressClicked();
    void onDelegateClicked();
    void onUseGeneratedAddressClicked();
    void refreshColdStaking();
    void onWithdrawClicked();

private:
    WalletModel *model;
    const PlatformStyle *platformStyle;

    // Tab 1: Create Delegation Address
    QLineEdit *editOwnerAddress;
    QLineEdit *editStakerAddress;
    QLineEdit *editResultAddress;
    QPushButton *btnGenerateAddress;
    QPushButton *btnCopyAddress;
    QPushButton *btnUseInDelegate;
    QLabel *labelStatus;

    // Tab 2: Delegate Coins
    QLineEdit *editDelegateAddress;
    QLineEdit *editDelegateAmount;
    QPushButton *btnSendDelegation;
    QLabel *labelDelegateStatus;

    // Tab 3: Monitor and withdraw delegated coins
    QTableWidget *tableContracts;
    QComboBox *comboWithdrawContract;
    QLineEdit *editWithdrawDestination;
    QLineEdit *editWithdrawAmount;
    QPushButton *btnRefreshContracts;
    QPushButton *btnWithdrawAll;
    QPushButton *btnWithdraw;
    QLabel *labelContractSummary;
    QLabel *labelWithdrawStatus;

    void setupUI();
};

#endif // XPCHAIN_QT_COLDSTAKINGDIALOG_H
