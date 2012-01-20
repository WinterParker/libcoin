// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2011 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file license.txt or http://www.opensource.org/licenses/mit-license.php.

#include "btcWallet/wallet.h"
#include "btcWallet/walletdb.h"
#include "btcWallet/WalletTx.h"
//#include "btcNode/db.h"
#include "btc/crypter.h"

#include <openssl/rand.h>

using namespace std;

//CCriticalSection cs_setpwalletRegistered;
//set<Wallet*> setpwalletRegistered;


//////////////////////////////////////////////////////////////////////////////
//
// mapWallet
//

bool Wallet::AddKey(const CKey& key)
{
    if (!CCryptoKeyStore::AddKey(key))
        return false;
    if (!fFileBacked)
        return true;
    if (!IsCrypted())
        return CWalletDB(_dataDir, strWalletFile).WriteKey(key.GetPubKey(), key.GetPrivKey());
    return true;
}

bool Wallet::AddCryptedKey(const vector<unsigned char> &vchPubKey, const vector<unsigned char> &vchCryptedSecret)
{
    if (!CCryptoKeyStore::AddCryptedKey(vchPubKey, vchCryptedSecret))
        return false;
    if (!fFileBacked)
        return true;
    CRITICAL_BLOCK(cs_wallet)
    {
        if (pwalletdbEncryption)
            return pwalletdbEncryption->WriteCryptedKey(vchPubKey, vchCryptedSecret);
        else
            return CWalletDB(_dataDir, strWalletFile).WriteCryptedKey(vchPubKey, vchCryptedSecret);
    }
}

bool Wallet::Unlock(const string& strWalletPassphrase)
{
    if (!IsLocked())
        return false;

    CCrypter crypter;
    CKeyingMaterial vMasterKey;

    CRITICAL_BLOCK(cs_wallet)
        BOOST_FOREACH(const MasterKeyMap::value_type& pMasterKey, mapMasterKeys)
        {
            if(!crypter.SetKeyFromPassphrase(strWalletPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod))
                return false;
            if (!crypter.Decrypt(pMasterKey.second.vchCryptedKey, vMasterKey))
                return false;
            if (CCryptoKeyStore::Unlock(vMasterKey))
                return true;
        }
    return false;
}

bool Wallet::ChangeWalletPassphrase(const string& strOldWalletPassphrase, const string& strNewWalletPassphrase)
{
    bool fWasLocked = IsLocked();

    CRITICAL_BLOCK(cs_wallet)
    {
        Lock();

        CCrypter crypter;
        CKeyingMaterial vMasterKey;
        BOOST_FOREACH(MasterKeyMap::value_type& pMasterKey, mapMasterKeys)
        {
            if(!crypter.SetKeyFromPassphrase(strOldWalletPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod))
                return false;
            if (!crypter.Decrypt(pMasterKey.second.vchCryptedKey, vMasterKey))
                return false;
            if (CCryptoKeyStore::Unlock(vMasterKey))
            {
                int64 nStartTime = GetTimeMillis();
                crypter.SetKeyFromPassphrase(strNewWalletPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod);
                pMasterKey.second.nDeriveIterations = pMasterKey.second.nDeriveIterations * (100 / ((double)(GetTimeMillis() - nStartTime)));

                nStartTime = GetTimeMillis();
                crypter.SetKeyFromPassphrase(strNewWalletPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod);
                pMasterKey.second.nDeriveIterations = (pMasterKey.second.nDeriveIterations + pMasterKey.second.nDeriveIterations * 100 / ((double)(GetTimeMillis() - nStartTime))) / 2;

                if (pMasterKey.second.nDeriveIterations < 25000)
                    pMasterKey.second.nDeriveIterations = 25000;

                printf("Wallet passphrase changed to an nDeriveIterations of %i\n", pMasterKey.second.nDeriveIterations);

                if (!crypter.SetKeyFromPassphrase(strNewWalletPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod))
                    return false;
                if (!crypter.Encrypt(vMasterKey, pMasterKey.second.vchCryptedKey))
                    return false;
                CWalletDB(_dataDir, strWalletFile).WriteMasterKey(pMasterKey.first, pMasterKey.second);
                if (fWasLocked)
                    Lock();
                return true;
            }
        }
    }

    return false;
}


// This class implements an addrIncoming entry that causes pre-0.4
// clients to crash on startup if reading a private-key-encrypted wallet.
class CCorruptAddress
{
public:
    IMPLEMENT_SERIALIZE
    (
        if (nType & SER_DISK)
            READWRITE(nVersion);
    )
};

bool Wallet::EncryptWallet(const string& strWalletPassphrase)
{
    if (IsCrypted())
        return false;

    CKeyingMaterial vMasterKey;
    RandAddSeedPerfmon();

    vMasterKey.resize(WALLET_CRYPTO_KEY_SIZE);
    RAND_bytes(&vMasterKey[0], WALLET_CRYPTO_KEY_SIZE);

    CMasterKey kMasterKey;

    RandAddSeedPerfmon();
    kMasterKey.vchSalt.resize(WALLET_CRYPTO_SALT_SIZE);
    RAND_bytes(&kMasterKey.vchSalt[0], WALLET_CRYPTO_SALT_SIZE);

    CCrypter crypter;
    int64 nStartTime = GetTimeMillis();
    crypter.SetKeyFromPassphrase(strWalletPassphrase, kMasterKey.vchSalt, 25000, kMasterKey.nDerivationMethod);
    kMasterKey.nDeriveIterations = 2500000 / ((double)(GetTimeMillis() - nStartTime));

    nStartTime = GetTimeMillis();
    crypter.SetKeyFromPassphrase(strWalletPassphrase, kMasterKey.vchSalt, kMasterKey.nDeriveIterations, kMasterKey.nDerivationMethod);
    kMasterKey.nDeriveIterations = (kMasterKey.nDeriveIterations + kMasterKey.nDeriveIterations * 100 / ((double)(GetTimeMillis() - nStartTime))) / 2;

    if (kMasterKey.nDeriveIterations < 25000)
        kMasterKey.nDeriveIterations = 25000;

    printf("Encrypting Wallet with an nDeriveIterations of %i\n", kMasterKey.nDeriveIterations);

    if (!crypter.SetKeyFromPassphrase(strWalletPassphrase, kMasterKey.vchSalt, kMasterKey.nDeriveIterations, kMasterKey.nDerivationMethod))
        return false;
    if (!crypter.Encrypt(vMasterKey, kMasterKey.vchCryptedKey))
        return false;

    CRITICAL_BLOCK(cs_wallet)
    {
        mapMasterKeys[++nMasterKeyMaxID] = kMasterKey;
        if (fFileBacked)
        {
            pwalletdbEncryption = new CWalletDB(_dataDir, strWalletFile);
            pwalletdbEncryption->TxnBegin();
            pwalletdbEncryption->WriteMasterKey(nMasterKeyMaxID, kMasterKey);
        }

        if (!EncryptKeys(vMasterKey))
        {
            if (fFileBacked)
                pwalletdbEncryption->TxnAbort();
            exit(1); //We now probably have half of our keys encrypted in memory, and half not...die and let the user reload their unencrypted wallet.
        }

        if (fFileBacked)
        {
            CCorruptAddress corruptAddress;
            pwalletdbEncryption->WriteSetting("addrIncoming", corruptAddress);
            if (!pwalletdbEncryption->TxnCommit())
                exit(1); //We now have keys encrypted in memory, but no on disk...die to avoid confusion and let the user reload their unencrypted wallet.

            pwalletdbEncryption->Close();
            pwalletdbEncryption = NULL;
        }

        Lock();
    }

    return true;
}

void Wallet::WalletUpdateSpent(const Transaction &tx)
{
    // Anytime a signature is successfully verified, it's proof the outpoint is spent.
    // Update the wallet spent flag if it doesn't know due to wallet.dat being
    // restored from backup or the user making copies of wallet.dat.
    CRITICAL_BLOCK(cs_wallet)
    {
        BOOST_FOREACH(const CTxIn& txin, tx.vin)
        {
            map<uint256, CWalletTx>::iterator mi = mapWallet.find(txin.prevout.hash);
            if (mi != mapWallet.end())
            {
                CWalletTx& wtx = (*mi).second;
                if (!wtx.IsSpent(txin.prevout.n) && IsMine(wtx.vout[txin.prevout.n]))
                {
                    printf("WalletUpdateSpent found spent coin %sbc %s\n", FormatMoney(wtx.GetCredit()).c_str(), wtx.GetHash().toString().c_str());
                    wtx.MarkSpent(txin.prevout.n);
                    wtx.WriteToDisk();
                    vWalletUpdated.push_back(txin.prevout.hash);
                }
            }
        }
    }
}

bool Wallet::AddToWallet(const CWalletTx& wtxIn)
{
    uint256 hash = wtxIn.GetHash();
    CRITICAL_BLOCK(cs_wallet)
    {
        // Inserts only if not already there, returns tx inserted or tx found
        pair<map<uint256, CWalletTx>::iterator, bool> ret = mapWallet.insert(make_pair(hash, wtxIn));
        CWalletTx& wtx = (*ret.first).second;
        wtx.pwallet = this;
        bool fInsertedNew = ret.second;
        if (fInsertedNew)
            wtx.nTimeReceived = GetAdjustedTime();

        bool fUpdated = false;
        if (!fInsertedNew)
        {
            // Merge
            if (wtxIn._blockHash != 0 && wtxIn._blockHash != wtx._blockHash)
            {
                wtx._blockHash = wtxIn._blockHash;
                fUpdated = true;
            }
            if (wtxIn._index != -1 && (wtxIn._merkleBranch != wtx._merkleBranch || wtxIn._index != wtx._index))
            {
                wtx._merkleBranch = wtxIn._merkleBranch;
                wtx._index = wtxIn._index;
                fUpdated = true;
            }
            if (wtxIn.fFromMe && wtxIn.fFromMe != wtx.fFromMe)
            {
                wtx.fFromMe = wtxIn.fFromMe;
                fUpdated = true;
            }
            fUpdated |= wtx.UpdateSpent(wtxIn.vfSpent);
        }

        //// debug print
        printf("AddToWallet %s  %s%s\n", wtxIn.GetHash().toString().substr(0,10).c_str(), (fInsertedNew ? "new" : ""), (fUpdated ? "update" : ""));

        // Write to disk
        if (fInsertedNew || fUpdated)
            if (!wtx.WriteToDisk())
                return false;

        // If default receiving address gets used, replace it with a new one
        CScript scriptDefaultKey;
        scriptDefaultKey.SetChainAddress(_blockChain.chain().networkId(), vchDefaultKey);
        BOOST_FOREACH(const CTxOut& txout, wtx.vout)
        {
            if (txout.scriptPubKey == scriptDefaultKey)
            {
                std::vector<unsigned char> newDefaultKey;
                if (GetKeyFromPool(newDefaultKey, false))
                {
                    SetDefaultKey(newDefaultKey);
                    SetAddressBookName(ChainAddress(_blockChain.chain().networkId(), vchDefaultKey), "");
                }
            }
        }

        // Notify UI
        vWalletUpdated.push_back(hash);

        // since AddToWallet is called directly for self-originating transactions, check for consumption of own coins
        WalletUpdateSpent(wtx);
    }

    // Refresh UI
    //    MainFrameRepaint();
    return true;
}

bool Wallet::AddToWalletIfInvolvingMe(const Transaction& tx, const Block* pblock, bool fUpdate)
{
    uint256 hash = tx.GetHash();
    CRITICAL_BLOCK(cs_wallet)
    {
        bool fExisted = mapWallet.count(hash);
        if (fExisted && !fUpdate) return false;
        if (fExisted || IsMine(tx) || IsFromMe(tx))
        {
            CWalletTx wtx(this,tx);
            // Get merkle branch if transaction was found in a block
            if (pblock && !pblock->isNull())
                wtx.setMerkleBranch(*pblock, _blockChain);
            return AddToWallet(wtx);
        }
        else
            WalletUpdateSpent(tx);
    }
    return false;
}

bool Wallet::EraseFromWallet(uint256 hash)
{
    if (!fFileBacked)
        return false;
    CRITICAL_BLOCK(cs_wallet)
    {
        if (mapWallet.erase(hash))
            CWalletDB(_dataDir, strWalletFile).EraseTx(hash);
    }
    return true;
}


bool Wallet::IsMine(const CTxIn &txin) const
{
    CRITICAL_BLOCK(cs_wallet)
    {
        map<uint256, CWalletTx>::const_iterator mi = mapWallet.find(txin.prevout.hash);
        if (mi != mapWallet.end())
        {
            const CWalletTx& prev = (*mi).second;
            if (txin.prevout.n < prev.vout.size())
                if (IsMine(prev.vout[txin.prevout.n]))
                    return true;
        }
    }
    return false;
}

int64 Wallet::GetDebit(const CTxIn &txin) const
{
    CRITICAL_BLOCK(cs_wallet)
    {
        map<uint256, CWalletTx>::const_iterator mi = mapWallet.find(txin.prevout.hash);
        if (mi != mapWallet.end())
        {
            const CWalletTx& prev = (*mi).second;
            if (txin.prevout.n < prev.vout.size())
                if (IsMine(prev.vout[txin.prevout.n]))
                    return prev.vout[txin.prevout.n].nValue;
        }
    }
    return 0;
}

bool Wallet::IsConfirmed(const CWalletTx& tx) const
{
    // Quick answer in most cases
    if (!_blockChain.isFinal(tx))
        return false;
    if (_blockChain.getDepthInMainChain(tx.GetHash()) >= 1)
        return true;
    if (!IsFromMe(tx)) // using wtx's cached debit
        return false;
    
    // If no confirmations but it's from us, we can still
    // consider it confirmed if all dependencies are confirmed
    std::map<uint256, const CMerkleTx*> mapPrev;
    std::vector<const CMerkleTx*> vWorkQueue;
    vWorkQueue.reserve(tx.vtxPrev.size()+1);
    vWorkQueue.push_back(&tx);
    for (int i = 0; i < vWorkQueue.size(); i++)
        {
        const CMerkleTx* ptx = vWorkQueue[i];
        
        if (!_blockChain.isFinal(*ptx))
            return false;
        if (_blockChain.getDepthInMainChain(ptx->GetHash()) >= 1)
            continue;
        if (!IsFromMe(*ptx))
            return false;
        
        if (mapPrev.empty())
            BOOST_FOREACH(const CMerkleTx& mtx, tx.vtxPrev)
                mapPrev[tx.GetHash()] = &mtx;
        
        BOOST_FOREACH(const CTxIn& txin, ptx->vin)
            {
            if (!mapPrev.count(txin.prevout.hash))
                return false;
            vWorkQueue.push_back(mapPrev[txin.prevout.hash]);
            }
        }
    return true;
}

/*
void Wallet::Sync() // sync from the start!
{
    CTxDB txdb;
    
    CRITICAL_BLOCK(cs_wallet)
    {
        for(KeyMap::iterator key = mapKeys.begin(); key != mapKeys.end(); ++key)
        {
            cout << key->first.toString() << endl;
            set<std::pair<uint256, unsigned int> > debit;
            txdb.ReadDrIndex((key->first).GetHash160(), debit);
            for (set<std::pair<uint256, unsigned int> >::iterator pair = debit.begin(); pair != debit.end(); ++pair) {
                Transaction tx;
                txdb.ReadDiskTx(pair->first, tx);
                cout << "\t" << pair->first.toString() << " - " << pair->second << endl;
                AddToWalletIfInvolvingMe(tx, NULL);
            }
            set<std::pair<uint256, unsigned int> > credit;
            txdb.ReadCrIndex((key->first).GetHash160(), credit);
            for (set<std::pair<uint256, unsigned int> >::iterator pair = credit.begin(); pair != credit.end(); ++pair) {
                Transaction tx;
                txdb.ReadDiskTx(pair->first, tx);
                cout << "\t" << pair->first.toString() << " - " << pair->second << endl;
                AddToWalletIfInvolvingMe(tx, NULL);
            }
        }
    }    
}
*/


int Wallet::ScanForWalletTransactions(const CBlockIndex* pindexStart, bool fUpdate)
{
    int ret = 0;

    const CBlockIndex* pindex = (pindexStart == NULL) ? _blockChain.getBlockIndex(_blockChain.getGenesisHash()) : pindexStart;
    CRITICAL_BLOCK(cs_wallet) {
        while (pindex) {
            Block block;
            _blockChain.getBlock(pindex, block);
        
            TransactionList txes = block.getTransactions();
            for(TransactionList::const_iterator tx = txes.begin(); tx != txes.end(); ++tx) {
                if (AddToWalletIfInvolvingMe(*tx, &block, fUpdate))
                    ret++;
            }
            pindex = pindex->pnext;
        }
    }
    return ret;
}


void Wallet::ReacceptWalletTransactions()
{
    //    CTxDB txdb("r");
    bool fRepeat = true;
    while (fRepeat) CRITICAL_BLOCK(cs_wallet) {
        fRepeat = false;
        vector<uint256> vMissingTx;
        BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, mapWallet) {
            CWalletTx& wtx = item.second;
            if (wtx.IsCoinBase() && wtx.IsSpent(0))
                continue;

            uint256 hash = wtx.GetHash();
            // figure out if this tx has been spend from somewhere else - so ask the blockChain if it is spent
        
            bool fUpdated = false;
            
            int spents = _blockChain.getNumSpent(hash);
            if(spents >= 0) {
                if(spents != wtx.vout.size()) {
                    printf("ERROR: ReacceptWalletTransactions() : txindex.vSpent.size() %d != wtx.vout.size() %d\n", spents, wtx.vout.size());
                    continue;
                }
            
                for (int i = 0; i < spents; i++) {
                    if (wtx.IsSpent(i))
                        continue;
                    Coin coin(hash, i);
                    if (!_blockChain.isSpent(coin) && IsMine(wtx.vout[i])) {
                        wtx.MarkSpent(i);
                        fUpdated = true;
                        vMissingTx.push_back(_blockChain.spentIn(coin));
                    }
                }
                if (fUpdated) {
                    printf("ReacceptWalletTransactions found spent coin %sbc %s\n", FormatMoney(wtx.GetCredit()).c_str(), wtx.GetHash().toString().c_str());
                    wtx.MarkDirty();
                    wtx.WriteToDisk();
                }
            
            }
            else {
                // Reaccept any txes of ours that aren't already in a block
                if (!wtx.IsCoinBase()) {
                    const Transaction tx(wtx);
                    acceptTransaction(tx);                
                }
                //                    _blockChain.acceptTransaction(wtx);
                //                    wtx.AcceptWalletTransaction(txdb, false);
            }
        }
        if (!vMissingTx.empty())
        {
            // TODO: optimize this to scan just part of the block chain?
            if (ScanForWalletTransactions(_blockChain.getBlockIndex(_blockChain.chain().genesisHash())))
                fRepeat = true;  // Found missing transactions: re-do Reaccept.
        }
    }
}

// This will be polled from the TransactionFilter infrequently
void Wallet::ResendWalletTransactions(set<uint256>& hashes) {
    // Do this infrequently and randomly to avoid giving away
    // that these are our transactions.
    static int64 nNextTime;
    if (GetTime() < nNextTime)
        return;
    bool fFirst = (nNextTime == 0);
    nNextTime = GetTime() + GetRand(30 * 60);
    if (fFirst)
        return;

    // Only do it if there's been a new block since last time
    static int64 nLastTime;
    if (_blockChain.getBestReceivedTime() < nLastTime)
        return;
    nLastTime = GetTime();

    // Rebroadcast any of our txes that aren't in a block yet
    printf("ResendWalletTransactions()\n");
    //    CTxDB txdb("r");
    CRITICAL_BLOCK(cs_wallet) {
        // Sort them in chronological order
        multimap<unsigned int, CWalletTx*> mapSorted;
        BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, mapWallet) {
            CWalletTx& wtx = item.second;
            // Don't rebroadcast until it's had plenty of time that
            // it should have gotten in already by now.
            if (_blockChain.getBestReceivedTime() - (int64)wtx.nTimeReceived > 5 * 60)
                mapSorted.insert(make_pair(wtx.nTimeReceived, &wtx));
        }
        BOOST_FOREACH(PAIRTYPE(const unsigned int, CWalletTx*)& item, mapSorted) {
            CWalletTx& wtx = *item.second;
            /*
             BOOST_FOREACH(const CMerkleTx& tx, vtxPrev)
             {
             if (!tx.IsCoinBase())
             {
             uint256 hash = tx.GetHash();
             if (!txdb.ContainsTx(hash))
             RelayMessage(Inventory(MSG_TX, hash), (Transaction)tx);
             }
             }
             if (!IsCoinBase())
             {
             uint256 hash = GetHash();
             if (!txdb.ContainsTx(hash))
             {
             printf("Relaying wtx %s\n", hash.toString().substr(0,10).c_str());
             RelayMessage(Inventory(MSG_TX, hash), (Transaction)*this);
             }
             }
             */
            const uint256 hash = wtx.GetHash();
            hashes.insert(hash);
            //            wtx.RelayWalletTransaction(txdb);
        }
    }
}

bool Wallet::WriteToDisk(const CWalletTx& wtx) {
    return CWalletDB(_dataDir, strWalletFile).WriteTx(wtx.GetHash(), wtx);
}

//////////////////////////////////////////////////////////////////////////////
//
// Actions
//


int64 Wallet::GetBalance(bool confirmed) const
{
    int64 nTotal = 0;
    CRITICAL_BLOCK(cs_wallet)
    {
        for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
        {
            const CWalletTx* pcoin = &(*it).second;
            if (confirmed && !IsConfirmed(*pcoin))
                continue;
            nTotal += pcoin->GetAvailableCredit();
        }
    }

    return nTotal;
}


bool Wallet::SelectCoinsMinConf(int64 nTargetValue, int nConfMine, int nConfTheirs, set<pair<const CWalletTx*,unsigned int> >& setCoinsRet, int64& nValueRet) const
{
    setCoinsRet.clear();
    nValueRet = 0;

    // List of values less than target
    pair<int64, pair<const CWalletTx*,unsigned int> > coinLowestLarger;
    coinLowestLarger.first = INT64_MAX;
    coinLowestLarger.second.first = NULL;
    vector<pair<int64, pair<const CWalletTx*,unsigned int> > > vValue;
    int64 nTotalLower = 0;

    CRITICAL_BLOCK(cs_wallet)
    {
       vector<const CWalletTx*> vCoins;
       vCoins.reserve(mapWallet.size());
       for (map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
           vCoins.push_back(&(*it).second);
       random_shuffle(vCoins.begin(), vCoins.end(), GetRandInt);

       BOOST_FOREACH(const CWalletTx* pcoin, vCoins)
       {
            if (!_blockChain.isFinal(*pcoin) || !IsConfirmed(*pcoin))
                continue;

            if (pcoin->IsCoinBase() && _blockChain.getBlocksToMaturity(*pcoin) > 0)
                continue;

       int nDepth = _blockChain.getDepthInMainChain(pcoin->GetHash());
            if (nDepth < (pcoin->IsFromMe() ? nConfMine : nConfTheirs))
                continue;

            for (int i = 0; i < pcoin->vout.size(); i++)
            {
                if (pcoin->IsSpent(i) || !IsMine(pcoin->vout[i]))
                    continue;

                int64 n = pcoin->vout[i].nValue;

                if (n <= 0)
                    continue;

                pair<int64,pair<const CWalletTx*,unsigned int> > coin = make_pair(n,make_pair(pcoin,i));

                if (n == nTargetValue)
                {
                    setCoinsRet.insert(coin.second);
                    nValueRet += coin.first;
                    return true;
                }
                else if (n < nTargetValue + CENT)
                {
                    vValue.push_back(coin);
                    nTotalLower += n;
                }
                else if (n < coinLowestLarger.first)
                {
                    coinLowestLarger = coin;
                }
            }
        }
    }

    if (nTotalLower == nTargetValue || nTotalLower == nTargetValue + CENT)
    {
        for (int i = 0; i < vValue.size(); ++i)
        {
            setCoinsRet.insert(vValue[i].second);
            nValueRet += vValue[i].first;
        }
        return true;
    }

    if (nTotalLower < nTargetValue + (coinLowestLarger.second.first ? CENT : 0))
    {
        if (coinLowestLarger.second.first == NULL)
            return false;
        setCoinsRet.insert(coinLowestLarger.second);
        nValueRet += coinLowestLarger.first;
        return true;
    }

    if (nTotalLower >= nTargetValue + CENT)
        nTargetValue += CENT;

    // Solve subset sum by stochastic approximation
    sort(vValue.rbegin(), vValue.rend());
    vector<char> vfIncluded;
    vector<char> vfBest(vValue.size(), true);
    int64 nBest = nTotalLower;

    for (int nRep = 0; nRep < 1000 && nBest != nTargetValue; nRep++)
    {
        vfIncluded.assign(vValue.size(), false);
        int64 nTotal = 0;
        bool fReachedTarget = false;
        for (int nPass = 0; nPass < 2 && !fReachedTarget; nPass++)
        {
            for (int i = 0; i < vValue.size(); i++)
            {
                if (nPass == 0 ? rand() % 2 : !vfIncluded[i])
                {
                    nTotal += vValue[i].first;
                    vfIncluded[i] = true;
                    if (nTotal >= nTargetValue)
                    {
                        fReachedTarget = true;
                        if (nTotal < nBest)
                        {
                            nBest = nTotal;
                            vfBest = vfIncluded;
                        }
                        nTotal -= vValue[i].first;
                        vfIncluded[i] = false;
                    }
                }
            }
        }
    }

    // If the next larger is still closer, return it
    if (coinLowestLarger.second.first && coinLowestLarger.first - nTargetValue <= nBest - nTargetValue)
    {
        setCoinsRet.insert(coinLowestLarger.second);
        nValueRet += coinLowestLarger.first;
    }
    else {
        for (int i = 0; i < vValue.size(); i++)
            if (vfBest[i])
            {
                setCoinsRet.insert(vValue[i].second);
                nValueRet += vValue[i].first;
            }

        //// debug print
        printf("SelectCoins() best subset: ");
        for (int i = 0; i < vValue.size(); i++)
            if (vfBest[i])
                printf("%s ", FormatMoney(vValue[i].first).c_str());
        printf("total %s\n", FormatMoney(nBest).c_str());
    }

    return true;
}

bool Wallet::SelectCoins(int64 nTargetValue, set<pair<const CWalletTx*,unsigned int> >& setCoinsRet, int64& nValueRet) const
{
    return (SelectCoinsMinConf(nTargetValue, 1, 6, setCoinsRet, nValueRet) ||
            SelectCoinsMinConf(nTargetValue, 1, 1, setCoinsRet, nValueRet) ||
            SelectCoinsMinConf(nTargetValue, 0, 1, setCoinsRet, nValueRet));
}




bool Wallet::CreateTransaction(const vector<pair<CScript, int64> >& vecSend, CWalletTx& wtxNew, CReserveKey& reservekey, int64& nFeeRet)
{
    int64 nValue = 0;
    BOOST_FOREACH (const PAIRTYPE(CScript, int64)& s, vecSend)
    {
        if (nValue < 0)
            return false;
        nValue += s.second;
    }
    if (vecSend.empty() || nValue < 0)
        return false;

    wtxNew.pwallet = this;

    CRITICAL_BLOCK(cs_wallet)
    {
        // txdb must be opened before the mapWallet lock
        //        CTxDB txdb("r");
        {
            nFeeRet = nTransactionFee;
            loop
            {
                wtxNew.vin.clear();
                wtxNew.vout.clear();
                wtxNew.fFromMe = true;

                int64 nTotalValue = nValue + nFeeRet;
                double dPriority = 0;
                // vouts to the payees
                BOOST_FOREACH (const PAIRTYPE(CScript, int64)& s, vecSend)
                    wtxNew.vout.push_back(CTxOut(s.second, s.first));

                // Choose coins to use
                set<pair<const CWalletTx*,unsigned int> > setCoins;
                int64 nValueIn = 0;
                if (!SelectCoins(nTotalValue, setCoins, nValueIn))
                    return false;
                BOOST_FOREACH(PAIRTYPE(const CWalletTx*, unsigned int) pcoin, setCoins)
                {
                    int64 nCredit = pcoin.first->vout[pcoin.second].nValue;
                    dPriority += (double)nCredit * _blockChain.getDepthInMainChain(pcoin.first->GetHash());
                }

                int64 nChange = nValueIn - nValue - nFeeRet;
                // if sub-cent change is required, the fee must be raised to at least MIN_TX_FEE
                // or until nChange becomes zero
                if (nFeeRet < MIN_TX_FEE && nChange > 0 && nChange < CENT)
                {
                    int64 nMoveToFee = min(nChange, MIN_TX_FEE - nFeeRet);
                    nChange -= nMoveToFee;
                    nFeeRet += nMoveToFee;
                }

                if (nChange > 0)
                {
                    // Note: We use a new key here to keep it from being obvious which side is the change.
                    //  The drawback is that by not reusing a previous key, the change may be lost if a
                    //  backup is restored, if the backup doesn't have the new private key for the change.
                    //  If we reused the old key, it would be possible to add code to look for and
                    //  rediscover unknown transactions that were written with keys of ours to recover
                    //  post-backup change.

                    // Reserve a new key pair from key pool
                    vector<unsigned char> vchPubKey = reservekey.GetReservedKey();
                    // assert(mapKeys.count(vchPubKey));

                    // Fill a vout to ourself, using same address type as the payment
                    CScript scriptChange;
                    if (vecSend[0].first.GetChainAddress(_blockChain.chain().networkId()).isValid(_blockChain.chain().networkId()))
                        scriptChange.SetChainAddress(_blockChain.chain().networkId(),vchPubKey);
                    else
                        scriptChange << vchPubKey << OP_CHECKSIG;

                    // Insert change txn at random position:
                    vector<CTxOut>::iterator position = wtxNew.vout.begin()+GetRandInt(wtxNew.vout.size());
                    wtxNew.vout.insert(position, CTxOut(nChange, scriptChange));
                }
                else
                    reservekey.ReturnKey();

                // Fill vin
                BOOST_FOREACH(const PAIRTYPE(const CWalletTx*,unsigned int)& coin, setCoins)
                    wtxNew.vin.push_back(CTxIn(coin.first->GetHash(),coin.second));

                // Sign
                int nIn = 0;
                BOOST_FOREACH(const PAIRTYPE(const CWalletTx*,unsigned int)& coin, setCoins)
                    if (!SignSignature(*this, *coin.first, wtxNew, nIn++))
                        return false;

                // Limit size
                unsigned int nBytes = ::GetSerializeSize(*(Transaction*)&wtxNew, SER_NETWORK);
                if (nBytes >= MAX_BLOCK_SIZE_GEN/5)
                    return false;
                dPriority /= nBytes;

                // Check that enough fee is included
                int64 nPayFee = nTransactionFee * (1 + (int64)nBytes / 1000);
                bool fAllowFree = Transaction::AllowFree(dPriority);
                int64 nMinFee = wtxNew.GetMinFee(1, fAllowFree);
                if (nFeeRet < max(nPayFee, nMinFee))
                {
                    nFeeRet = max(nPayFee, nMinFee);
                    continue;
                }

                // Fill vtxPrev by copying from previous transactions vtxPrev
                wtxNew.AddSupportingTransactions(_blockChain);
                wtxNew.fTimeReceivedIsTxTime = true;

                break;
            }
        }
    }
    return true;
}

bool Wallet::CreateTransaction(CScript scriptPubKey, int64 nValue, CWalletTx& wtxNew, CReserveKey& reservekey, int64& nFeeRet)
{
    vector< pair<CScript, int64> > vecSend;
    vecSend.push_back(make_pair(scriptPubKey, nValue));
    return CreateTransaction(vecSend, wtxNew, reservekey, nFeeRet);
}

// Call after CreateTransaction unless you want to abort
bool Wallet::CommitTransaction(CWalletTx& wtxNew, CReserveKey& reservekey)
{
    CRITICAL_BLOCK(cs_wallet)
    {
        printf("CommitTransaction:\n%s", wtxNew.toString().c_str());
        {
            // This is only to keep the database open to defeat the auto-flush for the
            // duration of this scope.  This is the only place where this optimization
            // maybe makes sense; please don't do it anywhere else.
            CWalletDB* pwalletdb = fFileBacked ? new CWalletDB(strWalletFile,"r") : NULL;

            // Take key pair from key pool so it won't be used again
            reservekey.KeepKey();

            // Add tx to wallet, because if it has change it's also ours,
            // otherwise just for transaction history.
            AddToWallet(wtxNew);

            // Mark old coins as spent
            set<CWalletTx*> setCoins;
            BOOST_FOREACH(const CTxIn& txin, wtxNew.vin)
            {
                CWalletTx &coin = mapWallet[txin.prevout.hash];
                coin.pwallet = this;
                coin.MarkSpent(txin.prevout.n);
                coin.WriteToDisk();
                vWalletUpdated.push_back(coin.GetHash());
            }

            if (fFileBacked)
                delete pwalletdb;
        }

        // Track how many getdata requests our transaction gets
        mapRequestCount[wtxNew.GetHash()] = 0;

        // Broadcast
        if (!acceptTransaction(wtxNew)) {
            // This must not fail. The transaction has already been signed and recorded.
            printf("CommitTransaction() : Error: Transaction not valid");
            return false;
        }
    //        wtxNew.RelayWalletTransaction(); // this is included in the acceptTransaction
    }

    return true;
}

// this is ugly - again we have calls to GUI methods from within the code code - need to make some call back structure
inline bool ThreadSafeAskFee(int64 nFeeRequired, const std::string& strCaption, void* parent)
{
    return true;
}


string Wallet::SendMoney(CScript scriptPubKey, int64 nValue, CWalletTx& wtxNew, bool fAskFee)
{
    CReserveKey reservekey(this);
    int64 nFeeRequired;

    if (IsLocked())
    {
        string strError = _("Error: Wallet locked, unable to create transaction  ");
        printf("SendMoney() : %s", strError.c_str());
        return strError;
    }
    if (!CreateTransaction(scriptPubKey, nValue, wtxNew, reservekey, nFeeRequired))
    {
        string strError;
        if (nValue + nFeeRequired > GetBalance())
            strError = strprintf(_("Error: This transaction requires a transaction fee of at least %s because of its amount, complexity, or use of recently received funds  "), FormatMoney(nFeeRequired).c_str());
        else
            strError = _("Error: Transaction creation failed  ");
        printf("SendMoney() : %s", strError.c_str());
        return strError;
    }

    if (fAskFee && !ThreadSafeAskFee(nFeeRequired, _("Sending..."), NULL))
        return "ABORTED";

    if (!CommitTransaction(wtxNew, reservekey))
        return _("Error: The transaction was rejected.  This might happen if some of the coins in your wallet were already spent, such as if you used a copy of wallet.dat and coins were spent in the copy but not marked as spent here.");

    return "";
}



string Wallet::SendMoneyToBitcoinAddress(const ChainAddress& address, int64 nValue, CWalletTx& wtxNew, bool fAskFee)
{
    // Check amount
    if (nValue <= 0)
        return _("Invalid amount");
    if (nValue + nTransactionFee > GetBalance())
        return _("Insufficient funds");

    // Parse bitcoin address
    CScript scriptPubKey;
    scriptPubKey.SetChainAddress(address);

    return SendMoney(scriptPubKey, nValue, wtxNew, fAskFee);
}

void Wallet::SetBestChain(const CBlockLocator& loc)
{
    CWalletDB walletdb(_dataDir, strWalletFile);
    walletdb.WriteBestBlock(loc);
}

int Wallet::LoadWallet(bool& fFirstRunRet)
{
    if (!fFileBacked)
        return false;
    fFirstRunRet = false;
    int nLoadWalletRet = CWalletDB(_dataDir, strWalletFile, "cr+").LoadWallet(this);
    if (nLoadWalletRet != DB_LOAD_OK)
        return nLoadWalletRet;
    fFirstRunRet = vchDefaultKey.empty();

    if (!HaveKey(ChainAddress(_blockChain.chain().networkId(), Hash160(vchDefaultKey))))
    {
        // Create new keyUser and set as default key
        RandAddSeedPerfmon();

        std::vector<unsigned char> newDefaultKey;
        if (!GetKeyFromPool(newDefaultKey, false))
            return DB_LOAD_FAIL;
        SetDefaultKey(newDefaultKey);
        if (!SetAddressBookName(ChainAddress(_blockChain.chain().networkId(), vchDefaultKey), ""))
            return DB_LOAD_FAIL;
    }

    //    CreateThread(ThreadFlushWalletDB, &strWalletFile);
    return DB_LOAD_OK;
}


bool Wallet::SetAddressBookName(const ChainAddress& address, const string& strName)
{
    mapAddressBook[address] = strName;
    if (!fFileBacked)
        return false;
    return CWalletDB(_dataDir, strWalletFile).WriteName(address.toString(), strName);
}

bool Wallet::DelAddressBookName(const ChainAddress& address)
{
    mapAddressBook.erase(address);
    if (!fFileBacked)
        return false;
    return CWalletDB(_dataDir, strWalletFile).EraseName(address.toString());
}


void Wallet::PrintWallet(const Block& block)
{
    CRITICAL_BLOCK(cs_wallet)
    {
        if (mapWallet.count(block.getTransactions()[0].GetHash()))
        {
            CWalletTx& wtx = mapWallet[block.getTransactions()[0].GetHash()];
            printf("    mine:  %d  %d  %d", _blockChain.getDepthInMainChain(wtx.GetHash()), _blockChain.getBlocksToMaturity(wtx), wtx.GetCredit());
        }
    }
    printf("\n");
}

bool Wallet::GetTransaction(const uint256 &hashTx, CWalletTx& wtx)
{
    CRITICAL_BLOCK(cs_wallet)
    {
        map<uint256, CWalletTx>::iterator mi = mapWallet.find(hashTx);
        if (mi != mapWallet.end())
        {
            wtx = (*mi).second;
            return true;
        }
    }
    return false;
}

bool Wallet::SetDefaultKey(const std::vector<unsigned char> &vchPubKey)
{
    if (fFileBacked)
    {
        if (!CWalletDB(_dataDir, strWalletFile).WriteDefaultKey(vchPubKey))
            return false;
    }
    vchDefaultKey = vchPubKey;
    return true;
}

bool GetWalletFile(Wallet* pwallet, string &strWalletFileOut)
{
    if (!pwallet->fFileBacked)
        return false;
    strWalletFileOut = pwallet->strWalletFile;
    return true;
}

bool Wallet::TopUpKeyPool()
{
    CRITICAL_BLOCK(cs_wallet)
    {
        if (IsLocked())
            return false;

        CWalletDB walletdb(_dataDir, strWalletFile);

        // Top up key pool
        int64 nTargetSize = max(GetArg("-keypool", 100), (int64)0);
        while (setKeyPool.size() < nTargetSize+1)
        {
            int64 nEnd = 1;
            if (!setKeyPool.empty())
                nEnd = *(--setKeyPool.end()) + 1;
            if (!walletdb.WritePool(nEnd, CKeyPool(GenerateNewKey())))
                throw runtime_error("TopUpKeyPool() : writing generated key failed");
            setKeyPool.insert(nEnd);
            printf("keypool added key %"PRI64d", size=%d\n", nEnd, setKeyPool.size());
        }
    }
    return true;
}

void Wallet::ReserveKeyFromKeyPool(int64& nIndex, CKeyPool& keypool)
{
    nIndex = -1;
    keypool.vchPubKey.clear();
    CRITICAL_BLOCK(cs_wallet)
    {
        if (!IsLocked())
            TopUpKeyPool();

        // Get the oldest key
        if(setKeyPool.empty())
            return;

        CWalletDB walletdb(_dataDir, strWalletFile);

        nIndex = *(setKeyPool.begin());
        setKeyPool.erase(setKeyPool.begin());
        if (!walletdb.ReadPool(nIndex, keypool))
            throw runtime_error("ReserveKeyFromKeyPool() : read failed");
        if (!HaveKey(ChainAddress(_blockChain.chain().networkId(), Hash160(keypool.vchPubKey))))
            throw runtime_error("ReserveKeyFromKeyPool() : unknown key in key pool");
        assert(!keypool.vchPubKey.empty());
        printf("keypool reserve %"PRI64d"\n", nIndex);
    }
}

void Wallet::KeepKey(int64 nIndex)
{
    // Remove from key pool
    if (fFileBacked)
    {
        CWalletDB walletdb(_dataDir, strWalletFile);
        walletdb.ErasePool(nIndex);
    }
    printf("keypool keep %"PRI64d"\n", nIndex);
}

void Wallet::ReturnKey(int64 nIndex)
{
    // Return to key pool
    CRITICAL_BLOCK(cs_wallet)
        setKeyPool.insert(nIndex);
    printf("keypool return %"PRI64d"\n", nIndex);
}

bool Wallet::GetKeyFromPool(vector<unsigned char>& result, bool fAllowReuse)
{
    int64 nIndex = 0;
    CKeyPool keypool;
    CRITICAL_BLOCK(cs_wallet)
    {
        ReserveKeyFromKeyPool(nIndex, keypool);
        if (nIndex == -1)
        {
            if (fAllowReuse && !vchDefaultKey.empty())
            {
                result = vchDefaultKey;
                return true;
            }
            if (IsLocked()) return false;
            result = GenerateNewKey();
            return true;
        }
        KeepKey(nIndex);
        result = keypool.vchPubKey;
    }
    return true;
}

int64 Wallet::GetOldestKeyPoolTime()
{
    int64 nIndex = 0;
    CKeyPool keypool;
    ReserveKeyFromKeyPool(nIndex, keypool);
    if (nIndex == -1)
        return GetTime();
    ReturnKey(nIndex);
    return keypool.nTime;
}

void TransactionListener::operator()(const Transaction& tx) {
    // sync with wallet
    _wallet.transactionAccepted(tx);
}

void BlockListener::operator()(const Block& blk) {
    // sync with wallet
    _wallet.blockAccepted(blk);
}

void TransactionReminder::operator()(std::set<uint256>& hashes) {
    // Fill the hashes with transactions not already in a blocks and return.
    _wallet.ResendWalletTransactions(hashes);
}



vector<unsigned char> CReserveKey::GetReservedKey()
{
    if (nIndex == -1)
    {
        CKeyPool keypool;
        pwallet->ReserveKeyFromKeyPool(nIndex, keypool);
        if (nIndex != -1)
            vchPubKey = keypool.vchPubKey;
        else
        {
            printf("CReserveKey::GetReservedKey(): Warning: using default key instead of a new key, top up your keypool.");
            vchPubKey = pwallet->vchDefaultKey;
        }
    }
    assert(!vchPubKey.empty());
    return vchPubKey;
}

void CReserveKey::KeepKey()
{
    if (nIndex != -1)
        pwallet->KeepKey(nIndex);
    nIndex = -1;
    vchPubKey.clear();
}

void CReserveKey::ReturnKey()
{
    if (nIndex != -1)
        pwallet->ReturnKey(nIndex);
    nIndex = -1;
    vchPubKey.clear();
}


