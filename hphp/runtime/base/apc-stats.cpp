/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include "hphp/runtime/base/apc-stats.h"

#include "hphp/runtime/base/typed-value.h"
#include "hphp/runtime/base/array-data.h"
#include "hphp/runtime/base/packed-array.h"
#include "hphp/runtime/base/packed-array-defs.h"
#include "hphp/runtime/base/mixed-array-defs.h"
#include "hphp/runtime/base/apc-handle.h"
#include "hphp/runtime/base/apc-array.h"
#include "hphp/runtime/base/apc-object.h"
#include "hphp/runtime/ext/apc/ext_apc.h"

#include "hphp/util/trace.h"


namespace HPHP {

TRACE_SET_MOD(apc);


///////////////////////////////////////////////////////////////////////////////

namespace {

size_t getMemSize(const TypedValue* tv, bool recurse = true) {
  const auto& v = tvAsCVarRef(tv);
  auto type = v.getType();
  if (isArrayLikeType(type)) {
    if (!recurse) return 0;
    auto a = v.getArrayData();
    return a->isStatic() ? sizeof(v) : getMemSize(a);
  }
  if (type == KindOfString) {
    return getMemSize(v.getStringData());
  }
  if (!isRefcountedType(type)) {
    return sizeof(Variant);
  }
  assertx(!"Unsupported Variant type for getMemSize()");
  return 0;
}

}

size_t getMemSize(const APCHandle* handle) {
  switch (handle->kind()) {
    case APCKind::Uninit:
    case APCKind::Null:
    case APCKind::Bool:
    case APCKind::Int:
    case APCKind::Double:
    case APCKind::PersistentFunc:
    case APCKind::FuncEntity:
    case APCKind::StaticString:
    case APCKind::StaticArray:
    case APCKind::StaticVec:
    case APCKind::StaticDict:
    case APCKind::StaticShape:
    case APCKind::StaticKeyset:
      return sizeof(APCHandle);

    case APCKind::UncountedString:
      return sizeof(APCTypedValue) +
             getMemSize(APCTypedValue::fromHandle(handle)->getStringData());
    case APCKind::SharedString:
      return getMemSize(APCString::fromHandle(handle));

    case APCKind::SerializedArray:
    case APCKind::SerializedVec:
    case APCKind::SerializedDict:
    case APCKind::SerializedShape:
    case APCKind::SerializedKeyset:
      return getMemSize(APCString::fromHandle(handle));

    case APCKind::UncountedArray:
      return sizeof(APCTypedValue) +
             getMemSize(APCTypedValue::fromHandle(handle)->getArrayData());
    case APCKind::UncountedVec:
      return sizeof(APCTypedValue) +
             getMemSize(APCTypedValue::fromHandle(handle)->getVecData());
    case APCKind::UncountedDict:
      return sizeof(APCTypedValue) +
             getMemSize(APCTypedValue::fromHandle(handle)->getDictData());
    case APCKind::UncountedShape:
      return sizeof(APCTypedValue) +
             getMemSize(APCTypedValue::fromHandle(handle)->getShapeData());
    case APCKind::UncountedKeyset:
      return sizeof(APCTypedValue) +
             getMemSize(APCTypedValue::fromHandle(handle)->getKeysetData());

    case APCKind::SharedArray:
    case APCKind::SharedPackedArray:
    case APCKind::SharedVec:
    case APCKind::SharedDict:
    case APCKind::SharedShape:
    case APCKind::SharedKeyset:
    case APCKind::SharedVArray:
    case APCKind::SharedDArray:
      return getMemSize(APCArray::fromHandle(handle));

    case APCKind::SerializedObject:
      return getMemSize(APCString::fromHandle(handle));

    case APCKind::SharedObject:
    case APCKind::SharedCollection:
      return getMemSize(APCObject::fromHandle(handle));
  }
  assertx(!"Unsupported APCHandle Type in getMemSize");
  return 0;
}

size_t getMemSize(const APCArray* arr) {
  auto memSize = sizeof(APCArray);
  auto size = arr->size();
  if (arr->isPacked()) {
    memSize += sizeof(APCHandle*) * size;
    for (auto i = 0; i < size; i++) {
      memSize += getMemSize(arr->getValue(i));
    }
  } else {
    memSize += sizeof(int) * (arr->m.m_capacity_mask + 1) +
               sizeof(APCArray::Bucket) * size;
    auto b = arr->buckets();
    for (auto i = 0; i < size; i++) {
      memSize += getMemSize(b[i].key);
      memSize += getMemSize(b[i].val);
    }
  }
  return memSize;
}

size_t getMemSize(const APCObject* obj) {
  if (obj->isPersistent()) {
    auto size = sizeof(APCObject) +
                sizeof(APCHandle*) * obj->m_propCount;
    auto prop = obj->persistentProps();
    auto const propEnd = prop + obj->m_propCount;
    for (; prop != propEnd; ++prop) {
      if (*prop) size += getMemSize(*prop);
    }
    return size;
  }

  auto size = sizeof(APCObject) +
              sizeof(APCObject::Prop) * obj->m_propCount;
  auto prop = obj->props();
  auto const propEnd = prop + obj->m_propCount;
  // we don't add property names and class names (or Class*) in Prop
  // assuming that is static data not owned or accounted by the APCObject
  for (; prop != propEnd; ++prop) {
    if (prop->val) size += getMemSize(prop->val);
  }
  return size;
}

size_t getMemSize(const ArrayData* arr, bool recurse) {
  switch (arr->kind()) {
  case ArrayData::ArrayKind::kPackedKind:
  case ArrayData::ArrayKind::kVecKind: {
    // we want to measure just the overhead of the array
    auto size = PackedArray::heapSize(arr) - (sizeof(TypedValue) * arr->m_size);
    auto const values = packedData(arr);
    auto const last = values + arr->m_size;
    for (auto ptr = values; ptr != last; ++ptr) {
      size += getMemSize(ptr, recurse);
    }
    return size;
  }
  case ArrayData::ArrayKind::kShapeKind:
  case ArrayData::ArrayKind::kDictKind:
  case ArrayData::ArrayKind::kMixedKind: {
    auto const mixed = MixedArray::asMixed(arr);
    auto size = sizeof(MixedArray) +
                sizeof(MixedArray::Elm) * (mixed->capacity() - mixed->m_used);
    auto elms = mixed->data();
    auto last = elms + mixed->m_used;
    for (auto ptr = elms; ptr != last; ++ptr) {
      if (MixedArray::isTombstone(ptr->data.m_type)) {
        size += sizeof(MixedArray::Elm);
        continue;
      }
      size += ptr->hasStrKey() ? getMemSize(ptr->skey) : sizeof(int64_t);
      size += getMemSize(&ptr->data, recurse);
    }
    return size;
  }
  case ArrayData::ArrayKind::kKeysetKind: {
    auto const set = SetArray::asSet(arr);
    auto size = sizeof(SetArray) +
                sizeof(SetArray::Elm) + (set->capacity() - set->m_used);
    auto const elms = set->data();
    auto const used = set->m_used;
    for (uint32_t i = 0; i < used; ++i) {
      auto const& elm = elms[i];
      if (elm.isTombstone()) {
        size += sizeof(SetArray::Elm);
      } else {
        size += elm.hasStrKey() ? getMemSize(elm.strKey()) : sizeof(int64_t);
      }
    }
    return size;
  }
  case ArrayData::ArrayKind::kEmptyKind:
    return sizeof(ArrayData);
  default:
    assertx(!"Unsupported Array type in getMemSize");
  }
  return 0;
}

///////////////////////////////////////////////////////////////////////////////

std::unique_ptr<APCStats> APCStats::s_apcStats = nullptr;

void APCStats::Create() {
  FTRACE(3, "APCStats::Create() called\n");

  s_apcStats = std::make_unique<APCStats>();
}

APCStats::APCStats() : m_valueSize(nullptr)
                     , m_keySize(nullptr)
                     , m_inFileSize(nullptr)
                     , m_livePrimedSize(nullptr)
                     , m_pendingDeleteSize(nullptr)
                     , m_entries(nullptr)
                     , m_primedEntries(nullptr)
                     , m_livePrimedEntries(nullptr)
                     , m_uncountedEntries(nullptr)
                     , m_uncountedBlocks(nullptr)
                     , m_detailedStats(nullptr) {
  m_valueSize = ServiceData::createCounter("apc.value_size.sum");
  m_keySize = ServiceData::createCounter("apc.key_size.sum");
  m_inFileSize = ServiceData::createCounter("apc.in_file_size.sum");
  m_livePrimedSize = ServiceData::createCounter("apc.primed_live_size.sum");
  m_pendingDeleteSize =
    ServiceData::createCounter("apc.pending_delete_size.sum");
  m_entries = ServiceData::createCounter("apc.entries");
  m_primedEntries = ServiceData::createCounter("apc.primed_entries");
  m_livePrimedEntries =
      ServiceData::createCounter("apc.primed_live_entries");
  m_uncountedEntries = ServiceData::createCounter("apc.uncounted_entries");
  m_uncountedBlocks =
    ServiceData::createCounter("apc.uncounted_blocks.mayNotBeAPCValues");
  if (RuntimeOption::EnableAPCStats) {
    m_detailedStats = new APCDetailedStats();
  }
}

APCStats::~APCStats() {
  if (m_detailedStats) delete m_detailedStats;
}

std::string APCStats::getStatsInfo() const {
  std::string info("APC info\nValue size: ");
  info += std::to_string(m_valueSize->getValue()) +
          "\nKey size: " +
          std::to_string(m_keySize->getValue()) +
          "\nMapped to file data size: " +
          std::to_string(m_inFileSize->getValue()) +
          "\nIn memory primed data size: " +
          std::to_string(m_livePrimedSize->getValue()) +
          "\nEntries count: " +
          std::to_string(m_entries->getValue()) +
          "\nPrimed entries count: " +
          std::to_string(m_primedEntries->getValue()) +
          "\nIn memory primed entries count: " +
          std::to_string(m_livePrimedEntries->getValue()) +
          "\nIn total uncounted entries count: " +
          std::to_string(m_uncountedEntries->getValue()) +
          "\nIn memory uncounted blocks: " +
          std::to_string(m_uncountedBlocks->getValue());
  if (apcExtension::UseUncounted) {
    info += "\nPending deletes via treadmill size: " +
            std::to_string(m_pendingDeleteSize->getValue());
  }
  if (m_detailedStats) {
    info += m_detailedStats->getStatsInfo();
  }
  return info + "\n";
}

const StaticString s_num_entries("num_entries");
const StaticString s_primedEntries("primed_entries");
const StaticString s_primedLiveEntries("primed_live_entries");
const StaticString s_valuesSize("values_size");
const StaticString s_keysSize("keys_size");
const StaticString s_primedInFileSize("primed_in_file_size");
const StaticString s_primeLiveSize("primed_live_size");
const StaticString s_pendingDeleteSize("pending_delete_size");
const StaticString s_uncountedEntries("uncounted_entires");
const StaticString s_uncountedBlocks("uncounted_blocks");

void APCStats::collectStats(std::map<const StringData*, int64_t>& stats) const {
  stats.insert(
      std::pair<const StringData*, int64_t>(s_num_entries.get(),
                                            m_entries->getValue()));
  stats.insert(
      std::pair<const StringData*, int64_t>(s_primedEntries.get(),
                                            m_primedEntries->getValue()));
  stats.insert(
      std::pair<const StringData*, int64_t>(s_primedLiveEntries.get(),
                                            m_livePrimedEntries->getValue()));
  stats.insert(
      std::pair<const StringData*, int64_t>( s_uncountedEntries.get(),
                                            m_uncountedEntries->getValue()));
  stats.insert(
    std::pair<const StringData*, int64_t> ( s_uncountedBlocks.get(),
                                      m_uncountedBlocks->getValue()));
  stats.insert(
      std::pair<const StringData*, int64_t>(s_valuesSize.get(),
                                            m_valueSize->getValue()));
  stats.insert(
      std::pair<const StringData*, int64_t>(s_keysSize.get(),
                                            m_keySize->getValue()));
  stats.insert(
      std::pair<const StringData*, int64_t>(s_primedInFileSize.get(),
                                            m_inFileSize->getValue()));
  stats.insert(
      std::pair<const StringData*, int64_t>(s_primeLiveSize.get(),
                                            m_livePrimedSize->getValue()));
  stats.insert(
      std::pair<const StringData*, int64_t>(s_pendingDeleteSize.get(),
                                            m_pendingDeleteSize->getValue()));
  if (m_detailedStats) {
    m_detailedStats->collectStats(stats);
  }
}

APCDetailedStats::APCDetailedStats() : m_uncounted(nullptr)
                                     , m_apcString(nullptr)
                                     , m_uncString(nullptr)
                                     , m_serArray(nullptr)
                                     , m_serVec(nullptr)
                                     , m_serDict(nullptr)
                                     , m_serShape(nullptr)
                                     , m_serKeyset(nullptr)
                                     , m_apcArray(nullptr)
                                     , m_apcVec(nullptr)
                                     , m_apcDict(nullptr)
                                     , m_apcKeyset(nullptr)
                                     , m_uncArray(nullptr)
                                     , m_uncVec(nullptr)
                                     , m_uncDict(nullptr)
                                     , m_uncKeyset(nullptr)
                                     , m_serObject(nullptr)
                                     , m_apcObject(nullptr)
                                     , m_setValues(nullptr)
                                     , m_delValues(nullptr)
                                     , m_replValues(nullptr)
                                     , m_expValues(nullptr) {
  m_uncounted = ServiceData::createCounter("apc.type_uncounted");
  m_apcString = ServiceData::createCounter("apc.type_apc_string");
  m_uncString = ServiceData::createCounter("apc.type_unc_string");
  m_serArray = ServiceData::createCounter("apc.type_ser_array");
  m_serVec = ServiceData::createCounter("apc.type_ser_vec");
  m_serDict = ServiceData::createCounter("apc.type_ser_dict");
  m_serShape = ServiceData::createCounter("apc.type_ser_shape");
  m_serKeyset = ServiceData::createCounter("apc.type_ser_keyset");
  m_apcArray = ServiceData::createCounter("apc.type_apc_array");
  m_apcVec = ServiceData::createCounter("apc.type_apc_vec");
  m_apcDict = ServiceData::createCounter("apc.type_apc_dict");
  m_apcShape = ServiceData::createCounter("apc.type_apc_shape");
  m_apcKeyset = ServiceData::createCounter("apc.type_apc_keyset");
  m_uncArray = ServiceData::createCounter("apc.type_unc_array");
  m_uncVec = ServiceData::createCounter("apc.type_unc_vec");
  m_uncDict = ServiceData::createCounter("apc.type_unc_dict");
  m_uncShape = ServiceData::createCounter("apc.type_unc_shape");
  m_uncKeyset = ServiceData::createCounter("apc.type_unc_keyset");
  m_serObject = ServiceData::createCounter("apc.type_ser_object");
  m_apcObject = ServiceData::createCounter("apc.type_apc_object");

  m_setValues = ServiceData::createCounter("apc.set_values");
  m_delValues = ServiceData::createCounter("apc.deleted_values");
  m_replValues = ServiceData::createCounter("apc.replaced_values");
  m_expValues = ServiceData::createCounter("apc.expired_values");
}

const StaticString s_typeUncounted("type_uncounted");
const StaticString s_typeAPCString("type_apc_string");
const StaticString s_typeUncountedString("type_unc_string");
const StaticString s_typeSerArray("type_ser_array");
const StaticString s_typeSerVec("type_ser_vec");
const StaticString s_typeSerDict("type_ser_dict");
const StaticString s_typeSerShape("type_ser_shape");
const StaticString s_typeSerKeyset("type_ser_keyset");
const StaticString s_typeAPCArray("type_apc_array");
const StaticString s_typeAPCVec("type_apc_vec");
const StaticString s_typeAPCDict("type_apc_dict");
const StaticString s_typeAPCShape("type_apc_shape");
const StaticString s_typeAPCKeyset("type_apc_keyset");
const StaticString s_typUncountedArray("type_unc_array");
const StaticString s_typUncountedVec("type_unc_vec");
const StaticString s_typUncountedDict("type_unc_dict");
const StaticString s_typUncountedShape("type_unc_shape");
const StaticString s_typUncountedKeyset("type_unc_keyset");
const StaticString s_typeSerObject("type_ser_object");
const StaticString s_typeAPCObject("type_apc_object");
const StaticString s_setValueCount("set_values_count");
const StaticString s_deleteValuesCount("deleted_values_count");
const StaticString s_replacedValueCount("replaced_values_count");
const StaticString s_expiredValueCount("expired_values_count");

std::string APCDetailedStats::getStatsInfo() const {
  return "\nPrimitve and static strings count: " +
         std::to_string(m_uncounted->getValue()) +
         "\nAPC strings count: " +
         std::to_string(m_apcString->getValue()) +
         "\nUncounted strings count: " +
         std::to_string(m_uncString->getValue()) +
         "\nSerialized array count: " +
         std::to_string(m_serArray->getValue()) +
         "\nSerialized vec count: " +
         std::to_string(m_serVec->getValue()) +
         "\nSerialized dict count: " +
         std::to_string(m_serDict->getValue()) +
         "\nSerialized shape count: " +
         std::to_string(m_serShape->getValue()) +
         "\nSerialized keyset count: " +
         std::to_string(m_serKeyset->getValue()) +
         "\nAPC array count: " +
         std::to_string(m_apcArray->getValue()) +
         "\nAPC vec count: " +
         std::to_string(m_apcVec->getValue()) +
         "\nAPC dict count: " +
         std::to_string(m_apcDict->getValue()) +
         "\nAPC shape count: " +
         std::to_string(m_apcShape->getValue()) +
         "\nAPC keyset count: " +
         std::to_string(m_apcKeyset->getValue()) +
         "\nUncounted array count: " +
         std::to_string(m_uncArray->getValue()) +
         "\nUncounted vec count: " +
         std::to_string(m_uncVec->getValue()) +
         "\nUncounted dict count: " +
         std::to_string(m_uncDict->getValue()) +
         "\nUncounted shape count: " +
         std::to_string(m_uncShape->getValue()) +
         "\nUncounted keyset count: " +
         std::to_string(m_uncKeyset->getValue()) +
         "\nSerialized object count: " +
         std::to_string(m_serObject->getValue()) +
         "\nAPC object count: " +
         std::to_string(m_apcObject->getValue()) +
         "\add count: " +
         std::to_string(m_setValues->getValue()) +
         "\ndelete count: " +
         std::to_string(m_delValues->getValue()) +
         "\nreplaced count: " +
         std::to_string(m_replValues->getValue()) +
         "\nexpired count: " +
         std::to_string(m_expValues->getValue()) +
         "\n";
}

void APCDetailedStats::collectStats(
    std::map<const StringData*, int64_t>& stats) const {
  stats.insert(
        std::pair<const StringData*, int64_t>(s_typeUncounted.get(),
                                              m_uncounted->getValue()));
  stats.insert(
        std::pair<const StringData*, int64_t>(s_typeAPCString.get(),
                                              m_apcString->getValue()));
  stats.insert(
        std::pair<const StringData*, int64_t>(s_typeUncountedString.get(),
                                              m_uncString->getValue()));
  stats.insert(
        std::pair<const StringData*, int64_t>(s_typeSerArray.get(),
                                              m_serArray->getValue()));
  stats.insert(
        std::pair<const StringData*, int64_t>(s_typeSerVec.get(),
                                              m_serVec->getValue()));
  stats.insert(
        std::pair<const StringData*, int64_t>(s_typeSerDict.get(),
                                              m_serDict->getValue()));
  stats.insert(
        std::pair<const StringData*, int64_t>(s_typeSerShape.get(),
                                              m_serShape->getValue()));
  stats.insert(
        std::pair<const StringData*, int64_t>(s_typeSerKeyset.get(),
                                              m_serKeyset->getValue()));
  stats.insert(
        std::pair<const StringData*, int64_t>(s_typeAPCArray.get(),
                                              m_apcArray->getValue()));
  stats.insert(
        std::pair<const StringData*, int64_t>(s_typeAPCVec.get(),
                                              m_apcVec->getValue()));
  stats.insert(
        std::pair<const StringData*, int64_t>(s_typeAPCDict.get(),
                                              m_apcDict->getValue()));
  stats.insert(
        std::pair<const StringData*, int64_t>(s_typeAPCShape.get(),
                                              m_apcShape->getValue()));
  stats.insert(
        std::pair<const StringData*, int64_t>(s_typeAPCKeyset.get(),
                                              m_apcKeyset->getValue()));
  stats.insert(
        std::pair<const StringData*, int64_t>(s_typUncountedArray.get(),
                                              m_uncArray->getValue()));
  stats.insert(
        std::pair<const StringData*, int64_t>(s_typUncountedVec.get(),
                                              m_uncVec->getValue()));
  stats.insert(
        std::pair<const StringData*, int64_t>(s_typUncountedDict.get(),
                                              m_uncDict->getValue()));
  stats.insert(
        std::pair<const StringData*, int64_t>(s_typUncountedShape.get(),
                                              m_uncShape->getValue()));
  stats.insert(
        std::pair<const StringData*, int64_t>(s_typUncountedKeyset.get(),
                                              m_uncKeyset->getValue()));
  stats.insert(
        std::pair<const StringData*, int64_t>(s_typeSerObject.get(),
                                              m_serObject->getValue()));
  stats.insert(
        std::pair<const StringData*, int64_t>(s_typeAPCObject.get(),
                                              m_apcObject->getValue()));
  stats.insert(
        std::pair<const StringData*, int64_t>(s_setValueCount.get(),
                                              m_setValues->getValue()));
  stats.insert(
        std::pair<const StringData*, int64_t>(s_deleteValuesCount.get(),
                                              m_delValues->getValue()));
  stats.insert(
        std::pair<const StringData*, int64_t>(s_replacedValueCount.get(),
                                              m_replValues->getValue()));
  stats.insert(
        std::pair<const StringData*, int64_t>(s_expiredValueCount.get(),
                                              m_expValues->getValue()));
}

void APCDetailedStats::addAPCValue(APCHandle* handle) {
  m_setValues->increment();
  addType(handle);
}

void APCDetailedStats::updateAPCValue(APCHandle* handle,
                                      APCHandle* oldHandle,
                                      bool expired) {
  removeType(oldHandle);
  addType(handle);
  if (expired) {
    m_expValues->increment();
  } else {
    m_replValues->increment();
  }
}

void APCDetailedStats::removeAPCValue(APCHandle* handle, bool expired) {
  removeType(handle);
  if (expired) {
    m_expValues->increment();
  } else {
    m_delValues->increment();
  }
}

ServiceData::ExportedCounter*
APCDetailedStats::counterFor(const APCHandle* handle) {
  switch (handle->kind()) {
    case APCKind::Uninit:
    case APCKind::Null:
    case APCKind::Bool:
    case APCKind::Int:
    case APCKind::Double:
    case APCKind::PersistentFunc:
    case APCKind::FuncEntity:
    case APCKind::StaticString:
    case APCKind::StaticArray:
    case APCKind::StaticVec:
    case APCKind::StaticDict:
    case APCKind::StaticShape:
    case APCKind::StaticKeyset:
      return m_uncounted;

    case APCKind::UncountedString:
      return m_uncString;

    case APCKind::SharedString:
      return m_apcString;

    case APCKind::UncountedArray:
      return m_uncArray;

    case APCKind::UncountedVec:
      return m_uncVec;

    case APCKind::UncountedDict:
      return m_uncDict;

    case APCKind::UncountedShape:
      return m_uncShape;

    case APCKind::UncountedKeyset:
      return m_uncKeyset;

    case APCKind::SerializedArray:
      return m_serArray;

    case APCKind::SerializedVec:
      return m_serVec;

    case APCKind::SerializedDict:
      return m_serDict;

    case APCKind::SerializedShape:
      return m_serShape;

    case APCKind::SerializedKeyset:
      return m_serKeyset;

    case APCKind::SharedArray:
    case APCKind::SharedPackedArray:
    case APCKind::SharedVArray:
    case APCKind::SharedDArray:
      return m_apcArray;

    case APCKind::SharedVec:
      return m_apcVec;

    case APCKind::SharedDict:
      return m_apcDict;

    case APCKind::SharedShape:
      return m_apcShape;

    case APCKind::SharedKeyset:
      return m_apcKeyset;

    case APCKind::SerializedObject:
      return m_serObject;

    case APCKind::SharedObject:
    case APCKind::SharedCollection:
      return m_apcObject;
  }
  not_reached();
}

void APCDetailedStats::addType(const APCHandle* handle) {
  counterFor(handle)->increment();
}

void APCDetailedStats::removeType(const APCHandle* handle) {
  counterFor(handle)->decrement();
}

///////////////////////////////////////////////////////////////////////////////

}
