/**
@file	 LocationInfo.cpp
@brief   Contains the infrastructure for APRS based EchoLink status updates
@author  Adi/DL1HRC and Steve/DH1DM
@date	 2009-03-12

\verbatim
SvxLink - A Multi Purpose Voice Services System for Ham Radio Use
Copyright (C) 2003-2015 Tobias Blomberg / SM0SVX

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
\endverbatim
*/



/****************************************************************************
 *
 * System Includes
 *
 ****************************************************************************/

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <limits>
#include <string>
#include <sys/time.h>
#include <regex.h>
#include <iomanip>

/****************************************************************************
 *
 * Project Includes
 *
 ****************************************************************************/

#include <AsyncConfig.h>
#include <AsyncTimer.h>


/****************************************************************************
 *
 * Local Includes
 *
 ****************************************************************************/

#include "version/SVXLINK.h"
#include "LocationInfo.h"
#include "AprsTcpClient.h"
#include "AprsUdpClient.h"


/****************************************************************************
 *
 * Namespaces to use
 *
 ****************************************************************************/

using namespace std;
using namespace Async;
using namespace EchoLink;


  // Put all locally defined types, functions and variables in an anonymous
  // namespace to make them file local. The "static" keyword has been
  // deprecated in C++ so it should not be used.
namespace
{

/****************************************************************************
 *
 * Defines & typedefs
 *
 ****************************************************************************/


/****************************************************************************
 *
 * Prototypes for local functions
 *
 ****************************************************************************/

void print_error(const string &name, const string &variable,
                 const string &value, const string &example = "");


/****************************************************************************
 *
 * Local Global Variables
 *
 ****************************************************************************/



/****************************************************************************
 *
 * Local class definitions
 *
 ****************************************************************************/


} // End of anonymous namespace

/****************************************************************************
 *
 * Exported Global Variables
 *
 ****************************************************************************/

# define M_PI           3.14159265358979323846  /* pi */
# define RADIUS         6378.16

/****************************************************************************
 *
 * Public member functions
 *
 ****************************************************************************/

LocationInfo* LocationInfo::_instance = 0;

bool LocationInfo::initialize(const Async::Config &cfg, const std::string &cfg_name)
{

   // check if already initialized
  if (LocationInfo::_instance->has_instance()) return false;

  bool init_ok = true;
  string value;

  LocationInfo::_instance = new LocationInfo();

  value = cfg.getValue(cfg_name, "CALLSIGN");

  if (value.find("EL-") != string::npos)
  {
    LocationInfo::_instance->loc_cfg.prefix = "L";
  }
  else if (value.find("ER-") != string::npos)
  {
    LocationInfo::_instance->loc_cfg.prefix = "R";
  }
  else
  {
    cerr << "*** ERROR: variable CALLSIGN must have a prefix (ER- or EL-)" <<
         " to indicate that is an Echolink station.\n" <<
         "Example: CALLSIGN=ER-DL1ABC\n";
    return false;
  }

  if (value.erase(0,3).length() < 4)
  {
    cerr << "*** ERROR: variable CALLSIGN in section " <<
        cfg_name << " is missing or wrong\nExample: CALLSIGN=ER-DL1ABC\n";
    return false;
  }

  LocationInfo::_instance->loc_cfg.mycall  = value;
  LocationInfo::_instance->loc_cfg.comment = cfg.getValue(cfg_name, "COMMENT");

  if(!cfg.getValue(cfg_name, "NMEA_DEVICE", value))
  {
    init_ok &= LocationInfo::_instance->parsePosition(cfg, cfg_name);
  }
  else
  {
    init_ok &=  LocationInfo::_instance->initNmeaDev(cfg, cfg_name);
  }

  init_ok &= LocationInfo::_instance->parseStationHW(cfg, cfg_name);
  init_ok &= LocationInfo::_instance->parsePath(cfg, cfg_name);
  init_ok &= LocationInfo::_instance->parseClients(cfg, cfg_name);

  unsigned int iv = atoi(cfg.getValue(cfg_name, "STATISTICS_INTERVAL").c_str());

  if (iv < 5 || iv > 60)
  {
    iv = 10;
  }
  LocationInfo::_instance->sinterval = iv;
  LocationInfo::_instance->startStatisticsTimer(iv*60000);

  value = cfg.getValue(cfg_name, "PTY_PATH");
  LocationInfo::_instance->initExtPty(value);

  if( !init_ok )
  {
    delete LocationInfo::_instance;
    LocationInfo::_instance = NULL;
  }

  return init_ok;

} /* LocationInfo::initialize */


void LocationInfo::updateDirectoryStatus(StationData::Status status)
{
  ClientList::const_iterator it;
  for (it = clients.begin(); it != clients.end(); it++)
  {
    (*it)->updateDirectoryStatus(status);
  }
} /* LocationInfo::updateDirectoryStatus */


void LocationInfo::updateQsoStatus(int action, const string& call,
                                   const string& info, list<string>& call_list)
{
  ClientList::const_iterator it;
  for (it = clients.begin(); it != clients.end(); it++)
  {
    (*it)->updateQsoStatus(action, call, info, call_list);
  }
} /* LocationInfo::updateQsoStatus */


void LocationInfo::update3rdState(const string& call, const string& info)
{
  ClientList::const_iterator it;
  for (it = clients.begin(); it != clients.end(); it++)
  {
    (*it)->update3rdState(call, info);
  }
} /* LocationInfo::update3rdState */


void LocationInfo::igateMessage(const std::string& info)
{
  ClientList::const_iterator it;
  for (it = clients.begin(); it != clients.end(); it++)
  {
    (*it)->igateMessage(info);
  }
} /* LocationInfo::igateMessage */


string LocationInfo::getCallsign(void)
{
  return loc_cfg.mycall;
} /* LocationInfo::getCallsign */


bool LocationInfo::getTransmitting(const std::string &name)
{
   return aprs_stats[name].tx_on;
} /* LocationInfo::getTransmitting */


void LocationInfo::setTransmitting(const std::string &name, struct timeval tv,
                                     bool state)
{
   aprs_stats[name].tx_on = state;
   if (state)
   {
      aprs_stats[name].tx_on_nr++;
      aprs_stats[name].last_tx_sec = tv;
   }
   else
   {
      aprs_stats[name].tx_sec += ((tv.tv_sec -
                aprs_stats[name].last_tx_sec.tv_sec) +
            (tv.tv_usec - aprs_stats[name].last_tx_sec.tv_usec)/1000000.0);
   }
} /* LocationInfo::isTransmitting */


void LocationInfo::setReceiving(const std::string &name, struct timeval tv,
                                 bool state)
{
   aprs_stats[name].squelch_on = state;
   if (state)
   {
      aprs_stats[name].rx_on_nr++;
      aprs_stats[name].last_rx_sec = tv;
   }
   else
   {
      aprs_stats[name].rx_sec += ((tv.tv_sec -
                aprs_stats[name].last_rx_sec.tv_sec) +
           (tv.tv_usec - aprs_stats[name].last_rx_sec.tv_usec)/1000000.0);
   }
} /* LocationInfo::isReceiving */

/****************************************************************************
 *
 * Protected member functions
 *
 ****************************************************************************/



/****************************************************************************
 *
 * Private member functions
 *
 ****************************************************************************/

bool LocationInfo::parsePosition(const Config &cfg, const string &name)
{
  bool success = true;

  string pos_str(cfg.getValue(name,"LAT_POSITION"));
  if (!parseLatitude(loc_cfg.lat_pos, pos_str)) // weshalb wird loc_cfg benötigt (und nicht cfg??)
  {
    print_error(name, "LAT_POSITION", pos_str, "LAT_POSITION=51.20.10N");
    success = false;
  }

  pos_str = cfg.getValue(name,"LON_POSITION");
  if (!parseLongitude(loc_cfg.lon_pos, pos_str))
  {
    print_error(name, "LON_POSITION", pos_str, "LON_POSITION=12.10.30E");
    success = false;
  }

  return success;

} /* LocationInfo::parsePosition */


bool LocationInfo::parseLatitude(Coordinate &pos, const string &value)
{
  unsigned int deg, min, sec;
  char dir, sep[2];
  stringstream ss(value);

  ss >> deg >> sep[0] >> min >> sep[1] >> sec >> dir >> ws;

  if (ss.fail() || !ss.eof())
  {
    return false;
  }
  if (sep[0] != '.'|| sep[1] != '.')
  {
    return false;
  }
  if ((deg > 90) || (min > 59) || (sec > 59) ||
      ((deg == 90) && ((min > 0) || (sec > 0))))
  {
    return false;
  }
  if ((dir != 'N') && (dir != 'S'))
  {
    return false;
  }

  pos.deg = deg;
  pos.min = min;
  pos.sec = sec;
  pos.dir = dir;

  return true;

} /* LocationInfo::parseLatitude */


bool LocationInfo::parseLongitude(Coordinate &pos, const string &value)
{
  unsigned int deg, min, sec;
  char dir, sep[2];
  stringstream ss(value);

  ss >> deg >> sep[0] >> min >> sep[1] >> sec >> dir >> ws;

  if (ss.fail() || !ss.eof())
  {
    return false;
  }
  if (sep[0] != '.'|| sep[1] != '.')
  {
    return false;
  }
  if ((deg > 180) || (min > 59) || (sec > 59) ||
      ((deg == 180) && ((min > 0) || (sec > 0))))
  {
    return false;
  }
  if ((dir != 'E') && (dir != 'W'))
  {
    return false;
  }

  pos.deg = deg;
  pos.min = min;
  pos.sec = sec;
  pos.dir = dir;

  return true;

} /* LocationInfo::parseLongitude */


bool LocationInfo::parseStationHW(const Async::Config &cfg, const string &name)
{
  float frequency = 0;
  bool success = true;

  if (!cfg.getValue(name, "FREQUENCY", frequency))
  {
    print_error(name, "FREQUENCY", cfg.getValue(name, "FREQUENCY"),
                "FREQUENCY=438.875");
    success = false;
  }
  else
  {
    loc_cfg.frequency = lrintf(1000.0 * frequency);
  }

  if (!cfg.getValue(name, "TX_POWER", 1U, numeric_limits<unsigned int>::max(),
		    loc_cfg.power))
  {
    print_error(name, "TX_POWER", cfg.getValue(name, "TX_POWER"),
                "TX_POWER=8");
    success = false;
  }

  if (!cfg.getValue(name, "ANTENNA_GAIN", loc_cfg.gain, true))
  {
    print_error(name, "ANTENNA_GAIN", cfg.getValue(name, "ANTENNA_GAIN"),
                "ANTENNA_GAIN=6");
    success = false;
  }

  if (!parseAntennaHeight(loc_cfg, cfg.getValue(name, "ANTENNA_HEIGHT")))
  {
    print_error(name, "ANTENNA_HEIGHT", cfg.getValue(name, "ANTENNA_HEIGHT"),
                "ANTENNA_HEIGHT=10m");
    success = false;
  }

  if (!cfg.getValue(name, "ANTENNA_DIR", loc_cfg.beam_dir, true))
  {
    print_error(name, "ANTENNA_DIR", cfg.getValue(name, "ANTENNA_DIR"),
                "ANTENNA_DIR=-1");
    success = false;
  }

  if (!cfg.getValue(name, "TONE", loc_cfg.tone, true))
  {
    print_error(name, "TONE", cfg.getValue(name, "TONE"), "TONE=0");
    success = false;
  }

  int interval = 10;
  int max = numeric_limits<int>::max();
  if (!cfg.getValue(name, "BEACON_INTERVAL", 10, max, interval, true))
  {
    print_error(name, "BEACON_INTERVAL", cfg.getValue(name, "BEACON_INTERVAL"),
                "BEACON_INTERVAL=10");
    success = false;
  }
  else
  {
    loc_cfg.interval = 60 * 1000 * interval;
  }

  loc_cfg.range = calculateRange(loc_cfg);

  return success;

} /* LocationInfo::parseStationHW */


bool LocationInfo::parsePath(const Async::Config &cfg, const string &name)
{
    // FIXME: Verify the path syntax!
  loc_cfg.path = cfg.getValue(name, "PATH");
  return true;
} /* LocationInfo::parsePath */


int LocationInfo::calculateRange(const Cfg &cfg)
{
  float range_factor(1.0);

  if (cfg.range_unit == 'k')
  {
    range_factor = 1.60934;
  }

  double tmp = sqrt(2.0 * loc_cfg.height * sqrt((loc_cfg.power / 10.0) *
                        pow(10, loc_cfg.gain / 10.0) / 2)) * range_factor;

  return lrintf(tmp);

} /* LocationInfo::calculateRange */


bool LocationInfo::parseAntennaHeight(Cfg &cfg, const std::string value)
{
  char unit;
  unsigned int height;

  if (value.empty())
  {
    return true;
  }

  stringstream ss(value);
  ss >> height >> unit >> ws;
  if (ss.fail() || !ss.eof())
  {
    return false;
  }
  cfg.height = height;

  if (unit == 'm' || unit == 'M')
  {
      // Convert metric antenna height into feet
    cfg.height = lrintf(loc_cfg.height * 3.2808);
    cfg.range_unit = 'k';
  }

  return true;

} /* LocationInfo::parseAntennaHeight */


bool LocationInfo::parseClients(const Async::Config &cfg, const string &name)
{
  string aprs_server_list(cfg.getValue(name, "APRS_SERVER_LIST"));
  stringstream clientStream(aprs_server_list);
  string client, host;
  int port;
  bool success = true;

  while (clientStream >> client)
  {
    if (!parseClientStr(host, port, client))
    {
      print_error(name, "APRS_SERVER_LIST", aprs_server_list,
                  "APRS_SERVER_LIST=euro.aprs2.net:14580");
      success = false;
    }
    else
    {
      AprsTcpClient *client = new AprsTcpClient(loc_cfg, host, port);
      clients.push_back(client);
    }
  }

  clientStream.clear();
  string status_server_list(cfg.getValue(name, "STATUS_SERVER_LIST"));
  clientStream.str(status_server_list);
  while (clientStream >> client)
  {
    if (!parseClientStr(host, port, client))
    {
      print_error(name, "STATUS_SERVER_LIST", status_server_list,
                  "STATUS_SERVER_LIST=aprs.echolink.org:5199");
      success = false;
    }
    else
    {
      AprsUdpClient *client = new AprsUdpClient(loc_cfg, host, port);
      clients.push_back(client);
    }
  }

  return success;

} /* LocationInfo::parseClients */


bool LocationInfo::parseClientStr(string &host, int &port, const string &val)
{
  if (val.empty())
  {
    return false;
  }

  int tmpPort;
  string::size_type hostEnd;

  hostEnd = val.find_last_of(':');
  if (hostEnd == string::npos)
  {
    return false;
  }

  string portStr(val.substr(hostEnd+1, string::npos));
  stringstream ssval(portStr);
  ssval >> tmpPort;
  if (!ssval)
  {
    return false;
  }

  if (tmpPort < 0 || tmpPort > 0xffff)
  {
    return false;
  }

  host = val.substr(0, hostEnd);
  port = tmpPort;

  return true;

} /* LocationInfo::parseClientStr */


void LocationInfo::startStatisticsTimer(int interval) {
  delete aprs_stats_timer;
  aprs_stats_timer = new Timer(interval, Timer::TYPE_PERIODIC);
  aprs_stats_timer->setEnable(true);
  aprs_stats_timer->expired.connect(mem_fun(*this, &LocationInfo::sendAprsStatistics));
} /* LocationInfo::statStatisticsTimer */


void LocationInfo::sendAprsStatistics(Timer *t)
{
  char info[255];
  info[0] ='\0';
  string head ="UNIT.RX Erlang,TX Erlang,RXcount/10m,TXcount/10m,none1,STxxxxxx,logic";

  sprintf(info, "E%s-%s>RXTLM-1,TCPIP,qAR,%s::E%s-%-6s:%s\n",
       loc_cfg.prefix.c_str(), loc_cfg.mycall.c_str(), loc_cfg.mycall.c_str(),
       loc_cfg.prefix.c_str(), loc_cfg.mycall.c_str(), head.c_str());

   // sends the Aprs stats header
  string message = info;
  igateMessage(message);

  struct timeval tv;
  gettimeofday(&tv, NULL);

   // loop for each logic
  std::map<string, AprsStatistics>::iterator it;

  for (it = LocationInfo::instance()->aprs_stats.begin();
           it != LocationInfo::instance()->aprs_stats.end(); it++)
  {

    if ((*it).second.squelch_on)
    {
      (*it).second.rx_sec += ((tv.tv_sec - (*it).second.last_rx_sec.tv_sec) +
                             (tv.tv_usec - (*it).second.last_rx_sec.tv_usec)/1000000.0);
    }
    if ((*it).second.tx_on)
    {
      (*it).second.tx_sec += ((tv.tv_sec - (*it).second.last_tx_sec.tv_sec) +
                             (tv.tv_usec - (*it).second.last_tx_sec.tv_usec)/1000000.0 );
    }

    info[0] = '\0';
    sprintf(info,
     "E%s-%s>RXTLM-1,TCPIP,qAR,%s:T#%03d,%3.2f,%3.2f,%d,%d,0.0,%d%d000000,%s\n",
      loc_cfg.prefix.c_str(), loc_cfg.mycall.c_str(), loc_cfg.mycall.c_str(),
      sequence, (*it).second.rx_sec/(60*sinterval),
      (*it).second.tx_sec/(60*sinterval), (*it).second.rx_on_nr,
      (*it).second.tx_on_nr, ((*it).second.squelch_on ? 1 : 0),
      ((*it).second.tx_on ? 1 : 0), (*it).first.c_str());

    // sends the Aprs stats information
    message = info;
    //cout << message;
    igateMessage(message);

     // reset statistics if needed
    (*it).second.reset();

    if ((*it).second.squelch_on)
    {
      (*it).second.rx_on_nr = 1;
      (*it).second.last_rx_sec = tv;
    }

    if ((*it).second.tx_on)
    {
      (*it).second.tx_on_nr = 1;
      (*it).second.last_tx_sec = tv;
    }

    if (++sequence > 999)
    {
      sequence = 0;
    }
  }
} /* LocationInfo::sendAprsStatistics */


void LocationInfo::initExtPty(std::string ptydevice)
{
  AprsPty *aprspty = new AprsPty();
  if (!aprspty->initialize(ptydevice))
  {
     cout << "*** ERROR: initializing aprs pty device " << ptydevice << endl;
  }
  else
  {
     aprspty->messageReceived.connect(mem_fun(*this,
                    &LocationInfo::mesReceived));
  }
} /* LocationInfo::initExtPty */


void LocationInfo::mesReceived(std::string message)
{
  string loc_call = LocationInfo::getCallsign();
  size_t found = message.find("XXXXXX");

  if (found != std::string::npos)
  {
    message.replace(found, 6, loc_call);
  }

  igateMessage(message);
  cout << message << endl;
} /* LocationInfo::mesReceived */

/****************************************************************************
 *
 * Private local functions
 *
 ****************************************************************************/

void LocationInfo::onNmeaReceived(char *buf, int count)
{
  nmeastream += buf;
  size_t found;
  while ((found = nmeastream.find("\n")) != string::npos)
  {
     if (found != 0)
    {
      handleNmea(nmeastream.substr(0, found));
    }
    nmeastream.erase(0, found+1);
  }
} /* LocationInfo::onNmeaReceived */


void LocationInfo::handleNmea(std::string message)
{
  //                $GPGLL,5119.48737,N,01201.09963,E,171526.00,A,A*6B
  std::string reg = "GPGLL,[0-9]{3,}.[0-9]{2,},[NS],[0-9]{2,}.[0-9]{2,},[EW]";
  if (rmatch(message, reg))
  {
    getNextStr(message);
    std::string lat = getNextStr(message);
    std::string ns = getNextStr(message);
    std::string lon = getNextStr(message);
    std::string ew = getNextStr(message);

    loc_cfg.lat_pos.deg = atoi(lat.substr(0,2).c_str());
    loc_cfg.lat_pos.min = atoi(lat.substr(2,2).c_str());
    loc_cfg.lat_pos.sec = 60*atoi(lat.substr(5,4).c_str())/10000;
    loc_cfg.lat_pos.dir = ns[0];

    float lat_dec = loc_cfg.lat_pos.deg + atof(lat.substr(2,8).c_str())/60.0;
    if (loc_cfg.lat_pos.dir == 'S') lat_dec *= -1.0;

    loc_cfg.lon_pos.deg = atoi(lon.substr(0,3).c_str());
    loc_cfg.lon_pos.min = atoi(lon.substr(3,2).c_str());
    loc_cfg.lon_pos.sec = 60*atoi(lon.substr(6,4).c_str())/10000;
    loc_cfg.lon_pos.dir = ew[0];

    float lon_dec = loc_cfg.lon_pos.deg + atof(lon.substr(3,8).c_str())/60.0;
    if (loc_cfg.lon_pos.dir == 'W') lon_dec *= -1.0;

    if (stored_lat == 0.0) stored_lat = lat_dec;
    if (stored_lon == 0.0) stored_lon = lon_dec;

    float dist = calcDistance(lat_dec, lon_dec, stored_lat, stored_lon);
    if (dist > 0.5)
    {
      stored_lat = lat_dec;
      stored_lon = lon_dec;
      ClientList::const_iterator it;
      for (it = clients.begin(); it != clients.end(); it++)
      {
        (*it)->sendBeacon();
      }
    }

/*    cout << "Dist:" << std::fixed << std::setprecision(6) << dist << "," << "lat-dec:" << lat_dec << ", Lon-dec:" << lon_dec << " - " << 
    loc_cfg.lat_pos.deg << "." << loc_cfg.lat_pos.min << "." << loc_cfg.lat_pos.sec << "." << loc_cfg.lat_pos.dir << " - " <<
    loc_cfg.lon_pos.deg << "." << loc_cfg.lon_pos.min << "." << loc_cfg.lon_pos.sec << "." << loc_cfg.lon_pos.dir << endl;
*/
  }
} /* LocationInfo::handleNmea */


float LocationInfo::calcDistance(float lat1, float lon1, float lat2, float lon2)
{
  double dlon = M_PI * (lon2 - lon1) / 180.0;
  double dlat = M_PI * (lat2 - lat1) / 180.0;
  double a = (sin(dlat / 2) * sin(dlat / 2)) + cos(M_PI*lat1/180.0) *
              cos(M_PI*lat2/180) * (sin(dlon / 2) * sin(dlon / 2));
  double angle = 2 * atan2(sqrt(a), sqrt(1 - a));
  return static_cast<float>(static_cast<int>(angle * RADIUS * 100.))/100.;
} /* LocationInfo::calcDistance */


std::string LocationInfo::getNextStr(std::string& h)
{
  size_t f;
  std::string t = h.substr(0, f = h.find(','));
  if (f != std::string::npos)
  {
    h.erase(0, f + 1);
  }
  return t;
} /* LocationInfo::getNextStr */


bool LocationInfo::initNmeaDev(const Config &cfg, const std::string &name)
{
  std::string value = cfg.getValue(name, "NMEA_DEVICE");

  nmeadev = new Serial(value);
  if (!nmeadev->open(true))
  {
    cerr << "*** ERROR: Opening serial port NMEA_DEVICE="
         << value << endl;
    return false;
  }
  else
  {
    int baudrate = atoi(cfg.getValue(name, "NMEA_BAUD").c_str());
    if (baudrate != 2400 && baudrate != 4800 && baudrate != 9600)
    {
      cout << "+++ Setting default baudrate 4800Bd for " 
           << "/NMEA_BAUD." << endl;
      baudrate = 4800; 
    }
    nmeadev->setParams(baudrate, Serial::PARITY_NONE, 8, 1, Serial::FLOW_NONE);
    nmeadev->charactersReceived.connect(
    	  mem_fun(*this, &LocationInfo::onNmeaReceived));
  }
  return true;
} /* LocationInfo::initExtPty */

// needed by regex
bool LocationInfo::rmatch(std::string tok, std::string pattern)
{
  regex_t re;
  int status = regcomp(&re, pattern.c_str(), REG_EXTENDED);
  if (status != 0)
  {
    return false;
  }

  bool success = (regexec(&re, tok.c_str(), 0, NULL, 0) == 0);
  regfree(&re);
  return success;

} /* rmatch */


  // Put all locally defined functions in an anonymous namespace to make them
  // file local. The "static" keyword has been deprecated in C++ so it
  // should not be used.
namespace
{

void print_error(const string &name, const string &variable,
                 const string &value, const string &example)
{
  cerr << "*** ERROR: Config variable [" << name << "]/" << variable << "="
       << value << " wrong or not set.";

  if (!example.empty())
  {
    cerr << "\n*** Example: " <<  example;
  }
  cerr << endl;
} /* print_error */

} // End of anonymous namespace


/*
 * This file has not been truncated
 */
