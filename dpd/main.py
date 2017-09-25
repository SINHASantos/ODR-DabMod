#!/usr/bin/env python
# -*- coding: utf-8 -*-
#
# DPD Calculation Engine main file.
#
# http://www.opendigitalradio.org
# Licence: The MIT License, see notice at the end of this file

"""This Python script is the main file for ODR-DabMod's DPD Computation Engine.
This engine calculates and updates the parameter of the digital
predistortion module of ODR-DabMod."""

import datetime
import os
import time
import sys
import matplotlib
matplotlib.use('GTKAgg')

import logging

dt = datetime.datetime.now().isoformat()
logging_path = "/tmp/dpd_{}".format(dt).replace(".", "_").replace(":", "-")
os.makedirs(logging_path)
logging.basicConfig(format='%(asctime)s - %(module)s - %(levelname)s - %(message)s',
                    datefmt='%Y-%m-%d %H:%M:%S',
                    filename='{}/dpd.log'.format(logging_path),
                    filemode='w',
                    level=logging.DEBUG)

# also log up to INFO to console
console = logging.StreamHandler()
console.setLevel(logging.INFO)
# set a format which is simpler for console use
formatter = logging.Formatter('%(asctime)s - %(module)s - %(levelname)s - %(message)s')
# tell the handler to use this format
console.setFormatter(formatter)
# add the handler to the root logger
logging.getLogger('').addHandler(console)

import numpy as np
import traceback
import src.Measure as Measure
import src.Model as Model
import src.ExtractStatistic as ExtractStatistic
import src.Adapt as Adapt
import src.Agc as Agc
import src.TX_Agc as TX_Agc
import src.Symbol_align
import src.const
import src.MER
import src.Measure_Shoulders
import argparse

parser = argparse.ArgumentParser(
    description="DPD Computation Engine for ODR-DabMod")
parser.add_argument('--port', default=50055, type=int,
                    help='port of DPD server to connect to (default: 50055)',
                    required=False)
parser.add_argument('--rc-port', default=9400, type=int,
                    help='port of ODR-DabMod ZMQ Remote Control to connect to (default: 9400)',
                    required=False)
parser.add_argument('--samplerate', default=8192000, type=int,
                    help='Sample rate',
                    required=False)
parser.add_argument('--coefs', default='poly.coef',
                    help='File with DPD coefficients, which will be read by ODR-DabMod',
                    required=False)
parser.add_argument('--txgain', default=-1,
                    help='TX Gain, -1 to leave unchanged',
                    required=False,
                    type=int)
parser.add_argument('--rxgain', default=30,
                    help='TX Gain, -1 to leave unchanged',
                    required=False,
                    type=int)
parser.add_argument('--digital_gain', default=1,
                    help='Digital Gain',
                    required=False,
                    type=float)
parser.add_argument('--target_median', default=0.1,
                    help='target_median',
                    required=False,
                    type=float)
parser.add_argument('--samps', default='81920', type=int,
                    help='Number of samples to request from ODR-DabMod',
                    required=False)
parser.add_argument('-i', '--iterations', default=1, type=int,
                    help='Number of iterations to run',
                    required=False)
parser.add_argument('-L', '--lut',
                    help='Use lookup table instead of polynomial predistorter',
                    action="store_true")

cli_args = parser.parse_args()
logging.info(cli_args)

port = cli_args.port
port_rc = cli_args.rc_port
coef_path = cli_args.coefs
digital_gain = cli_args.digital_gain
num_req = cli_args.samps
samplerate = cli_args.samplerate
num_iter = cli_args.iterations
target_median = cli_args.target_median
rxgain = cli_args.rxgain
txgain = cli_args.txgain

c = src.const.const(samplerate, target_median)
SA = src.Symbol_align.Symbol_align(c)
MER = src.MER.MER(c)
MS = src.Measure_Shoulders.Measure_Shoulder(c, plot=True)

meas = Measure.Measure(samplerate, port, num_req)
extStat = ExtractStatistic.ExtractStatistic(c, plot=True)
adapt = Adapt.Adapt(port_rc, coef_path)
dpddata = adapt.get_predistorter()

if cli_args.lut:
    model = Model.Lut(c, plot=True)
else:
    model = Model.Poly(c, plot=True)
adapt.set_predistorter(model.get_dpd_data())
adapt.set_digital_gain(digital_gain)

# Set RX Gain
if rxgain == -1:
    rxgain = adapt.get_rxgain()
else:
    adapt.set_rxgain(rxgain)

# Set TX Gain
if txgain == -1:
    txgain = adapt.get_txgain()
else:
    adapt.set_txgain(txgain)

tx_gain = adapt.get_txgain()
rx_gain = adapt.get_rxgain()
digital_gain = adapt.get_digital_gain()

dpddata = adapt.get_predistorter()
if dpddata[0] == "poly":
    coefs_am = dpddata[1]
    coefs_pm = dpddata[2]
    logging.info(
        "TX gain {}, RX gain {}, dpd_coefs_am {},"
        " dpd_coefs_pm {}, digital_gain {}".format(
            tx_gain, rx_gain, coefs_am, coefs_pm, digital_gain
        )
    )
elif dpddata[0] == "lut":
    scalefactor = dpddata[1]
    lut = dpddata[2]
    logging.info(
        "TX gain {}, RX gain {}, LUT scalefactor {},"
        " LUT {}, digital_gain {}".format(
            tx_gain, rx_gain, scalefactor, lut, digital_gain
        )
    )
else:
    logging.error("Unknown dpd data format {}".format(dpddata[0]))

tx_agc = TX_Agc.TX_Agc(adapt, c)

# Automatic Gain Control
agc = Agc.Agc(meas, adapt, c)
agc.run()

state = "measure"
i = 0
while i < num_iter:
    try:
        # Measure
        if state == "measure":
            txframe_aligned, tx_ts, rxframe_aligned, rx_ts, rx_median = meas.get_samples()
            rxframe_aligned.tofile("/tmp/rxframe_aligned.np")
            txframe_aligned.tofile("/tmp/txframe_aligned.np")
            if tx_agc.adapt_if_necessary(txframe_aligned):
                continue

            tx, rx, phase_diff, n_per_bin = extStat.extract(txframe_aligned, rxframe_aligned)

            if extStat.n_meas >= 100:
                state = "model"
            else:
                state = "measure"

        # Model
        elif state == "model":
            dpddata = model.train(tx, rx, phase_diff)
            dpddata = model.get_dpd_data()
            extStat = ExtractStatistic.ExtractStatistic(c, plot=True)
            state = "adapt"

        # Adapt
        elif state == "adapt":
            adapt.set_predistorter(dpddata)
            state = "report"

        # Report
        elif state == "report":
            try:
                i += 1
                path = adapt.dump()

                off = SA.calc_offset(txframe_aligned)
                tx_mer = MER.calc_mer(txframe_aligned[off:off+c.T_U], debug=True, debug_name="TX")
                rx_mer = MER.calc_mer(rxframe_aligned[off:off+c.T_U], debug=True, debug_name="RX")
                mse = np.mean(np.abs((txframe_aligned - rxframe_aligned)**2))
                tx_gain = adapt.get_txgain()
                rx_gain = adapt.get_rxgain()
                digital_gain = adapt.get_digital_gain()
                tx_median = np.median(np.abs(txframe_aligned))
                rx_shoulder_tuple = MS.average_shoulders(rxframe_aligned)
                tx_shoulder_tuple = MS.average_shoulders(txframe_aligned)

                logging.info(list((name, eval(name)) for name in
                                  ['i', 'tx_mer', 'tx_shoulder_tuple', 'rx_mer',
                                   'rx_shoulder_tuple', 'mse', 'tx_gain',
                                   'digital_gain', 'rx_gain', 'rx_median',
                                   'tx_median']))
                if dpddata[0] == "poly":
                    coefs_am = dpddata[1]
                    coefs_pm = dpddata[2]
                    logging.info("It {}: coefs_am {}".
                                 format(i, coefs_am))
                    logging.info("It {}: coefs_pm {}".
                                 format(i, coefs_pm))
                if dpddata[0] == "lut":
                    scalefactor = dpddata[1]
                    lut = dpddata[2]
                    logging.info("It {}: LUT scalefactor {}, LUT {}".
                                 format(i, scalefactor, lut))
                if tx_gain < 89:
                    adapt.set_txgain(tx_gain)
                else:
                    break
                state = "measure"
            except:
                logging.warning("Iteration {}: Report failed.".format(i))
                logging.warning(traceback.format_exc())
                state = "measure"

    except Exception as e:
        logging.warning("Iteration {} failed.".format(i))
        logging.warning(traceback.format_exc())

# The MIT License (MIT)
#
# Copyright (c) 2017 Andreas Steger
# Copyright (c) 2017 Matthias P. Braendli
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
