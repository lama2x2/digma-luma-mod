#!/bin/sh
# Живой тест Luma-мода БЕЗ перепрошивки. Запускается по ADB из /dev/shm/luma-test.
# Поверх штатного запускателя программы (система только для чтения) монтируется копия
# с модом — до перезагрузки. Программа подхватит мод при следующем старте
# (после отключения кабеля); если она уже работает — перезапускаем сразу.
set -e
R=/dev/shm/luma-test
# /dev/shm смонтирован с noexec — разрешаем запуск до перезагрузки
mount -o remount,exec /dev/shm
chmod 755 $R/ylc-ebook
# пропатченную программу берём только для запуска с модом; без мода — штатная
sed -e "s|LUMA_LIB=/opt/luma/libluma.so|LUMA_LIB=$R/libluma.so|" \
    -e "/LD_PRELOAD=/s|/opt/ylc-ebook/bin/ylc-ebook|$R/ylc-ebook|" \
    $R/ylc-daemon.sh.mod > $R/ylc-daemon.sh
chmod 755 $R/ylc-daemon.sh

umount /opt/ylc-ebook/bin/ylc-daemon.sh 2>/dev/null || true
mount --bind $R/ylc-daemon.sh /opt/ylc-ebook/bin/ylc-daemon.sh

if pidof ylc-ebook >/dev/null; then
	/etc/init.d/S99YlcDaemon stop || true
	sleep 1
	/etc/init.d/S99YlcDaemon start
	echo "Программа перезапущена с модом."
else
	echo "Мод подготовлен: включится, когда программа стартует (отключи кабель)."
fi
echo "Вернуть как было: перезагрузить книгу."
