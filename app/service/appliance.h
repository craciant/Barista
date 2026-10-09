#pragma once
#include <QString>

namespace barista::appliance {

// Only Barista's own configuration is read or modified. A renamed interface
// continues to match through its permanent hardware MAC, not its Linux name.
struct Radio {
    QString interfaceName;
    QString permanentMac;
};

Radio ConfiguredRadio();
QString PermanentMac(const QString& interfaceName);
bool Matches(const QString& interfaceName);
QString Enable(const QString& interfaceName);
QString Disable();
}
