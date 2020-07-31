/*
 * SpecialKeySpace.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2020 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "fdbclient/SpecialKeySpace.actor.h"
#include "flow/UnitTest.h"
#include "fdbclient/ManagementAPI.actor.h"
#include "fdbclient/StatusClient.h"
#include "flow/actorcompiler.h" // This must be the last #include.

std::unordered_map<SpecialKeySpace::MODULE, KeyRange> SpecialKeySpace::moduleToBoundary = {
	{ SpecialKeySpace::MODULE::TRANSACTION,
	  KeyRangeRef(LiteralStringRef("\xff\xff/transaction/"), LiteralStringRef("\xff\xff/transaction0")) },
	{ SpecialKeySpace::MODULE::WORKERINTERFACE,
	  KeyRangeRef(LiteralStringRef("\xff\xff/worker_interfaces/"), LiteralStringRef("\xff\xff/worker_interfaces0")) },
	{ SpecialKeySpace::MODULE::STATUSJSON, singleKeyRange(LiteralStringRef("\xff\xff/status/json")) },
	{ SpecialKeySpace::MODULE::CONNECTIONSTRING, singleKeyRange(LiteralStringRef("\xff\xff/connection_string")) },
	{ SpecialKeySpace::MODULE::CLUSTERFILEPATH, singleKeyRange(LiteralStringRef("\xff\xff/cluster_file_path")) },
	{ SpecialKeySpace::MODULE::METRICS,
	  KeyRangeRef(LiteralStringRef("\xff\xff/metrics/"), LiteralStringRef("\xff\xff/metrics0")) },
	{ SpecialKeySpace::MODULE::MANAGEMENT,
	  KeyRangeRef(LiteralStringRef("\xff\xff/conf/"), LiteralStringRef("\xff\xff/conf0")) },
	{ SpecialKeySpace::MODULE::ERRORMSG, singleKeyRange(LiteralStringRef("\xff\xff/error_message")) }
};

std::unordered_map<std::string, KeyRange> SpecialKeySpace::managementApiCommandToRange = {
	{ "exclude", KeyRangeRef(LiteralStringRef("excluded/"), LiteralStringRef("excluded0"))
	                 .withPrefix(moduleToBoundary[MODULE::MANAGEMENT].begin) },
	{ "failed", KeyRangeRef(LiteralStringRef("failed/"), LiteralStringRef("failed0"))
	                .withPrefix(moduleToBoundary[MODULE::MANAGEMENT].begin) }
};

std::unordered_set<std::string> SpecialKeySpace::options = { "exclude/force", "failed/force" };

// This function will move the given KeySelector as far as possible to the standard form:
// orEqual == false && offset == 1 (Standard form)
// If the corresponding key is not in the underlying key range, it will move over the range
// The cache object is used to cache the first read result from the rpc call during the key resolution,
// then when we need to do key resolution or result filtering,
// we, instead of rpc call, read from this cache object have consistent results
ACTOR Future<Void> moveKeySelectorOverRangeActor(const SpecialKeyRangeReadImpl* skrImpl, ReadYourWritesTransaction* ryw,
                                                 KeySelector* ks, Optional<Standalone<RangeResultRef>>* cache) {
	ASSERT(!ks->orEqual); // should be removed before calling
	ASSERT(ks->offset != 1); // never being called if KeySelector is already normalized

	state Key startKey(skrImpl->getKeyRange().begin);
	state Key endKey(skrImpl->getKeyRange().end);
	state Standalone<RangeResultRef> result;

	if (ks->offset < 1) {
		// less than the given key
		if (skrImpl->getKeyRange().contains(ks->getKey())) endKey = ks->getKey();
	} else {
		// greater than the given key
		if (skrImpl->getKeyRange().contains(ks->getKey())) startKey = ks->getKey();
	}
	ASSERT(startKey < endKey); // Note : startKey never equals endKey here

	TraceEvent(SevDebug, "NormalizeKeySelector")
	    .detail("OriginalKey", ks->getKey())
	    .detail("OriginalOffset", ks->offset)
	    .detail("SpecialKeyRangeStart", skrImpl->getKeyRange().begin)
	    .detail("SpecialKeyRangeEnd", skrImpl->getKeyRange().end);

	if (skrImpl->isAsync()) {
		const SpecialKeyRangeAsyncImpl* ptr = dynamic_cast<const SpecialKeyRangeAsyncImpl*>(skrImpl);
		Standalone<RangeResultRef> result_ = wait(ptr->getRange(ryw, KeyRangeRef(startKey, endKey), cache));
		result = result_;
	} else {
		Standalone<RangeResultRef> result_ = wait(skrImpl->getRange(ryw, KeyRangeRef(startKey, endKey)));
		result = result_;
	}

	if (result.size() == 0) {
		TraceEvent(SevDebug, "ZeroElementsIntheRange").detail("Start", startKey).detail("End", endKey);
		return Void();
	}
	// Note : KeySelector::setKey has byte limit according to the knobs, customize it if needed
	if (ks->offset < 1) {
		if (result.size() >= 1 - ks->offset) {
			ks->setKey(KeyRef(ks->arena(), result[result.size() - (1 - ks->offset)].key));
			ks->offset = 1;
		} else {
			ks->setKey(KeyRef(ks->arena(), result[0].key));
			ks->offset += result.size();
		}
	} else {
		if (result.size() >= ks->offset) {
			ks->setKey(KeyRef(ks->arena(), result[ks->offset - 1].key));
			ks->offset = 1;
		} else {
			ks->setKey(KeyRef(
			    ks->arena(),
			    keyAfter(result[result.size() - 1].key))); // TODO : the keyAfter will just return if key == \xff\xff
			ks->offset -= result.size();
		}
	}
	TraceEvent(SevDebug, "NormalizeKeySelector")
	    .detail("NormalizedKey", ks->getKey())
	    .detail("NormalizedOffset", ks->offset)
	    .detail("SpecialKeyRangeStart", skrImpl->getKeyRange().begin)
	    .detail("SpecialKeyRangeEnd", skrImpl->getKeyRange().end);
	return Void();
}

// This function will normalize the given KeySelector to a standard KeySelector:
// orEqual == false && offset == 1 (Standard form)
// If the corresponding key is outside the whole space, it will move to the begin or the end
// It does have overhead here since we query all keys twice in the worst case.
// However, moving the KeySelector while handling other parameters like limits makes the code much more complex and hard
// to maintain; Thus, separate each part to make the code easy to understand and more compact
// Boundary is the range of the legal key space, which, by default is the range of the module
// And (\xff\xff, \xff\xff\xff) if SPECIAL_KEY_SPACE_RELAXED is turned on
ACTOR Future<Void> normalizeKeySelectorActor(SpecialKeySpace* sks, ReadYourWritesTransaction* ryw, KeySelector* ks,
                                             KeyRangeRef boundary, int* actualOffset,
                                             Standalone<RangeResultRef>* result,
                                             Optional<Standalone<RangeResultRef>>* cache) {
	state RangeMap<Key, SpecialKeyRangeReadImpl*, KeyRangeRef>::iterator iter =
	    ks->offset < 1 ? sks->getReadImpls().rangeContainingKeyBefore(ks->getKey())
	                   : sks->getReadImpls().rangeContaining(ks->getKey());
	while ((ks->offset < 1 && iter->begin() > boundary.begin) || (ks->offset > 1 && iter->begin() < boundary.end)) {
		if (iter->value() != nullptr) {
			wait(moveKeySelectorOverRangeActor(iter->value(), ryw, ks, cache));
		}
		ks->offset < 1 ? --iter : ++iter;
	}
	*actualOffset = ks->offset;
	if (iter->begin() == boundary.begin || iter->begin() == boundary.end) ks->setKey(iter->begin());

	if (!ks->isFirstGreaterOrEqual()) {
		// The Key Selector clamps up to the legal key space
		TraceEvent(SevInfo, "ReadToBoundary")
		    .detail("TerminateKey", ks->getKey())
		    .detail("TerminateOffset", ks->offset);
		if (ks->offset < 1)
			result->readToBegin = true;
		else
			result->readThroughEnd = true;
		ks->offset = 1;
	}
	return Void();
}

SpecialKeySpace::SpecialKeySpace(KeyRef spaceStartKey, KeyRef spaceEndKey, bool testOnly)
  : range(KeyRangeRef(spaceStartKey, spaceEndKey)), readImpls(nullptr, spaceEndKey), writeImpls(nullptr, spaceEndKey),
    modules(testOnly ? SpecialKeySpace::MODULE::TESTONLY : SpecialKeySpace::MODULE::UNKNOWN, spaceEndKey) {
	// Default begin of KeyRangeMap is Key(), insert the range to update start key
	readImpls.insert(range, nullptr);
	writeImpls.insert(range, nullptr);
	if (!testOnly) modulesBoundaryInit(); // testOnly is used in the correctness workload
}

void SpecialKeySpace::modulesBoundaryInit() {
	for (const auto& pair : moduleToBoundary) {
		ASSERT(range.contains(pair.second));
		// Make sure the module is not overlapping with any registered read modules
		// Note: same like ranges, one module's end cannot be another module's start, relax the condition if needed
		ASSERT(modules.rangeContaining(pair.second.begin) == modules.rangeContaining(pair.second.end) &&
		       modules[pair.second.begin] == SpecialKeySpace::MODULE::UNKNOWN);
		modules.insert(pair.second, pair.first);
		// Note: Due to underlying implementation, the insertion here is important to make cross_module_read being
		// handled correctly
		readImpls.insert(pair.second, nullptr);
		writeImpls.insert(pair.second, nullptr);
	}
}

ACTOR Future<Standalone<RangeResultRef>> SpecialKeySpace::checkRYWValid(SpecialKeySpace* sks,
                                                                        ReadYourWritesTransaction* ryw,
                                                                        KeySelector begin, KeySelector end,
                                                                        GetRangeLimits limits, bool reverse) {
	ASSERT(ryw);
	choose {
		when(Standalone<RangeResultRef> result =
		         wait(SpecialKeySpace::getRangeAggregationActor(sks, ryw, begin, end, limits, reverse))) {
			return result;
		}
		when(wait(ryw->resetFuture())) { throw internal_error(); }
	}
}

ACTOR Future<Standalone<RangeResultRef>> SpecialKeySpace::getRangeAggregationActor(SpecialKeySpace* sks,
                                                                                   ReadYourWritesTransaction* ryw,
                                                                                   KeySelector begin, KeySelector end,
                                                                                   GetRangeLimits limits,
                                                                                   bool reverse) {
	// This function handles ranges which cover more than one keyrange and aggregates all results
	// KeySelector, GetRangeLimits and reverse are all handled here
	state Standalone<RangeResultRef> result;
	state Standalone<RangeResultRef> pairs;
	state RangeMap<Key, SpecialKeyRangeReadImpl*, KeyRangeRef>::iterator iter;
	state int actualBeginOffset;
	state int actualEndOffset;
	state KeyRangeRef moduleBoundary;
	// used to cache result from potential first read
	state Optional<Standalone<RangeResultRef>> cache;

	if (ryw->specialKeySpaceRelaxed()) {
		moduleBoundary = sks->range;
	} else {
		auto beginIter = sks->getModules().rangeContaining(begin.getKey());
		if (beginIter->begin() <= end.getKey() && end.getKey() <= beginIter->end()) {
			if (beginIter->value() == SpecialKeySpace::MODULE::UNKNOWN)
				throw special_keys_no_module_found();
			else
				moduleBoundary = beginIter->range();
		} else {
			TraceEvent(SevInfo, "SpecialKeyCrossModuleRead")
			    .detail("Begin", begin.toString())
			    .detail("End", end.toString())
			    .detail("BoundaryBegin", beginIter->begin())
			    .detail("BoundaryEnd", beginIter->end());
			throw special_keys_cross_module_read();
		}
	}

	wait(normalizeKeySelectorActor(sks, ryw, &begin, moduleBoundary, &actualBeginOffset, &result, &cache));
	wait(normalizeKeySelectorActor(sks, ryw, &end, moduleBoundary, &actualEndOffset, &result, &cache));
	// Handle all corner cases like what RYW does
	// return if range inverted
	if (actualBeginOffset >= actualEndOffset && begin.getKey() >= end.getKey()) {
		TEST(true);
		return RangeResultRef(false, false);
	}
	// If touches begin or end, return with readToBegin and readThroughEnd flags
	if (begin.getKey() == moduleBoundary.end || end.getKey() == moduleBoundary.begin) {
		TEST(true);
		return result;
	}
	state RangeMap<Key, SpecialKeyRangeReadImpl*, KeyRangeRef>::Ranges ranges =
	    sks->getReadImpls().intersectingRanges(KeyRangeRef(begin.getKey(), end.getKey()));
	// TODO : workaround to write this two together to make the code compact
	// The issue here is boost::iterator_range<> doest not provide rbegin(), rend()
	iter = reverse ? ranges.end() : ranges.begin();
	if (reverse) {
		while (iter != ranges.begin()) {
			--iter;
			if (iter->value() == nullptr) continue;
			KeyRangeRef kr = iter->range();
			KeyRef keyStart = kr.contains(begin.getKey()) ? begin.getKey() : kr.begin;
			KeyRef keyEnd = kr.contains(end.getKey()) ? end.getKey() : kr.end;
			if (iter->value()->isAsync() && cache.present()) {
				const SpecialKeyRangeAsyncImpl* ptr = dynamic_cast<const SpecialKeyRangeAsyncImpl*>(iter->value());
				Standalone<RangeResultRef> pairs_ = wait(ptr->getRange(ryw, KeyRangeRef(keyStart, keyEnd), &cache));
				pairs = pairs_;
			} else {
				Standalone<RangeResultRef> pairs_ = wait(iter->value()->getRange(ryw, KeyRangeRef(keyStart, keyEnd)));
				pairs = pairs_;
			}
			result.arena().dependsOn(pairs.arena());
			// limits handler
			for (int i = pairs.size() - 1; i >= 0; --i) {
				ASSERT(iter->range().contains(pairs[i].key));
				result.push_back(result.arena(), pairs[i]);
				// Note : behavior here is even the last k-v pair makes total bytes larger than specified, it's still
				// returned. In other words, the total size of the returned value (less the last entry) will be less
				// than byteLimit
				limits.decrement(pairs[i]);
				if (limits.isReached()) {
					result.more = true;
					result.readToBegin = false;
					return result;
				};
			}
		}
	} else {
		for (iter = ranges.begin(); iter != ranges.end(); ++iter) {
			if (iter->value() == nullptr) continue;
			KeyRangeRef kr = iter->range();
			KeyRef keyStart = kr.contains(begin.getKey()) ? begin.getKey() : kr.begin;
			KeyRef keyEnd = kr.contains(end.getKey()) ? end.getKey() : kr.end;
			if (iter->value()->isAsync() && cache.present()) {
				const SpecialKeyRangeAsyncImpl* ptr = dynamic_cast<const SpecialKeyRangeAsyncImpl*>(iter->value());
				Standalone<RangeResultRef> pairs_ = wait(ptr->getRange(ryw, KeyRangeRef(keyStart, keyEnd), &cache));
				pairs = pairs_;
			} else {
				Standalone<RangeResultRef> pairs_ = wait(iter->value()->getRange(ryw, KeyRangeRef(keyStart, keyEnd)));
				pairs = pairs_;
			}
			result.arena().dependsOn(pairs.arena());
			// limits handler
			for (int i = 0; i < pairs.size(); ++i) {
				ASSERT(iter->range().contains(pairs[i].key));
				result.push_back(result.arena(), pairs[i]);
				// Note : behavior here is even the last k-v pair makes total bytes larger than specified, it's still
				// returned. In other words, the total size of the returned value (less the last entry) will be less
				// than byteLimit
				limits.decrement(pairs[i]);
				if (limits.isReached()) {
					result.more = true;
					result.readThroughEnd = false;
					return result;
				};
			}
		}
	}
	return result;
}

Future<Standalone<RangeResultRef>> SpecialKeySpace::getRange(ReadYourWritesTransaction* ryw, KeySelector begin,
                                                             KeySelector end, GetRangeLimits limits, bool reverse) {
	// validate limits here
	if (!limits.isValid()) return range_limits_invalid();
	if (limits.isReached()) {
		TEST(true); // read limit 0
		return Standalone<RangeResultRef>();
	}
	// make sure orEqual == false
	begin.removeOrEqual(begin.arena());
	end.removeOrEqual(end.arena());

	if (begin.offset >= end.offset && begin.getKey() >= end.getKey()) {
		TEST(true); // range inverted
		return Standalone<RangeResultRef>();
	}

	return checkRYWValid(this, ryw, begin, end, limits, reverse);
}

ACTOR Future<Optional<Value>> SpecialKeySpace::getActor(SpecialKeySpace* sks, ReadYourWritesTransaction* ryw,
                                                        KeyRef key) {
	// use getRange to workaround this
	Standalone<RangeResultRef> result =
	    wait(sks->getRange(ryw, KeySelector(firstGreaterOrEqual(key)), KeySelector(firstGreaterOrEqual(keyAfter(key))),
	                       GetRangeLimits(CLIENT_KNOBS->TOO_MANY), false));
	ASSERT(result.size() <= 1);
	if (result.size()) {
		return Optional<Value>(result[0].value);
	} else {
		return Optional<Value>();
	}
}

Future<Optional<Value>> SpecialKeySpace::get(ReadYourWritesTransaction* ryw, const Key& key) {
	return getActor(this, ryw, key);
}

void SpecialKeySpace::set(ReadYourWritesTransaction* ryw, const KeyRef& key, const ValueRef& value) {
	if (!ryw->specialKeySpaceChangeConfiguration()) throw special_keys_write_disabled();
	auto impl = writeImpls[key];
	if (impl == nullptr) {
		TraceEvent(SevDebug, "SpecialKeySpaceNoWriteModuleFound")
		    .detail("Key", key.toString())
		    .detail("Value", value.toString());
		throw special_keys_no_write_module_found();
	}
	return impl->set(ryw, key, value);
}

void SpecialKeySpace::clear(ReadYourWritesTransaction* ryw, const KeyRangeRef& range) {
	if (!ryw->specialKeySpaceChangeConfiguration()) throw special_keys_write_disabled();
	if (range.empty()) return;
	auto begin = writeImpls[range.begin];
	auto end = writeImpls.rangeContainingKeyBefore(range.end)->value();
	if (begin != end) {
		TraceEvent(SevDebug, "SpecialKeySpaceCrossModuleClear").detail("Range", range.toString());
		throw special_keys_cross_module_clear(); // ban cross module clear
	} else if (begin == nullptr) {
		TraceEvent(SevDebug, "SpecialKeySpaceNoWriteModuleFound").detail("Range", range.toString());
		throw special_keys_no_write_module_found();
	}
	return begin->clear(ryw, range);
}

void SpecialKeySpace::clear(ReadYourWritesTransaction* ryw, const KeyRef& key) {
	if (!ryw->specialKeySpaceChangeConfiguration()) throw special_keys_write_disabled();
	auto impl = writeImpls[key];
	if (impl == nullptr) throw special_keys_no_write_module_found();
	return impl->clear(ryw, key);
}

void SpecialKeySpace::registerKeyRange(SpecialKeySpace::MODULE module, SpecialKeySpace::IMPLTYPE type,
                                       const KeyRangeRef& kr, SpecialKeyRangeReadImpl* impl) {
	// module boundary check
	if (module == SpecialKeySpace::MODULE::TESTONLY)
		ASSERT(normalKeys.contains(kr));
	else
		ASSERT(moduleToBoundary.at(module).contains(kr));
	// make sure the registered range is not overlapping with existing ones
	// Note: kr.end should not be the same as another range's begin, although it should work even they are the same
	for (auto iter = readImpls.rangeContaining(kr.begin); true; ++iter) {
		ASSERT(iter->value() == nullptr);
		if (iter == readImpls.rangeContaining(kr.end))
			break; // Note: relax the condition that the end can be another range's start, if needed
	}
	readImpls.insert(kr, impl);
	// if rw, it means the module can do both read and write
	if (type == SpecialKeySpace::IMPLTYPE::READWRITE) {
		// since write impls are always subset of read impls,
		// no need to check overlapped registration
		auto rwImpl = dynamic_cast<SpecialKeyRangeRWImpl*>(impl);
		ASSERT(rwImpl);
		writeImpls.insert(kr, rwImpl);
	}
}

Key SpecialKeySpace::decode(const KeyRef& key) {
	auto impl = writeImpls[key];
	ASSERT(impl != nullptr);
	return impl->decode(key);
}

KeyRange SpecialKeySpace::decode(const KeyRangeRef& kr) {
	// Only allow to decode key range in the same underlying impl range
	auto begin = writeImpls.rangeContaining(kr.begin);
	ASSERT(begin->value() != nullptr);
	auto end = writeImpls.rangeContainingKeyBefore(kr.end);
	ASSERT(begin == end);
	return KeyRangeRef(begin->value()->decode(kr.begin), begin->value()->decode(kr.end));
}

ACTOR Future<Void> commitActor(SpecialKeySpace* sks, ReadYourWritesTransaction* ryw) {
	state RangeMap<Key, std::pair<bool, Optional<Value>>, KeyRangeRef>::Ranges ranges =
	    ryw->getSpecialKeySpaceWriteMap().containedRanges(specialKeys);
	state RangeMap<Key, std::pair<bool, Optional<Value>>, KeyRangeRef>::iterator iter = ranges.begin();
	state std::set<SpecialKeyRangeRWImpl*> writeModulePtrs;
	while (iter != ranges.end()) {
		std::pair<bool, Optional<Value>> entry = iter->value();
		if (entry.first) {
			auto modulePtr = sks->getRWImpls().rangeContaining(iter->begin())->value();
			writeModulePtrs.insert(modulePtr);
		}
		++iter;
	}
	state std::set<SpecialKeyRangeRWImpl*>::const_iterator it;
	for (it = writeModulePtrs.begin(); it != writeModulePtrs.end(); ++it) {
		Optional<std::string> msg = wait((*it)->commit(ryw));
		if (msg.present()) {
			ryw->setSpecialKeySpaceErrorMsg(msg.get());
			TraceEvent(SevDebug, "SpecialKeySpaceManagemetnAPIError")
			    .detail("Reason", msg.get())
			    .detail("Range", (*it)->getKeyRange().toString());
			throw special_keys_api_failure();
		}
	}
	return Void();
}

Future<Void> SpecialKeySpace::commit(ReadYourWritesTransaction* ryw) {
	return commitActor(this, ryw);
}

ReadConflictRangeImpl::ReadConflictRangeImpl(KeyRangeRef kr) : SpecialKeyRangeReadImpl(kr) {}

ACTOR static Future<Standalone<RangeResultRef>> getReadConflictRangeImpl(ReadYourWritesTransaction* ryw, KeyRange kr) {
	wait(ryw->pendingReads());
	return ryw->getReadConflictRangeIntersecting(kr);
}

Future<Standalone<RangeResultRef>> ReadConflictRangeImpl::getRange(ReadYourWritesTransaction* ryw,
                                                                   KeyRangeRef kr) const {
	return getReadConflictRangeImpl(ryw, kr);
}

WriteConflictRangeImpl::WriteConflictRangeImpl(KeyRangeRef kr) : SpecialKeyRangeReadImpl(kr) {}

Future<Standalone<RangeResultRef>> WriteConflictRangeImpl::getRange(ReadYourWritesTransaction* ryw,
                                                                    KeyRangeRef kr) const {
	return ryw->getWriteConflictRangeIntersecting(kr);
}

ConflictingKeysImpl::ConflictingKeysImpl(KeyRangeRef kr) : SpecialKeyRangeReadImpl(kr) {}

Future<Standalone<RangeResultRef>> ConflictingKeysImpl::getRange(ReadYourWritesTransaction* ryw, KeyRangeRef kr) const {
	Standalone<RangeResultRef> result;
	if (ryw->getTransactionInfo().conflictingKeys) {
		auto krMapPtr = ryw->getTransactionInfo().conflictingKeys.get();
		auto beginIter = krMapPtr->rangeContaining(kr.begin);
		if (beginIter->begin() != kr.begin) ++beginIter;
		auto endIter = krMapPtr->rangeContaining(kr.end);
		for (auto it = beginIter; it != endIter; ++it) {
			// it->begin() is stored in the CoalescedKeyRangeMap in TransactionInfo
			// it->value() is always constants in SystemData.cpp
			// Thus, push_back() can be used
			result.push_back(result.arena(), KeyValueRef(it->begin(), it->value()));
		}
		if (endIter->begin() != kr.end)
			result.push_back(result.arena(), KeyValueRef(endIter->begin(), endIter->value()));
	}
	return result;
}

ACTOR Future<Standalone<RangeResultRef>> ddMetricsGetRangeActor(ReadYourWritesTransaction* ryw, KeyRangeRef kr) {
	try {
		auto keys = kr.removePrefix(ddStatsRange.begin);
		Standalone<VectorRef<DDMetricsRef>> resultWithoutPrefix =
		    wait(waitDataDistributionMetricsList(ryw->getDatabase(), keys, CLIENT_KNOBS->STORAGE_METRICS_SHARD_LIMIT));
		Standalone<RangeResultRef> result;
		for (const auto& ddMetricsRef : resultWithoutPrefix) {
			// each begin key is the previous end key, thus we only encode the begin key in the result
			KeyRef beginKey = ddMetricsRef.beginKey.withPrefix(ddStatsRange.begin, result.arena());
			// Use json string encoded in utf-8 to encode the values, easy for adding more fields in the future
			json_spirit::mObject statsObj;
			statsObj["shard_bytes"] = ddMetricsRef.shardBytes;
			std::string statsString =
			    json_spirit::write_string(json_spirit::mValue(statsObj), json_spirit::Output_options::raw_utf8);
			ValueRef bytes(result.arena(), statsString);
			result.push_back(result.arena(), KeyValueRef(beginKey, bytes));
		}
		return result;
	} catch (Error& e) {
		throw;
	}
}

DDStatsRangeImpl::DDStatsRangeImpl(KeyRangeRef kr) : SpecialKeyRangeAsyncImpl(kr) {}

Future<Standalone<RangeResultRef>> DDStatsRangeImpl::getRange(ReadYourWritesTransaction* ryw, KeyRangeRef kr) const {
	return ddMetricsGetRangeActor(ryw, kr);
}

Key SpecialKeySpace::getManagementApiCommandOptionSpecialKey(const std::string& command, const std::string& option) {
	Key prefix = LiteralStringRef("options/").withPrefix(moduleToBoundary[MODULE::MANAGEMENT].begin);
	auto pair = command + "/" + option;
	ASSERT(options.find(pair) != options.end());
	return prefix.withSuffix(pair);
}

ManagementCommandsOptionsImpl::ManagementCommandsOptionsImpl(KeyRangeRef kr) : SpecialKeyRangeRWImpl(kr) {}

Future<Standalone<RangeResultRef>> ManagementCommandsOptionsImpl::getRange(ReadYourWritesTransaction* ryw,
                                                                           KeyRangeRef kr) const {
	Standalone<RangeResultRef> result;
	// Since we only have limit number of options, a brute force loop here is enough
	for (const auto& option : SpecialKeySpace::getManagementApiOptionsSet()) {
		auto key = getKeyRange().begin.withSuffix(option);
		// ignore all invalid keys
		auto r = ryw->getSpecialKeySpaceWriteMap()[key];
		if (kr.contains(key) && r.first && r.second.present())
			result.push_back(result.arena(), KeyValueRef(key, ValueRef()));
	}
	return result;
}

void ManagementCommandsOptionsImpl::set(ReadYourWritesTransaction* ryw, const KeyRef& key, const ValueRef& value) {
	std::string option = key.removePrefix(getKeyRange().begin).toString();
	// ignore all invalid keys
	if (SpecialKeySpace::getManagementApiOptionsSet().find(option) !=
	    SpecialKeySpace::getManagementApiOptionsSet().end()) {
		TraceEvent(SevDebug, "ManagementApiOption").detail("Option", option).detail("Key", key);
		ryw->getSpecialKeySpaceWriteMap().insert(key, std::make_pair(true, Optional<Value>(value)));
	}
}

void ManagementCommandsOptionsImpl::clear(ReadYourWritesTransaction* ryw, const KeyRangeRef& range) {
	ryw->getSpecialKeySpaceWriteMap().rawErase(range);
}

void ManagementCommandsOptionsImpl::clear(ReadYourWritesTransaction* ryw, const KeyRef& key) {
	std::string option = key.removePrefix(getKeyRange().begin).toString();
	// ignore all invalid keys
	if (SpecialKeySpace::getManagementApiOptionsSet().find(option) !=
	    SpecialKeySpace::getManagementApiOptionsSet().end()) {
		ryw->getSpecialKeySpaceWriteMap().rawErase(singleKeyRange(key));
	}
}

Future<Optional<std::string>> ManagementCommandsOptionsImpl::commit(ReadYourWritesTransaction* ryw) {
	// Nothing to do, keys should be used by other impls' commit callback
	return Optional<std::string>();
}

// read from rwModule
ACTOR Future<Standalone<RangeResultRef>> rwModuleGetRangeActor(ReadYourWritesTransaction* ryw, KeyRangeRef range,
                                                               KeyRangeRef kr) {
	KeyRange krWithoutPrefix = ryw->getDatabase()->specialKeySpace->decode(kr);
	Standalone<RangeResultRef> resultWithoutPrefix = wait(ryw->getRange(krWithoutPrefix, CLIENT_KNOBS->TOO_MANY));
	ASSERT(!resultWithoutPrefix.more && resultWithoutPrefix.size() < CLIENT_KNOBS->TOO_MANY);
	Standalone<RangeResultRef> result;
	if (ryw->readYourWritesDisabled()) {
		for (const KeyValueRef& kv : resultWithoutPrefix) {
			KeyRef rk = kv.key.withPrefix(normalKeys.end, result.arena());
			ValueRef rv(result.arena(), kv.value);
			result.push_back(result.arena(), KeyValueRef(rk, rv));
		}
	} else {
		RangeMap<Key, std::pair<bool, Optional<Value>>, KeyRangeRef>::Ranges ranges =
		    ryw->getSpecialKeySpaceWriteMap().containedRanges(range);
		RangeMap<Key, std::pair<bool, Optional<Value>>, KeyRangeRef>::iterator iter = ranges.begin();
		int index = 0;
		while (iter != ranges.end()) {
			// add all previous entries into result
			while (index < resultWithoutPrefix.size() &&
			       resultWithoutPrefix[index].key.withPrefix(normalKeys.end) < iter->begin()) {
				const KeyValueRef& kv = resultWithoutPrefix[index];
				KeyRef rk = kv.key.withPrefix(normalKeys.end, result.arena());
				ValueRef rv(result.arena(), kv.value);
				result.push_back(result.arena(), KeyValueRef(rk, rv));
				++index;
			}
			std::pair<bool, Optional<Value>> entry = iter->value();
			if (entry.first) {
				// add the writen entries if exists
				if (entry.second.present()) {
					KeyRef rk(result.arena(), iter->begin());
					ValueRef rv(result.arena(), entry.second.get());
					result.push_back(result.arena(), KeyValueRef(rk, rv));
				}
				// move index to skip all entries in the iter->range
				while (index < resultWithoutPrefix.size() &&
				       iter->range().contains(resultWithoutPrefix[index].key.withPrefix(normalKeys.end)))
					++index;
			}
			++iter;
		}
		// add all remaining entries into result
		while (index < resultWithoutPrefix.size()) {
			const KeyValueRef& kv = resultWithoutPrefix[index];
			KeyRef rk = kv.key.withPrefix(normalKeys.end, result.arena());
			ValueRef rv(result.arena(), kv.value);
			result.push_back(result.arena(), KeyValueRef(rk, rv));
			++index;
		}
	}
	return result;
}

ExcludeServersRangeImpl::ExcludeServersRangeImpl(KeyRangeRef kr) : SpecialKeyRangeRWImpl(kr) {}

Future<Standalone<RangeResultRef>> ExcludeServersRangeImpl::getRange(ReadYourWritesTransaction* ryw,
                                                                     KeyRangeRef kr) const {
	return rwModuleGetRangeActor(ryw, getKeyRange(), kr);
}

void ExcludeServersRangeImpl::set(ReadYourWritesTransaction* ryw, const KeyRef& key, const ValueRef& value) {
	ryw->getSpecialKeySpaceWriteMap().insert(key, std::make_pair(true, Optional<Value>(value)));
}

void ExcludeServersRangeImpl::clear(ReadYourWritesTransaction* ryw, const KeyRef& key) {
	ryw->getSpecialKeySpaceWriteMap().insert(key, std::make_pair(true, Optional<Value>()));
}

void ExcludeServersRangeImpl::clear(ReadYourWritesTransaction* ryw, const KeyRangeRef& range) {
	ryw->getSpecialKeySpaceWriteMap().insert(range, std::make_pair(true, Optional<Value>()));
}

bool parseNetWorkAddrFromKeys(ReadYourWritesTransaction* ryw, KeyRangeRef range,
                              std::vector<AddressExclusion>& addresses, std::set<AddressExclusion>& exclusions,
                              Optional<std::string>& msg) {
	auto ranges = ryw->getSpecialKeySpaceWriteMap().containedRanges(range);
	auto iter = ranges.begin();
	while (iter != ranges.end()) {
		auto entry = iter->value();
		// only check for exclude(set) operation, include(clear) are not checked
		TraceEvent(SevInfo, "ParseNetworkAddress")
		    .detail("Valid", entry.first)
		    .detail("Set", entry.second.present())
		    .detail("Key", iter->begin().toString());
		if (entry.first && entry.second.present()) {
			Key address = iter->begin().removePrefix(range.begin);
			auto a = AddressExclusion::parse(address);
			if (!a.isValid()) {
				std::string error = "ERROR: \'" + address.toString() + "\' is not a valid network endpoint address\n";
				if (address.toString().find(":tls") != std::string::npos)
					error += "        Do not include the `:tls' suffix when naming a process\n";
				msg = ManagementAPIError::toJsonString(false, entry.second.present() ? "exclude" : "include", error);
				return false;
			}
			addresses.push_back(a);
			exclusions.insert(a);
		}
		++iter;
	}
	return true;
}

ACTOR Future<bool> checkExclusion(Database db, std::vector<AddressExclusion>* addresses,
                                  std::set<AddressExclusion>* exclusions, bool markFailed, Optional<std::string>* msg) {

	if (markFailed) {
		state bool safe;
		try {
			bool _safe = wait(checkSafeExclusions(db, *addresses));
			safe = _safe;
		} catch (Error& e) {
			TraceEvent("CheckSafeExclusionsError").error(e);
			safe = false;
		}
		if (!safe) {
			std::string temp = "ERROR: It is unsafe to exclude the specified servers at this time.\n"
			                   "Please check that this exclusion does not bring down an entire storage team.\n"
			                   "Please also ensure that the exclusion will keep a majority of coordinators alive.\n"
			                   "You may add more storage processes or coordinators to make the operation safe.\n";
			*msg = ManagementAPIError::toJsonString(false, markFailed ? "exclude failed" : "exclude", temp);
			return false;
		}
	}
	StatusObject status = wait(StatusClient::statusFetcher(db));
	state std::string errorString =
	    "ERROR: Could not calculate the impact of this exclude on the total free space in the cluster.\n"
	    "Please try the exclude again in 30 seconds.\n";

	StatusObjectReader statusObj(status);

	StatusObjectReader statusObjCluster;
	if (!statusObj.get("cluster", statusObjCluster)) {
		*msg = ManagementAPIError::toJsonString(false, markFailed ? "exclude failed" : "exclude", errorString);
		return false;
	}

	StatusObjectReader processesMap;
	if (!statusObjCluster.get("processes", processesMap)) {
		*msg = ManagementAPIError::toJsonString(false, markFailed ? "exclude failed" : "exclude", errorString);
		return false;
	}

	state int ssTotalCount = 0;
	state int ssExcludedCount = 0;
	state double worstFreeSpaceRatio = 1.0;
	try {
		for (auto proc : processesMap.obj()) {
			bool storageServer = false;
			StatusArray rolesArray = proc.second.get_obj()["roles"].get_array();
			for (StatusObjectReader role : rolesArray) {
				if (role["role"].get_str() == "storage") {
					storageServer = true;
					break;
				}
			}
			// Skip non-storage servers in free space calculation
			if (!storageServer) continue;

			StatusObjectReader process(proc.second);
			std::string addrStr;
			if (!process.get("address", addrStr)) {
				*msg = ManagementAPIError::toJsonString(false, markFailed ? "exclude failed" : "exclude", errorString);
				return false;
			}
			NetworkAddress addr = NetworkAddress::parse(addrStr);
			bool excluded =
			    (process.has("excluded") && process.last().get_bool()) || addressExcluded(*exclusions, addr);
			ssTotalCount++;
			if (excluded) ssExcludedCount++;

			if (!excluded) {
				StatusObjectReader disk;
				if (!process.get("disk", disk)) {
					*msg =
					    ManagementAPIError::toJsonString(false, markFailed ? "exclude failed" : "exclude", errorString);
					return false;
				}

				int64_t total_bytes;
				if (!disk.get("total_bytes", total_bytes)) {
					*msg =
					    ManagementAPIError::toJsonString(false, markFailed ? "exclude failed" : "exclude", errorString);
					return false;
				}

				int64_t free_bytes;
				if (!disk.get("free_bytes", free_bytes)) {
					*msg =
					    ManagementAPIError::toJsonString(false, markFailed ? "exclude failed" : "exclude", errorString);
					return false;
				}

				worstFreeSpaceRatio = std::min(worstFreeSpaceRatio, double(free_bytes) / total_bytes);
			}
		}
	} catch (...) // std::exception
	{
		*msg = ManagementAPIError::toJsonString(false, markFailed ? "exclude failed" : "exclude", errorString);
		return false;
	}

	if (ssExcludedCount == ssTotalCount ||
	    (1 - worstFreeSpaceRatio) * ssTotalCount / (ssTotalCount - ssExcludedCount) > 0.9) {
		std::string temp = "ERROR: This exclude may cause the total free space in the cluster to drop below 10%.";
		*msg = ManagementAPIError::toJsonString(false, markFailed ? "exclude failed" : "exclude", temp);
		return false;
	}
	return true;
}

void includeServers(ReadYourWritesTransaction* ryw) {
	ryw->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
	ryw->setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);
	ryw->setOption(FDBTransactionOptions::LOCK_AWARE);
	ryw->setOption(FDBTransactionOptions::USE_PROVISIONAL_PROXIES);
	// includeServers might be used in an emergency transaction, so make sure it is retry-self-conflicting and
	// CAUSAL_WRITE_RISKY
	ryw->setOption(FDBTransactionOptions::CAUSAL_WRITE_RISKY);
	std::string versionKey = deterministicRandom()->randomUniqueID().toString();
	// for exluded servers
	auto ranges = ryw->getSpecialKeySpaceWriteMap().containedRanges(excludedServersKeys.withPrefix(normalKeys.end));
	auto iter = ranges.begin();
	Transaction& tr = ryw->getTransaction();
	while (iter != ranges.end()) {
		auto entry = iter->value();
		if (entry.first && !entry.second.present()) {
			tr.addReadConflictRange(singleKeyRange(excludedServersVersionKey));
			tr.set(excludedServersVersionKey, versionKey);
			tr.clear(ryw->getDatabase()->specialKeySpace->decode(iter->range()));
		}
		++iter;
	}
	// for failed servers
	ranges = ryw->getSpecialKeySpaceWriteMap().containedRanges(failedServersKeys.withPrefix(normalKeys.end));
	iter = ranges.begin();
	while (iter != ranges.end()) {
		auto entry = iter->value();
		if (entry.first && !entry.second.present()) {
			tr.addReadConflictRange(singleKeyRange(failedServersVersionKey));
			tr.set(failedServersVersionKey, versionKey);
			tr.clear(ryw->getDatabase()->specialKeySpace->decode(iter->range()));
		}
		++iter;
	}
}

ACTOR Future<Optional<std::string>> excludeCommitActor(ReadYourWritesTransaction* ryw, bool failed) {
	// parse network addresses
	state Optional<std::string> result;
	state std::vector<AddressExclusion> addresses;
	state std::set<AddressExclusion> exclusions;
	if (!parseNetWorkAddrFromKeys(
	        ryw, failed ? failedServersKeys.withPrefix(normalKeys.end) : excludedServersKeys.withPrefix(normalKeys.end),
	        addresses, exclusions, result))
		return result;
	// If force option is not set, we need to do safety check
	auto force = ryw->getSpecialKeySpaceWriteMap()[SpecialKeySpace::getManagementApiCommandOptionSpecialKey(
	    failed ? "failed" : "exclude", "force")];
	// only do safety check when we have servers to be excluded and the force option key is not set
	if (addresses.size() && !(force.first && force.second.present())) {
		bool safe = wait(checkExclusion(ryw->getDatabase(), &addresses, &exclusions, failed, &result));
		if (!safe) return result;
	}
	excludeServers(ryw->getTransaction(), addresses, failed);
	includeServers(ryw);

	return result;
}

Future<Optional<std::string>> ExcludeServersRangeImpl::commit(ReadYourWritesTransaction* ryw) {
	return excludeCommitActor(ryw, false);
}

FailedServersRangeImpl::FailedServersRangeImpl(KeyRangeRef kr) : SpecialKeyRangeRWImpl(kr) {}

Future<Standalone<RangeResultRef>> FailedServersRangeImpl::getRange(ReadYourWritesTransaction* ryw,
                                                                    KeyRangeRef kr) const {
	return rwModuleGetRangeActor(ryw, getKeyRange(), kr);
}

void FailedServersRangeImpl::set(ReadYourWritesTransaction* ryw, const KeyRef& key, const ValueRef& value) {
	ryw->getSpecialKeySpaceWriteMap().insert(key, std::make_pair(true, Optional<Value>(value)));
}

void FailedServersRangeImpl::clear(ReadYourWritesTransaction* ryw, const KeyRef& key) {
	ryw->getSpecialKeySpaceWriteMap().insert(key, std::make_pair(true, Optional<Value>()));
}

void FailedServersRangeImpl::clear(ReadYourWritesTransaction* ryw, const KeyRangeRef& range) {
	ryw->getSpecialKeySpaceWriteMap().insert(range, std::make_pair(true, Optional<Value>()));
}

Future<Optional<std::string>> FailedServersRangeImpl::commit(ReadYourWritesTransaction* ryw) {
	return excludeCommitActor(ryw, true);
}

ACTOR Future<Standalone<RangeResultRef>> ExclusionInProgressActor(ReadYourWritesTransaction* ryw, KeyRef prefix,
                                                                  KeyRangeRef kr) {
	state Standalone<RangeResultRef> result;
	state Transaction& tr = ryw->getTransaction();
	tr.setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
	tr.setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE); // necessary?
	tr.setOption(FDBTransactionOptions::LOCK_AWARE);

	state std::vector<AddressExclusion> excl = wait((getExcludedServers(&tr)));
	state std::set<AddressExclusion> exclusions(excl.begin(), excl.end());
	state std::set<NetworkAddress> inProgressExclusion;
	// Just getting a consistent read version proves that a set of tlogs satisfying the exclusions has completed
	// recovery Check that there aren't any storage servers with addresses violating the exclusions
	state Standalone<RangeResultRef> serverList = wait(tr.getRange(serverListKeys, CLIENT_KNOBS->TOO_MANY));
	ASSERT(!serverList.more && serverList.size() < CLIENT_KNOBS->TOO_MANY);

	for (auto& s : serverList) {
		auto addresses = decodeServerListValue(s.value).getKeyValues.getEndpoint().addresses;
		if (addressExcluded(exclusions, addresses.address)) {
			inProgressExclusion.insert(addresses.address);
		}
		if (addresses.secondaryAddress.present() && addressExcluded(exclusions, addresses.secondaryAddress.get())) {
			inProgressExclusion.insert(addresses.secondaryAddress.get());
		}
	}

	Optional<Standalone<StringRef>> value = wait(tr.get(logsKey));
	ASSERT(value.present());
	auto logs = decodeLogsValue(value.get());
	for (auto const& log : logs.first) {
		if (log.second == NetworkAddress() || addressExcluded(exclusions, log.second)) {
			inProgressExclusion.insert(log.second);
		}
	}
	for (auto const& log : logs.second) {
		if (log.second == NetworkAddress() || addressExcluded(exclusions, log.second)) {
			inProgressExclusion.insert(log.second);
		}
	}

	for (auto const& address : inProgressExclusion) {
		Key addrKey = prefix.withSuffix(address.toString());
		ASSERT(addrKey.startsWith(LiteralStringRef("\xff\xff/conf/")));
		if (kr.contains(addrKey)) {
			result.push_back(result.arena(), KeyValueRef(addrKey, ValueRef()));
			result.arena().dependsOn(addrKey.arena());
		}
	}
	return result;
}

ExclusionInProgressRangeImpl::ExclusionInProgressRangeImpl(KeyRangeRef kr) : SpecialKeyRangeAsyncImpl(kr) {}

Future<Standalone<RangeResultRef>> ExclusionInProgressRangeImpl::getRange(ReadYourWritesTransaction* ryw,
                                                                          KeyRangeRef kr) const {
	return ExclusionInProgressActor(ryw, getKeyRange().begin, kr);
}
