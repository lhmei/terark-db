// NarkDb_size_storer.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include <nark/db/db_table.hpp>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"
#include <nark/io/DataIO.hpp>
#include <nark/io/FileStream.hpp>
#include <nark/io/StreamBuffer.hpp>

namespace mongo { namespace narkdb {

using namespace nark;

using std::string;

struct NarkDbSizeStorer::Entry {
    Entry() : numRecords(0), dataSize(0), dirty(false), rs(NULL) {}
    llong numRecords;
    llong dataSize;
    RecordStore* rs;  // not owned
    bool dirty;
    static const size_t MySize = 2 * sizeof(llong);
    DATA_IO_LOAD_SAVE(Entry, &numRecords&dataSize)
};

NarkDbSizeStorer::NarkDbSizeStorer() {
}

NarkDbSizeStorer::~NarkDbSizeStorer() {
}

void NarkDbSizeStorer::onCreate(RecordStore* rs, llong numRecords, llong dataSize) {
    std::lock_guard<std::mutex> lk(m_mutex);
    Entry& entry = m_entries[rs->ns()];
    entry.numRecords = numRecords;
    entry.dataSize = dataSize;
    entry.rs = rs;
    entry.dirty = true;
}

void NarkDbSizeStorer::onDestroy(RecordStore* rs) {
    stdx::lock_guard<stdx::mutex> lk(m_mutex);
    Entry& entry = m_entries[rs->ns()];
    entry.numRecords = rs->numRecords(NULL);
    entry.dataSize = rs->dataSize(NULL);
    entry.dirty = true;
    entry.rs = NULL;
}

void NarkDbSizeStorer::storeToCache(nark::fstring uri, llong numRecords, llong dataSize) {
    stdx::lock_guard<stdx::mutex> lk(m_entries);
    Entry& entry = m_entries[uri];
    entry.numRecords = numRecords;
    entry.dataSize = dataSize;
    entry.dirty = true;
}

void NarkDbSizeStorer::loadFromCache(nark::fstring uri, llong* numRecords, llong* dataSize) const {
    stdx::lock_guard<stdx::mutex> lk(m_mutex);
    size_t it = m_entries.find_i(uri);
    if (it == m_entries.end_i()) {
        *numRecords = 0;
        *dataSize = 0;
        return;
    }
    *numRecords = m_entries.val(it).numRecords;
    *dataSize = m_entries.val(it).dataSize;
}

void NarkDbSizeStorer::fillCache() {
    hash_strmap<Entry> m;
    {
    	FileStream fp(m_filepath.c_str(), "rb");
		NativeDataInput<InputBuffer> dio;
		dio.attach(&fp);
		dio >> m_entries;
    }
    stdx::lock_guard<stdx::mutex> lk(m_mutex);
    m_entries.swap(m);
}

void NarkDbSizeStorer::syncCache(bool syncToDisk) {
	NativeDataOutput<AutoGrownMemIO> buf;
	{
		stdx::lock_guard<stdx::mutex> lk(m_mutex);
		for (size_t i = m_entries.beg_i(); m_entries.end_i() != i; i = m_entries.next_i(i)) {
			fstring uriKey = m_entries.key(i);
			Entry& entry = m_entries.val(i);
			if (entry.rs) {
				if (entry.dataSize != entry.rs->dataSize(NULL)) {
					entry.dataSize = entry.rs->dataSize(NULL);
					entry.dirty = true;
				}
				if (entry.numRecords != entry.rs->numRecords(NULL)) {
					entry.numRecords = entry.rs->numRecords(NULL);
					entry.dirty = true;
				}
			}
		}
		if (!syncToDisk)
			return;
		buf.resize(m_entries.total_key_size() + m_entries.size() * Entry::MySize);
		buf << m_entries;
	}
	FileStream fp(m_filepath.c_str(), "wb");
	fp.ensureWrite(buf.begin(), buf.tell());
}

} }  // namespace mongo::nark



