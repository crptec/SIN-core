// Copyright (c) 2011-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/overviewpage.h>
#include <qt/forms/ui_overviewpage.h>

#include <qt/sinunits.h>
#include <qt/clientmodel.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/transactionfilterproxy.h>
#include <qt/transactiontablemodel.h>
#include <qt/utilitydialog.h>
#include <qt/walletmodel.h>
#include <wallet/wallet.h>
#include <validation.h>
#include <qt/darksendconfig.h>
#include <interfaces/node.h>

#include <init.h>
#include <util.h>
#include <shutdown.h>
#include <instantx.h>
#include <masternode-sync.h>
#include <privatesend-client.h>

#include <QAbstractItemDelegate>
#include <QPainter>
#include <QSettings>
#include <QTimer>
#include <QDesktopServices>
#include <QUrl>

#define ICON_OFFSET 16
#define DECORATION_SIZE 54
#define NUM_ITEMS 5
#define NUM_ITEMS_ADV 7

Q_DECLARE_METATYPE(interfaces::WalletBalances)

class TxViewDelegate : public QAbstractItemDelegate
{
    Q_OBJECT
public:
    explicit TxViewDelegate(const PlatformStyle *_platformStyle, QObject *parent=nullptr):
        QAbstractItemDelegate(parent), unit(BitcoinUnits::SIN),
        platformStyle(_platformStyle)
    {

    }

    inline void paint(QPainter *painter, const QStyleOptionViewItem &option,
                      const QModelIndex &index ) const
    {
        painter->save();

        QIcon icon = qvariant_cast<QIcon>(index.data(TransactionTableModel::RawDecorationRole));
        QRect mainRect = option.rect;
        mainRect.moveLeft(ICON_OFFSET);
        QRect decorationRect(mainRect.topLeft(), QSize(DECORATION_SIZE, DECORATION_SIZE));
        int xspace = DECORATION_SIZE + 8;
        int ypad = 6;
        int halfheight = (mainRect.height() - 2*ypad)/2;
        QRect amountRect(mainRect.left() + xspace, mainRect.top()+ypad, mainRect.width() - xspace - ICON_OFFSET, halfheight);
        QRect addressRect(mainRect.left() + xspace, mainRect.top()+ypad+halfheight, mainRect.width() - xspace, halfheight);
        icon = platformStyle->SingleColorIcon(icon);
        icon.paint(painter, decorationRect);

        QDateTime date = index.data(TransactionTableModel::DateRole).toDateTime();
        QString address = index.data(Qt::DisplayRole).toString();
        qint64 amount = index.data(TransactionTableModel::AmountRole).toLongLong();
        bool confirmed = index.data(TransactionTableModel::ConfirmedRole).toBool();
        QVariant value = index.data(Qt::ForegroundRole);
        QColor foreground = option.palette.color(QPalette::Text);
        if(value.canConvert<QBrush>())
        {
            QBrush brush = qvariant_cast<QBrush>(value);
            foreground = brush.color();
        }

        painter->setPen(foreground);
        QRect boundingRect;
        painter->drawText(addressRect, Qt::AlignLeft|Qt::AlignVCenter, address, &boundingRect);

        if (index.data(TransactionTableModel::WatchonlyRole).toBool())
        {
            QIcon iconWatchonly = qvariant_cast<QIcon>(index.data(TransactionTableModel::WatchonlyDecorationRole));
            QRect watchonlyRect(boundingRect.right() + 5, mainRect.top()+ypad+halfheight, 16, halfheight);
            iconWatchonly.paint(painter, watchonlyRect);
        }

        if(amount < 0)
        {
            foreground = COLOR_NEGATIVE;
        }
        else if(!confirmed)
        {
            foreground = COLOR_UNCONFIRMED;
        }
        else
        {
            foreground = option.palette.color(QPalette::Text);
        }
        painter->setPen(foreground);

        QString amountText = BitcoinUnits::formatWithUnit(unit, amount, true, BitcoinUnits::separatorAlways);
        if(!confirmed)
        {
            amountText = QString("[") + amountText + QString("]");
        }
        painter->drawText(amountRect, Qt::AlignRight|Qt::AlignVCenter, amountText);

        painter->setPen(option.palette.color(QPalette::Text));
        painter->drawText(amountRect, Qt::AlignLeft|Qt::AlignVCenter, GUIUtil::dateTimeStr(date));

        painter->restore();
    }

    inline QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
    {
        return QSize(DECORATION_SIZE, DECORATION_SIZE);
    }

    int unit;
    const PlatformStyle *platformStyle;

};
#include <qt/overviewpage.moc>

OverviewPage::OverviewPage(const PlatformStyle *platformStyle, QWidget *parent) :
    QWidget(parent),
    timer(nullptr),
    ui(new Ui::OverviewPage),
    clientModel(0),
    walletModel(0),
    txdelegate(new TxViewDelegate(platformStyle, this))
{
    ui->setupUi(this);
    //ui->pushButton_Website->setIcon(QIcon(GUIUtil::getThemeImage(":/icons/website")));
    ui->pushButton_Website->setStatusTip(tr("Sinovate Website"));
    ui->pushButton_Discord->setStatusTip(tr("Sinovate Discord Channel"));
    ui->pushButton_Telegram->setStatusTip(tr("Sinovate Telegram Channel"));
    ui->pushButton_Twitter->setStatusTip(tr("Sinovate Twitter Channel"));
    ui->pushButton_Explorer->setStatusTip(tr("Sinovate Block Explorer"));


    m_balances.balance = -1;

    // use a SingleColorIcon for the "out of sync warning" icon
    QIcon icon = platformStyle->SingleColorIcon(":/icons/warning");
    icon.addPixmap(icon.pixmap(QSize(64,64), QIcon::Normal), QIcon::Disabled); // also set the disabled icon because we are using a disabled QPushButton to work around missing HiDPI support of QLabel (https://bugreports.qt.io/browse/QTBUG-42503)
    ui->labelTransactionsStatus->setIcon(icon);
    ui->labelWalletStatus->setIcon(icon);

    // Recent transactions
    ui->listTransactions->setItemDelegate(txdelegate);
    ui->listTransactions->setIconSize(QSize(DECORATION_SIZE, DECORATION_SIZE));
    ui->listTransactions->setMinimumHeight(NUM_ITEMS * (DECORATION_SIZE + 2));
    ui->listTransactions->setAttribute(Qt::WA_MacShowFocusRect, false);

    connect(ui->listTransactions, SIGNAL(clicked(QModelIndex)), this, SLOT(handleTransactionClicked(QModelIndex)));

    // init "out of sync" warning labels
    ui->labelWalletStatus->setText("(" + tr("out of sync") + ")");
    ui->labelPrivateSendSyncStatus->setText("(" + tr("out of sync") + ")");
    ui->labelTransactionsStatus->setText("(" + tr("out of sync") + ")");

    // hide PS frame (helps to preserve saved size)
    // we'll setup and make it visible in updateAdvancedPSUI() later if we are not in litemode
    ui->framePrivateSend->setVisible(false);

    // start with displaying the "out of sync" warnings
    showOutOfSyncWarning(true);
    connect(ui->labelWalletStatus, SIGNAL(clicked()), this, SLOT(handleOutOfSyncWarningClicks()));
    connect(ui->labelTransactionsStatus, SIGNAL(clicked()), this, SLOT(handleOutOfSyncWarningClicks()));

    // that's it for litemode
    if(fLiteMode) return;

    // Disable any PS UI for masternode or when autobackup is disabled or failed for whatever reason
    if( fMasterNode || nWalletBackups <= 0){
        DisablePrivateSendCompletely();
        if (nWalletBackups <= 0) {
            ui->labelPrivateSendEnabled->setToolTip(tr("Automatic backups are disabled, no mixing available!"));
        }
    } else {
        if(!privateSendClient.fEnablePrivateSend){
            ui->togglePrivateSend->setText(tr("Start Mixing"));
        } else {
            ui->togglePrivateSend->setText(tr("Stop Mixing"));
        }
        // Disable privateSendClient builtin support for automatic backups while we are in GUI,
        // we'll handle automatic backups and user warnings in privateSendStatus()
        privateSendClient.fCreateAutoBackups = false;

        timer = new QTimer(this);
        connect(timer, SIGNAL(timeout()), this, SLOT(privateSendStatus()));
        timer->start(1000);
    }
}

void OverviewPage::handleTransactionClicked(const QModelIndex &index)
{
    if(filter)
        Q_EMIT transactionClicked(filter->mapToSource(index));
}

void OverviewPage::handleOutOfSyncWarningClicks()
{
    Q_EMIT outOfSyncWarningClicked();
}

OverviewPage::~OverviewPage()
{
    if(timer) disconnect(timer, SIGNAL(timeout()), this, SLOT(privateSendStatus()));
    delete ui;
}

void OverviewPage::setBalance(const interfaces::WalletBalances& balances)
{
    int unit = walletModel->getOptionsModel()->getDisplayUnit();
    m_balances = balances;
    ui->labelBalance->setText(BitcoinUnits::floorHtmlWithUnit(unit, balances.balance, false, BitcoinUnits::separatorAlways));
    ui->labelUnconfirmed->setText(BitcoinUnits::floorHtmlWithUnit(unit, balances.unconfirmed_balance, false, BitcoinUnits::separatorAlways));
    ui->labelImmature->setText(BitcoinUnits::floorHtmlWithUnit(unit, balances.immature_balance, false, BitcoinUnits::separatorAlways));
    ui->labelAnonymized->setText(BitcoinUnits::floorHtmlWithUnit(unit, balances.anonymized_balance, false, BitcoinUnits::separatorAlways));
    ui->labelTotal->setText(BitcoinUnits::floorHtmlWithUnit(unit, balances.balance + balances.unconfirmed_balance + balances.immature_balance, false, BitcoinUnits::separatorAlways));
    ui->labelWatchAvailable->setText(BitcoinUnits::floorHtmlWithUnit(unit, balances.watch_only_balance, false, BitcoinUnits::separatorAlways));
    ui->labelWatchPending->setText(BitcoinUnits::floorHtmlWithUnit(unit, balances.unconfirmed_watch_only_balance, false, BitcoinUnits::separatorAlways));
    ui->labelWatchImmature->setText(BitcoinUnits::floorHtmlWithUnit(unit, balances.immature_watch_only_balance, false, BitcoinUnits::separatorAlways));
    ui->labelWatchTotal->setText(BitcoinUnits::floorHtmlWithUnit(unit, balances.watch_only_balance + balances.unconfirmed_watch_only_balance + balances.immature_watch_only_balance, false, BitcoinUnits::separatorAlways));

    // only show immature (newly mined) balance if it's non-zero, so as not to complicate things
    // for the non-mining users
    bool showImmature = balances.immature_balance != 0;
    bool showWatchOnlyImmature = balances.immature_watch_only_balance != 0;

    // for symmetry reasons also show immature label when the watch-only one is shown
    ui->labelImmature->setVisible(showImmature || showWatchOnlyImmature);
    ui->labelImmatureText->setVisible(showImmature || showWatchOnlyImmature);
    ui->labelWatchImmature->setVisible(showWatchOnlyImmature); // show watch-only immature balance

    updatePrivateSendProgress();

    static int cachedTxLocks = 0;

    if(cachedTxLocks != nCompleteTXLocks){
        cachedTxLocks = nCompleteTXLocks;
        ui->listTransactions->update();
    }
}

// show/hide watch-only labels
void OverviewPage::updateWatchOnlyLabels(bool showWatchOnly)
{
    ui->labelSpendable->setVisible(showWatchOnly);      // show spendable label (only when watch-only is active)
    ui->labelWatchonly->setVisible(showWatchOnly);      // show watch-only label
    ui->lineWatchBalance->setVisible(showWatchOnly);    // show watch-only balance separator line
    ui->labelWatchAvailable->setVisible(showWatchOnly); // show watch-only available balance
    ui->labelWatchPending->setVisible(showWatchOnly);   // show watch-only pending balance
    ui->labelWatchTotal->setVisible(showWatchOnly);     // show watch-only total balance

    if (!showWatchOnly){
        ui->labelWatchImmature->hide();
    }
    else{
        ui->labelBalance->setIndent(20);
        ui->labelUnconfirmed->setIndent(20);
        ui->labelImmature->setIndent(20);
        ui->labelTotal->setIndent(20);
    }
}

void OverviewPage::setClientModel(ClientModel *model)
{
    this->clientModel = model;
    if(model)
    {
        // Show warning if this is a prerelease version
        connect(model, SIGNAL(alertsChanged(QString)), this, SLOT(updateAlerts(QString)));
        updateAlerts(model->getStatusBarWarnings());
    }
}

void OverviewPage::setWalletModel(WalletModel *model)
{
    this->walletModel = model;
    if(model && model->getOptionsModel())
    {
        // Set up transaction list
        filter.reset(new TransactionFilterProxy());
        filter->setSourceModel(model->getTransactionTableModel());
        filter->setLimit(NUM_ITEMS);
        filter->setDynamicSortFilter(true);
        filter->setSortRole(Qt::EditRole);
        filter->setShowInactive(false);
        filter->sort(TransactionTableModel::Date, Qt::DescendingOrder);

        ui->listTransactions->setModel(filter.get());
        ui->listTransactions->setModelColumn(TransactionTableModel::ToAddress);

        // Keep up to date with wallet
        interfaces::Wallet& wallet = model->wallet();
        interfaces::WalletBalances balances = wallet.getBalances();
        setBalance(balances);
        connect(model, SIGNAL(balanceChanged(interfaces::WalletBalances)), this, SLOT(setBalance(interfaces::WalletBalances)));

        connect(model->getOptionsModel(), SIGNAL(displayUnitChanged(int)), this, SLOT(updateDisplayUnit()));
        updateWatchOnlyLabels(wallet.haveWatchOnly());
        connect(model, SIGNAL(notifyWatchonlyChanged(bool)), this, SLOT(updateWatchOnlyLabels(bool)));

        // explicitly update PS frame and transaction list to reflect actual settings
        updateAdvancedPSUI(model->getOptionsModel()->getShowAdvancedPSUI());

        // that's it for litemode
        if(fLiteMode) return;
        connect(model->getOptionsModel(), SIGNAL(privateSendRoundsChanged()), this, SLOT(updatePrivateSendProgress()));
        connect(model->getOptionsModel(), SIGNAL(privateSentAmountChanged()), this, SLOT(updatePrivateSendProgress()));
        connect(model->getOptionsModel(), SIGNAL(advancedPSUIChanged(bool)), this, SLOT(updateAdvancedPSUI(bool)));

        connect(ui->privateSendAuto, SIGNAL(clicked()), this, SLOT(privateSendAuto()));
        connect(ui->privateSendReset, SIGNAL(clicked()), this, SLOT(privateSendReset()));
        connect(ui->privateSendInfo, SIGNAL(clicked()), this, SLOT(privateSendInfo()));
        connect(ui->togglePrivateSend, SIGNAL(clicked()), this, SLOT(togglePrivateSend()));

        // privatesend buttons will not react to spacebar must be clicked on
        ui->privateSendAuto->setFocusPolicy(Qt::NoFocus);
        ui->privateSendReset->setFocusPolicy(Qt::NoFocus);
        ui->privateSendInfo->setFocusPolicy(Qt::NoFocus);
        ui->togglePrivateSend->setFocusPolicy(Qt::NoFocus);
    }
    // update the display unit, to not use the default ("SIN")
    updateDisplayUnit();
}

void OverviewPage::updateDisplayUnit()
{
    if(walletModel && walletModel->getOptionsModel())
    {
        if (m_balances.balance != -1) {
            setBalance(m_balances);
        }

        // Update txdelegate->unit with the current unit
        txdelegate->unit = walletModel->getOptionsModel()->getDisplayUnit();

        ui->listTransactions->update();
    }
}

void OverviewPage::updateAlerts(const QString &warnings)
{
    this->ui->labelAlerts->setVisible(!warnings.isEmpty());
    this->ui->labelAlerts->setText(warnings);
}

void OverviewPage::showOutOfSyncWarning(bool fShow)
{
    ui->labelWalletStatus->setVisible(fShow);
    ui->labelPrivateSendSyncStatus->setVisible(fShow);
    ui->labelTransactionsStatus->setVisible(fShow);
}

void OverviewPage::updatePrivateSendProgress()
{
    if(!masternodeSync.IsBlockchainSynced() || ShutdownRequested()) return;

    /*if(!pwalletMain) return;*/

    QString strAmountAndRounds;
    QString strPrivateSendAmount = BitcoinUnits::formatHtmlWithUnit(nDisplayUnit, privateSendClient.nPrivateSendAmount * COIN, false, BitcoinUnits::separatorAlways);

    if(m_balances.balance == 0)
    {

        ui->privateSendProgress->setValue(0);
        ui->privateSendProgress->setToolTip(tr("No inputs detected"));

        // when balance is zero just show info from settings
        strPrivateSendAmount = strPrivateSendAmount.remove(strPrivateSendAmount.indexOf("."), BitcoinUnits::decimals(nDisplayUnit) + 1);
        strAmountAndRounds = strPrivateSendAmount + " / " + tr("%n Rounds", "", privateSendClient.nPrivateSendRounds);

        ui->labelAmountRounds->setToolTip(tr("No inputs detected"));
        ui->labelAmountRounds->setText(strAmountAndRounds);
        return;
    }

    CAmount nAnonymizableBalance = walletModel->wallet().GetAnonymizableBalance(false, false);

    CAmount nMaxToAnonymize = nAnonymizableBalance + m_balances.anonymized_balance;
    // If it's more than the anon threshold, limit to that.
    if(nMaxToAnonymize > privateSendClient.nPrivateSendAmount*COIN) nMaxToAnonymize = privateSendClient.nPrivateSendAmount*COIN;

    if(nMaxToAnonymize == 0) return;

    if(nMaxToAnonymize >= privateSendClient.nPrivateSendAmount * COIN) {
        ui->labelAmountRounds->setToolTip(tr("Found enough compatible inputs to anonymize %1")
                                          .arg(strPrivateSendAmount));
        strPrivateSendAmount = strPrivateSendAmount.remove(strPrivateSendAmount.indexOf("."), BitcoinUnits::decimals(nDisplayUnit) + 1);
        strAmountAndRounds = strPrivateSendAmount + " / " + tr("%n Rounds", "", privateSendClient.nPrivateSendRounds);
    } else {
        QString strMaxToAnonymize = BitcoinUnits::formatHtmlWithUnit(nDisplayUnit, nMaxToAnonymize, false, BitcoinUnits::separatorAlways);
        ui->labelAmountRounds->setToolTip(tr("Not enough compatible inputs to anonymize <span style='color:red;'>%1</span>,<br>"
                                             "will anonymize <span style='color:red;'>%2</span> instead")
                                          .arg(strPrivateSendAmount)
                                          .arg(strMaxToAnonymize));
        strMaxToAnonymize = strMaxToAnonymize.remove(strMaxToAnonymize.indexOf("."), BitcoinUnits::decimals(nDisplayUnit) + 1);
        strAmountAndRounds = "<span style='color:red;'>" +
                QString(BitcoinUnits::factor(nDisplayUnit) == 1 ? "" : "~") + strMaxToAnonymize +
                " / " + tr("%n Rounds", "", privateSendClient.nPrivateSendRounds) + "</span>";
    }

    ui->labelAmountRounds->setText(strAmountAndRounds);

    if (!fShowAdvancedPSUI) return;

    CAmount nDenominatedConfirmedBalance;
    CAmount nDenominatedUnconfirmedBalance;
    CAmount nNormalizedAnonymizedBalance;
    float nAverageAnonymizedRounds;

    nDenominatedConfirmedBalance = walletModel->wallet().GetDenominatedBalance();
    nDenominatedUnconfirmedBalance = walletModel->wallet().GetDenominatedBalance(true);
    nNormalizedAnonymizedBalance = walletModel->wallet().GetNormalizedAnonymizedBalance();
    nAverageAnonymizedRounds = walletModel->wallet().GetAverageAnonymizedRounds();
    // calculate parts of the progress, each of them shouldn't be higher than 1
    // progress of denominating
    float denomPart = 0;
    // mixing progress of denominated balance
    float anonNormPart = 0;
    // completeness of full amount anonymization
    float anonFullPart = 0;

    CAmount denominatedBalance = nDenominatedConfirmedBalance + nDenominatedUnconfirmedBalance;
    denomPart = (float)denominatedBalance / nMaxToAnonymize;
    denomPart = denomPart > 1 ? 1 : denomPart;
    denomPart *= 100;

    anonNormPart = (float)nNormalizedAnonymizedBalance / nMaxToAnonymize;
    anonNormPart = anonNormPart > 1 ? 1 : anonNormPart;
    anonNormPart *= 100;

    anonFullPart = (float)m_balances.anonymized_balance / nMaxToAnonymize;
    anonFullPart = anonFullPart > 1 ? 1 : anonFullPart;
    anonFullPart *= 100;

    // apply some weights to them ...
    float denomWeight = 1;
    float anonNormWeight = privateSendClient.nPrivateSendRounds;
    float anonFullWeight = 2;
    float fullWeight = denomWeight + anonNormWeight + anonFullWeight;
    // ... and calculate the whole progress
    float denomPartCalc = ceilf((denomPart * denomWeight / fullWeight) * 100) / 100;
    float anonNormPartCalc = ceilf((anonNormPart * anonNormWeight / fullWeight) * 100) / 100;
    float anonFullPartCalc = ceilf((anonFullPart * anonFullWeight / fullWeight) * 100) / 100;
    float progress = denomPartCalc + anonNormPartCalc + anonFullPartCalc;
    if(progress >= 100) progress = 100;

    ui->privateSendProgress->setValue(progress);
    QString strToolPip = ("<b>" + tr("Overall progress") + ": %1%</b><br/>" +
                          tr("Denominated") + ": %2%<br/>" +
                          tr("Mixed") + ": %3%<br/>" +
                          tr("Anonymized") + ": %4%<br/>" +
                          tr("Denominated inputs have %5 of %n rounds on average", "", privateSendClient.nPrivateSendRounds))
            .arg(progress).arg(denomPart).arg(anonNormPart).arg(anonFullPart)
            .arg(nAverageAnonymizedRounds);
    ui->privateSendProgress->setToolTip(strToolPip);
}

void OverviewPage::updateAdvancedPSUI(bool fShowAdvancedPSUI) {
    this->fShowAdvancedPSUI = fShowAdvancedPSUI;
    int nNumItems = (fLiteMode || !fShowAdvancedPSUI) ? NUM_ITEMS : NUM_ITEMS_ADV;
    SetupTransactionList(nNumItems);

    if (fLiteMode) return;

    ui->framePrivateSend->setVisible(true);
    ui->labelCompletitionText->setVisible(fShowAdvancedPSUI);
    ui->privateSendProgress->setVisible(fShowAdvancedPSUI);
    ui->labelSubmittedDenomText->setVisible(fShowAdvancedPSUI);
    ui->labelSubmittedDenom->setVisible(fShowAdvancedPSUI);
    ui->privateSendAuto->setVisible(fShowAdvancedPSUI);
    ui->privateSendReset->setVisible(fShowAdvancedPSUI);
    ui->privateSendInfo->setVisible(true);
    ui->labelPrivateSendLastMessage->setVisible(fShowAdvancedPSUI);
}

void OverviewPage::privateSendStatus()
{
    if(!masternodeSync.IsBlockchainSynced() || ShutdownRequested()) return;

    static int64_t nLastDSProgressBlockTime = 0;
    int nBestHeight = clientModel->getNumBlocks();

    // We are processing more then 1 block per second, we'll just leave
    if(((nBestHeight - privateSendClient.nCachedNumBlocks) / (GetTimeMillis() - nLastDSProgressBlockTime + 1) > 1)) return;
    nLastDSProgressBlockTime = GetTimeMillis();

    QString strKeysLeftText(tr("keys left: %1").arg(walletModel->wallet().getKeysLeftSinceAutoBackup()));
    if(walletModel->wallet().getKeysLeftSinceAutoBackup() < PRIVATESEND_KEYS_THRESHOLD_WARNING) {
        strKeysLeftText = "<span style='color:red;'>" + strKeysLeftText + "</span>";
    }
    ui->labelPrivateSendEnabled->setToolTip(strKeysLeftText);

    if (!privateSendClient.fEnablePrivateSend) {
        if (nBestHeight != privateSendClient.nCachedNumBlocks) {
            privateSendClient.nCachedNumBlocks = nBestHeight;
            updatePrivateSendProgress();
        }

        ui->labelPrivateSendLastMessage->setText("");
        ui->togglePrivateSend->setText(tr("Start Mixing"));

        QString strEnabled = tr("Disabled");
        // Show how many keys left in advanced PS UI mode only
        if (fShowAdvancedPSUI) strEnabled += ", " + strKeysLeftText;
        ui->labelPrivateSendEnabled->setText(strEnabled);

        return;
    }

    // Warn user that wallet is running out of keys
    // NOTE: we do NOT warn user and do NOT create autobackups if mixing is not running
    if (nWalletBackups > 0 && walletModel->wallet().getKeysLeftSinceAutoBackup() < PRIVATESEND_KEYS_THRESHOLD_WARNING) {
        QSettings settings;
        if(settings.value("fLowKeysWarning").toBool()) {
            QString strWarn =   tr("Very low number of keys left since last automatic backup!") + "<br><br>" +
                                tr("We are about to create a new automatic backup for you, however "
                                   "<span style='color:red;'> you should always make sure you have backups "
                                   "saved in some safe place</span>!") + "<br><br>" +
                                tr("Note: You can turn this message off in options.");
            ui->labelPrivateSendEnabled->setToolTip(strWarn);
            LogPrintf("OverviewPage::privateSendStatus -- Very low number of keys left since last automatic backup, warning user and trying to create new backup...\n");
            QMessageBox::warning(this, tr("ShadowSend"), strWarn, QMessageBox::Ok, QMessageBox::Ok);
        } else {
            LogPrintf("OverviewPage::privateSendStatus -- Very low number of keys left since last automatic backup, skipping warning and trying to create new backup...\n");
        }

        std::string strBackupWarning;
        std::string strBackupError;
        std::vector<std::shared_ptr<CWallet>> wallets = GetWallets();
        std::shared_ptr<CWallet> pwallet = (wallets.size() > 0) ? wallets[0] : nullptr;

        if(!AutoBackupWallet(pwallet, "", strBackupWarning, strBackupError)) {
            if (!strBackupWarning.empty()) {
                // It's still more or less safe to continue but warn user anyway
                LogPrintf("OverviewPage::privateSendStatus -- WARNING! Something went wrong on automatic backup: %s\n", strBackupWarning);

                QMessageBox::warning(this, tr("ShadowSend"),
                    tr("WARNING! Something went wrong on automatic backup") + ":<br><br>" + strBackupWarning.c_str(),
                    QMessageBox::Ok, QMessageBox::Ok);
            }
            if (!strBackupError.empty()) {
                // Things are really broken, warn user and stop mixing immediately
                LogPrintf("OverviewPage::privateSendStatus -- ERROR! Failed to create automatic backup: %s\n", strBackupError);

                QMessageBox::warning(this, tr("ShadowSend"),
                    tr("ERROR! Failed to create automatic backup") + ":<br><br>" + strBackupError.c_str() + "<br>" +
                    tr("Mixing is disabled, please close your wallet and fix the issue!"),
                    QMessageBox::Ok, QMessageBox::Ok);
            }
        }
    }

    QString strEnabled = privateSendClient.fEnablePrivateSend ? tr("Enabled") : tr("Disabled");
    // Show how many keys left in advanced PS UI mode only
    if(fShowAdvancedPSUI) strEnabled += ", " + strKeysLeftText;
    ui->labelPrivateSendEnabled->setText(strEnabled);

    if(nWalletBackups == -1) {
        // Automatic backup failed, nothing else we can do until user fixes the issue manually
        DisablePrivateSendCompletely();

        QString strError =  tr("ERROR! Failed to create automatic backup") + ", " +
                            tr("see debug.log for details.") + "<br><br>" +
                            tr("Mixing is disabled, please close your wallet and fix the issue!");
        ui->labelPrivateSendEnabled->setToolTip(strError);

        return;
    } else if(nWalletBackups == -2) {
        // We were able to create automatic backup but keypool was not replenished because wallet is locked.
        QString strWarning = tr("WARNING! Failed to replenish keypool, please unlock your wallet to do so.");
        ui->labelPrivateSendEnabled->setToolTip(strWarning);
    }

    // check privatesend status and unlock if needed
    if(nBestHeight != privateSendClient.nCachedNumBlocks) {
        // Balance and number of transactions might have changed
        privateSendClient.nCachedNumBlocks = nBestHeight;
        updatePrivateSendProgress();
    }

    QString strStatus = QString(privateSendClient.GetStatus().c_str());

    QString s = tr("Last ShadowSend message:\n") + strStatus;

    if(s != ui->labelPrivateSendLastMessage->text())
        LogPrintf("OverviewPage::privateSendStatus -- Last ShadowSend message: %s\n", strStatus.toStdString());

    ui->labelPrivateSendLastMessage->setText(s);

    if(privateSendClient.nSessionDenom == 0){
        ui->labelSubmittedDenom->setText(tr("N/A"));
    } else {
        QString strDenom(CPrivateSend::GetDenominationsToString(privateSendClient.nSessionDenom).c_str());
        ui->labelSubmittedDenom->setText(strDenom);
    }

}

void OverviewPage::privateSendAuto(){
    privateSendClient.DoAutomaticDenominating(*g_connman);
}

void OverviewPage::privateSendReset(){
    privateSendClient.ResetPool();

    QMessageBox::warning(this, tr("ShadowSend"),
        tr("ShadowSend was successfully reset."),
        QMessageBox::Ok, QMessageBox::Ok);
}

void OverviewPage::privateSendInfo(){
    std::unique_ptr<interfaces::Node> node = interfaces::MakeNode();
    HelpMessageDialog dlg(*node, nullptr, HelpMessageDialog::pshelp);
    dlg.exec();
}

void OverviewPage::togglePrivateSend(){
    QSettings settings;
    // Popup some information on first mixing
    QString hasMixed = settings.value("hasMixed").toString();
    if(hasMixed.isEmpty()){
        QMessageBox::information(this, tr("PrivateSend"),
                tr("If you don't want to see internal PrivateSend fees/transactions select \"Most Common\" as Type on the \"Transactions\" tab."),
                QMessageBox::Ok, QMessageBox::Ok);
        settings.setValue("hasMixed", "hasMixed");
    }
    if(!privateSendClient.fEnablePrivateSend){
        const CAmount nMinAmount = CPrivateSend::GetSmallestDenomination() + CPrivateSend::GetMaxCollateralAmount();
        if(m_balances.balance < nMinAmount){
            QString strMinAmount(BitcoinUnits::formatWithUnit(nDisplayUnit, nMinAmount));
            QMessageBox::warning(this, tr("ShadowSend"),
                tr("ShadowSend requires at least %1 to use.").arg(strMinAmount),
                QMessageBox::Ok, QMessageBox::Ok);
            return;
        }

        // if wallet is locked, ask for a passphrase
        if (walletModel->getEncryptionStatus() == WalletModel::Locked)
        {
            WalletModel::UnlockContext ctx(walletModel->requestUnlock(true));
            if(!ctx.isValid())
            {
                //unlock was cancelled
                privateSendClient.nCachedNumBlocks = std::numeric_limits<int>::max();
                QMessageBox::warning(this, tr("ShadowSend"),
                    tr("Wallet is locked and user declined to unlock. Disabling ShadowSend."),
                    QMessageBox::Ok, QMessageBox::Ok);
                LogPrint(BCLog::PRIVATESEND, "OverviewPage::togglePrivateSend -- Wallet is locked and user declined to unlock. Disabling ShadowSend.\n");
                return;
            }
        }

    }

    privateSendClient.fEnablePrivateSend = !privateSendClient.fEnablePrivateSend;
    privateSendClient.nCachedNumBlocks = std::numeric_limits<int>::max();

    if(!privateSendClient.fEnablePrivateSend){
        ui->togglePrivateSend->setText(tr("Start Mixing"));
        privateSendClient.ResetPool();
    } else {
        ui->togglePrivateSend->setText(tr("Stop Mixing"));

        /* show darksend configuration if client has defaults set */

        if(privateSendClient.nPrivateSendAmount == 0){
            DarksendConfig dlg(this);
            dlg.setModel(walletModel);
            dlg.exec();
        }

    }
}

void OverviewPage::SetupTransactionList(int nNumItems) {
    ui->listTransactions->setMinimumHeight(nNumItems * (DECORATION_SIZE + 2));

    if(walletModel && walletModel->getOptionsModel()) {
        // Set up transaction list
        filter.reset(new TransactionFilterProxy());
        filter->setSourceModel(walletModel->getTransactionTableModel());
        filter->setLimit(nNumItems);
        filter->setDynamicSortFilter(true);
        filter->setSortRole(Qt::EditRole);
        filter->setShowInactive(false);
        filter->sort(TransactionTableModel::Date, Qt::DescendingOrder);

        ui->listTransactions->setModel(filter.get());
        ui->listTransactions->setModelColumn(TransactionTableModel::ToAddress);
    }
}

void OverviewPage::DisablePrivateSendCompletely() {
    ui->togglePrivateSend->setText("(" + tr("Disabled") + ")");
    ui->privateSendAuto->setText("(" + tr("Disabled") + ")");
    ui->privateSendReset->setText("(" + tr("Disabled") + ")");
    ui->framePrivateSend->setEnabled(false);
    if (nWalletBackups <= 0) {
        ui->labelPrivateSendEnabled->setText("<span style='color:#FF8204;'>(" + tr("Disabled") + ")</span>");
    }
    privateSendClient.fEnablePrivateSend = false;
}

void OverviewPage::on_pushButton_Website_clicked() {
    QDesktopServices::openUrl(QUrl("https://sinovate.io/", QUrl::TolerantMode));
}
void OverviewPage::on_pushButton_Discord_clicked() {
    QDesktopServices::openUrl(QUrl("https://sinovate.io/links/discord", QUrl::TolerantMode));
}
void OverviewPage::on_pushButton_Telegram_clicked() {
    QDesktopServices::openUrl(QUrl("https://sinovate.io/links/telegram", QUrl::TolerantMode));
}
void OverviewPage::on_pushButton_Twitter_clicked() {
    QDesktopServices::openUrl(QUrl("https://sinovate.io/links/twitter", QUrl::TolerantMode));
}
void OverviewPage::on_pushButton_Explorer_clicked() {
    QDesktopServices::openUrl(QUrl("http://sinovate.io/links/explorer", QUrl::TolerantMode));
}
void OverviewPage::on_pushButton_Btctalk_clicked() {
    QDesktopServices::openUrl(QUrl("https://sinovate.io/links/btctalk", QUrl::TolerantMode));
}
