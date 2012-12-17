#include "LoadManager.h"

LoadManager::LoadManager(int creditRate, int creditLimit, int debitWarn, int debitLimit) :
		mCreditRate(creditRate), mCreditLimit(creditLimit), mDebitWarn(debitWarn), mDebitLimit(debitLimit),
		mCosts(LT_MAX)
{
	addLoadCost(LoadCost(LT_InvalidRequest,		10,		LC_CPU | LC_Network));
	addLoadCost(LoadCost(LT_RequestNoReply,		1,		LC_CPU | LC_Disk));
	addLoadCost(LoadCost(LT_InvalidSignature,	100,	LC_CPU));
	addLoadCost(LoadCost(LT_UnwantedData,		5,		LC_CPU | LC_Network));

	addLoadCost(LoadCost(LT_NewTrusted,			10,		0));
	addLoadCost(LoadCost(LT_NewTransaction,		2,		0));
	addLoadCost(LoadCost(LT_NeededData,			10,		0));

	addLoadCost(LoadCost(LT_RequestData,		5,		LC_Disk | LC_Network));
	addLoadCost(LoadCost(LT_CheapQuery,			1,		LC_CPU));
}


int LoadManager::getCreditRate() const
{
	boost::mutex::scoped_lock sl(mLock);
	return mCreditRate;
}

int LoadManager::getCreditLimit() const
{
	boost::mutex::scoped_lock sl(mLock);
	return mCreditLimit;
}

int LoadManager::getDebitWarn() const
{
	boost::mutex::scoped_lock sl(mLock);
	return mDebitWarn;
}

int LoadManager::getDebitLimit() const
{
	boost::mutex::scoped_lock sl(mLock);
	return mDebitLimit;
}

void LoadManager::setCreditRate(int r)
{
	boost::mutex::scoped_lock sl(mLock);
	mCreditRate = r;
}

void LoadManager::setCreditLimit(int r)
{
	boost::mutex::scoped_lock sl(mLock);
	mCreditLimit = r;
}

void LoadManager::setDebitWarn(int r)
{
	boost::mutex::scoped_lock sl(mLock);
	mDebitWarn = r;
}

void LoadManager::setDebitLimit(int r)
{
	boost::mutex::scoped_lock sl(mLock);
	mDebitLimit = r;
}

void LoadManager::canonicalize(LoadSource& source, const time_t now) const
{
	if (source.mLastUpdate != now)
	{
		if (source.mLastUpdate < now)
		{
			source.mBalance += mCreditRate * (now - source.mLastUpdate);
			if (source.mBalance > mCreditLimit)
				source.mBalance = mCreditLimit;
		}
		source.mLastUpdate = now;
	}
}

bool LoadManager::shouldWarn(LoadSource& source) const
{
	time_t now = time(NULL);
	boost::mutex::scoped_lock sl(mLock);

	canonicalize(source, now);
	if (source.isPrivileged() || (source.mBalance < mDebitWarn) || (source.mLastWarning == now))
		return false;

	source.mLastWarning = now;
	return true;
}

bool LoadManager::shouldCutoff(LoadSource& source) const
{
	time_t now = time(NULL);
	boost::mutex::scoped_lock sl(mLock);

	canonicalize(source, now);
	return !source.isPrivileged() && (source.mBalance < mDebitLimit);
}

bool LoadManager::adjust(LoadSource& source, LoadType t) const
{ // FIXME: Scale by category
	LoadCost cost = mCosts[static_cast<int>(t)];
	return adjust(source, cost.mCost);
}

bool LoadManager::adjust(LoadSource& source, int credits) const
{ // return: true = need to warn/cutoff
	time_t now = time(NULL);
	boost::mutex::scoped_lock sl(mLock);

	// We do it this way in case we want to add exponential decay later
	canonicalize(source, now);
	source.mBalance += credits;
	if (source.mBalance > mCreditLimit)
		source.mBalance = mCreditLimit;

	if (source.isPrivileged()) // privileged sources never warn/cutoff
		return false;

	if (source.mBalance < mDebitLimit) // over-limit
		return true;

	if ((source.mBalance < mDebitWarn) && (source.mLastWarning != now)) // need to warn
		return true;

	return false;
}

uint64 LoadFeeTrack::mulDiv(uint64 value, uint32 mul, uint32 div)
{ // compute (value)*(mul)/(div) - avoid overflow but keep precision
	static uint64 boundary = (0x00000000FFFFFFFF);
	if (value > boundary)							// Large value, avoid overflow
		return (value / div) * mul;
	else											// Normal value, preserve accuracy
		return (value * mul) / div;
}

uint64 LoadFeeTrack::scaleFeeLoad(uint64 fee)
{
	static uint64 midrange(0x00000000FFFFFFFF);
	bool big = (fee > midrange);

	{
		boost::mutex::scoped_lock sl(mLock);

		if (big)				// big fee, divide first to avoid overflow
			fee /= mBaseFee;
		else					// normal fee, multiply first for accuracy
			fee *= mBaseRef;

		fee = mulDiv(fee, std::max(mLocalTxnLoadFee, mRemoteTxnLoadFee), lftNormalFee);

		if (big)				// Fee was big to start, must now multiply
			fee *= mBaseRef;
		else					// Fee was small to start, mst now divide
			fee /= mBaseFee;
	}

	return fee;
}

uint64 LoadFeeTrack::scaleFeeBase(uint64 fee)
{

	{
		boost::mutex::scoped_lock sl(mLock);
		fee = mulDiv(fee, mBaseRef, mBaseFee);
	}

	return fee;
}

uint32 LoadFeeTrack::getBaseFee()
{
	boost::mutex::scoped_lock sl(mLock);
	return mBaseFee;
}

uint32 LoadFeeTrack::getRemoteFee()
{
	boost::mutex::scoped_lock sl(mLock);
	return mRemoteTxnLoadFee;
}

uint32 LoadFeeTrack::getLocalFee()
{
	boost::mutex::scoped_lock sl(mLock);
	return mLocalTxnLoadFee;
}

uint32 LoadFeeTrack::getBaseRef()
{
	boost::mutex::scoped_lock sl(mLock);
	return mBaseRef;
}

void LoadFeeTrack::setRemoteFee(uint32 f)
{
	boost::mutex::scoped_lock sl(mLock);
	mRemoteTxnLoadFee = f;
}

void LoadFeeTrack::raiseLocalFee()
{
	boost::mutex::scoped_lock sl(mLock);
	if (mLocalTxnLoadFee < mLocalTxnLoadFee) // make sure this fee takes effect
		mLocalTxnLoadFee = mLocalTxnLoadFee;

	mLocalTxnLoadFee += (mLocalTxnLoadFee / lftFeeIncFraction); // increment by 1/16th

	if (mLocalTxnLoadFee > lftFeeMax)
		mLocalTxnLoadFee = lftFeeMax;
}

void LoadFeeTrack::lowerLocalFee()
{
	boost::mutex::scoped_lock sl(mLock);
	mLocalTxnLoadFee -= (mLocalTxnLoadFee / lftFeeDecFraction ); // reduce by 1/16th

	if (mLocalTxnLoadFee < lftNormalFee)
		mLocalTxnLoadFee = lftNormalFee;
}

Json::Value LoadFeeTrack::getJson(int)
{
	Json::Value j(Json::objectValue);

	{
		boost::mutex::scoped_lock sl(mLock);

		// base_fee = The cost to send a "reference" transaction under no load, in millionths of a Ripple
		j["base_fee"] = Json::Value::UInt(mBaseFee);

		// load_fee = The cost to send a "reference" transaction now, in millionths of a Ripple
		j["load_fee"] = Json::Value::UInt(
			mulDiv(mBaseFee, std::max(mLocalTxnLoadFee, mRemoteTxnLoadFee), lftNormalFee));
	}

	return j;
}

// vim:ts=4
