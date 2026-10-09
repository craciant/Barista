# Dedicated Wi-Fi appliance mode

Barista can reserve one physical Wi-Fi adapter exclusively for Wii U GamePad
sessions. It does not blacklist the driver or disable NetworkManager globally.

## Setup

1. Use another adapter or Ethernet for your host connection.
2. Install the Barista service and the `barista-appliance-session` wrapper.
3. In **Settings → General**, select the dedicated adapter and enable
   **Appliance mode**. Authorize the request when polkit asks.
4. Verify `nmcli device status`: the dedicated adapter should be unmanaged.
5. Start/pair normally.

A root service creates **only**
`/etc/NetworkManager/conf.d/90-barista-appliance.conf` (marked as
Barista-owned), matching the adapter's **permanent hardware MAC**.
The reservation survives reboot and renaming of the interface. NetworkManager
is instructed not to reconnect it. The original session workflow is retained
for non-appliance sessions.

At session start, a root-owned wrapper moves the selected wireless PHY into
a temporary, dedicated Linux network namespace. The Barista radio engine and
its hostapd child use that namespace. Its Wii U-side addressing therefore
does not alter the host's routing or address assignments.

At session end, the wrapper returns the PHY to the initial network namespace
and removes the temporary namespace. NetworkManager *continues* to leave the
adapter alone. Stopping a session is **not** the same as undoing appliance mode.

**Undo:** Stop your session, uncheck Appliance mode, and authorize the change.
This removes only Barista's own policy and reloads NetworkManager. Barista
refuses to replace or remove unrelated configuration files.

## Recovery

Unexpected termination may leave `barista-<PID>` namespaces. Normally the
wrapper's TERM/INT traps restore the PHY; SIGKILL or a severe crash does not
run shell traps. Before manually cleaning up, stop Barista and inspect:

```sh
sudo ip netns list
sudo ip netns exec barista-<PID> iw dev
sudo ip netns exec barista-<PID> iw phy phy<N> set netns 1
sudo ip netns delete barista-<PID>
```

Use the **actual** namespace and PHY values printed by the preceding
commands. Do not remove a namespace while it still contains the radio.
After a reboot, transient namespaces disappear, but persistent NetworkManager
reservation remains until deliberately undone.

## Limitations

- Appliance mode reserves **one** adapter; it does not claim a second adapter.
- Physical cards sharing a PHY cannot be independently reserved.
- Regulatory domain constraints still apply, especially for Intel devices.
- Configuring a radio does not automatically start a GamePad session.
- A namespace is network isolation, not a separate kernel or a defense against
  privileged processes on the same host.
