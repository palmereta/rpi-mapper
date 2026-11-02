#!/bin/sh
# Instala o actualiza el mapper en la Pi. Correr desde la carpeta copiada:  sudo sh instalar.sh
# Idempotente: compila, copia, y reinicia los servicios. No toca el estado ni los archivos subidos.
set -e
cd "$(dirname "$0")"
id mapper >/dev/null 2>&1 || useradd --system --home-dir /var/lib/mapper --shell /usr/sbin/nologin \
  --groups video,render,audio mapper
make -C src >/dev/null
install -d -o mapper -g mapper /var/lib/mapper /var/lib/mapper/media /var/lib/mapper/originales /var/lib/mapper/miniaturas
install -d /opt/mapper/bin /opt/mapper/web
install -m 755 src/mapper /opt/mapper/bin/mapper
install -m 644 web/server.py web/index.html web/app.js web/app.css /opt/mapper/web/
install -m 644 system/mapper.service system/mapper-web.service /etc/systemd/system/
install -m 644 system/mapper.tmpfiles /etc/tmpfiles.d/mapper.conf
install -m 440 system/mapper.sudoers /etc/sudoers.d/mapper
visudo -cf /etc/sudoers.d/mapper >/dev/null
systemd-tmpfiles --create /etc/tmpfiles.d/mapper.conf
chown -R mapper:mapper /var/lib/mapper /run/mapper
systemctl daemon-reload
systemctl enable mapper mapper-web >/dev/null 2>&1
systemctl restart mapper mapper-web
echo "mapper instalado: http://$(hostname).local/  (o http://$(hostname -I | cut -d' ' -f1)/)"
