#pragma once
// Named configs live as plain text files in %LOCALAPPDATA%\OAR External\configs; the live settings are
// autosaved next to them in settings.cfg.
#include "settings.h"

#include <string>
#include <vector>

namespace store
{
struct Entry
{
    std::string name;
    double      modified = 0; // seconds since the Unix epoch
};

std::string Directory();
std::string Sanitize(std::string name);          // strips path characters and trims; empty if nothing is left
std::vector<Entry> List();                       // newest first
bool Save(const std::string& name, const Settings& s);
bool Load(const std::string& name, Settings& out);
bool Remove(const std::string& name);
bool Exists(const std::string& name);

bool LoadCurrent(Settings& out);                 // the autosaved live settings
void SaveCurrent(const Settings& s);

double AgeSeconds(const Entry& e);
}
