// Copyright (c) 2018-2019, The TurtleCoin Developers
//
// Please see the included LICENSE file for more information.

/////////////////////////////////////////////
#include <WalletBackend/WalletSynchronizer.h>
/////////////////////////////////////////////

#include <Common/StringTools.h>

#include <config/WalletConfig.h>

#include <crypto/crypto.h>

#include <future>

#include <iostream>

#include <Logger/Logger.h>

#include <Utilities/ThreadSafeQueue.h>
#include <Utilities/ThreadSafeDeque.h>
#include <Utilities/Utilities.h>

#include <WalletBackend/Constants.h>

///////////////////////////////////
/* CONSTRUCTORS / DECONSTRUCTORS */
///////////////////////////////////

/* Default constructor */
WalletSynchronizer::WalletSynchronizer() :
    m_shouldStop(false),
    m_startTimestamp(0),
    m_startHeight(0)
{
}

/* Parameterized constructor */
WalletSynchronizer::WalletSynchronizer(
    const std::shared_ptr<Nigel> daemon,
    const uint64_t startHeight,
    const uint64_t startTimestamp,
    const Crypto::SecretKey privateViewKey,
    const std::shared_ptr<EventHandler> eventHandler) :

    m_daemon(daemon),
    m_shouldStop(false),
    m_startHeight(startHeight),
    m_startTimestamp(startTimestamp),
    m_privateViewKey(privateViewKey),
    m_eventHandler(eventHandler),
    m_blockDownloader(daemon, nullptr, startHeight, startTimestamp)
{
}

/* Move constructor */
WalletSynchronizer::WalletSynchronizer(WalletSynchronizer && old)
{
    /* Call the move assignment operator */
    *this = std::move(old);
}

/* Move assignment operator */
WalletSynchronizer & WalletSynchronizer::operator=(WalletSynchronizer && old)
{
    /* Stop any running threads */
    stop();

    m_syncThread = std::move(old.m_syncThread);

    m_startTimestamp = std::move(old.m_startTimestamp);
    m_startHeight = std::move(old.m_startHeight);

    m_privateViewKey = std::move(old.m_privateViewKey);

    m_eventHandler = std::move(old.m_eventHandler);

    m_daemon = std::move(old.m_daemon);

    m_blockDownloader = std::move(old.m_blockDownloader);

    return *this;
}

/* Deconstructor */
WalletSynchronizer::~WalletSynchronizer()
{
    stop();
}

/////////////////////
/* CLASS FUNCTIONS */
/////////////////////

void WalletSynchronizer::mainLoop()
{
    auto lastCheckedLockedTransactions = std::chrono::system_clock::now();

    while (!m_shouldStop)
    {
        const auto blocks = m_blockDownloader.fetchBlocks(Constants::BLOCK_PROCESSING_CHUNK);

        for (const auto &block : blocks)
        {
            if (m_shouldStop)
            {
                return;
            }

            processBlock(block);
        }

        /* If we're synced, check any transactions that may be in the pool */
        if (getCurrentScanHeight() >= m_daemon->localDaemonBlockCount() && !m_shouldStop)
        {
            const auto now = std::chrono::system_clock::now();
            const auto timeDiff = now - lastCheckedLockedTransactions;

            /* Not a viewwallet and haven't checked transactions in last 15 secs */
            if (!m_subWallets->isViewWallet() && timeDiff > std::chrono::seconds(15))
            {
                checkLockedTransactions();
                lastCheckedLockedTransactions = now;
            }

            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }
}

std::vector<std::tuple<Crypto::PublicKey, WalletTypes::TransactionInput>> WalletSynchronizer::processBlockOutputs(
    const WalletTypes::WalletBlockInfo &block) const
{
    std::vector<std::tuple<Crypto::PublicKey, WalletTypes::TransactionInput>> inputs;

    if (WalletConfig::processCoinbaseTransactions)
    {
        const auto newInputs = processTransactionOutputs(
            block.coinbaseTransaction, block.blockHeight
        );

        inputs.insert(inputs.end(), newInputs.begin(), newInputs.end());
    }

    for (const auto tx : block.transactions)
    {
        const auto newInputs = processTransactionOutputs(tx, block.blockHeight);

        inputs.insert(inputs.end(), newInputs.begin(), newInputs.end());
    }

    return inputs;
}

void WalletSynchronizer::processBlock(const WalletTypes::WalletBlockInfo &block)
{
    Logger::logger.log(
        "Processing block " + std::to_string(block.blockHeight),
        Logger::DEBUG,
        {Logger::SYNC}
    );

    /* Chain forked, invalidate previous transactions */
    if (m_blockDownloader.getHeight() >= block.blockHeight)
    {
        Logger::logger.log(
            "Blockchain forked, resolving...",
            Logger::INFO,
            {Logger::SYNC}
        );

        removeForkedTransactions(block.blockHeight);
    }

    /* Prune old inputs that are out of our 'confirmation' window */
    if (block.blockHeight % Constants::PRUNE_SPENT_INPUTS_INTERVAL == 0
     && block.blockHeight > Constants::PRUNE_SPENT_INPUTS_INTERVAL)
    {
        m_subWallets->pruneSpentInputs(block.blockHeight - Constants::PRUNE_SPENT_INPUTS_INTERVAL);
    }

    auto ourInputs = processBlockOutputs(block);

    std::unordered_map<Crypto::Hash, std::vector<uint64_t>> globalIndexes;

    for (auto &[publicKey, input] : ourInputs)
    {
        if (!m_subWallets->isViewWallet() && !input.globalOutputIndex)
        {
            if (globalIndexes.empty())
            {
                globalIndexes = getGlobalIndexes(block.blockHeight);
            }

            const auto it = globalIndexes.find(input.parentTransactionHash);

            /* Daemon returns indexes for hashes in a range. If we don't
               find our hash, either the chain has forked, or the daemon
               is faulty. Print a warning message, then return so we
               can fetch new blocks, in the likely case the daemon has
               forked.

               Also need to check there are enough indexes for the one we want */
            if (it == globalIndexes.end() || it->second.size() <= input.transactionIndex)
            {
                Logger::logger.log(
                    "Warning: Failed to get correct global indexes from daemon."
                    "\nIf you see this error message repeatedly, the daemon "
                    "may be faulty. More likely, the chain just forked.",
                    Logger::WARNING,
                    {Logger::SYNC, Logger::DAEMON}
                );

                return;
            }

            input.globalOutputIndex = it->second[input.transactionIndex];
        }
    }

    BlockScanTmpInfo blockScanInfo = processBlockTransactions(block, ourInputs);

    for (const auto tx : blockScanInfo.transactionsToAdd)
    {
        std::stringstream stream;

        stream << "Adding transaction: " << tx.hash;

        Logger::logger.log(
            stream.str(),
            Logger::INFO,
            {Logger::SYNC, Logger::TRANSACTIONS}
        );

        m_subWallets->addTransaction(tx);
        m_eventHandler->onTransaction.fire(tx);
    }

    for (const auto [publicKey, input] : blockScanInfo.inputsToAdd)
    {
        std::stringstream stream;

        stream << "Adding input: " << input.key;

        Logger::logger.log(
            stream.str(),
            Logger::INFO,
            {Logger::SYNC}
        );

        m_subWallets->storeTransactionInput(publicKey, input);
    }

    /* The input has been spent, discard the key image so we
       don't double spend it */
    for (const auto [publicKey, keyImage] : blockScanInfo.keyImagesToMarkSpent)
    {
        std::stringstream stream;

        stream << "Marking key image: " << keyImage << " as spent";

        Logger::logger.log(
            stream.str(),
            Logger::INFO,
            {Logger::SYNC}
        );

        m_subWallets->markInputAsSpent(keyImage, publicKey, block.blockHeight);
    }

    /* Make sure to do this at the end, once the transactions are fully
       processed! Otherwise, we could miss a transaction depending upon
       when we save */
    m_blockDownloader.dropBlock(block.blockHeight, block.blockHash);

    if (block.blockHeight >= m_daemon->networkBlockCount())
    {
        m_eventHandler->onSynced.fire(block.blockHeight);
    }

    Logger::logger.log(
        "Finshed processing block " + std::to_string(block.blockHeight),
        Logger::DEBUG,
        {Logger::SYNC}
    );
}

BlockScanTmpInfo WalletSynchronizer::processBlockTransactions(
    const WalletTypes::WalletBlockInfo &block,
    const std::vector<std::tuple<Crypto::PublicKey, WalletTypes::TransactionInput>> &inputs) const
{
    BlockScanTmpInfo txData;

    if (WalletConfig::processCoinbaseTransactions)
    {
        const auto tx = processCoinbaseTransaction(block, inputs);

        if (tx)
        {
            txData.transactionsToAdd.push_back(*tx);
        }
    }

    for (const auto rawTX : block.transactions)
    {
        const auto [tx, keyImagesToMarkSpent] = processTransaction(
            block, inputs, rawTX
        );

        if (tx)
        {
            txData.transactionsToAdd.push_back(*tx);

            txData.keyImagesToMarkSpent.insert(
                txData.keyImagesToMarkSpent.end(),
                keyImagesToMarkSpent.begin(),
                keyImagesToMarkSpent.end()
            );
        }
    }

    txData.inputsToAdd = inputs;

    return txData;
}

std::optional<WalletTypes::Transaction> WalletSynchronizer::processCoinbaseTransaction(
    const WalletTypes::WalletBlockInfo &block,
    const std::vector<std::tuple<Crypto::PublicKey, WalletTypes::TransactionInput>> &inputs) const
{
    const auto tx = block.coinbaseTransaction;

    std::unordered_map<Crypto::PublicKey, int64_t> transfers;

    std::vector<std::tuple<Crypto::PublicKey, WalletTypes::TransactionInput>> relevantInputs;

    std::copy_if(inputs.begin(), inputs.end(), std::back_inserter(relevantInputs), [&](const auto input) {
        return std::get<1>(input).parentTransactionHash == tx.hash;
    });

    for (const auto &[publicSpendKey, input] : relevantInputs)
    {
        transfers[publicSpendKey] += input.amount;
    }

    if (!transfers.empty())
    {
        const uint64_t fee = 0;
        const bool isCoinbaseTransaction = true;
        const std::string paymentID;

        return WalletTypes::Transaction(
            transfers, tx.hash, fee, block.blockTimestamp, block.blockHeight,
            paymentID, tx.unlockTime, isCoinbaseTransaction
        );
    }

    return std::nullopt;
}

std::tuple<std::optional<WalletTypes::Transaction>, std::vector<std::tuple<Crypto::PublicKey, Crypto::KeyImage>>> WalletSynchronizer::processTransaction(
    const WalletTypes::WalletBlockInfo &block,
    const std::vector<std::tuple<Crypto::PublicKey, WalletTypes::TransactionInput>> &inputs,
    const WalletTypes::RawTransaction &tx) const
{
    std::unordered_map<Crypto::PublicKey, int64_t> transfers;

    std::vector<std::tuple<Crypto::PublicKey, WalletTypes::TransactionInput>> relevantInputs;

    std::copy_if(inputs.begin(), inputs.end(), std::back_inserter(relevantInputs), [&](const auto input) {
        return std::get<1>(input).parentTransactionHash == tx.hash;
    });

    for (const auto &[publicSpendKey, input] : relevantInputs)
    {
        transfers[publicSpendKey] += input.amount;
    }

    std::vector<std::tuple<Crypto::PublicKey, Crypto::KeyImage>> spentKeyImages;

    for (const auto input : tx.keyInputs)
    {
        const auto [found, publicSpendKey] = m_subWallets->getKeyImageOwner(
            input.keyImage
        );

        if (found)
        {
            transfers[publicSpendKey] -= input.amount;
            spentKeyImages.emplace_back(publicSpendKey, input.keyImage);
        }
    }

    if (!transfers.empty())
    {
        uint64_t fee = 0;

        for (const auto input : tx.keyInputs)
        {
            fee += input.amount;
        }

        for (const auto output : tx.keyOutputs)
        {
            fee -= output.amount;
        }

        const bool isCoinbaseTransaction = false;

        const auto newTX = WalletTypes::Transaction(
            transfers, tx.hash, fee, block.blockTimestamp, block.blockHeight,
            tx.paymentID, tx.unlockTime, isCoinbaseTransaction
        );

        return {newTX, spentKeyImages};
    }

    return {std::nullopt, {}};
}

std::vector<std::tuple<Crypto::PublicKey, WalletTypes::TransactionInput>> WalletSynchronizer::processTransactionOutputs(
    const WalletTypes::RawCoinbaseTransaction &rawTX,
    const uint64_t blockHeight) const
{
    std::vector<std::tuple<Crypto::PublicKey, WalletTypes::TransactionInput>> inputs;

    Crypto::KeyDerivation derivation;

    Crypto::generate_key_derivation(rawTX.transactionPublicKey, m_privateViewKey, derivation);

    const std::vector<Crypto::PublicKey> spendKeys = m_subWallets->m_publicSpendKeys;

    uint64_t outputIndex = 0;

    for (const auto output : rawTX.keyOutputs)
    {
        Crypto::PublicKey derivedSpendKey;

        Crypto::underive_public_key(derivation, outputIndex, output.key, derivedSpendKey);

        /* See if the derived spend key matches any of our spend keys */
        const auto ourSpendKey = std::find(spendKeys.begin(), spendKeys.end(),
                                           derivedSpendKey);

        /* If it does, the transaction belongs to us */
        if (ourSpendKey != spendKeys.end())
        {
            /* We need to fill in the key image of the transaction input -
               we'll let the subwallet do this since we need the private spend
               key. We use the key images to detect outgoing transactions,
               and we use the transaction inputs to make transactions ourself */
            const Crypto::KeyImage keyImage = m_subWallets->getTxInputKeyImage(
                derivedSpendKey, derivation, outputIndex
            );

            const uint64_t spendHeight = 0;

            const WalletTypes::TransactionInput input({
                keyImage, output.amount, blockHeight, rawTX.transactionPublicKey,
                outputIndex, output.globalOutputIndex, output.key, spendHeight,
                rawTX.unlockTime, rawTX.hash
            });

            inputs.emplace_back(derivedSpendKey, input);
        }

        outputIndex++;
    }

    return inputs;
}

/* When we get the global indexes, we pass in a range of blocks, to obscure
   which transactions we are interested in - the ones that belong to us.
   To do this, we get the global indexes for all transactions in a range.

   For example, if we want the global indexes for a transaction in block
   17, we get all the indexes from block 10 to block 20. */
std::unordered_map<Crypto::Hash, std::vector<uint64_t>> WalletSynchronizer::getGlobalIndexes(
    const uint64_t blockHeight) const
{
    uint64_t startHeight = Utilities::getLowerBound(
        blockHeight, Constants::GLOBAL_INDEXES_OBSCURITY
    );

    uint64_t endHeight = Utilities::getUpperBound(
        blockHeight, Constants::GLOBAL_INDEXES_OBSCURITY
    );

    const auto [success, indexes] = m_daemon->getGlobalIndexesForRange(
        startHeight, endHeight
    );

    if (!success)
    {
        return {};
    }

    return indexes;
}

void WalletSynchronizer::checkLockedTransactions()
{
    /* Get the hashes of any locked tx's we have */
    const auto lockedTxHashes = m_subWallets->getLockedTransactionsHashes();

    if (lockedTxHashes.size() != 0)
    {
        Logger::logger.log(
            "Checking locked transactions",
            Logger::DEBUG,
            {Logger::TRANSACTIONS}
        );

        /* Transactions that are in the pool - we'll query these again
           next time to see if they have moved */
        std::unordered_set<Crypto::Hash> transactionsInPool;

        /* Transactions that are in a block - don't need to do anything,
           when we get to the block they will be processed and unlocked. */
        std::unordered_set<Crypto::Hash> transactionsInBlock;

        /* Transactions that the daemon doesn't know about - returned to
           our wallet for timeout or other reason */
        std::unordered_set<Crypto::Hash> cancelledTransactions;

        /* Get the status of the locked transactions */
        bool success = m_daemon->getTransactionsStatus(
            lockedTxHashes, transactionsInPool, transactionsInBlock,
            cancelledTransactions
        );

        /* Couldn't get info from the daemon, try again later */
        if (!success)
        {
            Logger::logger.log(
                "Failed to get locked transaction information from daemon",
                Logger::WARNING,
                {Logger::TRANSACTIONS, Logger::DAEMON}
            );

            return;
        }

        /* If some transactions have been cancelled, remove them, and their
           inputs */
        if (cancelledTransactions.size() != 0)
        {
            m_subWallets->removeCancelledTransactions(cancelledTransactions);
        }
    }
}

/* Launch the worker thread in the background. It's safest to do this in a
   seperate function, so everything in the constructor gets initialized,
   and if we do any inheritance, things don't go awry. */
void WalletSynchronizer::start()
{
    Logger::logger.log(
        "Starting sync process",
        Logger::DEBUG,
        {Logger::SYNC}
    );

    /* Reinit any vars which may have changed if we previously called stop() */
    m_shouldStop = false;

    if (m_daemon == nullptr)
    {
        throw std::runtime_error("Daemon has not been initialized!");
    }

    m_blockDownloader.start();

    m_syncThread = std::thread(&WalletSynchronizer::mainLoop, this);
}

void WalletSynchronizer::stop()
{
    Logger::logger.log(
        "Stopping sync process",
        Logger::DEBUG,
        {Logger::SYNC}
    );

    /* Tell the threads to stop */
    m_shouldStop = true;

    /* Tell the block downloader to stop and wait for it */
    m_blockDownloader.stop();

    /* Wait for the block downloader thread to finish (if applicable) */
    if (m_syncThread.joinable())
    {
        m_syncThread.join();
    }
}

void WalletSynchronizer::reset(uint64_t startHeight)
{
    /* Reset start height / timestamp */
    m_startHeight = startHeight;
    m_startTimestamp = 0;

    /* Discard downloaded blocks and sync status */
    m_blockDownloader = BlockDownloader(m_daemon, m_subWallets, m_startHeight, m_startTimestamp);

    /* Need to call start in your calling code - We don't call it here so
       you can schedule the start correctly */
}

/* Remove any transactions at this height or above, they were on a forked
   chain */
void WalletSynchronizer::removeForkedTransactions(const uint64_t forkHeight)
{
    m_subWallets->removeForkedTransactions(forkHeight);
}

void WalletSynchronizer::initializeAfterLoad(
    const std::shared_ptr<Nigel> daemon,
    const std::shared_ptr<EventHandler> eventHandler)
{
    m_daemon = daemon;
    m_eventHandler = eventHandler;
    m_blockDownloader.initializeAfterLoad(m_daemon);
}

uint64_t WalletSynchronizer::getCurrentScanHeight() const
{
    return m_blockDownloader.getHeight();
}

void WalletSynchronizer::swapNode(const std::shared_ptr<Nigel> daemon)
{
    m_daemon = daemon;
}

void WalletSynchronizer::fromJSON(const JSONObject &j)
{
    m_startTimestamp = getUint64FromJSON(j, "startTimestamp");
    m_startHeight = getUint64FromJSON(j, "startHeight");
    m_privateViewKey.fromString(getStringFromJSON(j, "privateViewKey"));

    m_blockDownloader.fromJSON(
        getObjectFromJSON(j, "transactionSynchronizerStatus"),
        m_startHeight,
        m_startTimestamp
    );
}

void WalletSynchronizer::toJSON(rapidjson::Writer<rapidjson::StringBuffer> &writer) const
{
    writer.StartObject();

    writer.Key("transactionSynchronizerStatus");
    m_blockDownloader.toJSON(writer);

    writer.Key("startTimestamp");
    writer.Uint64(m_startTimestamp);

    writer.Key("startHeight");
    writer.Uint64(m_startHeight);

    writer.Key("privateViewKey");
    m_privateViewKey.toJSON(writer);

    writer.EndObject();
}

void WalletSynchronizer::setSubWallets(const std::shared_ptr<SubWallets> subWallets)
{
    m_subWallets = subWallets;
    m_blockDownloader.setSubWallets(m_subWallets);
}