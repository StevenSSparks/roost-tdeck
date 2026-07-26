#ifndef MAP_URL_H
#define MAP_URL_H

#include <string>

std::string geoapifyStaticUrl(double lat, double lon, int zoom, int w, int h, const std::string& key);

#endif
