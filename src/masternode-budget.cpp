
#include "src/masternode-budget.h"

void CBudgetManager::MarkSynced()
{
    LOCK(cs);

    /*
        Mark that we've sent all valid items
    */

    std::map<uint256, CFinalizedBudgetBroadcast>::iterator it3 = mapSeenFinalizedBudgets.begin();
    while(it3 != mapSeenFinalizedBudgets.end()){
        CFinalizedBudget* pfinalizedBudget = FindFinalizedBudget((*it3).first);
        if(pfinalizedBudget && pfinalizedBudget->fValid){

            //mark votes
            std::map<uint256, CFinalizedBudgetVote>::iterator it4 = pfinalizedBudget->mapVotes.begin();
            while(it4 != pfinalizedBudget->mapVotes.end()){
                if((*it4).second.fValid)
                    (*it4).second.fSynced = true;
                ++it4;
            }
        }
        ++it3;
    }

}

//mark that a full sync is needed
void CBudgetManager::ResetSync()
{
    LOCK(cs);


    std::map<uint256, CBudgetProposalBroadcast>::iterator it1 = mapSeenMasternodeBudgetProposals.begin();
    while(it1 != mapSeenMasternodeBudgetProposals.end()){
        CBudgetProposal* pbudgetProposal = FindProposal((*it1).first);
        if(pbudgetProposal && pbudgetProposal->fValid){
        
            //mark votes
            std::map<uint256, CBudgetVote>::iterator it2 = pbudgetProposal->mapVotes.begin();
            while(it2 != pbudgetProposal->mapVotes.end()){
                (*it2).second.fSynced = false;
                ++it2;
            }
        }
        ++it1;
    }

    std::map<uint256, CFinalizedBudgetBroadcast>::iterator it3 = mapSeenFinalizedBudgets.begin();
    while(it3 != mapSeenFinalizedBudgets.end()){
        CFinalizedBudget* pfinalizedBudget = FindFinalizedBudget((*it3).first);
        if(pfinalizedBudget && pfinalizedBudget->fValid){

            //send votes
            std::map<uint256, CFinalizedBudgetVote>::iterator it4 = pfinalizedBudget->mapVotes.begin();
            while(it4 != pfinalizedBudget->mapVotes.end()){
                (*it4).second.fSynced = false;
                ++it4;
            }
        }
        ++it3;
    }
}

void CBudgetManager::ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{
    // lite mode is not supported
    if(fLiteMode) return;
    if(!masternodeSync.IsBlockchainSynced()) return;

    LOCK(cs_budget);

    // todo : 12.1 - split out into two messages
    // ---  one for finalized budgets and one for gov objs
    if (strCommand == NetMsgType::MNBUDGETVOTESYNC) { //Masternode vote sync
        uint256 nProp;
        vRecv >> nProp;

        if(Params().NetworkIDString() == CBaseChainParams::MAIN){
            if(nProp == uint256()) {
                if(pfrom->HasFulfilledRequest(NetMsgType::MNBUDGETVOTESYNC)) {
                    LogPrintf("mnvs - peer already asked me for the list\n");
                    Misbehaving(pfrom->GetId(), 20);
                    return;
                }
                pfrom->FulfilledRequest(NetMsgType::MNBUDGETVOTESYNC);
            }
        }

        Sync(pfrom, nProp);
        LogPrintf("mnvs - Sent Masternode votes to %s\n", pfrom->addr.ToString());
    }

    if (strCommand == NetMsgType::MNBUDGETFINAL) { //Finalized Budget Suggestion
        CFinalizedBudgetBroadcast finalizedBudgetBroadcast;
        vRecv >> finalizedBudgetBroadcast;

        if(mapSeenFinalizedBudgets.count(finalizedBudgetBroadcast.GetHash())){
            masternodeSync.AddedBudgetItem(finalizedBudgetBroadcast.GetHash());
            return;
        }

        std::string strError = "";
        int nConf = 0;
        if(!IsBudgetCollateralValid(finalizedBudgetBroadcast.nFeeTXHash, finalizedBudgetBroadcast.GetHash(), strError, finalizedBudgetBroadcast.nTime, nConf)){
            LogPrintf("Finalized Budget FeeTX is not valid - %s - %s\n", finalizedBudgetBroadcast.nFeeTXHash.ToString(), strError);

            if(nConf >= 1) vecImmatureFinalizedBudgets.push_back(finalizedBudgetBroadcast);
            return;
        }

        mapSeenFinalizedBudgets.insert(make_pair(finalizedBudgetBroadcast.GetHash(), finalizedBudgetBroadcast));

        if(!finalizedBudgetBroadcast.IsValid(pCurrentBlockIndex, strError)) {
            LogPrintf("fbs - invalid finalized budget - %s\n", strError);
            return;
        }

        LogPrintf("fbs - new finalized budget - %s\n", finalizedBudgetBroadcast.GetHash().ToString());

        CFinalizedBudget finalizedBudget(finalizedBudgetBroadcast);
        if(AddFinalizedBudget(finalizedBudget)) {finalizedBudgetBroadcast.Relay();}
        masternodeSync.AddedBudgetItem(finalizedBudgetBroadcast.GetHash());

        //we might have active votes for this budget that are now valid
        CheckOrphanVotes();
    }

    if (strCommand == NetMsgType::MNBUDGETFINALVOTE) { //Finalized Budget Vote
        CFinalizedBudgetVote vote;
        vRecv >> vote;
        vote.fValid = true;

        if(mapSeenFinalizedBudgetVotes.count(vote.GetHash())){
            masternodeSync.AddedBudgetItem(vote.GetHash());
            return;
        }

        CMasternode* pmn = mnodeman.Find(vote.vin);
        if(pmn == NULL) {
            LogPrint("mnbudget", "fbvote - unknown masternode - vin: %s\n", vote.vin.ToString());
            mnodeman.AskForMN(pfrom, vote.vin);
            return;
        }

        mapSeenFinalizedBudgetVotes.insert(make_pair(vote.GetHash(), vote));

        if(!vote.IsValid(true)){
            if(masternodeSync.IsSynced()) Misbehaving(pfrom->GetId(), 20);
            // it could just be a non-synced masternode
            mnodeman.AskForMN(pfrom, vote.vin);
            return;
        }

        std::string strError = "";
        if(UpdateFinalizedBudget(vote, pfrom, strError)) {
            vote.Relay();
            masternodeSync.AddedBudgetItem(vote.GetHash());

            LogPrintf("fbvote - new finalized budget vote - %s\n", vote.GetHash().ToString());
        } else {
            LogPrintf("fbvote - rejected finalized budget vote - %s - %s\n", vote.GetHash().ToString(), strError);
        }
    }
}

void CBudgetManager::NewBlock()
{
    TRY_LOCK(cs, fBudgetNewBlock);
    if(!fBudgetNewBlock) return;

    if(!pCurrentBlockIndex) return;

    if (masternodeSync.RequestedMasternodeAssets <= MASTERNODE_SYNC_BUDGET) return;

    if (strBudgetMode == "suggest") { //suggest the budget we see
        SubmitFinalBudget();
    }

    //this function should be called 1/6 blocks, allowing up to 100 votes per day on all proposals
    if(pCurrentBlockIndex->nHeight % 6 != 0) return;

    // incremental sync with our peers
    std::map<uint256, CFinalizedBudget>::iterator it3 = mapFinalizedBudgets.begin();
    while(it3 != mapFinalizedBudgets.end()){
        (*it3).second.CleanAndRemove(false);
        ++it3;
    }

    std::vector<CFinalizedBudgetBroadcast>::iterator it5 = vecImmatureFinalizedBudgets.begin();
    while(it5 != vecImmatureFinalizedBudgets.end())
    {
        std::string strError = "";
        int nConf = 0;
        if(!IsBudgetCollateralValid((*it5).nFeeTXHash, (*it5).GetHash(), strError, (*it5).nTime, nConf)){
            ++it5;
            continue;
        }

        if(!(*it5).IsValid(pCurrentBlockIndex, strError)) {
            LogPrintf("fbs (immature) - invalid finalized budget - %s\n", strError);
            it5 = vecImmatureFinalizedBudgets.erase(it5); 
            continue;
        }

        LogPrintf("fbs (immature) - new finalized budget - %s\n", (*it5).GetHash().ToString());

        CFinalizedBudget finalizedBudget((*it5));
        if(AddFinalizedBudget(finalizedBudget)) {(*it5).Relay();}

        it5 = vecImmatureFinalizedBudgets.erase(it5); 
    }
}

CFinalizedBudget *CBudgetManager::FindFinalizedBudget(uint256 nHash)
{
    if(mapFinalizedBudgets.count(nHash))
        return &mapFinalizedBudgets[nHash];

    return NULL;
}

CAmount CBudgetManager::GetTotalBudget(int nHeight)
{
    if(!pCurrentBlockIndex) return 0;

    //get min block value and calculate from that
    CAmount nSubsidy = 5 * COIN;

    const Consensus::Params consensusParams = Params().GetConsensus();

    // TODO: Remove this to further unify logic among mainnet/testnet/whatevernet,
    //       use single formula instead (the one that is for current mainnet).
    //       Probably a good idea to use a significally lower consensusParams.nSubsidyHalvingInterval
    //       for testnet (like 10 times for example) to see the effect of halving there faster.
    //       Will require testnet restart.
    if(Params().NetworkIDString() == CBaseChainParams::TESTNET){
        for(int i = 46200; i <= nHeight; i += consensusParams.nSubsidyHalvingInterval) nSubsidy -= nSubsidy/14;
    } else {
        // yearly decline of production by 7.1% per year, projected 21.3M coins max by year 2050.
        for(int i = consensusParams.nSubsidyHalvingInterval; i <= nHeight; i += consensusParams.nSubsidyHalvingInterval) nSubsidy -= nSubsidy/14;
    }

    // 10%
    return ((nSubsidy/100)*10)*consensusParams.nBudgetPaymentsCycleBlocks;
}

std::vector<CFinalizedBudget*> CBudgetManager::GetFinalizedBudgets()
{
    LOCK(cs);

    std::vector<CFinalizedBudget*> vFinalizedBudgetsRet;
    std::vector<std::pair<CFinalizedBudget*, int> > vFinalizedBudgetsSort;

    // ------- Grab The Budgets In Order

    std::map<uint256, CFinalizedBudget>::iterator it = mapFinalizedBudgets.begin();
    while(it != mapFinalizedBudgets.end())
    {
        CFinalizedBudget* pfinalizedBudget = &((*it).second);

        vFinalizedBudgetsSort.push_back(make_pair(pfinalizedBudget, pfinalizedBudget->GetVoteCount()));
        ++it;
    }
    std::sort(vFinalizedBudgetsSort.begin(), vFinalizedBudgetsSort.end(), sortFinalizedBudgetsByVotes());

    std::vector<std::pair<CFinalizedBudget*, int> >::iterator it2 = vFinalizedBudgetsSort.begin();
    while(it2 != vFinalizedBudgetsSort.end())
    {
        vFinalizedBudgetsRet.push_back((*it2).first);
        ++it2;
    }

    return vFinalizedBudgetsRet;
}

bool CBudgetManager::IsBudgetPaymentBlock(int nBlockHeight)
{
    int nHighestCount = -1;

    std::map<uint256, CFinalizedBudget>::iterator it = mapFinalizedBudgets.begin();
    while(it != mapFinalizedBudgets.end())
    {
        CFinalizedBudget* pfinalizedBudget = &((*it).second);
        if(pfinalizedBudget->GetVoteCount() > nHighestCount && 
            nBlockHeight >= pfinalizedBudget->GetBlockStart() && 
            nBlockHeight <= pfinalizedBudget->GetBlockEnd()){
            nHighestCount = pfinalizedBudget->GetVoteCount();
        }

        ++it;
    }

    /*
        If budget doesn't have 5% of the network votes, then we should pay a masternode instead
    */
    if(nHighestCount > mnodeman.CountEnabled(MIN_BUDGET_PEER_PROTO_VERSION)/20) return true;

    return false;
}

bool CBudgetManager::AddFinalizedBudget(CFinalizedBudget& finalizedBudget)
{
    std::string strError = "";
    if(!finalizedBudget.IsValid(pCurrentBlockIndex, strError)) return false;

    if(mapFinalizedBudgets.count(finalizedBudget.GetHash())) {
        return false;
    }

    mapFinalizedBudgets.insert(make_pair(finalizedBudget.GetHash(), finalizedBudget));
    return true;
}

void CBudgetManager::SubmitFinalBudget()
{
    if(!pCurrentBlockIndex) return;

    int nBlockStart = pCurrentBlockIndex->nHeight - pCurrentBlockIndex->nHeight % Params().GetConsensus().nBudgetPaymentsCycleBlocks + Params().GetConsensus().nBudgetPaymentsCycleBlocks;
    if(nSubmittedFinalBudget >= nBlockStart) return;
    if(nBlockStart - pCurrentBlockIndex->nHeight > 576*2) return; //submit final budget 2 days before payment

    std::vector<CBudgetProposal*> vBudgetProposals = budget.GetBudget();
    std::string strBudgetName = "main";
    std::vector<CTxBudgetPayment> vecTxBudgetPayments;

    for(unsigned int i = 0; i < vBudgetProposals.size(); i++){
        CTxBudgetPayment txBudgetPayment;
        txBudgetPayment.nProposalHash = vBudgetProposals[i]->GetHash();
        txBudgetPayment.payee = vBudgetProposals[i]->GetPayee();
        txBudgetPayment.nAmount = vBudgetProposals[i]->GetAllotted();
        vecTxBudgetPayments.push_back(txBudgetPayment);
    }

    if(vecTxBudgetPayments.size() < 1) {
        LogPrintf("CBudgetManager::SubmitFinalBudget - Found No Proposals For Period\n");
        return;
    }

    CFinalizedBudgetBroadcast tempBudget(strBudgetName, nBlockStart, vecTxBudgetPayments, uint256());
    if(mapSeenFinalizedBudgets.count(tempBudget.GetHash())) {
        LogPrintf("CBudgetManager::SubmitFinalBudget - Budget already exists - %s\n", tempBudget.GetHash().ToString());    
        nSubmittedFinalBudget = pCurrentBlockIndex->nHeight;
        return; //already exists
    }

    //create fee tx
    CTransaction tx;
    if(!mapCollateral.count(tempBudget.GetHash())){
        CWalletTx wtx;
        if(!pwalletMain->GetBudgetSystemCollateralTX(wtx, tempBudget.GetHash(), false)){
            LogPrintf("CBudgetManager::SubmitFinalBudget - Can't make collateral transaction\n");
            return;
        }
        
        // make our change address
        CReserveKey reservekey(pwalletMain);
        //send the tx to the network
        pwalletMain->CommitTransaction(wtx, reservekey, NetMsgType::IX);

        mapCollateral.insert(make_pair(tempBudget.GetHash(), (CTransaction)wtx));
        tx = (CTransaction)wtx;
    } else {
        tx = mapCollateral[tempBudget.GetHash()];
    }

    CTxIn in(COutPoint(tx.GetHash(), 0));
    int conf = GetInputAgeIX(tx.GetHash(), in);
    /*
        Wait will we have 1 extra confirmation, otherwise some clients might reject this feeTX
        -- This function is tied to NewBlock, so we will propagate this budget while the block is also propagating
    */
    if(conf < BUDGET_FEE_CONFIRMATIONS+1){
        LogPrintf ("CBudgetManager::SubmitFinalBudget - Collateral requires at least %d confirmations - %s - %d confirmations\n", BUDGET_FEE_CONFIRMATIONS, tx.GetHash().ToString(), conf);
        return;
    }

    nSubmittedFinalBudget = nBlockStart;

    //create the proposal incase we're the first to make it
    CFinalizedBudgetBroadcast finalizedBudgetBroadcast(strBudgetName, nBlockStart, vecTxBudgetPayments, tx.GetHash());

    std::string strError = "";
    if(!finalizedBudgetBroadcast.IsValid(pCurrentBlockIndex, strError)){
        LogPrintf("CBudgetManager::SubmitFinalBudget - Invalid finalized budget - %s \n", strError);
        return;
    }

    LOCK(cs);
    mapSeenFinalizedBudgets.insert(make_pair(finalizedBudgetBroadcast.GetHash(), finalizedBudgetBroadcast));
    finalizedBudgetBroadcast.Relay();
    budget.AddFinalizedBudget(finalizedBudgetBroadcast);
}

bool CBudgetManager::HasNextFinalizedBudget()
{
    if(!pCurrentBlockIndex) return false;

    if(masternodeSync.IsBudgetFinEmpty()) return true;

    int nBlockStart = pCurrentBlockIndex->nHeight - pCurrentBlockIndex->nHeight % Params().GetConsensus().nBudgetPaymentsCycleBlocks + Params().GetConsensus().nBudgetPaymentsCycleBlocks;
    if(nBlockStart - pCurrentBlockIndex->nHeight > 576*2) return true; //we wouldn't have the budget yet

    if(budget.IsBudgetPaymentBlock(nBlockStart)) return true;

    LogPrintf("CBudgetManager::HasNextFinalizedBudget() - Client is missing budget - %lli\n", nBlockStart);

    return false;
}

bool CBudgetManager::UpdateFinalizedBudget(CFinalizedBudgetVote& vote, CNode* pfrom, std::string& strError)
{
    LOCK(cs);

    if(!mapFinalizedBudgets.count(vote.nBudgetHash)){
        if(pfrom){
            // only ask for missing items after our syncing process is complete -- 
            //   otherwise we'll think a full sync succeeded when they return a result
            if(!masternodeSync.IsSynced()) return false;

            LogPrintf("CBudgetManager::UpdateFinalizedBudget - Unknown Finalized Proposal %s, asking for source budget\n", vote.nBudgetHash.ToString());
            mapOrphanFinalizedBudgetVotes[vote.nBudgetHash] = vote;

            if(!askedForSourceProposalOrBudget.count(vote.nBudgetHash)){
                pfrom->PushMessage(NetMsgType::MNBUDGETVOTESYNC, vote.nBudgetHash);
                askedForSourceProposalOrBudget[vote.nBudgetHash] = GetTime();
            }

        }

        strError = "Finalized Budget not found!";
        return false;
    }

    return mapFinalizedBudgets[vote.nBudgetHash].AddOrUpdateVote(vote, strError);
}

// todo - 12.1 - terrible name - how about IsBlockTransactionValid
bool CBudgetManager::IsTransactionValid(const CTransaction& txNew, int nBlockHeight)
{
    LOCK(cs);

    int nHighestCount = 0;
    std::vector<CFinalizedBudget*> ret;

    // ------- Grab The Highest Count

    std::map<uint256, CFinalizedBudget>::iterator it = mapFinalizedBudgets.begin();
    while(it != mapFinalizedBudgets.end())
    {
        CFinalizedBudget* pfinalizedBudget = &((*it).second);
        if(pfinalizedBudget->GetVoteCount() > nHighestCount &&
                nBlockHeight >= pfinalizedBudget->GetBlockStart() &&
                nBlockHeight <= pfinalizedBudget->GetBlockEnd()){
                    nHighestCount = pfinalizedBudget->GetVoteCount();
        }

        ++it;
    }

    /*
        If budget doesn't have 5% of the network votes, then we should pay a masternode instead
    */
    if(nHighestCount < mnodeman.CountEnabled(MIN_BUDGET_PEER_PROTO_VERSION)/20) return false;

    // check the highest finalized budgets (+/- 10% to assist in consensus)

    it = mapFinalizedBudgets.begin();
    while(it != mapFinalizedBudgets.end())
    {
        CFinalizedBudget* pfinalizedBudget = &((*it).second);

        if(pfinalizedBudget->GetVoteCount() > nHighestCount - mnodeman.CountEnabled(MIN_BUDGET_PEER_PROTO_VERSION)/10){
            if(nBlockHeight >= pfinalizedBudget->GetBlockStart() && nBlockHeight <= pfinalizedBudget->GetBlockEnd()){
                if(pfinalizedBudget->IsTransactionValid(txNew, nBlockHeight)){
                    return true;
                }
            }
        }

        ++it;
    }

    //we looked through all of the known budgets
    return false;
}

std::string CBudgetManager::GetRequiredPaymentsString(int nBlockHeight)
{
    LOCK(cs);

    std::string ret = "unknown-budget";

    std::map<uint256, CFinalizedBudget>::iterator it = mapFinalizedBudgets.begin();
    while(it != mapFinalizedBudgets.end())
    {
        CFinalizedBudget* pfinalizedBudget = &((*it).second);
        if(nBlockHeight >= pfinalizedBudget->GetBlockStart() && nBlockHeight <= pfinalizedBudget->GetBlockEnd()){
            CTxBudgetPayment payment;
            if(pfinalizedBudget->GetBudgetPaymentByBlock(nBlockHeight, payment)){
                if(ret == "unknown-budget"){
                    ret = payment.nProposalHash.ToString();
                } else {
                    ret += ",";
                    ret += payment.nProposalHash.ToString();
                }
            } else {
                LogPrintf("CBudgetManager::GetRequiredPaymentsString - Couldn't find budget payment for block %d\n", nBlockHeight);
            }
        }

        ++it;
    }

    return ret;
}

void CBudgetManager::FillBlockPayee(CMutableTransaction& txNew, CAmount nFees)
{
    LOCK(cs);

    AssertLockHeld(cs_main);
    if(!chainActive.Tip()) return;

    int nHighestCount = 0;
    CScript payee;
    CAmount nAmount = 0;

    // ------- Grab The Highest Count

    std::map<uint256, CFinalizedBudget>::iterator it = mapFinalizedBudgets.begin();
    while(it != mapFinalizedBudgets.end())
    {
        CFinalizedBudget* pfinalizedBudget = &((*it).second);
        if(pfinalizedBudget->GetVoteCount() > nHighestCount &&
                chainActive.Tip()->nHeight + 1 >= pfinalizedBudget->GetBlockStart() &&
                chainActive.Tip()->nHeight + 1 <= pfinalizedBudget->GetBlockEnd() &&
                pfinalizedBudget->GetPayeeAndAmount(chainActive.Tip()->nHeight + 1, payee, nAmount)){
                    nHighestCount = pfinalizedBudget->GetVoteCount();
        }

        ++it;
    }

    //miners get the full amount on these blocks
    txNew.vout[0].nValue = nFees + GetBlockSubsidy(chainActive.Tip()->nBits, chainActive.Tip()->nHeight, Params().GetConsensus());

    if(nHighestCount > 0){
        txNew.vout.resize(2);

        //these are super blocks, so their value can be much larger than normal
        txNew.vout[1].scriptPubKey = payee;
        txNew.vout[1].nValue = nAmount;

        CTxDestination address1;
        ExtractDestination(payee, address1);
        CBitcoinAddress address2(address1);

        LogPrintf("CBudgetManager::FillBlockPayee - Budget payment to %s for %lld\n", address2.ToString(), nAmount);
    }

}


void CBudgetManager::CheckOrphanVotes()
{
    LOCK(cs);

    std::map<uint256, CFinalizedBudgetVote>::iterator it2 = mapOrphanFinalizedBudgetVotes.begin();
    while(it2 != mapOrphanFinalizedBudgetVotes.end()){
        if(budget.UpdateFinalizedBudget(((*it2).second),NULL, strError)){
            LogPrintf("CBudgetManager::CheckOrphanVotes - Proposal/Budget is known, activating and removing orphan vote\n");
            mapOrphanFinalizedBudgetVotes.erase(it2++);
        } else {
            ++it2;
        }
    }
}

void CBudgetManager::CheckAndRemove()
{
    LogPrintf("CBudgetManager::CheckAndRemove \n");

    if(!pCurrentBlockIndex) return;

    std::string strError = "";
    std::map<uint256, CFinalizedBudget>::iterator it = mapFinalizedBudgets.begin();
    while(it != mapFinalizedBudgets.end())
    {
        CFinalizedBudget* pfinalizedBudget = &((*it).second);

        pfinalizedBudget->fValid = pfinalizedBudget->IsValid(pCurrentBlockIndex, strError);
        if(pfinalizedBudget->fValid) {
            pfinalizedBudget->AutoCheck();
            ++it;
            continue;
        } else if(pfinalizedBudget->nBlockStart != 0 && pfinalizedBudget->nBlockStart < pCurrentBlockIndex->nHeight - Params().GetConsensus().nBudgetPaymentsCycleBlocks) {
            // it's too old, remove it
            mapFinalizedBudgets.erase(it++);
            LogPrintf("CBudgetManager::CheckAndRemove - removing budget %s\n", pfinalizedBudget->GetHash().ToString());
            continue;
        }
        // it's not valid already but it's not too old yet, keep it and move to the next one
        ++it;
    }
}

std::string CBudgetManager::ToString() const
{
    std::ostringstream info;

    info << "Budgets: " << (int)mapFinalizedBudgets.size() <<
            ", Seen Final Budgets: " << (int)mapSeenFinalizedBudgets.size() <<
            ", Seen Final Budget Votes: " << (int)mapSeenFinalizedBudgetVotes.size();

    return info.str();
}

// todo - 12.1 - rename : UpdateBlockTip
void CBudgetManager::UpdatedBlockTip(const CBlockIndex *pindex)
{
    pCurrentBlockIndex = pindex;
    LogPrint("mnbudget", "pCurrentBlockIndex->nHeight: %d\n", pCurrentBlockIndex->nHeight);

    if(!fLiteMode && masternodeSync.RequestedMasternodeAssets > MASTERNODE_SYNC_LIST)
        NewBlock();
}

