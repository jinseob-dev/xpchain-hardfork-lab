#ifndef XPCHAIN_QT_MINTINGVIEW_H
#define XPCHAIN_QT_MINTINGVIEW_H

#include <qt/guiutil.h>

#include <QWidget>
#include <QComboBox>
#include <qt/mintingfilterproxy.h>

class PlatformStyle;
class MintingFilterProxy;
class WalletModel;


QT_BEGIN_NAMESPACE
class QComboBox;
class QDateTimeEdit;
class QEvent;
class QFrame;
class QLabel;
class QLineEdit;
class QMenu;
class QModelIndex;
class QPushButton;
class QSignalMapper;
class QTableView;
QT_END_NAMESPACE

class MintingView : public QWidget
{
    Q_OBJECT
public:
    explicit MintingView(const PlatformStyle *platformStyle, QWidget *parent = 0);
    void setModel(WalletModel *model);

    enum MintingEnum
    {
        Minting10min,
        Minting1day,
        Minting7days,
        Minting30days,
        Minting60days,
    };

protected:
    void changeEvent(QEvent *event) override;

private:
    WalletModel *model;
    QTableView *mintingView;
    QComboBox *mintingCombo;
    MintingFilterProxy *mintingProxyModel;
    QMenu *contextMenu;
    QFrame *guidanceFrame;
    QLabel *guidanceLabel;
    QPushButton *guidanceButton;

    QLabel *youngColorSwatch;
    QLabel *matureColorSwatch;
    QLabel *oldColorSwatch;

    void updateThemeColors();

private Q_SLOTS:
    void updateGuidanceBanner();
    void contextualMenu(const QPoint &);
    void copyAddress();
    void copyTransactionId();
    void showHideAddress();
    void showHideTxID();
    void guidanceButtonClicked();
    void onColdStakingClicked();

Q_SIGNALS:
    void unlockForStakingRequested();

public Q_SLOTS:
    void exportClicked();
    void chooseMintingInterval(int idx);

private:
    const PlatformStyle *platformStyle;
};

#endif // XPCHAIN_QT_MINTINGVIEW_H
