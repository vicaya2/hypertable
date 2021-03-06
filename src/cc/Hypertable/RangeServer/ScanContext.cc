/** -*- c++ -*-
 * Copyright (C) 2008 Doug Judd (Zvents, Inc.)
 * 
 * This file is part of Hypertable.
 * 
 * Hypertable is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2 of the
 * License.
 * 
 * Hypertable is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#include <algorithm>
#include <cassert>

#include "Common/Logger.h"

#include "Hypertable/Lib/Key.h"

#include "ScanContext.h"

using namespace Hypertable;

/**
 *
 */
void ScanContext::initialize(uint64_t ts, ScanSpec *ss, RangeSpec *range_, SchemaPtr &sp) {
  Schema::ColumnFamily *columnFamily;
  uint32_t max_versions = 0;

  // set time interval
  if (ss) {
    interval.first = ss->interval.first;
    interval.second = (ss->interval.second != 0) ? std::min(ts, ss->interval.second) : ts;
  }
  else {
    interval.first = 0;
    interval.second = ts;
  }

  if (interval.second == 0)
    interval.second = END_OF_TIME;

  spec = ss;
  range = range_;

  if (spec == 0)
    memset(familyMask, true, 256*sizeof(bool));
  else {
    memset(familyMask, false, 256*sizeof(bool));
    max_versions = spec->max_versions;
  }

  memset(familyInfo, 0, 256*sizeof(CellFilterInfoT));

  if (sp) {

    schemaPtr = sp;
    if (spec && spec->columns.size() > 0) {
      for (std::vector<const char *>::const_iterator iter = spec->columns.begin(); iter != spec->columns.end(); iter++) {
	columnFamily = schemaPtr->get_column_family(*iter);

	if (columnFamily == 0)
	  throw Hypertable::Exception(Error::RANGESERVER_INVALID_COLUMNFAMILY, *iter);

	familyMask[columnFamily->id] = true;
	if (columnFamily->ttl == 0)
	  familyInfo[columnFamily->id].cutoffTime = 0;
	else
	  familyInfo[columnFamily->id].cutoffTime = ts - ((uint64_t)columnFamily->ttl * 1000000000LL);
	if (max_versions == 0)
	  familyInfo[columnFamily->id].max_versions = columnFamily->max_versions;
	else {
	  if (columnFamily->max_versions == 0)
	    familyInfo[columnFamily->id].max_versions = max_versions;
	  else
	    familyInfo[columnFamily->id].max_versions = (max_versions < columnFamily->max_versions) ? max_versions : columnFamily->max_versions;
	}
      }
    }
    else {
      list<Schema::AccessGroup *> *agList = schemaPtr->get_access_group_list();

      familyMask[0] = true;  // ROW_DELETE records have 0 column family, so this allows them to pass through
      for (list<Schema::AccessGroup *>::iterator agIter = agList->begin(); agIter != agList->end(); agIter++) {
	for (list<Schema::ColumnFamily *>::iterator cfIter = (*agIter)->columns.begin(); cfIter != (*agIter)->columns.end(); cfIter++) {
	  if ((*cfIter)->id == 0)
	    throw Hypertable::Exception(Error::RANGESERVER_SCHEMA_INVALID_CFID, (std::string)"Bad ID for Column Family '" + (*cfIter)->name + "'");
	  familyMask[(*cfIter)->id] = true;
	  if ((*cfIter)->ttl == 0)
	    familyInfo[(*cfIter)->id].cutoffTime = 0;
	  else
	    familyInfo[(*cfIter)->id].cutoffTime = ts - ((uint64_t)(*cfIter)->ttl * 1000000000LL);

	  if (max_versions == 0)
	    familyInfo[(*cfIter)->id].max_versions = (*cfIter)->max_versions;
	  else {
	    if ((*cfIter)->max_versions == 0)
	      familyInfo[(*cfIter)->id].max_versions = max_versions;
	    else
	      familyInfo[(*cfIter)->id].max_versions = (max_versions < (*cfIter)->max_versions) ? max_versions : (*cfIter)->max_versions;
	  }
	}
      }
    }
  }

  /**
   * Create Start Key and End Key
   */
  if (spec) {

    // start row
    start_row = spec->start_row;
    if (!spec->start_row_inclusive)
      start_row.append(1,1);  // bump to next row

    // end row
    if (spec->end_row[0] == 0)
      end_row = Key::END_ROW_MARKER;
    else {
      end_row = spec->end_row;
      if (spec->end_row_inclusive) {
	uint8_t last_char = spec->end_row[strlen(spec->end_row)-1];
	if (last_char == 0xff)
	  end_row.append(1,1);    // bump to next row
	else
	  end_row[strlen(spec->end_row)-1] = (last_char+1);
      }
    }
  }
  else {
    start_row = "";
    end_row = Key::END_ROW_MARKER;
  }

}
