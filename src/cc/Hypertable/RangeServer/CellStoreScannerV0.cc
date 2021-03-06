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

#include <cassert>

#include "Common/Error.h"
#include "Common/System.h"

#include "Hypertable/Lib/BlockCompressionHeader.h"
#include "Global.h"
#include "CellStoreScannerV0.h"

using namespace Hypertable;

namespace {
  const uint32_t MINIMUM_READAHEAD_AMOUNT = 65536;
}

//#define STAT 1


CellStoreScannerV0::CellStoreScannerV0(CellStorePtr &cellStorePtr,
                                       ScanContextPtr &scanContextPtr) :
    CellListScanner(scanContextPtr), m_cell_store_ptr(cellStorePtr),
    m_cell_store_v0(dynamic_cast< CellStoreV0*>(m_cell_store_ptr.get())),
    m_index(m_cell_store_v0->m_index),
    m_check_for_range_end(false), m_end_inclusive(true),
    m_readahead(true), m_fd(-1), m_start_offset(0), m_end_offset(0),
    m_returned(0) {
  ByteString  key;
  DynamicBuffer dbuf(0);
  bool start_inclusive = false;

  assert(m_cell_store_v0);
  m_file_id = m_cell_store_v0->m_file_id;
  m_zcodec = m_cell_store_v0->create_block_compression_codec();
  memset(&m_block, 0, sizeof(m_block));

  // compute start row
  // this is wrong ...
  m_start_row = m_cell_store_v0->get_start_row();
  if (m_start_row < scanContextPtr->start_row) {
    start_inclusive = true;
    m_start_row = scanContextPtr->start_row;
  }

  // compute end row
  m_end_row = m_cell_store_v0->get_end_row();
  if (scanContextPtr->end_row < m_end_row) {
    m_end_inclusive = false;
    m_end_row = scanContextPtr->end_row;
  }

  append_as_byte_string(dbuf, m_start_row.c_str());
  key.ptr = dbuf.base;

  if (start_inclusive)
    m_iter = m_index.lower_bound(key);
  else
    m_iter = m_index.upper_bound(key);

  m_cur_key.ptr = 0;

  if (m_iter == m_index.end())
    return;

  /**
   * If we're just scanning a single row, turn off readahead
   */
  if (m_start_row == m_end_row || (scanContextPtr->spec && scanContextPtr->spec->row_limit == 1)) {
    m_readahead = false;
    memset(&m_block, 0, sizeof(m_block));
    if (!fetch_next_block()) {
      m_iter = m_index.end();
      return;
    }
  }
  else {
    uint32_t buf_size = m_cell_store_ptr->get_blocksize();

    if (buf_size < MINIMUM_READAHEAD_AMOUNT)
      buf_size = MINIMUM_READAHEAD_AMOUNT;

    m_start_offset = (*m_iter).second;

    dbuf.clear();
    append_as_byte_string(dbuf, m_end_row.c_str());
    key.ptr = dbuf.base;

    if ((m_end_iter = m_index.upper_bound(key)) == m_index.end())
      m_end_offset = m_cell_store_v0->m_trailer.fix_index_offset;
    else {
      CellStoreV0::IndexMapT::iterator iter_next = m_end_iter;
      iter_next++;
      if (iter_next == m_index.end())
	m_end_offset = m_cell_store_v0->m_trailer.fix_index_offset;
      else
	m_end_offset = (*iter_next).second;
    }

    try {
      m_fd = m_cell_store_v0->m_filesys->open_buffered(
              m_cell_store_ptr->get_filename(), buf_size, 2,
              m_start_offset, m_end_offset);
    }
    catch (Exception &e) {
      m_iter = m_index.end();
      throw Exception(e.code(), format("Problem opening cell store in "
                      "readahead mode: %s", e.what()));
    }

    if (!fetch_next_block_readahead()) {
      m_iter = m_index.end();
      return;
    }
  }

  /**
   * Seek to start of range in block
   */
  m_cur_key.ptr = m_block.ptr;
  m_cur_value.ptr = m_block.ptr + m_cur_key.length();

  if (start_inclusive) {
    while (strcmp(m_cur_key.str(), m_start_row.c_str()) < 0) {
      m_block.ptr = m_cur_value.ptr + m_cur_value.length();
      if (m_block.ptr >= m_block.end) {
	m_iter = m_index.end();
	HT_ERRORF("Unable to find start of range (row='%s') in %s", m_start_row.c_str(), m_cell_store_ptr->get_filename().c_str());
	return;
      }
      m_cur_key.ptr = m_block.ptr;
      m_cur_value.ptr = m_block.ptr + m_cur_key.length();
    }
  }
  else {
    while (strcmp(m_cur_key.str(), m_start_row.c_str()) <= 0) {
      m_block.ptr = m_cur_value.ptr + m_cur_value.length();
      if (m_block.ptr >= m_block.end) {
	if (m_readahead) {
	  if (!fetch_next_block_readahead())
	    return;
	}
	else if (!fetch_next_block())
	  return;
      }
      m_cur_key.ptr = m_block.ptr;
      m_cur_value.ptr = m_block.ptr + m_cur_key.length();
    }
  }


  /**
   * End of range check
   */
  if (m_end_inclusive) {
    if (strcmp(m_cur_key.str(), m_end_row.c_str()) > 0) {
      m_iter = m_index.end();
      return;
    }
  }
  else {
    if (strcmp(m_cur_key.str(), m_end_row.c_str()) >= 0) {
      m_iter = m_index.end();
      return;
    }
  }


  /**
   * Column family check
   */
  Key keyComps;
  if (!keyComps.load(m_cur_key)) {
    HT_ERROR("Problem parsing key!");
  }
  else if (keyComps.flag != FLAG_DELETE_ROW && !m_scan_context_ptr->familyMask[keyComps.column_family_code])
    forward();

}


CellStoreScannerV0::~CellStoreScannerV0() {
  try {
    if (m_fd != -1) {
      try { m_cell_store_v0->m_filesys->close(m_fd, 0); }
      catch (Exception &e) {
        throw Exception(e.code(), format("Problem closing cellstore: %s",
                        m_cell_store_ptr->get_filename().c_str()));
      }
    }

    if (m_readahead)
      delete [] m_block.base;
    else {
      if (m_block.base != 0)
        Global::blockCache->checkin(m_file_id, m_block.offset);
    }
    delete m_zcodec;

#ifdef STAT
    cout << flush;
    cout << "STAT[~CellStoreScannerV0]\tget\t" << m_returned << "\t";
    cout << m_cell_store_v0->get_filename() << "[" << m_start_row << ".." << m_end_row << "]" << endl;
#endif
  }
  catch (Exception &e) {
    HT_ERRORF("Exception caught in %s: %s", __func__, e.what());
  }
  catch (...) {
    HT_ERRORF("Unknown exception caught in %s", __func__);
  }
}


bool CellStoreScannerV0::get(ByteString &key, ByteString &value) {

  if (m_iter == m_index.end())
    return false;

#ifdef STAT
  m_returned++;
#endif

  key = m_cur_key;
  value = m_cur_value;

  return true;
}



void CellStoreScannerV0::forward() {
  Key keyComps;

  while (true) {

    if (m_iter == m_index.end())
      return;

    m_block.ptr = m_cur_value.ptr + m_cur_value.length();

    if (m_block.ptr >= m_block.end) {
      if (m_readahead) {
	if (!fetch_next_block_readahead())
	  return;
      }
      else if (!fetch_next_block())
	return;
    }

    m_cur_key.ptr = m_block.ptr;
    m_cur_value.ptr = m_block.ptr + m_cur_key.length();

    if (m_check_for_range_end) {
      if (m_end_inclusive) {
	if (strcmp(m_cur_key.str(), m_end_row.c_str()) > 0) {
	  m_iter = m_index.end();
	  return;
	}
      }
      else {
	if (strcmp(m_cur_key.str(), m_end_row.c_str()) >= 0) {
	  m_iter = m_index.end();
	  return;
	}
      }
    }

    /**
     * Column family check
     */
    if (!keyComps.load(m_cur_key)) {
      HT_ERROR("Problem parsing key!");
      break;
    }
    if (keyComps.flag == FLAG_DELETE_ROW || m_scan_context_ptr->familyMask[keyComps.column_family_code])
      break;
  }
}



/**
 * This method fetches the 'next' compressed block of key/value pairs from
 * the underlying CellStore.
 *
 * Preconditions required to call this method:
 *  1. m_block is cleared and m_iter points to the m_index entry of the first block to fetch
 *    'or'
 *  2. m_block is loaded with the current block and m_iter points to the m_index entry of
 *     the current block
 *
 * @return true if next block successfully fetched, false if no next block
 */
bool CellStoreScannerV0::fetch_next_block() {
  uint8_t *buf = 0;
  bool rval = false;
  int error;

  // If we're at the end of the current block, deallocate and move to next
  if (m_block.base != 0 && m_block.ptr >= m_block.end) {
    Global::blockCache->checkin(m_file_id, m_block.offset);
    memset(&m_block, 0, sizeof(m_block));
    m_iter++;
  }

  if (m_block.base == 0 && m_iter != m_index.end()) {
    DynamicBuffer expandBuffer(0);
    uint32_t len;

    m_block.offset = (*m_iter).second;

    CellStoreV0::IndexMapT::iterator iterNext = m_iter;
    iterNext++;
    if (iterNext == m_index.end()) {
      m_block.zlength = m_cell_store_v0->m_trailer.fix_index_offset - m_block.offset;
      if (m_end_row.c_str()[0] != (char)0xff)
	m_check_for_range_end = true;
    }
    else {
      if (strcmp((*iterNext).first.str(), m_end_row.c_str()) >= 0)
	m_check_for_range_end = true;
      m_block.zlength = (*iterNext).second - m_block.offset;
    }

    /**
     * Cache lookup / block read
     */
    if (!Global::blockCache->checkout(m_file_id, (uint32_t)m_block.offset, &m_block.base, &len)) {
      /** Read compressed block **/
      buf = new uint8_t [ m_block.zlength ];
      try {
        m_cell_store_v0->m_filesys->pread(m_cell_store_v0->m_fd, buf,
                                          m_block.zlength, m_block.offset);
      }
      catch (Exception &e) {
        HT_ERRORF("Error reading cellstore (%s) block: %s",
                  m_cell_store_ptr->get_filename().c_str(), e.what());
        goto abort;
      }

      /** inflate compressed block **/
      BlockCompressionHeader header;
      DynamicBuffer input(0);
      input.base = buf;
      input.ptr = buf + m_block.zlength;
      if ((error = m_zcodec->inflate(input, expandBuffer, header))
          != Error::OK) {
        HT_ERRORF("Problem inflating cell store (%s) block - %s",
                  m_cell_store_ptr->get_filename().c_str(),
                  Error::get_text(error));
        input.base = 0;
        goto abort;
      }
      input.base = 0;
      if (!header.check_magic(CellStoreV0::DATA_BLOCK_MAGIC)) {
        HT_ERROR("Problem inflating cell store block - magic string mismatch");
        goto abort;
      }

      /** take ownership of inflate buffer **/
      size_t fill;
      m_block.base = expandBuffer.release(&fill);
      len = fill;

      /** Insert block into cache  **/
      if (!Global::blockCache->insert_and_checkout(m_file_id, (uint32_t)m_block.offset, m_block.base, len)) {
	delete [] m_block.base;
	if (!Global::blockCache->checkout(m_file_id, (uint32_t)m_block.offset, &m_block.base, &len)) {
	  HT_ERRORF("Problem checking out block from cache fileId=%d, offset=%ld", m_file_id, (uint32_t)m_block.offset);
	  HT_ABORT;
	}
      }
    }
    m_block.ptr = m_block.base;
    m_block.end = m_block.base + len;

    rval = true;
  }

 abort:
  delete [] buf;
  return rval;
}



/**
 * This method fetches the 'next' compressed block of key/value pairs from
 * the underlying CellStore.
 *
 * Preconditions required to call this method:
 *  1. m_block is cleared and m_iter points to the m_index entry of the first block to fetch
 *    'or'
 *  2. m_block is loaded with the current block and m_iter points to the m_index entry of
 *     the current block
 *
 * @return true if next block successfully fetched, false if no next block
 */
bool CellStoreScannerV0::fetch_next_block_readahead() {
  int error;
  uint8_t *buf = 0;
  bool rval = false;

  // If we're at the end of the current block, deallocate and move to next
  if (m_block.base != 0 && m_block.ptr >= m_block.end) {
    delete [] m_block.base;
    memset(&m_block, 0, sizeof(m_block));
    m_iter++;
  }

  if (m_block.base == 0 && m_iter != m_index.end()) {
    DynamicBuffer expandBuffer(0);
    uint32_t len;
    uint32_t nread;

    m_block.offset = (*m_iter).second;
    assert(m_block.offset == m_start_offset);

    CellStoreV0::IndexMapT::iterator iterNext = m_iter;
    iterNext++;
    if (iterNext == m_index.end()) {
      m_block.zlength = m_cell_store_v0->m_trailer.fix_index_offset - m_block.offset;
      if (m_end_row.c_str()[0] != (char)0xff)
	m_check_for_range_end = true;
    }
    else {
      if (strcmp((*iterNext).first.str(), m_end_row.c_str()) >= 0)
	m_check_for_range_end = true;
      m_block.zlength = (*iterNext).second - m_block.offset;
    }

    /** Read compressed block **/
    buf = new uint8_t [ m_block.zlength ];

    try {
      nread = m_cell_store_v0->m_filesys->read(m_fd, buf, m_block.zlength);
    }
    catch (Exception &e) {
      HT_ERRORF("Problem reading cell store file '%s': %s",
		m_cell_store_ptr->get_filename().c_str(), e.what());
      goto abort;
    }
    if (nread != m_block.zlength) {
      HT_ERRORF("short read %ld != %ld", nread, m_block.zlength);
      HT_ABORT;
    }
    assert(nread == m_block.zlength);
    m_start_offset += nread;

    /** inflate compressed block **/
    {
      BlockCompressionHeader header;
      DynamicBuffer input(0);
      input.base = buf;
      input.ptr = buf + m_block.zlength;
      if ((error = m_zcodec->inflate(input, expandBuffer, header)) != Error::OK) {
	HT_ERRORF("Problem inflating cell store (%s) block - %s", m_cell_store_ptr->get_filename().c_str(), Error::get_text(error));
	input.base = 0;
	goto abort;
      }
      input.base = 0;
      if (!header.check_magic(CellStoreV0::DATA_BLOCK_MAGIC)) {
	HT_ERROR("Problem inflating cell store block - magic string mismatch");
	goto abort;
      }
    }

    /** take ownership of inflate buffer **/
    size_t fill;
    m_block.base = expandBuffer.release(&fill);
    len = fill;

    m_block.ptr = m_block.base;
    m_block.end = m_block.base + len;

    rval = true;
  }

 abort:
  delete [] buf;
  return rval;
}
