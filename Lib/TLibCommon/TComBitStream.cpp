/* The copyright in this software is being made available under the BSD
 * License, included below. This software may be subject to other third party
 * and contributor rights, including patent rights, and no such rights are
 * granted under this license.  
 *
 * Copyright (c) 2010-2011, ITU/ISO/IEC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *  * Neither the name of the ITU/ISO/IEC nor the names of its contributors may
 *    be used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

/** \file     TComBitStream.cpp
    \brief    class for handling bitstream
*/

#include <stdint.h>
#include <vector>
#include "TComBitStream.h"
#include <string.h>
#include <memory.h>

using namespace std;

// ====================================================================================================================
// Constructor / destructor / create / destroy
// ====================================================================================================================

TComOutputBitstream::TComOutputBitstream()
{
  m_fifo = new vector<uint8_t>;
  clear();
}

TComOutputBitstream::~TComOutputBitstream()
{
  delete m_fifo;
}

TComInputBitstream::TComInputBitstream(vector<uint8_t>* buf)
{
  m_fifo = buf;
  m_fifo_idx = 0;
  m_held_bits = 0;
  m_num_held_bits = 0;
}

// ====================================================================================================================
// Public member functions
// ====================================================================================================================

char* TComOutputBitstream::getByteStream() const
{
  return (char*) &m_fifo->front();
}

unsigned int TComOutputBitstream::getByteStreamLength()
{
  return unsigned(m_fifo->size());
}

void TComOutputBitstream::clear()
{
  m_fifo->clear();
  m_held_bits = 0;
  m_num_held_bits = 0;
}

Void TComOutputBitstream::write   ( UInt uiBits, UInt uiNumberOfBits )
{
  assert( uiNumberOfBits <= 32 );

  /* any modulo 8 remainder of num_total_bits cannot be written this time,
   * and will be held until next time. */
  unsigned num_total_bits = uiNumberOfBits + m_num_held_bits;
  unsigned next_num_held_bits = num_total_bits % 8;

  /* form a byte aligned word (write_bits), by concatenating any held bits
   * with the new bits, discarding the bits that will form the next_held_bits.
   * eg: H = held bits, V = n new bits        /---- next_held_bits
   * len(H)=7, len(V)=1: ... ---- HHHH HHHV . 0000 0000, next_num_held_bits=0
   * len(H)=7, len(V)=2: ... ---- HHHH HHHV . V000 0000, next_num_held_bits=1
   * if total_bits < 8, the value of v_ is not used */
  unsigned char next_held_bits = uiBits << (8 - next_num_held_bits);

  if (!(num_total_bits >> 3))
  {
    /* insufficient bits accumulated to write out, append new_held_bits to
     * current held_bits */
    /* NB, this requires that v only contains 0 in bit positions {31..n} */
    m_held_bits |= next_held_bits;
    m_num_held_bits = next_num_held_bits;
    return;
  }

  /* topword serves to justify held_bits to align with the msb of uiBits */
  unsigned topword = (uiNumberOfBits - next_num_held_bits) & ~((1 << 3) -1);
  unsigned int write_bits = (m_held_bits << topword) | (uiBits >> next_num_held_bits);

  switch (num_total_bits >> 3)
  {
  case 4: m_fifo->push_back(write_bits >> 24);
  case 3: m_fifo->push_back(write_bits >> 16);
  case 2: m_fifo->push_back(write_bits >> 8);
  case 1: m_fifo->push_back(write_bits);
  }

  m_held_bits = next_held_bits;
  m_num_held_bits = next_num_held_bits;
}

Void TComOutputBitstream::writeAlignOne()
{
  unsigned int num_bits = getNumBitsUntilByteAligned();
  write((1 << num_bits) - 1, num_bits);
  return;
}

Void TComOutputBitstream::writeAlignZero()
{
  if (0 == m_num_held_bits)
    return;
  m_fifo->push_back(m_held_bits);
  m_held_bits = 0;
  m_num_held_bits = 0;
}

#if LCEC_INTRA_MODE || QC_LCEC_INTER_MODE
/**
 * read #uiNumberOfBits# from bitstream without updating the bitstream
 * state, storing the result in #ruiBits#.
 *
 * If reading #uiNumberOfBits# would overrun the bitstream buffer,
 * the bitsream is effectively padded with sufficient zero-bits to
 * avoid the overrun.
 */
Void TComInputBitstream::pseudoRead ( UInt uiNumberOfBits, UInt& ruiBits )
{
  unsigned int saved_num_held_bits = m_num_held_bits;
  unsigned char saved_held_bits = m_held_bits;
  unsigned int saved_fifo_idx = m_fifo_idx;

  unsigned num_bits_to_read = min(uiNumberOfBits, getNumBitsLeft());
  read(num_bits_to_read, ruiBits);
  ruiBits <<= (uiNumberOfBits - num_bits_to_read);

  m_fifo_idx = saved_fifo_idx;
  m_held_bits = saved_held_bits;
  m_num_held_bits = saved_num_held_bits;
}
#endif


Void TComInputBitstream::read (UInt uiNumberOfBits, UInt& ruiBits)
{
  assert( uiNumberOfBits <= 32 );
  
  /* NB, bits are extracted from the MSB of each byte. */
  unsigned retval = 0;
  if (uiNumberOfBits <= m_num_held_bits)
  {
    /* n=1, len(H)=7:   -VHH HHHH, shift_down=6, mask=0xfe
     * n=3, len(H)=7:   -VVV HHHH, shift_down=4, mask=0xf8
     */
    retval = m_held_bits >> (m_num_held_bits - uiNumberOfBits);
    retval &= ~(0xff << uiNumberOfBits);
    m_num_held_bits -= uiNumberOfBits;
    ruiBits = retval;
    return;
  }

  /* all num_held_bits will go into retval
   *   => need to mask leftover bits from previous extractions
   *   => align retval with top of extracted word */
  /* n=5, len(H)=3: ---- -VVV, mask=0x07, shift_up=5-3=2,
   * n=9, len(H)=3: ---- -VVV, mask=0x07, shift_up=9-3=6 */
  uiNumberOfBits -= m_num_held_bits;
  retval = m_held_bits & ~(0xff << m_num_held_bits);
  retval <<= uiNumberOfBits;

  /* number of whole bytes that need to be loaded to form retval */
  /* n=32, len(H)=0, load 4bytes, shift_down=0
   * n=32, len(H)=1, load 4bytes, shift_down=1
   * n=31, len(H)=1, load 4bytes, shift_down=1+1
   * n=8,  len(H)=0, load 1byte,  shift_down=0
   * n=8,  len(H)=3, load 1byte,  shift_down=3
   * n=5,  len(H)=1, load 1byte,  shift_down=1+3
   */
  unsigned aligned_word = 0;
  unsigned num_bytes_to_load = (uiNumberOfBits - 1) >> 3;
  assert(m_fifo_idx + num_bytes_to_load < m_fifo->size());

  switch (num_bytes_to_load)
  {
  case 3: aligned_word  = (*m_fifo)[m_fifo_idx++] << 24;
  case 2: aligned_word |= (*m_fifo)[m_fifo_idx++] << 16;
  case 1: aligned_word |= (*m_fifo)[m_fifo_idx++] <<  8;
  case 0: aligned_word |= (*m_fifo)[m_fifo_idx++];
  }

  /* resolve remainder bits */
  unsigned next_num_held_bits = (32 - uiNumberOfBits) % 8;

  /* copy required part of aligned_word into retval */
  retval |= aligned_word >> next_num_held_bits;

  /* store held bits */
  m_num_held_bits = next_num_held_bits;
  m_held_bits = aligned_word;

  ruiBits = retval;
}

/**
 * insert the contents of the bytealigned (and flushed) bitstream @src
 * into @this at byte position @pos.
 */
void TComOutputBitstream::insertAt(const TComOutputBitstream& src, unsigned pos)
{
  unsigned src_bits = src.getNumberOfWrittenBits();
  assert(0 == src_bits % 8);

  vector<uint8_t>::iterator at = this->m_fifo->begin() + pos;
  this->m_fifo->insert(at, src.m_fifo->begin(), src.m_fifo->end());
}
