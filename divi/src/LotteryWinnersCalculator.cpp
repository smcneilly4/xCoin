#include <LotteryWinnersCalculator.h>

#include <SuperblockHelpers.h>
#include <hash.h>
#include <uint256.h>
#include <primitives/transaction.h>
#include <chain.h>
#include <timedata.h>
#include <numeric>
#include <spork.h>
#include <BlockDiskAccessor.h>
#include <I_SuperblockHeightValidator.h>


LotteryWinnersCalculator::LotteryWinnersCalculator(
    int startOfLotteryBlocks,
    CChain& activeChain,
    CSporkManager& sporkManager,
    const I_SuperblockHeightValidator& superblockHeightValidator
    ): startOfLotteryBlocks_(startOfLotteryBlocks)
    , activeChain_(activeChain)
    , sporkManager_(sporkManager)
    , superblockHeightValidator_(superblockHeightValidator)
{
}

int LotteryWinnersCalculator::minimumCoinstakeForTicket(int nHeight) const
{
    int nMinStakeValue = 10000; // default is 10k

    if(sporkManager_.IsSporkActive(SPORK_16_LOTTERY_TICKET_MIN_VALUE)) {
        MultiValueSporkList<LotteryTicketMinValueSporkValue> vValues;
        CSporkManager::ConvertMultiValueSporkVector(sporkManager_.GetMultiValueSpork(SPORK_16_LOTTERY_TICKET_MIN_VALUE), vValues);
        auto nBlockTime = activeChain_[nHeight] ? activeChain_[nHeight]->nTime : GetAdjustedTime();
        LotteryTicketMinValueSporkValue activeSpork = CSporkManager::GetActiveMultiValueSpork(vValues, nHeight, nBlockTime);

        if(activeSpork.IsValid()) {
            // we expect that this value is in coins, not in satoshis
            nMinStakeValue = activeSpork.nEntryTicketValue;
        }
    }

    return nMinStakeValue;
}

uint256 LotteryWinnersCalculator::CalculateLotteryScore(const uint256 &hashCoinbaseTx, const uint256 &hashLastLotteryBlock) const
{
    // Deterministically calculate a "score" for a Masternode based on any given (block)hash
    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << hashCoinbaseTx << hashLastLotteryBlock;
    return ss.GetHash();
}

bool LotteryWinnersCalculator::IsCoinstakeValidForLottery(const CTransaction &tx, int nHeight) const
{
    CAmount nAmount = 0;
    if(tx.IsCoinBase()) {
        nAmount = tx.vout[0].nValue;
    }
    else {
        auto payee = tx.vout[1].scriptPubKey;
        nAmount = std::accumulate(std::begin(tx.vout), std::end(tx.vout), CAmount(0), [payee](CAmount accum, const CTxOut &out) {
                return out.scriptPubKey == payee ? accum + out.nValue : accum;
    });
    }

    return nAmount > minimumCoinstakeForTicket(nHeight) * COIN; // only if stake is more than 10k
}

uint256 LotteryWinnersCalculator::GetLastLotteryBlockHashBeforeHeight(int blockHeight) const
{
    const int lotteryBlockPaymentCycle = superblockHeightValidator_.GetLotteryBlockPaymentCycle(blockHeight);
    const int nLastLotteryHeight = std::max(startOfLotteryBlocks_,  lotteryBlockPaymentCycle* ((blockHeight - 1) / lotteryBlockPaymentCycle) );
    return activeChain_[nLastLotteryHeight]->GetBlockHash();
}

bool LotteryWinnersCalculator::UpdateCoinstakes(const uint256& lastLotteryBlockHash, LotteryCoinstakes& updatedCoinstakes) const
{
    struct RankAwareScore
    {
        uint256 score;
        size_t rank;
    };
    std::map<uint256,RankAwareScore> rankedScoreAwareCoinstakes;
    for(const auto& lotteryCoinstake : updatedCoinstakes)
    {
        RankAwareScore rankedScore = {
            CalculateLotteryScore(lotteryCoinstake.first, lastLotteryBlockHash), rankedScoreAwareCoinstakes.size() };
        rankedScoreAwareCoinstakes.emplace(lotteryCoinstake.first, std::move(rankedScore));
    }

    // biggest entry at the begining
    bool shouldUpdateCoinstakeData = true;
    if(rankedScoreAwareCoinstakes.size() > 1)
    {
        std::stable_sort(std::begin(updatedCoinstakes), std::end(updatedCoinstakes),
            [&rankedScoreAwareCoinstakes](const LotteryCoinstake& lhs, const LotteryCoinstake& rhs)
            {
                return rankedScoreAwareCoinstakes[lhs.first].score > rankedScoreAwareCoinstakes[rhs.first].score;
            }
        );
        shouldUpdateCoinstakeData = rankedScoreAwareCoinstakes[updatedCoinstakes.back().first].rank != 11;
    }
    if( updatedCoinstakes.size() > 11) updatedCoinstakes.pop_back();
    return shouldUpdateCoinstakeData;
}

LotteryCoinstakeData LotteryWinnersCalculator::CalculateUpdatedLotteryWinners(
    const CTransaction& coinMintTransaction,
    const LotteryCoinstakeData& previousBlockLotteryCoinstakeData,
    int nHeight) const
{
    if(nHeight <= 0) return LotteryCoinstakeData();
    if(superblockHeightValidator_.IsValidLotteryBlockHeight(nHeight)) return LotteryCoinstakeData(nHeight);
    if(nHeight <= startOfLotteryBlocks_) return previousBlockLotteryCoinstakeData.getShallowCopy();
    if(!IsCoinstakeValidForLottery(coinMintTransaction, nHeight)) return previousBlockLotteryCoinstakeData.getShallowCopy();

    auto hashLastLotteryBlock = GetLastLotteryBlockHashBeforeHeight(nHeight);
    LotteryCoinstakes updatedCoinstakes = previousBlockLotteryCoinstakeData.getLotteryCoinstakes();
    updatedCoinstakes.emplace_back(coinMintTransaction.GetHash(), coinMintTransaction.IsCoinBase()? coinMintTransaction.vout[0].scriptPubKey:coinMintTransaction.vout[1].scriptPubKey);


    if(UpdateCoinstakes(hashLastLotteryBlock,updatedCoinstakes))
    {
        return LotteryCoinstakeData(nHeight,updatedCoinstakes);
    }
    else
    {
        return previousBlockLotteryCoinstakeData.getShallowCopy();
    }
}