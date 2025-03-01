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

#pragma once

#ifdef HAVE_CONFIG_H
#   include <config.h>
#endif

#include "ModPlugin.h"

#include <sys/types.h>

class FrequencyInterleaver : public ModCodec
{
public:
    FrequencyInterleaver(size_t mode, bool fixedPoint);
    virtual ~FrequencyInterleaver();
    FrequencyInterleaver(const FrequencyInterleaver&) = delete;
    FrequencyInterleaver& operator=(const FrequencyInterleaver&) = delete;

    int process(Buffer* const dataIn, Buffer* dataOut) override;
    const char* name() override { return "FrequencyInterleaver"; }

protected:
    bool m_fixedPoint;
    size_t m_carriers;
    size_t *m_indices;
};

