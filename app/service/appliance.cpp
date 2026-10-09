#include "appliance.h"

#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringList>

namespace barista::appliance {
namespace {
constexpr auto ConfigFile = "/etc/NetworkManager/conf.d/90-barista-appliance.conf";
constexpr auto Marker = "# Barista dedicated adapter v1";
const QRegularExpression macPattern(
    QStringLiteral("^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$"));
const QRegularExpression interfacePattern(QStringLiteral("^[A-Za-z0-9_.-]{1,15}$"));

bool RunNmcli(const QStringList& arguments, QString* output = nullptr)
{
    QProcess process;
    process.start(QStringLiteral(BARISTA_NMCLI), arguments);
    if (!process.waitForStarted(3000)) return false;
    if (!process.waitForFinished(8000)) {
        process.kill();
        process.waitForFinished(1000);
        return false;
    }
    if (output) *output = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

QString Contents(const Radio& radio)
{
    return QString::fromLatin1(Marker) + "\n"
        + "# interface=" + radio.interfaceName + "\n"
        + "[device-barista-appliance]\n"
        + "match-device=mac:" + radio.permanentMac + "\n"
        + "managed=0\n";
}

bool Save(const QString& data)
{
    QSaveFile file(QString::fromLatin1(ConfigFile));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
        QFileDevice::ReadGroup | QFileDevice::ReadOther);
    if (file.write(data.toUtf8()) != data.toUtf8().size()) return false;
    return file.commit();
}

bool IsOwnedFile()
{
    const QFileInfo file(QString::fromLatin1(ConfigFile));
    if (!file.exists() && !file.isSymLink()) return false;
    // Do not overwrite unexpected system policy or a symlink.
    if (!file.isFile() || file.isSymLink()) return false;
    QFile input(QString::fromLatin1(ConfigFile));
    return input.open(QIODevice::ReadOnly | QIODevice::Text) &&
        input.readLine().trimmed() == Marker;
}
} // namespace

Radio ConfiguredRadio()
{
    if (!IsOwnedFile()) return {};
    QFile input(QString::fromLatin1(ConfigFile));
    if (!input.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    const auto lines = QString::fromUtf8(input.readAll()).split('\n');
    if (lines.size() != 6 || lines[0] != Marker ||
        !lines[1].startsWith("# interface=") ||
        lines[2] != "[device-barista-appliance]" ||
        !lines[3].startsWith("match-device=mac:") || lines[4] != "managed=0" ||
        !lines[5].isEmpty()) return {};
    Radio radio{lines[1].mid(12), lines[3].mid(17).toLower()};
    if (!interfacePattern.match(radio.interfaceName).hasMatch() ||
        !macPattern.match(radio.permanentMac).hasMatch()) return {};
    return radio;
}

QString PermanentMac(const QString& interfaceName)
{
    if (!interfacePattern.match(interfaceName).hasMatch() ||
        !QFileInfo::exists("/sys/class/net/" + interfaceName + "/phy80211")) return {};
    QString mac;
    // NM reports the permanent address, even when a client has changed the
    // current address. Do not persist a scan-randomized / cloned address.
    if (!RunNmcli({"-g", "GENERAL.PERM-HWADDR", "device", "show", interfaceName}, &mac))
        return {};
    mac = mac.toLower();
    return macPattern.match(mac).hasMatch() ? mac : QString();
}

bool Matches(const QString& interfaceName)
{
    const auto radio = ConfiguredRadio();
    return !radio.permanentMac.isEmpty() && PermanentMac(interfaceName) == radio.permanentMac;
}

QString Enable(const QString& interfaceName)
{
    const QString mac = PermanentMac(interfaceName);
    if (mac.isEmpty()) return "Could not determine the Wi-Fi adapter's permanent MAC address";
    const QFileInfo file(QString::fromLatin1(ConfigFile));
    if ((file.exists() || file.isSymLink()) && !IsOwnedFile())
        return "Refusing to overwrite an existing NetworkManager policy";
    const auto current = ConfiguredRadio();
    if (IsOwnedFile() && current.permanentMac.isEmpty())
        return "Existing Barista appliance configuration is invalid; repair it manually";
    if (!current.permanentMac.isEmpty() && current.permanentMac != mac)
        return "Undo the existing dedicated adapter before selecting another";

    const QString oldData = current.permanentMac.isEmpty() ? QString() : Contents(current);
    if (!Save(Contents({interfaceName, mac})))
        return "Could not write NetworkManager appliance configuration";
    if (!RunNmcli({"general", "reload"}) ||
        !RunNmcli({"device", "set", interfaceName, "managed", "no"})) {
        if (oldData.isEmpty()) QFile::remove(QString::fromLatin1(ConfigFile));
        else Save(oldData);
        RunNmcli({"general", "reload"});
        return "NetworkManager did not accept appliance configuration; changes rolled back";
    }
    return {};
}

QString Disable()
{
    const QFileInfo file(QString::fromLatin1(ConfigFile));
    if (!file.exists() && !file.isSymLink()) return {};
    if (!IsOwnedFile()) return "Refusing to remove a NetworkManager policy not owned by Barista";
    const Radio radio = ConfiguredRadio();
    if (radio.permanentMac.isEmpty())
        return "Existing Barista appliance configuration is invalid; repair it manually";
    if (!QFile::remove(QString::fromLatin1(ConfigFile)))
        return "Could not remove appliance configuration";
    if (!RunNmcli({"general", "reload"})) {
        Save(Contents(radio));
        RunNmcli({"general", "reload"});
        return "NetworkManager reload failed; appliance configuration restored";
    }
    // Renamed or unplugged adapters are fine: the persistent exclusion is gone.
    if (PermanentMac(radio.interfaceName) == radio.permanentMac)
        RunNmcli({"device", "set", radio.interfaceName, "managed", "yes"});
    return {};
}

} // namespace barista::appliance
