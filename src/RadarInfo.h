/******************************************************************************
 *
 * Project:  OpenCPN
 * Purpose:  Navico BR24 Radar Plugin
 * Author:   David Register
 *           Dave Cowell
 *           Kees Verruijt
 *           Douwe Fokkema
 *           Sean D'Epagnier
 ***************************************************************************
 *   Copyright (C) 2010 by David S. Register              bdbcat@yahoo.com *
 *   Copyright (C) 2012-2013 by Dave Cowell                                *
 *   Copyright (C) 2012-2013 by Kees Verruijt         canboat@verruijt.net *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************
 */

#ifndef _RADAR_INFO_H_
#define _RADAR_INFO_H_

#include "br24radar_pi.h"

class RadarDraw;
class RadarCanvas;
class RadarFrame;

class radar_control_item
{
public:
    int value;
    int button;
    bool mod;

    void Update(int v);
};

class RadarInfo {

public:
    wxString                name; // Either "Radar", "Radar A", "Radar B".
    int                     radar; // Which radar this is (0..., max 2 for now)

    /* User radar settings */

    bool                    mod;
    radar_control_item      range;
    radar_control_item      gain;
    radar_control_item      interference_rejection;
    radar_control_item      target_separation;
    radar_control_item      noise_rejection;
    radar_control_item      target_boost;
    radar_control_item      sea;
    radar_control_item      rain;
    radar_control_item      scan_speed;
    radar_control_item      bearing_alignment;
    radar_control_item      antenna_height;
    radar_control_item      local_interference_rejection;
    radar_control_item      side_lobe_suppression;

    /* Per radar objects */

    br24Transmit           *transmit;
    br24Receive            *receive;
    br24ControlsDialog     *control_dialog;
    RadarFrame             *radar_frame;
    RadarCanvas            *radar_canvas;
    RadarDraw              *draw;               // Abstract painting method

    /* Abstractions of our own */

    bool                    data_seen;
    bool                    radar_seen;
    int                     range_meters;
    int                     commanded_range_meters;
    RadarType               radar_type;
    bool                    auto_range_mode;
    bool                    control_box_closed;
    bool                    control_box_opened;

    GuardZone              *guard_zone[GUARD_ZONES];
    receive_statistics      statistics;

    bool                    multi_sweep_filter;
    UINT8                   history[LINES_PER_ROTATION][RETURNS_PER_LINE];
#define HISTORY_FILTER_ALLOW(x) (HasBitCount2[(x) & 7])

    volatile bool           m_quit;

    /* Methods */

    RadarInfo(br24radar_pi *pi, wxString name, int radar);
    ~RadarInfo();

    bool Init(int verbose);
    void StartReceive();
    void SetName(wxString name);
    void SetRangeMeters(int range);
    bool SetControlValue(ControlType controlType, int value);
    void ResetSpokes();
    void ProcessRadarSpoke(SpokeBearing angle, UINT8 * data, size_t len, int range_meters, wxLongLong nowMillis);
    void ProcessRadarPacket(time_t now);
    void RenderGuardZone(wxPoint radar_center, double v_scale_ppm);
    void RenderRadarImage(wxPoint center, double scale, double rotation, bool overlay);
    void ShowRadarWindow();
    void ShowRadarWindow(bool show);

private:
    br24radar_pi           *m_pi;
    int                     m_verbose;
    int                     m_refresh_countdown;
    bool                    m_use_shader;
    bool                    m_color_option;
};

#endif

// vim: sw=4:ts=8: