#include "map_url.h"
#include <cstdio>
#include <cstring>

std::string geoapifyStaticUrl(double lat, double lon, int zoom, int w, int h, const std::string& key) {
  // Format coordinates with 6 decimal places, then strip trailing zeros
  char lon_str[32];
  snprintf(lon_str, sizeof(lon_str), "%.6f", lon);
  // Strip trailing zeros
  int len = strlen(lon_str);
  while (len > 0 && lon_str[len-1] == '0') len--;
  if (len > 0 && lon_str[len-1] == '.') len--;
  lon_str[len] = '\0';

  char lat_str[32];
  snprintf(lat_str, sizeof(lat_str), "%.6f", lat);
  // Strip trailing zeros
  len = strlen(lat_str);
  while (len > 0 && lat_str[len-1] == '0') len--;
  if (len > 0 && lat_str[len-1] == '.') len--;
  lat_str[len] = '\0';

  char buffer[512];
  snprintf(buffer, sizeof(buffer),
    "https://maps.geoapify.com/v1/staticmap?style=osm-bright&width=%d&height=%d&center=lonlat:%s,%s&zoom=%d&format=jpeg&marker=lonlat:%s,%s;color:%%2334e2c0;size:medium&apiKey=%s",
    w, h, lon_str, lat_str, zoom, lon_str, lat_str, key.c_str());

  return std::string(buffer);
}
