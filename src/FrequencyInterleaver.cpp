/*
   Copyright (C) 2005, 2006, 2007, 2008, 2009, 2010, 2011 Her Majesty
   the Queen in Right of Canada (Communications Research Center Canada)
 */
/*
   This file is part of ODR-DabMod.

   ODR-DabMod is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as
   published by the Free Software Foundation, either version 3 of the
   License, or (at your option) any later version.

   ODR-DabMod is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with ODR-DabMod.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "FrequencyInterleaver.h"
#include "PcDebug.h"

#include <stdexcept>
#include <string>
#include <cstdio>
#include <cstdlib>


FrequencyInterleaver::FrequencyInterleaver(size_t mode, bool fixedPoint) :
    ModCodec(),
    m_fixedPoint(fixedPoint)
{
    PDEBUG("FrequencyInterleaver::FrequencyInterleaver(%zu) @ %p\n",
            mode, this);

    size_t num;
    size_t alpha = 13;
    size_t beta;
    switch (mode) {
    case 1:
        m_carriers = 1536;
        num = 2048;
        beta = 511;
        break;
    case 2:
        m_carriers = 384;
        num = 512;
        beta = 127;
        break;
    case 3:
        m_carriers = 192;
        num = 256;
        beta = 63;
        break;
    case 0:
    case 4:
        m_carriers = 768;
        num = 1024;
        beta = 255;
        break;
    default:
        PDEBUG("Carriers: %zu\n", (m_carriers >> 1) << 1);
        throw std::runtime_error("FrequencyInterleaver: invalid dab mode");
    }

    const int ret = posix_memalign((void**)(&m_indices), 16, m_carriers * sizeof(size_t));
    if (ret != 0) {
        throw std::runtime_error("memory allocation failed: " + std::to_string(ret));
    }

    size_t *index = m_indices;
    size_t perm = 0;
    PDEBUG("i: %4u, R: %4u\n", 0, 0);
    for (size_t j = 1; j < num; ++j) {
        perm = (alpha * perm + beta) & (num - 1);
        if (perm >= ((num - m_carriers) / 2)
                && perm <= (num - (num - m_carriers) / 2)
                && perm != (num / 2)) {
            PDEBUG("i: %4zu, R: %4zu, d: %4zu, n: %4zu, k: %5zi, index: %zu\n",
                    j, perm, perm, index - m_indices, perm - num / 2,
                    perm > num / 2
                    ?  perm - (1 + (num / 2))
                    : perm + (m_carriers - (num / 2)));
            *(index++) = perm > num / 2 ?
                perm - (1 + (num / 2)) : perm + (m_carriers - (num / 2));
        }
        else {
            PDEBUG("i: %4zu, R: %4zu\n", j, perm);
        }
    }
}


FrequencyInterleaver::~FrequencyInterleaver()
{
    PDEBUG("FrequencyInterleaver::~FrequencyInterleaver() @ %p\n", this);

    free(m_indices);
}

template<typename T>
void do_process(Buffer* const dataIn, Buffer* dataOut,
        size_t carriers, const size_t * const indices)
{
    const T* in = reinterpret_cast<const T*>(dataIn->getData());
    T* out = reinterpret_cast<T*>(dataOut->getData());
    size_t sizeIn = dataIn->getLength() / sizeof(T);

    if (sizeIn % carriers != 0) {
        throw std::runtime_error(
                "FrequencyInterleaver::process input size not valid!");
    }

    for (size_t i = 0; i < sizeIn;) {
//      memset(out, 0, m_carriers * sizeof(T));
        for (size_t j = 0; j < carriers; i += 4, j += 4) {
            out[indices[j]] = in[i];
            out[indices[j + 1]] = in[i + 1];
            out[indices[j + 2]] = in[i + 2];
            out[indices[j + 3]] = in[i + 3];
        }
        out += carriers;
    }
}

int FrequencyInterleaver::process(Buffer* const dataIn, Buffer* dataOut)
{
    PDEBUG("FrequencyInterleaver::process"
            "(dataIn: %p, sizeIn: %zu, dataOut: %p, sizeOut: %zu)\n",
            dataIn, dataIn->getLength(), dataOut, dataOut->getLength());

    dataOut->setLength(dataIn->getLength());

    if (m_fixedPoint) {
        do_process<complexfix>(dataIn, dataOut, m_carriers, m_indices);
    }
    else {
        do_process<complexf>(dataIn, dataOut, m_carriers, m_indices);
    }

    return 1;
}
