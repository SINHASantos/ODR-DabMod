/*
   Copyright (C) 2005, 2006, 2007, 2008, 2009, 2010 Her Majesty the
   Queen in Right of Canada (Communications Research Center Canada)

   Copyright (C) 2024
   Matthias P. Braendli, matthias.braendli@mpb.li

    http://opendigitalradio.org
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

#include "output/SDR.h"
#include "output/UHD.h"
#include "output/Lime.h"
#include "output/Dexter.h"

#include "PcDebug.h"
#include "Log.h"
#include "RemoteControl.h"
#include "Utils.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <assert.h>
#include <stdexcept>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>

using namespace std;

namespace Output {

// Maximum number of frames that can wait in frames.
// Keep it low when not using synchronised transmission, in order to reduce delay.
// When using synchronised transmission, use a 6s buffer to give us enough margin.
static constexpr size_t FRAMES_MAX_SIZE_UNSYNC = 8;
static constexpr size_t FRAMES_MAX_SIZE_SYNC = 250;

// If the timestamp is further in the future than
// 100 seconds, abort
static constexpr double TIMESTAMP_ABORT_FUTURE = 100;

SDR::SDR(SDRDeviceConfig& config, std::shared_ptr<SDRDevice> device) :
    ModOutput(), ModMetadata(), RemoteControllable("sdr"),
    m_config(config),
    m_device(device)
{
    // muting is remote-controllable
    m_config.muting = false;

    m_running.store(true);
    m_device_thread = std::thread(&SDR::process_thread_entry, this);

    if (m_config.dpdFeedbackServerPort > 0) {
        m_dpd_feedback_server = make_shared<DPDFeedbackServer>(
                m_device,
                m_config.dpdFeedbackServerPort,
                m_config.sampleRate);
    }

    RC_ADD_PARAMETER(txgain, "TX gain");
    RC_ADD_PARAMETER(rxgain, "RX gain for DPD feedback");
    RC_ADD_PARAMETER(bandwidth, "Analog front-end bandwidth");
    RC_ADD_PARAMETER(freq, "Transmission frequency in Hz");
    RC_ADD_PARAMETER(channel, "Transmission frequency as channel");
    RC_ADD_PARAMETER(muting, "Mute the output by stopping the transmitter");
    RC_ADD_PARAMETER(temp, "Temperature in degrees C of the device");
    RC_ADD_PARAMETER(underruns, "Counter of number of underruns");
    RC_ADD_PARAMETER(latepackets, "Counter of number of late packets");
    RC_ADD_PARAMETER(frames, "Counter of number of frames modulated");
    RC_ADD_PARAMETER(synchronous, "1 if configured for synchronous transmission");
    RC_ADD_PARAMETER(max_gps_holdover_time, "Max holdover duration in seconds");

#ifdef HAVE_OUTPUT_UHD
    if (std::dynamic_pointer_cast<UHD>(device)) {
        RC_ADD_PARAMETER(gpsdo_num_sv, "Number of Satellite Vehicles tracked by GPSDO");
        RC_ADD_PARAMETER(gpsdo_holdover, "1 if the GPSDO is in holdover, 0 if it is using gnss");
    }
#endif // HAVE_OUTPUT_UHD

    RC_ADD_PARAMETER(queued_frames_ms, "Number of frames queued, represented in milliseconds");

#ifdef HAVE_LIMESDR
    if (std::dynamic_pointer_cast<Lime>(device)) {
        RC_ADD_PARAMETER(fifo_fill, "A value representing the Lime FIFO fullness [percent]");
    }
#endif // HAVE_LIMESDR

#ifdef HAVE_DEXTER
    if (std::dynamic_pointer_cast<Dexter>(device)) {
        RC_ADD_PARAMETER(in_holdover_since, "DEXTER timestamp when holdover began");
        RC_ADD_PARAMETER(remaining_holdover_s, "DEXTER remaining number of seconds in holdover");
        RC_ADD_PARAMETER(clock_state, "DEXTER clock state: startup/normal/holdover");
    }
#endif // HAVE_DEXTER


}

SDR::~SDR()
{
    m_running.store(false);

    m_queue.trigger_wakeup();

    if (m_device_thread.joinable()) {
        m_device_thread.join();
    }
}

void SDR::set_sample_size(size_t size)
{
    m_size = size;
}

int SDR::process(Buffer *dataIn)
{
    if (not m_running) {
        throw std::runtime_error("SDR thread failed");
    }

    const uint8_t* pDataIn = (uint8_t*)dataIn->getData();
    m_frame.resize(dataIn->getLength());
    std::copy(pDataIn, pDataIn + dataIn->getLength(),
            m_frame.begin());

    // We will effectively transmit the frame once we got the metadata.

    return dataIn->getLength();
}

meta_vec_t SDR::process_metadata(const meta_vec_t& metadataIn)
{
    if (m_device and m_running) {
        FrameData frame;
        frame.buf = std::move(m_frame);
        frame.sampleSize = m_size;

        if (metadataIn.empty()) {
            etiLog.level(info) <<
                "SDR output: dropping one frame with invalid FCT";
        }
        else {
            /* In transmission modes where several ETI frames are needed to
             * build one transmission frame (like in TM 1), we will have
             * several entries in metadataIn. Take the first one, which
             * comes from the earliest ETI frame.
             * This behaviour is different to earlier versions of ODR-DabMod,
             * which took the timestamp from the latest ETI frame.
             */
            frame.ts = metadataIn[0].ts;

            // TODO check device running

            try {
                if (m_dpd_feedback_server) {
                    m_dpd_feedback_server->set_tx_frame(frame.buf, frame.ts);
                }
            }
            catch (const runtime_error& e) {
                etiLog.level(warn) <<
                    "SDR output: Feedback server failed, restarting...";

                m_dpd_feedback_server = std::make_shared<DPDFeedbackServer>(
                        m_device,
                        m_config.dpdFeedbackServerPort,
                        m_config.sampleRate);
            }


            const auto max_size = m_config.enableSync ? FRAMES_MAX_SIZE_SYNC : FRAMES_MAX_SIZE_UNSYNC;
            auto r = m_queue.push_overflow(std::move(frame), max_size);
            etiLog.log(trace, "SDR,push %d %zu", r.overflowed, r.new_size);

            num_queue_overflows += r.overflowed ? 1 : 0;
        }
    }
    else {
        // Ignore frame
    }
    return {};
}


void SDR::process_thread_entry()
{
    // Set thread priority to realtime
    if (int ret = set_realtime_prio(1)) {
        etiLog.level(error) << "Could not set priority for SDR device thread:" << ret;
    }

    set_thread_name("sdrdevice");

    last_tx_time_initialised = false;

    try {
        while (m_running.load()) {
            struct FrameData frame;
            etiLog.log(trace, "SDR,wait");
            m_queue.wait_and_pop(frame);
            etiLog.log(trace, "SDR,pop");

            if (m_running.load() == false) {
                break;
            }

            if (m_device) {
                handle_frame(std::move(frame));
            }
        }
    }
    catch (const ThreadsafeQueueWakeup& e) { }
    catch (const runtime_error& e) {
        etiLog.level(error) << "SDR output thread caught runtime error: " <<
            e.what();
    }

    m_running.store(false);
}

const char* SDR::name()
{
    if (m_device) {
        m_name = "OutputSDR(";
        m_name += m_device->device_name();
        m_name += ")";
    }
    else {
        m_name = "OutputSDR(<no device>)";
    }
    return m_name.c_str();
}


void SDR::handle_frame(struct FrameData&& frame)
{
    // Assumes m_device is valid

    if (not m_device->is_clk_source_ok()) {
        return;
    }

    const auto& time_spec = frame.ts;

    if (m_config.enableSync and m_config.muteNoTimestamps and not time_spec.timestamp_valid) {
        etiLog.log(info, "OutputSDR: Muting sample %d : no timestamp\n", frame.ts.fct);
        return;
    }

    if (m_config.enableSync and time_spec.timestamp_valid) {
        // Tx time from MNSC and TIST
        const uint32_t tx_second = frame.ts.timestamp_sec;
        const uint32_t tx_pps    = frame.ts.timestamp_pps;

        const double device_time = m_device->get_real_secs();

        if (not frame.ts.timestamp_valid) {
            /* We have not received a full timestamp through
             * MNSC. We sleep through the frame.
             */
            etiLog.level(info) <<
                "OutputSDR: Throwing sample " << frame.ts.fct <<
                " away: incomplete timestamp " << tx_second <<
                " / " << tx_pps;
            return;
        }

        if (frame.ts.offset_changed) {
            etiLog.level(debug) << "TS offset changed";
            m_device->require_timestamp_refresh();
        }

        if (last_tx_time_initialised) {
            const size_t sizeIn = frame.buf.size() / frame.sampleSize;

            // Checking units for the increment calculation:
            // samps  * ticks/s  / (samps/s)
            // (samps * ticks * s) / (s * samps)
            // ticks
            const uint64_t increment = (uint64_t)sizeIn * 16384000ul /
                                       (uint64_t)m_config.sampleRate;

            uint32_t expected_sec = last_tx_second + increment / 16384000ul;
            uint32_t expected_pps = last_tx_pps + increment % 16384000ul;

            while (expected_pps >= 16384000) {
                expected_sec++;
                expected_pps -= 16384000;
            }

            if (expected_sec != tx_second or expected_pps != tx_pps) {
                etiLog.level(warn) << "OutputSDR: timestamp irregularity at FCT=" << frame.ts.fct <<
                    std::fixed <<
                    " Expected " <<
                    expected_sec << "+" << (double)expected_pps/16384000.0 <<
                    "(" << expected_pps << ")" <<
                    " Got " <<
                    tx_second << "+" << (double)tx_pps/16384000.0 <<
                    "(" << tx_pps << ")";

                m_device->require_timestamp_refresh();
            }
        }

        last_tx_second = tx_second;
        last_tx_pps    = tx_pps;
        last_tx_time_initialised = true;

        const double pps_offset = tx_pps / 16384000.0;

        etiLog.log(trace, "SDR,tist %f", time_spec.get_real_secs());

        if (time_spec.get_real_secs() < device_time) {
            etiLog.level(warn) <<
                "OutputSDR: Timestamp in the past at FCT=" << frame.ts.fct << " offset: " <<
                std::fixed <<
                time_spec.get_real_secs() - device_time <<
                "  (" << device_time << ")"
                " frame " << frame.ts.fct <<
                ", tx_second " << tx_second <<
                ", pps " << pps_offset;
            m_device->require_timestamp_refresh();
            return;
        }

        if (time_spec.get_real_secs() > device_time + TIMESTAMP_ABORT_FUTURE) {
            etiLog.level(error) <<
                "OutputSDR: Timestamp way too far in the future at FCT=" << frame.ts.fct << " offset: " <<
                std::fixed <<
                time_spec.get_real_secs() - device_time;
            throw std::runtime_error("Timestamp error. Aborted.");
        }
    }

    if (m_config.muting) {
        etiLog.log(info, "OutputSDR: Muting FCT=%d requested", frame.ts.fct);
        m_device->require_timestamp_refresh();
        return;
    }

    m_device->transmit_frame(std::move(frame));
}

// =======================================
// Remote Control
// =======================================
void SDR::set_parameter(const string& parameter, const string& value)
{
    stringstream ss(value);
    ss.exceptions ( stringstream::failbit | stringstream::badbit );

    if (parameter == "txgain") {
        ss >> m_config.txgain;
        m_device->set_txgain(m_config.txgain);
    }
    else if (parameter == "rxgain") {
        ss >> m_config.rxgain;
        m_device->set_rxgain(m_config.rxgain);
    }
    else if (parameter == "bandwidth") {
        ss >> m_config.bandwidth;
        m_device->set_bandwidth(m_config.bandwidth);
    }
    else if (parameter == "freq") {
        ss >> m_config.frequency;
        m_device->tune(m_config.lo_offset, m_config.frequency);
    }
    else if (parameter == "channel") {
        try {
            const double frequency = parse_channel(value);

            m_config.frequency = frequency;
            m_device->tune(m_config.lo_offset, m_config.frequency);
        }
        catch (const std::out_of_range& e) {
            throw ParameterError("Cannot parse channel");
        }
    }
    else if (parameter == "muting") {
        ss >> m_config.muting;
    }
    else if (parameter == "synchronous") {
        uint32_t enableSync = 0;
        ss >> enableSync;
        m_config.enableSync = enableSync > 0;
    }
    else if (parameter == "max_gps_holdover_time") {
        ss >> m_config.maxGPSHoldoverTime;
    }
    else {
        stringstream ss_err;
        ss_err << "Parameter '" << parameter
            << "' is read-only or not exported by controllable " << get_rc_name();
        throw ParameterError(ss_err.str());
    }
}

const string SDR::get_parameter(const string& parameter) const
{
    stringstream ss;
    ss << std::fixed;
    if (parameter == "txgain") {
        ss << m_config.txgain;
    }
    else if (parameter == "rxgain") {
        ss << m_config.rxgain;
    }
    else if (parameter == "bandwidth") {
        ss << m_config.bandwidth;
    }
    else if (parameter == "freq") {
        ss << m_config.frequency;
    }
    else if (parameter == "channel") {
        const auto maybe_freq = convert_frequency_to_channel(m_config.frequency);

        if (maybe_freq.has_value()) {
            ss << *maybe_freq;
        }
        else {
            throw ParameterError("Frequency is outside list of channels");
        }
    }
    else if (parameter == "muting") {
        ss << m_config.muting;
    }
    else if (parameter == "temp") {
        if (not m_device) {
            throw ParameterError("OutputSDR has no device");
        }
        const std::optional<double> temp = m_device->get_temperature();
        if (temp) {
            ss << *temp;
        }
        else {
            throw ParameterError("Temperature not available");
        }
    }
    else if (parameter == "queued_frames_ms") {
        ss << m_queue.size() *
            chrono::duration_cast<chrono::milliseconds>(transmission_frame_duration(m_config.dabMode))
            .count();
    }
    else if (parameter == "synchronous") {
        ss << m_config.enableSync;
    }
    else if (parameter == "max_gps_holdover_time") {
        ss << m_config.maxGPSHoldoverTime;
    }
    else {
        if (m_device) {
            const auto stat = m_device->get_run_statistics();
            try {
                const auto& value = stat.at(parameter).v;
                if (std::holds_alternative<string>(value)) {
                    ss << std::get<string>(value);
                }
                else if (std::holds_alternative<double>(value)) {
                    ss << std::get<double>(value);
                }
                else if (std::holds_alternative<ssize_t>(value)) {
                    ss << std::get<ssize_t>(value);
                }
                else if (std::holds_alternative<size_t>(value)) {
                    ss << std::get<size_t>(value);
                }
                else if (std::holds_alternative<bool>(value)) {
                    ss << (std::get<bool>(value) ? 1 : 0);
                }
                else if (std::holds_alternative<std::nullopt_t>(value)) {
                    ss << "";
                }
                else {
                    throw std::logic_error("variant alternative not handled");
                }
                return ss.str();
            }
            catch (const std::out_of_range&) {
            }
        }

        ss << "Parameter '" << parameter <<
            "' is not exported by controllable " << get_rc_name();
        throw ParameterError(ss.str());
    }
    return ss.str();
}

const json::map_t SDR::get_all_values() const
{
    json::map_t stat = m_device->get_run_statistics();

    stat["txgain"].v = m_config.txgain;
    stat["rxgain"].v = m_config.rxgain;
    stat["freq"].v = m_config.frequency;
    stat["muting"].v = m_config.muting;
    stat["temp"].v = std::nullopt;

    const auto maybe_freq = convert_frequency_to_channel(m_config.frequency);

    if (maybe_freq.has_value()) {
        stat["channel"].v = *maybe_freq;
    }
    else {
        stat["channel"].v = std::nullopt;
    }

    if (m_device) {
        const std::optional<double> temp = m_device->get_temperature();
        if (temp) {
            stat["temp"].v = *temp;
        }
    }
    stat["queued_frames_ms"].v = m_queue.size() *
            (size_t)chrono::duration_cast<chrono::milliseconds>(transmission_frame_duration(m_config.dabMode))
            .count();

    stat["synchronous"].v = m_config.enableSync;
    stat["max_gps_holdover_time"].v = (size_t)m_config.maxGPSHoldoverTime;

    return stat;
}

} // namespace Output
