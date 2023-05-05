/*
   Copyright (C) 2005, 2006, 2007, 2008, 2009, 2010 Her Majesty the
   Queen in Right of Canada (Communications Research Center Canada)

   Copyright (C) 2023
   Matthias P. Braendli, matthias.braendli@mpb.li

    http://opendigitalradio.org

DESCRIPTION:
   It is an output driver using libiio targeting the PrecisionWave DEXTER board.
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

#include "output/Dexter.h"

#ifdef HAVE_DEXTER

#include <chrono>
#include <limits>
#include <cstdio>
#include <iomanip>

#include "Log.h"
#include "Utils.h"

using namespace std;

namespace Output {

static constexpr uint64_t DSP_CLOCK = 2048000uLL * 80;

static constexpr uint64_t IIO_TIMEOUT_MS = 1000;

static constexpr size_t TRANSMISSION_FRAME_LEN_SAMPS = (2656 + 76 * 2552) * /* I+Q */ 2;
static constexpr size_t IIO_BUFFERS = 2;
static constexpr size_t IIO_BUFFER_LEN_SAMPS = TRANSMISSION_FRAME_LEN_SAMPS / IIO_BUFFERS;

static string get_iio_error(int err)
{
    char dst[256];
    iio_strerror(-err, dst, sizeof(dst));
    return string(dst);
}

static void fill_time(struct timespec *t)
{
    if (clock_gettime(CLOCK_REALTIME, t) != 0) {
        throw std::runtime_error(string("Failed to retrieve CLOCK_REALTIME") + strerror(errno));
    }
}

Dexter::Dexter(SDRDeviceConfig& config) :
    SDRDevice(),
    m_conf(config)
{
    etiLog.level(info) << "Dexter:Creating the device";

    m_ctx = iio_create_local_context();
    if (not m_ctx) {
        throw std::runtime_error("Dexter: Unable to create iio context");
    }

    int r;
    if ((r = iio_context_set_timeout(m_ctx, IIO_TIMEOUT_MS)) != 0) {
        etiLog.level(error) << "Failed to set IIO timeout " << get_iio_error(r);
    }

    m_dexter_dsp_tx = iio_context_find_device(m_ctx, "dexter_dsp_tx");
    if (not m_dexter_dsp_tx) {
        throw std::runtime_error("Dexter: Unable to find dexter_dsp_tx iio device");
    }

    m_ad9957 = iio_context_find_device(m_ctx, "ad9957");
    if (not m_ad9957) {
        throw std::runtime_error("Dexter: Unable to find ad9957 iio device");
    }

    m_ad9957_tx0 = iio_context_find_device(m_ctx, "ad9957_tx0");
    if (not m_ad9957_tx0) {
        throw std::runtime_error("Dexter: Unable to find ad9957_tx0 iio device");
    }

    // TODO make DC offset configurable and add to RC
    if ((r = iio_device_attr_write_longlong(m_dexter_dsp_tx, "dc0", 0)) != 0) {
        throw std::runtime_error("Failed to set dexter_dsp_tx.dc0 = false: " + get_iio_error(r));
    }

    if ((r = iio_device_attr_write_longlong(m_dexter_dsp_tx, "dc1", 0)) != 0) {
        throw std::runtime_error("Failed to set dexter_dsp_tx.dc1 = false: " + get_iio_error(r));
    }

    if (m_conf.sampleRate != 2048000) {
        throw std::runtime_error("Dexter: Only 2048000 samplerate supported");
    }

    tune(m_conf.lo_offset, m_conf.frequency);
    // TODO m_conf.frequency = m_dexter_dsp_tx->getFrequency(SOAPY_SDR_TX, 0);
    etiLog.level(info) << "Dexter:Actual frequency: " <<
        std::fixed << std::setprecision(3) <<
        m_conf.frequency / 1000.0 << " kHz.";

    // skip: Set bandwidth

    // skip: antenna

    // The FIFO should not contain data, but setting gain=0 before setting start_clks to zero is an additional security
    if ((r = iio_device_attr_write_longlong(m_dexter_dsp_tx, "gain0", 0)) != 0) {
        throw std::runtime_error("Failed to set dexter_dsp_tx.gain0 = 0 : " + get_iio_error(r));
    }

    if ((r = iio_device_attr_write_longlong(m_dexter_dsp_tx, "stream0_flush_fifo_trigger", 1)) != 0) {
        throw std::runtime_error("Failed to set dexter_dsp_tx.stream0_flush_fifo_trigger = 1 : " + get_iio_error(r));
    }

    if ((r = iio_device_attr_write_longlong(m_dexter_dsp_tx, "stream0_start_clks", 0)) != 0) {
        throw std::runtime_error("Failed to set dexter_dsp_tx.stream0_start_clks = 0 : " + get_iio_error(r));
    }

    constexpr int CHANNEL_INDEX = 0;
    m_tx_channel = iio_device_get_channel(m_ad9957_tx0, CHANNEL_INDEX);
    if (m_tx_channel == nullptr) {
        throw std::runtime_error("Dexter: Cannot create IIO channel.");
    }

    iio_channel_enable(m_tx_channel);

    m_buffer = iio_device_create_buffer(m_ad9957_tx0, IIO_BUFFER_LEN_SAMPS, 0);
    if (not m_buffer) {
        throw std::runtime_error("Dexter: Cannot create IIO buffer.");
    }

    // Flush the FPGA FIFO
    {
        constexpr size_t buflen_samps = TRANSMISSION_FRAME_LEN_SAMPS / IIO_BUFFERS;
        constexpr size_t buflen = buflen_samps * sizeof(int16_t);

        memset(iio_buffer_start(m_buffer), 0, buflen);
        ssize_t pushed = iio_buffer_push(m_buffer);
        if (pushed < 0) {
            etiLog.level(error) << "Dexter: init push buffer " << get_iio_error(pushed);
        }
        this_thread::sleep_for(chrono::milliseconds(200));
    }

    if ((r = iio_device_attr_write_longlong(m_dexter_dsp_tx, "gain0", m_conf.txgain)) != 0) {
        etiLog.level(error) << "Failed to set dexter_dsp_tx.gain0 = " << m_conf.txgain <<
            " : " << get_iio_error(r);
    }

    m_running = true;
    m_underflow_read_thread = std::thread(&Dexter::underflow_read_process, this);
}

void Dexter::channel_up()
{
    int r;
    if ((r = iio_device_attr_write_longlong(m_dexter_dsp_tx, "gain0", m_conf.txgain)) != 0) {
        etiLog.level(error) << "Failed to set dexter_dsp_tx.gain0 = " << m_conf.txgain <<
            " : " << get_iio_error(r);
    }

    m_channel_is_up = true;
    etiLog.level(debug) << "DEXTER CHANNEL_UP";
}

void Dexter::channel_down()
{
    int r;
    if ((r = iio_device_attr_write_longlong(m_dexter_dsp_tx, "gain0", 0)) != 0) {
        etiLog.level(error) << "Failed to set dexter_dsp_tx.gain0 = 0: " << get_iio_error(r);
    }

    // This will flush out the FIFO
    if ((r = iio_device_attr_write_longlong(m_dexter_dsp_tx, "stream0_start_clks", 0)) != 0) {
        etiLog.level(warn) << "Failed to set dexter_dsp_tx.stream0_start_clks = 0 : " << get_iio_error(r);
    }

    m_channel_is_up = false;
    etiLog.level(debug) << "DEXTER CHANNEL_DOWN";
}

void Dexter::handle_hw_time()
{
    /*
     * On startup, wait until `gpsdo_locked==1` and `pps_loss_of_signal==0`,
     * then do the clocks alignment and go to normal state.
     *
     * In normal state, if `pps_loss_of_signal==1`, go to holdover state.
     *
     * If we've been in holdover state for longer than the configured time, or
     * if `pps_loss_of_signal==0` stop the mod and restart.
     */
    int r;

    switch (m_clock_state) {
        case DexterClockState::Startup:
            {
                long long gpsdo_locked = 0;
                if ((r = iio_device_attr_read_longlong(m_dexter_dsp_tx, "gpsdo_locked", &gpsdo_locked)) != 0) {
                    etiLog.level(error) << "Failed to get dexter_dsp_tx.gpsdo_locked: " << get_iio_error(r);
                    throw std::runtime_error("Dexter: Cannot read IIO attribute");
                }

                long long pps_loss_of_signal = 0;
                if ((r = iio_device_attr_read_longlong(m_dexter_dsp_tx, "pps_loss_of_signal", &pps_loss_of_signal)) != 0) {
                    etiLog.level(error) << "Failed to get dexter_dsp_tx.pps_loss_of_signal: " << get_iio_error(r);
                    throw std::runtime_error("Dexter: Cannot read IIO attribute");
                }

                if (gpsdo_locked == 1 and pps_loss_of_signal == 0) {
                    /* Procedure:
                     * Wait 200ms after second change, fetch pps_clks attribute
                     * idem at the next second, and check that pps_clks incremented by DSP_CLOCK
                     * If ok, store the correspondence between current second change (measured in UTC clock time)
                     * and the counter value at pps rising edge. */

                    etiLog.level(info) << "Dexter: Waiting for second change...";

                    struct timespec time_at_startup;
                    fill_time(&time_at_startup);
                    time_at_startup.tv_nsec = 0;

                    struct timespec time_now;
                    do {
                        fill_time(&time_now);
                        this_thread::sleep_for(chrono::milliseconds(1));
                    } while (time_at_startup.tv_sec == time_now.tv_sec);
                    this_thread::sleep_for(chrono::milliseconds(200));

                    long long pps_clks = 0;
                    if ((r = iio_device_attr_read_longlong(m_dexter_dsp_tx, "pps_clks", &pps_clks)) != 0) {
                        etiLog.level(error) << "Failed to get dexter_dsp_tx.pps_clks: " << get_iio_error(r);
                        throw std::runtime_error("Dexter: Cannot read IIO attribute");
                    }

                    time_t tnow = time_now.tv_sec;
                    etiLog.level(info) << "Dexter: pps_clks " << pps_clks << " at UTC " <<
                        put_time(std::gmtime(&tnow), "%Y-%m-%d %H:%M:%S");

                    time_at_startup.tv_sec = time_now.tv_sec;
                    do {
                        fill_time(&time_now);
                        this_thread::sleep_for(chrono::milliseconds(1));
                    } while (time_at_startup.tv_sec == time_now.tv_sec);
                    this_thread::sleep_for(chrono::milliseconds(200));

                    long long pps_clks2 = 0;
                    if ((r = iio_device_attr_read_longlong(m_dexter_dsp_tx, "pps_clks", &pps_clks2)) != 0) {
                        etiLog.level(error) << "Failed to get dexter_dsp_tx.pps_clks: " << get_iio_error(r);
                        throw std::runtime_error("Dexter: Cannot read IIO attribute");
                    }
                    tnow = time_now.tv_sec;
                    etiLog.level(info) << "Dexter: pps_clks increased by " << pps_clks2 - pps_clks << " at UTC " <<
                        put_time(std::gmtime(&tnow), "%Y-%m-%d %H:%M:%S");

                    if ((uint64_t)pps_clks + DSP_CLOCK != (uint64_t)pps_clks2) {
                        throw std::runtime_error("Dexter: Wrong increase of pps_clks, expected " + to_string(DSP_CLOCK));
                    }

                    m_utc_seconds_at_startup = time_now.tv_sec;
                    m_clock_count_at_startup = pps_clks2;
                    m_holdover_since = chrono::steady_clock::time_point::min();
                    m_holdover_since_t = 0;
                    m_clock_state = DexterClockState::Normal;
                    etiLog.level(debug) << "Dexter: switch clock state Startup -> Normal";
                }
            }
            break;
        case DexterClockState::Normal:
            {
                long long pps_loss_of_signal = 0;
                if ((r = iio_device_attr_read_longlong(m_dexter_dsp_tx, "pps_loss_of_signal", &pps_loss_of_signal)) != 0) {
                    etiLog.level(error) << "Failed to get dexter_dsp_tx.pps_loss_of_signal: " << get_iio_error(r);
                    throw std::runtime_error("Dexter: Cannot read IIO attribute");
                }

                if (pps_loss_of_signal == 1) {
                    m_holdover_since = chrono::steady_clock::now();
                    m_holdover_since_t = chrono::system_clock::to_time_t(chrono::system_clock::now());
                    m_clock_state = DexterClockState::Holdover;
                    etiLog.level(debug) << "Dexter: switch clock state Normal -> Holdover";
                }
            }
            break;
        case DexterClockState::Holdover:
            {
                using namespace chrono;

                long long pps_loss_of_signal = 0;
                if ((r = iio_device_attr_read_longlong(m_dexter_dsp_tx, "pps_loss_of_signal", &pps_loss_of_signal)) != 0) {
                    etiLog.level(error) << "Failed to get dexter_dsp_tx.pps_loss_of_signal: " << get_iio_error(r);
                    throw std::runtime_error("Dexter: Cannot read IIO attribute");
                }

                const duration<double> d = steady_clock::now() - m_holdover_since;
                const auto max_holdover_duration = seconds(m_conf.maxGPSHoldoverTime);
                if (d > max_holdover_duration or pps_loss_of_signal == 0) {
                    m_clock_state = DexterClockState::Startup;
                    m_utc_seconds_at_startup = 0;
                    m_clock_count_at_startup = 0;
                    m_holdover_since = chrono::steady_clock::time_point::min();
                    m_holdover_since_t = 0;
                    etiLog.level(debug) << "Dexter: switch clock state Holdover -> Startup";
                }
            }
            break;
    }
}

void Dexter::tune(double lo_offset, double frequency)
{
    // lo_offset is applied to the DSP, and frequency is given to the ad9957

    long long freq = frequency;
    int r = 0;
    if ((r = iio_device_attr_write_longlong(m_ad9957, "center_frequency", freq)) != 0) {
        etiLog.level(warn) << "Failed to set ad9957.center_frequency = " << freq << " : " << get_iio_error(r);
    }

    long long lo_offs = lo_offset;

    if ((r = iio_device_attr_write_longlong(m_dexter_dsp_tx, "frequency0", lo_offs)) != 0) {
        etiLog.level(warn) << "Failed to set dexter_dsp_tx.frequency0 = " << lo_offs << " : " << get_iio_error(r);
    }
}

double Dexter::get_tx_freq(void) const
{
    long long lo_offset = 0;
    int r = 0;

    if ((r = iio_device_attr_read_longlong(m_dexter_dsp_tx, "frequency0", &lo_offset)) != 0) {
        etiLog.level(warn) << "Failed to read dexter_dsp_tx.frequency0: " << get_iio_error(r);
        return 0;
    }

    long long frequency = 0;
    if ((r = iio_device_attr_read_longlong(m_ad9957, "center_frequency", &frequency)) != 0) {
        etiLog.level(warn) << "Failed to read ad9957.center_frequency: " << get_iio_error(r);
        return 0;
    }

    return frequency + lo_offset;
}

void Dexter::set_txgain(double txgain)
{
    int r = 0;
    if ((r = iio_device_attr_write_longlong(m_dexter_dsp_tx, "gain0", txgain)) != 0) {
        etiLog.level(warn) << "Failed to set dexter_dsp_tx.gain0 = " << txgain << ": " << get_iio_error(r);
    }

    long long txgain_readback = 0;
    if ((r = iio_device_attr_read_longlong(m_dexter_dsp_tx, "gain0", &txgain_readback)) != 0) {
        etiLog.level(warn) << "Failed to read dexter_dsp_tx.gain0: " << get_iio_error(r);
    }
    else {
        m_conf.txgain = txgain_readback;
    }
}

double Dexter::get_txgain(void) const
{
    long long txgain_readback = 0;
    int r = 0;
    if ((r = iio_device_attr_read_longlong(m_dexter_dsp_tx, "gain0", &txgain_readback)) != 0) {
        etiLog.level(warn) << "Failed to read dexter_dsp_tx.gain0: " << get_iio_error(r);
    }
    return txgain_readback;
}

void Dexter::set_bandwidth(double bandwidth)
{
    return;
}

double Dexter::get_bandwidth(void) const
{
    return 0;
}

template <typename T>
static void attr_to_stat(
        Dexter::run_statistics_t& rs, iio_device *dexter_dsp_tx,
        const char* attr_name, const char* stat_name) {
    long long attr_value = 0;
    int r = 0;
    if ((r = iio_device_attr_read_longlong(dexter_dsp_tx, attr_name, &attr_value)) == 0) {
        rs[stat_name] = (T)attr_value;
    }
    else {
        rs[stat_name] = (ssize_t)-1;
        etiLog.level(error) << "Failed to get dexter_dsp_tx." << attr_name << ": " << get_iio_error(r);
    }
};

SDRDevice::run_statistics_t Dexter::get_run_statistics(void) const
{
    run_statistics_t rs;
    {
        std::unique_lock<std::mutex> lock(m_attr_thread_mutex);
        rs["underruns"] = underflows;
    }
    rs["latepackets"] = num_late;
    rs["frames"] = num_frames_modulated;

    attr_to_stat<size_t>(rs, m_dexter_dsp_tx, "clks", "clks");
    attr_to_stat<size_t>(rs, m_dexter_dsp_tx, "stream0_fifo_not_empty_clks", "fifo_not_empty_clks");
    attr_to_stat<size_t>(rs, m_dexter_dsp_tx, "gpsdo_locked", "gpsdo_locked");
    attr_to_stat<ssize_t>(rs, m_dexter_dsp_tx, "pps_clk_error_hz", "pps_clk_error_hz");
    attr_to_stat<size_t>(rs, m_dexter_dsp_tx, "pps_cnt", "pps_cnt");
    attr_to_stat<size_t>(rs, m_dexter_dsp_tx, "pps_loss_of_signal", "pps_loss_of_signal");
    attr_to_stat<size_t>(rs, m_dexter_dsp_tx, "dsp_version", "dsp_version");

    rs["in_holdover_since"] = 0;
    switch (m_clock_state) {
        case DexterClockState::Startup:
            rs["clock_state"] = "startup"; break;
        case DexterClockState::Normal:
            rs["clock_state"] = "normal"; break;
        case DexterClockState::Holdover:
            rs["clock_state"] = "holdover";
            rs["in_holdover_since"] = m_holdover_since_t;
            break;
    }

    constexpr double VMINFACT = 0.85;
    constexpr double VMAXFACT = 1.15;
    bool voltage_ok = true;
    bool temp_ok = true;

    {
        std::ifstream in("/sys/bus/i2c/devices/1-002f/hwmon/hwmon0/in2_input", std::ios::in | std::ios::binary);
        if (in) {
            double vcc3v3;
            in >> vcc3v3;
            rs["vcc3v3"] = vcc3v3 * (18+36)/36.0/1000.0;
            voltage_ok = (vcc3v3 > VMINFACT * 3.3) and (vcc3v3 < VMAXFACT * 3.3);
        }
        else {
            rs["vcc3v3"] = -1;
            voltage_ok = false;
        }
    }
    {
        std::ifstream in("/sys/bus/i2c/devices/1-002f/hwmon/hwmon0/in1_input", std::ios::in | std::ios::binary);
        if (in) {
            double vcc5v4;
            in >> vcc5v4;
            rs["vcc5v4"] = vcc5v4 * (51+36)/36.0/1000.0;
            voltage_ok = (vcc5v4 > VMINFACT * 5.4) and (vcc5v4 < VMAXFACT * 5.4);
        }
        else {
            rs["vcc5v4"] = -1;
            voltage_ok = false;
        }
    }
    {
        std::ifstream in("/sys/bus/i2c/devices/1-002f/hwmon/hwmon0/in3_input", std::ios::in | std::ios::binary);
        if (in) {
            double vfan;
            in >> vfan;
            rs["vfan"] = vfan * (560+22)/22.0/1000.0;
        }
        else {
            rs["vfan"] = -1;
            voltage_ok = false;
        }
    }
    {
        std::ifstream in("/sys/bus/i2c/devices/1-002f/hwmon/hwmon0/in0_input", std::ios::in | std::ios::binary);
        if (in) {
            double vccmainin;
            in >> vccmainin;
            rs["vcc_main_in"] = vccmainin * (560+22)/22.0/1000.0;
            voltage_ok |= vccmainin > 10.0;
        }
        else {
            rs["vcc_main_in"] = -1;
            voltage_ok = false;
        }
    }
    {
        std::ifstream in("/sys/bus/i2c/devices/1-002f/hwmon/hwmon0/in4_input", std::ios::in | std::ios::binary);
        if (in) {
            double vcc3v3pll;
            in >> vcc3v3pll;
            rs["vcc3v3pll"] = vcc3v3pll * (18+36)/36.0/1000.0;
            voltage_ok = (vcc3v3pll > VMINFACT * 3.3) and (vcc3v3pll < VMAXFACT * 3.3);
        }
        else {
            rs["vcc3v3pll"] = -1;
            voltage_ok = false;
        }
    }
    {
        std::ifstream in("/sys/bus/i2c/devices/1-002f/hwmon/hwmon0/in5_input", std::ios::in | std::ios::binary);
        if (in) {
            double vcc2v5io;
            in >> vcc2v5io;
            rs["vcc2v5io"] = vcc2v5io * (4.7+36)/36.0/1000.0;
            voltage_ok = (vcc2v5io > VMINFACT * 2.5) and (vcc2v5io < VMAXFACT * 2.5);
        }
        else {
            rs["vcc2v5io"] = -1;
            voltage_ok = false;
        }
    }
    {
        std::ifstream in("/sys/bus/i2c/devices/1-002f/hwmon/hwmon0/in6_input", std::ios::in | std::ios::binary);
        if (in) {
            double vccocxo;
            in >> vccocxo;
            rs["vccocxo"] = vccocxo * (51+36)/36.0/1000.0;
        }
        else {
            rs["vccocxo"] = -1;
            voltage_ok = false;
        }
    }

    optional<double> tfpga;
    for (int i = 0; i < 100; i++) {
        std::string path = "/sys/bus/iio/devices/iio:device";
        path += to_string(i);

        std::ifstream iio_name(path + "/name", std::ios::in | std::ios::binary);
        std::ostringstream sstr;
        sstr << iio_name.rdbuf();
        if (sstr.str() == "xadc\n") {
            std::ifstream in_scale(path + "/in_temp0_scale", std::ios::in | std::ios::binary);
            std::ifstream in_offset(path + "/in_temp0_offset", std::ios::in | std::ios::binary);
            std::ifstream in_temp0_raw(path + "/in_temp0_raw", std::ios::in | std::ios::binary);

            if (in_scale and in_offset and in_temp0_raw) {
                double scale, offset, temp0_raw ;
                in_scale >> scale;
                in_offset >> offset;
                in_temp0_raw >> temp0_raw;

                tfpga = (temp0_raw + offset) * scale / 1000.0;
            }
            break;
        }
    }

    if (tfpga) {
        rs["tempfpga"] = *tfpga;
        temp_ok |= *tfpga <= 85;
    }
    else {
        rs["tempfpga"] = -1;
        temp_ok = false;
    }

    rs["voltage_alarm"] = not voltage_ok;
    rs["temp_alarm"] = not temp_ok;

    return rs;
}


double Dexter::get_real_secs(void) const
{
    long long clks = 0;
    int r = 0;
    if ((r = iio_device_attr_read_longlong(m_dexter_dsp_tx, "clks", &clks)) != 0) {
        etiLog.level(error) << "Failed to get dexter_dsp_tx.clks: " << get_iio_error(r);
        throw std::runtime_error("Dexter: Cannot read IIO attribute");
    }

    switch (m_clock_state) {
        case DexterClockState::Startup:
            return 0;
        case DexterClockState::Normal:
        case DexterClockState::Holdover:
            return (double)m_utc_seconds_at_startup + (double)(clks - m_clock_count_at_startup) / (double)DSP_CLOCK;
    }
    throw std::logic_error("Unhandled switch");
}

void Dexter::set_rxgain(double rxgain)
{
    // TODO
}

double Dexter::get_rxgain(void) const
{
    // TODO
    return 0;
}

size_t Dexter::receive_frame(
        complexf *buf,
        size_t num_samples,
        frame_timestamp& ts,
        double timeout_secs)
{
    // TODO
    return 0;
}


bool Dexter::is_clk_source_ok()
{
    if (m_conf.enableSync) {
        handle_hw_time();
        return m_clock_state != DexterClockState::Startup;
    }
    else {
        return true;
    }
}

const char* Dexter::device_name(void) const
{
    return "Dexter";
}

std::optional<double> Dexter::get_temperature(void) const
{
    std::ifstream in("/sys/bus/i2c/devices/1-002f/hwmon/hwmon0/temp1_input", std::ios::in | std::ios::binary);
    if (in) {
        double tbaseboard;
        in >> tbaseboard;
        return tbaseboard / 1000.0;
    }
    else {
        return {};
    }
}

void Dexter::transmit_frame(struct FrameData&& frame)
{
    constexpr size_t frame_len_bytes = TRANSMISSION_FRAME_LEN_SAMPS * sizeof(int16_t);
    if (frame.buf.size() != frame_len_bytes) {
        etiLog.level(debug) << "Dexter::transmit_frame Expected " <<
            frame_len_bytes << " got " << frame.buf.size();
        throw std::runtime_error("Dexter: invalid buffer size");
    }

    const bool require_timestamped_tx = (m_conf.enableSync and frame.ts.timestamp_valid);

    if (not m_channel_is_up) {
        if (require_timestamped_tx) {
            if (m_clock_state == DexterClockState::Startup) {
                return; // not ready
            }
            else {
                constexpr uint64_t TIMESTAMP_PPS_PER_DSP_CLOCKS = DSP_CLOCK / 16384000;
                // TIMESTAMP_PPS_PER_DSP_CLOCKS=10 because timestamp_pps is represented in 16.384 MHz clocks
                uint64_t frame_start_clocks =
                    // at second level
                    ((int64_t)frame.ts.timestamp_sec - (int64_t)m_utc_seconds_at_startup) * DSP_CLOCK + m_clock_count_at_startup +
                    // at subsecond level
                    (uint64_t)frame.ts.timestamp_pps * TIMESTAMP_PPS_PER_DSP_CLOCKS;

                const double margin_s = frame.ts.offset_to_system_time();

                long long clks = 0;
                int r = 0;
                if ((r = iio_device_attr_read_longlong(m_dexter_dsp_tx, "clks", &clks)) != 0) {
                    etiLog.level(error) << "Failed to get dexter_dsp_tx.clks: " << get_iio_error(r);
                    throw std::runtime_error("Dexter: Cannot read IIO attribute");
                }

                const double margin_device_s = (double)(frame_start_clocks - clks) / DSP_CLOCK;

                etiLog.level(debug) << "DEXTER FCT " << frame.ts.fct << " TS CLK " <<
                    ((int64_t)frame.ts.timestamp_sec - (int64_t)m_utc_seconds_at_startup) * DSP_CLOCK << " + " <<
                    m_clock_count_at_startup << " + " <<
                    (uint64_t)frame.ts.timestamp_pps * TIMESTAMP_PPS_PER_DSP_CLOCKS << " = " <<
                    frame_start_clocks << " DELTA " << margin_s << " " << margin_device_s;

                // Ensure we hand the frame over to HW with a bit of margin
                if (margin_s < 0.2) {
                    etiLog.level(warn) << "Skip frame short margin " << margin_s;
                    num_late++;
                    return;
                }

                if ((r = iio_device_attr_write_longlong(m_dexter_dsp_tx, "stream0_start_clks", frame_start_clocks)) != 0) {
                    etiLog.level(warn) << "Skip frame, failed to set dexter_dsp_tx.stream0_start_clks = " << frame_start_clocks << " : " << get_iio_error(r);
                    num_late++;
                    return;
                }
                m_require_timestamp_refresh = false;
            }
        }

        channel_up();
    }

    if (m_require_timestamp_refresh) {
        etiLog.level(debug) << "DEXTER REQUIRE REFRESH";
        channel_down();
        m_require_timestamp_refresh = false;
    }

    // DabMod::launch_modulator ensures we get int16_t IQ here
    //const size_t num_samples = frame.buf.size() / (2*sizeof(int16_t));
    //const int16_t *buf = reinterpret_cast<const int16_t*>(frame.buf.data());

    if (m_channel_is_up) {
        for (size_t i = 0; i < IIO_BUFFERS; i++) {
            constexpr size_t buflen_samps = TRANSMISSION_FRAME_LEN_SAMPS / IIO_BUFFERS;
            constexpr size_t buflen = buflen_samps * sizeof(int16_t);

            memcpy(iio_buffer_start(m_buffer), frame.buf.data() + (i * buflen), buflen);
            ssize_t pushed = iio_buffer_push(m_buffer);
            if (pushed < 0) {
                etiLog.level(error) << "Dexter: failed to push buffer " << get_iio_error(pushed) <<
                    " after " << num_buffers_pushed << " bufs";
                num_buffers_pushed = 0;
                channel_down();
                break;
            }
            num_buffers_pushed++;
        }
        num_frames_modulated++;
    }

    {
        std::unique_lock<std::mutex> lock(m_attr_thread_mutex);
        size_t u = underflows;
        lock.unlock();

        if (u != 0 and u != prev_underflows) {
            etiLog.level(warn) << "Dexter: underflow! " << prev_underflows << " -> " << u;
        }

        prev_underflows = u;
    }
}

void Dexter::underflow_read_process()
{
    m_underflow_ctx = iio_create_local_context();
    if (not m_underflow_ctx) {
        throw std::runtime_error("Dexter: Unable to create iio context for underflow");
    }

    auto dexter_dsp_tx = iio_context_find_device(m_ctx, "dexter_dsp_tx");
    if (not dexter_dsp_tx) {
        throw std::runtime_error("Dexter: Unable to find dexter_dsp_tx iio device");
    }

    set_thread_name("dexter_underflow");

    while (m_running) {
        this_thread::sleep_for(chrono::seconds(1));
        long long underflows_attr = 0;

        int r = iio_device_attr_read_longlong(m_dexter_dsp_tx, "buffer_underflows0", &underflows_attr);

        if (r == 0) {
            size_t underflows_new = underflows_attr;

            std::unique_lock<std::mutex> lock(m_attr_thread_mutex);
            if (underflows_new != underflows and underflows_attr != 0) {
                underflows = underflows_new;
            }
        }
    }
    m_running = false;
}

Dexter::~Dexter()
{
    m_running = false;
    if (m_underflow_read_thread.joinable()) {
        m_underflow_read_thread.join();
    }

    if (m_ctx) {
        if (m_dexter_dsp_tx) {
            iio_device_attr_write_longlong(m_dexter_dsp_tx, "gain0", 0);
        }

        if (m_buffer) {
            iio_buffer_destroy(m_buffer);
            m_buffer = nullptr;
        }

        if (m_tx_channel) {
            iio_channel_disable(m_tx_channel);
        }

        iio_context_destroy(m_ctx);
        m_ctx = nullptr;
    }

    if (m_underflow_ctx) {
        iio_context_destroy(m_underflow_ctx);
        m_underflow_ctx = nullptr;
    }
}

} // namespace Output

#endif // HAVE_DEXTER
