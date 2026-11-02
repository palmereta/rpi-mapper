#!/bin/sh
# Optimizacion de la RPi 3 para kiosko de video mapping. Idempotente. Correr con sudo.
# Modo HDMI fijo: el nativo del proyector (MODO=1280x720@60 sudo sh optimizar.sh para otro).
set -e
MODO=${MODO:-1024x768@60}
# 1. cloud-init ya hizo su trabajo (primer arranque de Imager): apagarlo
touch /etc/cloud/cloud-init.disabled
# 2. Sin escritorio
systemctl set-default multi-user.target
# 3. Servicios que no aportan a un kiosko
for u in lightdm bluetooth hciuart cups cups.path cups.socket cups-browsed ModemManager \
         udisks2 accounts-daemon nfs-blkmap rpcbind rpcbind.socket nfs-client.target \
         NetworkManager-wait-online glamor-test rp1-test wayvnc-control rpi-eeprom-update \
         apt-daily.timer apt-daily-upgrade.timer man-db.timer dpkg-db-backup.timer e2scrub_all.timer e2scrub_reap \
         regenerate_ssh_host_keys sshswitch triggerhappy; do
  systemctl disable --now "$u" >/dev/null 2>&1 || true
done
systemctl mask cloud-init-hotplugd.socket rpcbind.socket >/dev/null 2>&1 || true
# 4. Logs en RAM (cuida la SD), tope 32 MB
mkdir -p /etc/systemd/journald.conf.d
printf '[Journal]\nStorage=volatile\nRuntimeMaxUse=32M\n' > /etc/systemd/journald.conf.d/kiosko.conf
# 5. Watchdog de hardware: si el sistema se cuelga, reinicia solo
mkdir -p /etc/systemd/system.conf.d
printf '[Manager]\nRuntimeWatchdogSec=15\nRebootWatchdogSec=2min\n' > /etc/systemd/system.conf.d/watchdog.conf
# 6. Sin pipewire en las sesiones de usuario: el audio lo abre mpv directo por ALSA
systemctl --global mask pipewire.socket pipewire-pulse.socket wireplumber.service pipewire.service pipewire-pulse.service >/dev/null 2>&1 || true
# 6b. Locale que manda la Mac por SSH (evita el warning de setlocale)
sed -i 's/^# *\(en_AU.UTF-8 UTF-8\)/\1/; s/^# *\(es_AR.UTF-8 UTF-8\)/\1/' /etc/locale.gen
locale-gen >/dev/null
# 7. Hostname
if [ "$(hostname)" != mapper ]; then
  hostnamectl set-hostname mapper
  sed -i 's/\brpi\b/mapper/g' /etc/hosts
fi
# 8. Firmware: sin splash, sin bluetooth (libera la UART), sin autodeteccion de camara/DSI
CFG=/boot/firmware/config.txt
cp -n $CFG $CFG.orig
sed -i 's/^camera_auto_detect=1/camera_auto_detect=0/; s/^display_auto_detect=1/display_auto_detect=0/' $CFG
grep -q '^# kiosko mapper' $CFG || cat >> $CFG <<'EOT'
# kiosko mapper
disable_splash=1
dtoverlay=disable-bt
boot_delay=0
EOT
# 9. Kernel: consola fuera de la pantalla del proyector, sin logo ni cursor, sin plymouth
CMD=/boot/firmware/cmdline.txt
cp -n $CMD $CMD.orig
sed -i 's/ console=tty1/ console=tty3/; s/ splash//; s/ plymouth.ignore-serial-consoles//; s/ ds=nocloud;i=[^ ]*//' $CMD
# video=...D: HDMI siempre encendido aunque el proyector se prenda despues de la Pi;
# MODO = nativo del proyector. Mismo modo que state.json para que no resincronice dos veces
sed -i 's/ video=HDMI-A-1:[^ ]*//' $CMD
for a in logo.nologo vt.global_cursor_default=0 consoleblank=0 loglevel=3 video=HDMI-A-1:${MODO}D; do
  grep -q "$a" $CMD || sed -i "s/\$/ $a/" $CMD
done
systemctl mask plymouth-start plymouth-quit plymouth-quit-wait plymouth-read-write >/dev/null 2>&1 || true
# tty1 sin login: el proyector solo muestra el mapper
systemctl disable getty@tty1 >/dev/null 2>&1 || true
echo OPTIMIZADO
