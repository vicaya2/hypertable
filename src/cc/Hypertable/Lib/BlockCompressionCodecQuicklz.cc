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

#include "Common/Checksum.h"
#include "Common/Thread.h"
#include "Common/DynamicBuffer.h"
#include "Common/Error.h"
#include "Common/Logger.h"

#include "BlockCompressionCodecQuicklz.h"

#include "quicklz.c"

using namespace Hypertable;

/**
 *
 */
BlockCompressionCodecQuicklz::BlockCompressionCodecQuicklz(const Args &args) {
  size_t amount = ((SCRATCH_DECOMPRESS) < (SCRATCH_COMPRESS)) ? (SCRATCH_COMPRESS) : (SCRATCH_DECOMPRESS);
  m_workmem = new uint8_t [ amount ];
}


/**
 *
 */
BlockCompressionCodecQuicklz::~BlockCompressionCodecQuicklz() {
  delete [] m_workmem;
}



/**
 * 
 */
int BlockCompressionCodecQuicklz::deflate(const DynamicBuffer &input, DynamicBuffer &output, BlockCompressionHeader &header, size_t reserve) {
  uint32_t avail_out = input.fill() + 400;
  size_t len;

  output.clear();
  output.reserve( header.length() + avail_out + reserve );

  // compress 
  len = qlz_compress((char *)input.base, (char *)output.base+header.length(), input.fill(), (char *)m_workmem);

  /* check for an incompressible block */
  if (len >= input.fill()) {
    header.set_compression_type(NONE);
    memcpy(output.base+header.length(), input.base, input.fill());
    header.set_data_length(input.fill());
    header.set_data_zlength(input.fill());
  }
  else {
    header.set_compression_type(QUICKLZ);
    header.set_data_length(input.fill());
    header.set_data_zlength(len);
  }
  header.set_data_checksum(fletcher32(output.base + header.length(), header.get_data_zlength()));
  
  output.ptr = output.base;
  header.encode(&output.ptr);
  output.ptr += header.get_data_zlength();

  return Error::OK;
}


/**
 * 
 */
int BlockCompressionCodecQuicklz::inflate(const DynamicBuffer &input, DynamicBuffer &output, BlockCompressionHeader &header) {
  int error;
  uint8_t *msg_ptr = input.base;
  size_t remaining = input.fill();

  if ((error = header.decode(&msg_ptr, &remaining)) != Error::OK)
    return error;

  if (header.get_data_zlength() != remaining) {
    HT_ERRORF("Block decompression error, header zlength = %d, actual = %d", header.get_data_zlength(), remaining);
    return Error::BLOCK_COMPRESSOR_BAD_HEADER;
  }

  uint32_t checksum = fletcher32(msg_ptr, remaining);
  if (checksum != header.get_data_checksum()) {
    HT_ERRORF("Compressed block checksum mismatch header=%d, computed=%d", header.get_data_checksum(), checksum);
    return Error::BLOCK_COMPRESSOR_CHECKSUM_MISMATCH;
  }

  output.reserve(header.get_data_length());

   // check compress type
  if (header.get_compression_type() == NONE)
    memcpy(output.base, msg_ptr, header.get_data_length());
  else {
    size_t len;
    // decompress 
    len = qlz_decompress((char *)msg_ptr, (char *)output.base, (char *)m_workmem);
    assert(len == header.get_data_length());
  }
  output.ptr = output.base + header.get_data_length();

  return Error::OK;
}
