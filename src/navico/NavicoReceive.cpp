/******************************************************************************
 *
 * Project:  OpenCPN
 * Purpose:  Radar Plugin
 * Author:   David Register
 *           Dave Cowell
 *           Kees Verruijt
 *           Hakan Svensson
 *           Douwe Fokkema
 *           Sean D'Epagnier
 ***************************************************************************
 *   Copyright (C) 2010 by David S. Register              bdbcat@yahoo.com *
 *   Copyright (C) 2012-2013 by Dave Cowell                                *
 *   Copyright (C) 2012-2016 by Kees Verruijt         canboat@verruijt.net *
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

#include "NavicoReceive.h"

PLUGIN_BEGIN_NAMESPACE

/*
 * This file not only contains the radar receive threads, it is also
 * the only unit that understands what the radar returned data looks like.
 * The rest of the plugin uses a (slightly) abstract definition of the radar.
 */

#define MILLIS_PER_SELECT 250
#define SECONDS_SELECT(x) ((x)*MILLISECONDS_PER_SECOND / MILLIS_PER_SELECT)

//
// Navico radars use an internal spoke ID that has range [0..4096> but they
// only send half of them
//
#define SPOKES (4096)
#define SCALE_RAW_TO_DEGREES(raw) ((raw) * (double)DEGREES_PER_ROTATION / SPOKES)
#define SCALE_DEGREES_TO_RAW(angle) ((int)((angle) * (double)SPOKES / DEGREES_PER_ROTATION))


// A marker that uniquely identifies BR24 generation scanners, as opposed to 4G(eneration)
// Note that 3G scanners are BR24's with better power, so they are more BR24+ than 4G-.
// As far as we know they 3G's use exactly the same command set.

// If BR24MARK is found, we switch to BR24 mode, otherwise 4G.
static UINT8 BR24MARK[] = {0x00, 0x44, 0x0d, 0x0e};

/*
 Heading on radar. Observed in field:
 - Hakan: BR24, no RI: 0x9234 = negative, with recognisable 1234 in hex?
 - Marcus: 3G, RI, true heading: 0x45be
 - Kees: 4G, RI, mag heading: 0x07d6 = 2006 = 176,6 deg
 - Kees: 4G, RI, no heading: 0x8000 = -1 = negative
 Known values for heading value:
*/
#define HEADING_TRUE_FLAG 0x4000
#define HEADING_MASK (SPOKES - 1)
#define HEADING_VALID(x) (((x) & ~(HEADING_TRUE_FLAG | HEADING_MASK)) == 0)

#pragma pack(push, 1)

struct common_header {
  UINT8 headerLen;       // 1 bytes
  UINT8 status;          // 1 bytes
  UINT8 scan_number[2];  // 2 bytes, 0-4095
  UINT8 u00[4];          // 4 bytes
  UINT8 angle[2];        // 2 bytes
  UINT8 heading[2];      // 2 bytes heading with RI-10/11. See bitmask explanation above.
};

struct br24_header {
  UINT8 headerLen;       // 1 bytes
  UINT8 status;          // 1 bytes
  UINT8 scan_number[2];  // 2 bytes, 0-4095
  UINT8 mark[4];         // 4 bytes 0x00, 0x44, 0x0d, 0x0e
  UINT8 angle[2];        // 2 bytes
  UINT8 heading[2];      // 2 bytes heading with RI-10/11. See bitmask explanation above.
  UINT8 range[4];        // 4 bytes
  UINT8 u01[2];          // 2 bytes blank
  UINT8 u02[2];          // 2 bytes
  UINT8 u03[4];          // 4 bytes blank
};                       /* total size = 24 */

struct br4g_header {
  UINT8 headerLen;       // 1 bytes
  UINT8 status;          // 1 bytes
  UINT8 scan_number[2];  // 2 bytes, 0-4095
  UINT8 u00[2];          // Always 0x4400 (integer)
  UINT8 largerange[2];   // 2 bytes or -1
  UINT8 angle[2];        // 2 bytes
  UINT8 heading[2];      // 2 bytes heading with RI-10/11 or -1. See bitmask explanation above.
  UINT8 smallrange[2];   // 2 bytes or -1
  UINT8 rotation[2];     // 2 bytes, rotation/angle
  UINT8 u02[4];          // 4 bytes signed integer, always -1
  UINT8 u03[4];          // 4 bytes signed integer, mostly -1 (0x80 in last byte) or 0xa0 in last byte
};                       /* total size = 24 */

struct radar_line {
  union {
    common_header common;
    br24_header br24;
    br4g_header br4g;
  };
  UINT8 data[RETURNS_PER_LINE];
};

/* Normally the packets are have 32 spokes, or scan lines, but we assume nothing
 * so we take up to 120 spokes. This is the nearest round figure without going over
 * 64kB.
 */

struct radar_frame_pkt {
  UINT8 frame_hdr[8];
  radar_line line[120];  //  scan lines, or spokes
};
#pragma pack(pop)

bool g_first_receive = true;

// ProcessFrame
// ------------
// Process one radar frame packet, which can contain up to 32 'spokes' or lines extending outwards
// from the radar up to the range indicated in the packet.
//
void NavicoReceive::ProcessFrame(const UINT8 *data, int len) {
  time_t now = time(0);

  double lat;
  double lon;

  m_pi->GetRadarPosition(&lat, &lon);

  // log_line.time_rec = wxGetUTCTimeMillis();
  wxLongLong time_rec = wxGetUTCTimeMillis();

  radar_frame_pkt *packet = (radar_frame_pkt *)data;

  wxCriticalSectionLocker lock(m_ri->m_exclusive);

  m_ri->m_radar_timeout = now + WATCHDOG_TIMEOUT;
  m_ri->m_data_timeout = now + DATA_TIMEOUT;
  m_ri->m_state.Update(RADAR_TRANSMIT);

  m_ri->m_statistics.packets++;
  if (len < (int)sizeof(packet->frame_hdr)) {
    // The packet is so small it contains no scan_lines, quit!
    m_ri->m_statistics.broken_packets++;
    return;
  }
  int scanlines_in_packet = (len - sizeof(packet->frame_hdr)) / sizeof(radar_line);
  if (scanlines_in_packet != 32) {
    m_ri->m_statistics.broken_packets++;
  }

  if (g_first_receive) {
    g_first_receive = false;
    wxLongLong startup_elapsed = wxGetUTCTimeMillis() - m_pi->GetBootMillis();
    LOG_INFO(wxT("radar_pi: First radar spoke received after %llu ms\n"), startup_elapsed);
  }

  for (int scanline = 0; scanline < scanlines_in_packet; scanline++) {
    radar_line *line = &packet->line[scanline];

    // Validate the spoke
    int spoke = line->common.scan_number[0] | (line->common.scan_number[1] << 8);
    m_ri->m_statistics.spokes++;
    if (line->common.headerLen != 0x18) {
      LOG_RECEIVE(wxT("radar_pi: strange header length %d"), line->common.headerLen);
      // Do not draw something with this...
      m_ri->m_statistics.missing_spokes++;
      m_next_spoke = (spoke + 1) % SPOKES;
      continue;
    }
    if (line->common.status != 0x02 && line->common.status != 0x12) {
      LOG_RECEIVE(wxT("radar_pi: strange status %02x"), line->common.status);
      m_ri->m_statistics.broken_spokes++;
    }
    if (m_next_spoke >= 0 && spoke != m_next_spoke) {
      if (spoke > m_next_spoke) {
        m_ri->m_statistics.missing_spokes += spoke - m_next_spoke;
      } else {
        m_ri->m_statistics.missing_spokes += SPOKES + spoke - m_next_spoke;
      }
    }
    m_next_spoke = (spoke + 1) % SPOKES;

    int range_raw = 0;
    int angle_raw = 0;
    short int heading_raw = 0;
    int range_meters = 0;

    heading_raw = (line->common.heading[1] << 8) | line->common.heading[0];

    if (memcmp(line->br24.mark, BR24MARK, sizeof(BR24MARK)) == 0) {
      // BR24 and 3G mode
      range_raw = ((line->br24.range[2] & 0xff) << 16 | (line->br24.range[1] & 0xff) << 8 | (line->br24.range[0] & 0xff));
      angle_raw = (line->br24.angle[1] << 8) | line->br24.angle[0];
      range_meters = (int)((double)range_raw * 10.0 / sqrt(2.0));
#ifdef TODO
      if (m_ri->m_radar_type == RT_UNKNOWN) {
        LOG_INFO(wxT("radar_pi: %s is Navico type BR24 or 3G"), m_ri->m_name.c_str());
        m_ri->m_radar_type = RT_BR24;
        m_pi->m_pMessageBox->SetRadarType(RT_BR24);
      }
#endif
    } else {
      // 4G mode
      short int large_range = (line->br4g.largerange[1] << 8) | line->br4g.largerange[0];
      short int small_range = (line->br4g.smallrange[1] << 8) | line->br4g.smallrange[0];
      angle_raw = (line->br4g.angle[1] << 8) | line->br4g.angle[0];
      if (large_range == 0x80) {
        if (small_range == -1) {
          range_raw = 0;  // Invalid range received
        } else {
          range_raw = small_range;
        }
      } else {
        range_raw = large_range * 256;
      }
      range_meters = range_raw / 4;
    }

    /*
    LOG_BINARY_RECEIVE(wxString::Format(wxT("range=%d, angle=%d hdg=%d"), range_raw, angle_raw, heading_raw),
                       (uint8_t *)&line->br24, sizeof(line->br24));
    */

    bool radar_heading_valid = HEADING_VALID(heading_raw);
    bool radar_heading_true = (heading_raw & HEADING_TRUE_FLAG) != 0;
    double heading;
    int bearing_raw;

    if (radar_heading_valid && !m_pi->m_settings.ignore_radar_heading) {
      heading = MOD_DEGREES_FLOAT(SCALE_RAW_TO_DEGREES(heading_raw));
      m_pi->SetRadarHeading(heading, radar_heading_true);
    } else {
      m_pi->SetRadarHeading();
    }
    // Guess the heading for the spoke. This is updated much less frequently than the
    // data from the radar (which is accurate 10x per second), likely once per second.
    heading_raw = SCALE_DEGREES_TO_RAW(m_pi->GetHeadingTrue());  // include variation
    bearing_raw = angle_raw + heading_raw;
    // until here all is based on 4096 (SPOKES) scanlines

    SpokeBearing a = MOD_SPOKES(angle_raw / 2);    // divide by 2 to map on 2048 scanlines
    SpokeBearing b = MOD_SPOKES(bearing_raw / 2);  // divide by 2 to map on 2048 scanlines
    m_ri->ProcessRadarSpoke(a, b, line->data, RETURNS_PER_LINE, range_meters, time_rec, lat, lon);
  }
}

SOCKET NavicoReceive::PickNextEthernetCard() {
  SOCKET socket = INVALID_SOCKET;
  CLEAR_STRUCT(m_interface_addr);

  // Pick the next ethernet card
  // If set, we used this one last time. Go to the next card.
  if (m_interface) {
    m_interface = m_interface->ifa_next;
  }
  // Loop until card with a valid IPv4 address
  while (m_interface && !VALID_IPV4_ADDRESS(m_interface)) {
    m_interface = m_interface->ifa_next;
  }
  if (!m_interface) {
    if (m_interface_array) {
      freeifaddrs(m_interface_array);
      m_interface_array = 0;
    }
    if (!getifaddrs(&m_interface_array)) {
      m_interface = m_interface_array;
    }
    // Loop until card with a valid IPv4 address
    while (m_interface && !VALID_IPV4_ADDRESS(m_interface)) {
      m_interface = m_interface->ifa_next;
    }
  }
  if (m_interface && VALID_IPV4_ADDRESS(m_interface)) {
    m_interface_addr.addr = ((struct sockaddr_in *)m_interface->ifa_addr)->sin_addr;
    m_interface_addr.port = 0;
  }

  socket = GetNewReportSocket();

  return socket;
}

SOCKET NavicoReceive::GetNewReportSocket() {
  SOCKET socket;
  wxString error;

  if (m_interface_addr.addr.s_addr == 0) {
    return INVALID_SOCKET;
  }

  socket = startUDPMulticastReceiveSocket(m_interface_addr, m_report_addr, error);
  if (socket != INVALID_SOCKET) {
    wxString addr = FormatNetworkAddress(m_interface_addr);
    wxString rep_addr = FormatNetworkAddressPort(m_report_addr);

    LOG_RECEIVE(wxT("radar_pi: %s scanning interface %s for data from %s"), m_ri->m_name.c_str(), addr.c_str(), rep_addr.c_str());

    wxString s;
    s << m_ri->m_name << wxT(": ") << _("Scanning interface") << addr;
    SetStatus(s);
  } else {
    SetStatus(error);
    wxLogError(wxT("radar_pi: Unable to listen to socket: %s"), error.c_str());
  }
  return socket;
}

SOCKET NavicoReceive::GetNewDataSocket() {
  SOCKET socket;
  wxString error;

  if (m_interface_addr.addr.s_addr == 0) {
    return INVALID_SOCKET;
  }

  socket = startUDPMulticastReceiveSocket(m_interface_addr, m_data_addr, error);
  if (socket != INVALID_SOCKET) {
    wxString addr = FormatNetworkAddress(m_interface_addr);
    wxString rep_addr = FormatNetworkAddressPort(m_report_addr);

    LOG_RECEIVE(wxT("radar_pi: %s listening for data on %s from %s"), m_ri->m_name.c_str(), addr.c_str(), rep_addr.c_str());
  } else {
    SetStatus(error);
    wxLogError(wxT("radar_pi: Unable to listen to socket: %s"), error.c_str());
  }
  return socket;
}

/*
 * Entry
 *
 * Called by wxThread when the new thread is running.
 * It should remain running until Shutdown is called.
 */
void *NavicoReceive::Entry(void) {
  int r = 0;
  int no_data_timeout = 0;
  int no_spoke_timeout = 0;
  union {
    sockaddr_storage addr;
    sockaddr_in ipv4;
  } rx_addr;
  socklen_t rx_len;

  UINT8 data[sizeof(radar_frame_pkt)];
  m_interface_array = 0;
  m_interface = 0;
  struct sockaddr_in radarFoundAddr;
  sockaddr_in *radar_addr = 0;

  SOCKET dataSocket = INVALID_SOCKET;
  SOCKET reportSocket = INVALID_SOCKET;

  LOG_VERBOSE(wxT("radar_pi: NavicoReceive thread %s starting"), m_ri->m_name.c_str());

  if (m_interface_addr.addr.s_addr == 0) {
    reportSocket = GetNewReportSocket();
  }

  while (m_receive_socket != INVALID_SOCKET) {
    if (reportSocket == INVALID_SOCKET) {
      reportSocket = PickNextEthernetCard();
      if (reportSocket != INVALID_SOCKET) {
        no_data_timeout = 0;
        no_spoke_timeout = 0;
      }
    }
    if (radar_addr) {
      // If we have detected a radar antenna at this address start opening more sockets.
      // We do this later for 2 reasons:
      // - Resource consumption
      // - Timing. If we start processing radar data before the rest of the system
      //           is initialized then we get ordering/race condition issues.
      if (dataSocket == INVALID_SOCKET) {
        dataSocket = GetNewDataSocket();
      }
    } else {
      if (dataSocket != INVALID_SOCKET) {
        closesocket(dataSocket);
        dataSocket = INVALID_SOCKET;
      }
    }

    struct timeval tv = {(long)0, (long)(MILLIS_PER_SELECT * 1000)};

    fd_set fdin;
    FD_ZERO(&fdin);

    int maxFd = INVALID_SOCKET;
    if (m_receive_socket != INVALID_SOCKET) {
      FD_SET(m_receive_socket, &fdin);
      maxFd = MAX(m_receive_socket, maxFd);
    }
    if (reportSocket != INVALID_SOCKET) {
      FD_SET(reportSocket, &fdin);
      maxFd = MAX(reportSocket, maxFd);
    }
    if (dataSocket != INVALID_SOCKET) {
      FD_SET(dataSocket, &fdin);
      maxFd = MAX(dataSocket, maxFd);
    }

    r = select(maxFd + 1, &fdin, 0, 0, &tv);

    if (r > 0) {
      if (m_receive_socket != INVALID_SOCKET && FD_ISSET(m_receive_socket, &fdin)) {
        rx_len = sizeof(rx_addr);
        r = recvfrom(m_receive_socket, (char *)data, sizeof(data), 0, (struct sockaddr *)&rx_addr, &rx_len);
        if (r > 0) {
          LOG_VERBOSE(wxT("radar_pi: %s received stop instruction"), m_ri->m_name.c_str());
          break;
        }
      }

      if (dataSocket != INVALID_SOCKET && FD_ISSET(dataSocket, &fdin)) {
        rx_len = sizeof(rx_addr);
        r = recvfrom(dataSocket, (char *)data, sizeof(data), 0, (struct sockaddr *)&rx_addr, &rx_len);
        if (r > 0) {
          ProcessFrame(data, r);
          no_data_timeout = -15;
          no_spoke_timeout = -5;
        } else {
          closesocket(dataSocket);
          dataSocket = INVALID_SOCKET;
          wxLogError(wxT("radar_pi: %s illegal frame"), m_ri->m_name.c_str());
        }
      }

      if (reportSocket != INVALID_SOCKET && FD_ISSET(reportSocket, &fdin)) {
        rx_len = sizeof(rx_addr);
        r = recvfrom(reportSocket, (char *)data, sizeof(data), 0, (struct sockaddr *)&rx_addr, &rx_len);
        if (r > 0) {
          NetworkAddress radar_address;
          radar_address.addr = rx_addr.ipv4.sin_addr;
          radar_address.port = rx_addr.ipv4.sin_port;

          if (ProcessReport(data, r)) {
            if (!radar_addr) {
              wxCriticalSectionLocker lock(m_lock);
              m_ri->DetectedRadar(m_interface_addr, radar_address);  // enables transmit data

              // the dataSocket is opened in the next loop

              radarFoundAddr = rx_addr.ipv4;
              radar_addr = &radarFoundAddr;
              m_addr = FormatNetworkAddress(radar_address);

              if (m_ri->m_state.GetValue() == RADAR_OFF) {
                LOG_INFO(wxT("radar_pi: %s detected at %s"), m_ri->m_name.c_str(), m_addr.c_str());
                m_ri->m_state.Update(RADAR_STANDBY);
              }
            }
            no_data_timeout = SECONDS_SELECT(-15);
          }
        } else {
          wxLogError(wxT("radar_pi: %s illegal report"), m_ri->m_name.c_str());
          closesocket(reportSocket);
          reportSocket = INVALID_SOCKET;
        }
      }

    } else {  // no data received -> select timeout

      if (no_data_timeout >= SECONDS_SELECT(2)) {
        no_data_timeout = 0;
        if (reportSocket != INVALID_SOCKET) {
          closesocket(reportSocket);
          reportSocket = INVALID_SOCKET;
          m_ri->m_state.Update(RADAR_OFF);
          CLEAR_STRUCT(m_interface_addr);
          radar_addr = 0;
        }
      } else {
        no_data_timeout++;
      }

      if (no_spoke_timeout >= SECONDS_SELECT(2)) {
        no_spoke_timeout = 0;
        m_ri->ResetRadarImage();
      } else {
        no_spoke_timeout++;
      }
    }

    if (reportSocket == INVALID_SOCKET) {
      // If we closed the reportSocket then close the command and data socket
      if (dataSocket != INVALID_SOCKET) {
        closesocket(dataSocket);
        dataSocket = INVALID_SOCKET;
      }
    }

  }  // endless loop until thread destroy

  if (dataSocket != INVALID_SOCKET) {
    closesocket(dataSocket);
  }
  if (reportSocket != INVALID_SOCKET) {
    closesocket(reportSocket);
  }
  if (m_send_socket != INVALID_SOCKET) {
    closesocket(m_send_socket);
    m_send_socket = INVALID_SOCKET;
  }
  if (m_receive_socket != INVALID_SOCKET) {
    closesocket(m_receive_socket);
  }

  if (m_interface_array) {
    freeifaddrs(m_interface_array);
  }

#ifdef TEST_THREAD_RACES
  LOG_VERBOSE(wxT("radar_pi: %s receive thread sleeping"), m_ri->m_name.c_str());
  wxMilliSleep(1000);
#endif
  LOG_VERBOSE(wxT("radar_pi: %s receive thread stopping"), m_ri->m_name.c_str());
  m_is_shutdown = true;
  return 0;
}

/*
 RADAR REPORTS

 The radars send various reports. The first 2 bytes indicate what the report type is.
 The types seen on a BR24 are:

 2nd byte C4:   01 02 03 04 05 07 08
 2nd byte F5:   08 0C 0D 0F 10 11 12 13 14

 Not definitive list for
 4G radars only send the C4 data.

*/

//
// The following is the received radar state. It sends this regularly
// but especially after something sends it a state change.
//
#pragma pack(push, 1)

struct RadarReport_01C4_18 {  // 01 C4 with length 18
  UINT8 what;                 // 0   0x01
  UINT8 command;              // 1   0xC4
  UINT8 radar_status;         // 2
  UINT8 field3;               // 3
  UINT8 field4;               // 4
  UINT8 field5;               // 5
  UINT16 field6;              // 6-7
  UINT16 field8;              // 8-9
  UINT16 field10;             // 10-11
};

struct RadarReport_02C4_99 {     // length 99
  UINT8 what;                    // 0   0x02
  UINT8 command;                 // 1 0xC4
  UINT32 range;                  //  2-3   0x06 0x09
  UINT16 field4;                 // 6-7    0
  UINT32 field8;                 // 8-11   1
  UINT8 gain;                    // 12
  UINT8 sea_auto;                // 13  0 = off, 1 = harbour, 2 = offshore
  UINT8 field14;                 // 14
  UINT16 field15;                // 15-16
  UINT32 sea;                    // 17-20   sea state (17)
  UINT8 field21;                 // 21
  UINT8 rain;                    // 22   rain clutter
  UINT8 field23;                 // 23
  UINT32 field24;                // 24-27
  UINT32 field28;                // 28-31
  UINT8 field32;                 // 32
  UINT8 field33;                 // 33
  UINT8 interference_rejection;  // 34
  UINT8 field35;                 // 35
  UINT8 field36;                 // 36
  UINT8 field37;                 // 37
  UINT8 target_expansion;        // 38
  UINT8 field39;                 // 39
  UINT8 field40;                 // 40
  UINT8 field41;                 // 41
  UINT8 target_boost;            // 42
};

struct RadarReport_03C4_129 {
  UINT8 what;
  UINT8 command;
  UINT8 radar_type;  // I hope! 01 = 4G, 08 = 3G, 0F = BR24
  UINT8 u00[55];     // Lots of unknown
  UINT16 firmware_date[16];
  UINT16 firmware_time[16];
  UINT8 u01[7];
};

struct RadarReport_04C4_66 {  // 04 C4 with length 66
  UINT8 what;                 // 0   0x04
  UINT8 command;              // 1   0xC4
  UINT32 field2;              // 2-5
  UINT16 bearing_alignment;   // 6-7
  UINT16 field8;              // 8-9
  UINT16 antenna_height;      // 10-11
};

struct RadarReport_08C4_18 {           // 08 c4  length 18
  UINT8 what;                          // 0  0x08
  UINT8 command;                       // 1  0xC4
  UINT8 field2;                        // 2
  UINT8 local_interference_rejection;  // 3
  UINT8 scan_speed;                    // 4
  UINT8 sls_auto;                      // 5 installation: sidelobe suppression auto
  UINT8 field6;                        // 6
  UINT8 field7;                        // 7
  UINT8 field8;                        // 8
  UINT8 side_lobe_suppression;         // 9 installation: sidelobe suppression
  UINT16 field10;                      // 10-11
  UINT8 noise_rejection;               // 12    noise rejection
  UINT8 target_sep;                    // 13
};
#pragma pack(pop)

static void AppendChar16String(wxString &dest, UINT16 *src) {
  for (; *src; src++) {
    wchar_t wc = (wchar_t)*src;
    dest << wc;
  }
}

bool NavicoReceive::ProcessReport(const UINT8 *report, int len) {
  LOG_BINARY_RECEIVE(wxT("ProcessReport"), report, len);

  time_t now = time(0);

  m_ri->m_radar_timeout = now + WATCHDOG_TIMEOUT;

#ifdef TODO
  if (m_ri->m_radar == 1) {
    if (m_ri->m_radar_type != RT_4G) {
      //   LOG_INFO(wxT("radar_pi: Radar report from 2nd radar tells us this a Navico 4G"));
      m_ri->m_radar_type = RT_4G;
      m_pi->m_pMessageBox->SetRadarType(RT_4G);
    }
  }
#endif

  if (report[1] == 0xC4) {
    // Looks like a radar report. Is it a known one?
    switch ((len << 8) + report[0]) {
      case (18 << 8) + 0x01: {  //  length 18, 01 C4
        RadarReport_01C4_18 *s = (RadarReport_01C4_18 *)report;
        // Radar status in byte 2
        if (s->radar_status != m_radar_status) {
          m_radar_status = s->radar_status;
          wxString stat;

          switch (m_radar_status) {
            case 0x01:
              m_ri->m_state.Update(RADAR_STANDBY);
              LOG_VERBOSE(wxT("radar_pi: %s reports status STANDBY"), m_ri->m_name.c_str());
              stat = _("Standby");
              break;
            case 0x02:
              m_ri->m_state.Update(RADAR_TRANSMIT);
              LOG_VERBOSE(wxT("radar_pi: %s reports status TRANSMIT"), m_ri->m_name.c_str());
              stat = _("Transmit");
              break;
            case 0x05:
              m_ri->m_state.Update(RADAR_WAKING_UP);
              m_ri->m_data_timeout = now + DATA_TIMEOUT;
              LOG_VERBOSE(wxT("radar_pi: %s reports status WAKING UP"), m_ri->m_name.c_str());
              stat = _("Waking up");
              break;
            default:
              LOG_BINARY_RECEIVE(wxT("received unknown radar status"), report, len);
              stat = _("Unknown status");
              break;
          }
          SetStatus(wxString::Format(wxT("%s IP %s %s"), m_ri->m_name.c_str(), stat.c_str()));
        }
        break;
      }

      case (99 << 8) + 0x02: {  // length 99, 02 C4
        RadarReport_02C4_99 *s = (RadarReport_02C4_99 *)report;
        if (s->field8 == 1) {
          m_ri->m_gain.Update(AUTO_RANGE - 1);  // auto gain
        } else {
          m_ri->m_gain.Update(s->gain * 100 / 255);
        }
        m_ri->m_rain.Update(s->rain * 100 / 255);
        if (s->sea_auto > 0) {
          m_ri->m_sea.Update(AUTO_RANGE - s->sea_auto);
        } else {
          m_ri->m_sea.Update(s->sea * 100 / 255);
        }
        m_ri->m_target_boost.Update(s->target_boost);
        m_ri->m_interference_rejection.Update(s->interference_rejection);
        m_ri->m_target_expansion.Update(s->target_expansion);
        m_ri->m_range.Update(s->range / 10);

        LOG_RECEIVE(wxT("radar_pi: %s state range=%u gain=%u sea=%u rain=%u if_rejection=%u tgt_boost=%u tgt_expansion=%u"),
                    m_ri->m_name.c_str(), s->range, s->gain, s->sea, s->rain, s->interference_rejection, s->target_boost,
                    s->target_expansion);
        break;
      }

      case (129 << 8) + 0x03: {  // 129 bytes starting with 03 C4
        RadarReport_03C4_129 *s = (RadarReport_03C4_129 *)report;
        LOG_RECEIVE(wxT("radar_pi: %s RadarReport_03C4_129 radar_type=%u"), m_ri->m_name.c_str(), s->radar_type);

#ifdef TODO
        switch (s->radar_type) {
          case 0x0f:
            if (m_ri->m_radar_type == RT_UNKNOWN) {
              LOG_INFO(wxT("radar_pi: Radar report tells us this a Navico BR24"));
              m_ri->m_radar_type = RT_BR24;
              m_pi->m_pMessageBox->SetRadarType(RT_BR24);
            }
            break;
          case 0x08:
            if (m_ri->m_radar_type == RT_UNKNOWN || m_ri->m_radar_type == RT_BR24) {
              LOG_INFO(wxT("radar_pi: Radar report tells us this a Navico 3G"));
              m_ri->m_radar_type = RT_3G;
              m_pi->m_pMessageBox->SetRadarType(RT_3G);
            }
            break;
          case 0x01:
            if (m_ri->m_radar_type == RT_UNKNOWN) {
              LOG_INFO(wxT("radar_pi: Radar report tells us this a Navico 4G"));
              m_ri->m_radar_type = RT_4G;
              m_pi->m_pMessageBox->SetRadarType(RT_4G);
            }
            break;
          default:
            LOG_INFO(wxT("radar_pi: Unknown radar_type %u"), s->radar_type);
            return false;
        }
#endif

        wxString ts;

        ts << wxT("Firmware date: ");
        AppendChar16String(ts, s->firmware_date);
        ts << wxT(" ");
        AppendChar16String(ts, s->firmware_time);

        m_pi->m_pMessageBox->SetRadarBuildInfo(ts);

        break;
      }

      case (66 << 8) + 0x04: {  // 66 bytes starting with 04 C4
        if (m_pi->m_settings.verbose >= 2) {
          LOG_BINARY_RECEIVE(wxT("received RadarReport_04C4_66"), report, len);
        }
        RadarReport_04C4_66 *data = (RadarReport_04C4_66 *)report;

        // bearing alignment
        int ba = (int)data->bearing_alignment / 10;
        if (ba > 180) {
          ba = ba - 360;
        }
        m_ri->m_bearing_alignment.Update(ba);

        // antenna height
        m_ri->m_antenna_height.Update(data->antenna_height / 1000);
        break;
      }

#ifdef TODO
      case (564 << 8) + 0x05: {  // length 564, 05 C4
        // Content unknown, but we know that BR24 radomes send this
        LOG_RECEIVE(wxT("received familiar BR24 report"), report, len);

        if (m_ri->m_radar_type == RT_UNKNOWN) {
          LOG_INFO(wxT("radar_pi: Radar report tells us this a Navico BR24"));
          m_ri->m_radar_type = RT_BR24;
          m_pi->m_pMessageBox->SetRadarType(RT_BR24);
        }
        break;
      }
#endif

      case (18 << 8) + 0x08: {  // length 18, 08 C4
        // contains scan speed, noise rejection and target_separation and sidelobe suppression
        RadarReport_08C4_18 *s08 = (RadarReport_08C4_18 *)report;

        LOG_BINARY_RECEIVE(wxString::Format(wxT("scanspeed= %d, noise = %u target_sep %u"), s08->scan_speed, s08->noise_rejection,
                                            s08->target_sep),
                           report, len);
        m_ri->m_scan_speed.Update(s08->scan_speed);
        m_ri->m_noise_rejection.Update(s08->noise_rejection);
        m_ri->m_target_separation.Update(s08->target_sep);
        if (s08->sls_auto == 1) {
          m_ri->m_side_lobe_suppression.Update(AUTO_RANGE - 1);
        } else {
          m_ri->m_side_lobe_suppression.Update(s08->side_lobe_suppression * 100 / 255);
        }
        m_ri->m_local_interference_rejection.Update(s08->local_interference_rejection);

        if (m_pi->m_settings.verbose >= 2) {
          LOG_BINARY_RECEIVE(wxT("received RadarReport_08C4_18"), report, len);
        }
        break;
      }
      default: {
        if (m_pi->m_settings.verbose >= 2) {
          LOG_BINARY_RECEIVE(wxT("received unknown report"), report, len);
        }
        break;
      }
    }
    return true;
  } else if (report[1] == 0xF5) {
#ifdef TODO
    // Looks like a radar report. Is it a known one?
    switch ((len << 8) + report[0]) {
      case (16 << 8) + 0x0f:
        if (m_pi->m_settings.verbose >= 2) {
          LOG_BINARY_RECEIVE(wxT("received BR24 report"), report, len);
        }
        if (m_ri->m_radar_type == RT_UNKNOWN) {
          LOG_INFO(wxT("radar_pi: Radar report tells us this a Navico BR24"));
          m_ri->m_radar_type = RT_BR24;
          m_pi->m_pMessageBox->SetRadarType(RT_BR24);
        }

        break;

      case (8 << 8) + 0x10:
      case (10 << 8) + 0x12:
      case (46 << 8) + 0x13:
        // Content unknown, but we know that BR24 radomes send this
        if (m_pi->m_settings.verbose >= 2) {
          LOG_BINARY_RECEIVE(wxT("received familiar report"), report, len);
        }
        break;

      default:
        if (m_pi->m_settings.verbose >= 2) {
          LOG_BINARY_RECEIVE(wxT("received unknown report"), report, len);
        }
        break;
    }
#endif
    return true;
  }

  if (m_pi->m_settings.verbose >= 2) {
    LOG_BINARY_RECEIVE(wxT("received unknown message"), report, len);
  }
  return false;
}

// Called from the main thread to stop this thread.
// We send a simple one byte message to the thread so that it awakens from the select() call with
// this message ready for it to be read on 'm_receive_socket'. See the constructor in NavicoReceive.h
// for the setup of these two sockets.

void NavicoReceive::Shutdown() {
  if (m_send_socket != INVALID_SOCKET) {
    m_shutdown_time_requested = wxGetUTCTimeMillis();
    if (send(m_send_socket, "!", 1, MSG_DONTROUTE) > 0) {
      LOG_VERBOSE(wxT("radar_pi: %s requested receive thread to stop"), m_ri->m_name.c_str());
      return;
    }
  }
  LOG_INFO(wxT("radar_pi: %s receive thread will take long time to stop"), m_ri->m_name.c_str());
}

wxString NavicoReceive::GetStatus() {
  wxCriticalSectionLocker lock(m_lock);
  // Called on the UI thread, so be gentle

  return m_status;
}

PLUGIN_END_NAMESPACE
