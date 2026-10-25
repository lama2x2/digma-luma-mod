#!/bin/bash
# Помощник для Digma Luma по ADB (кабель к компьютеру).
#   ./luma.sh check        — видно ли книгу, модель, сборка прошивки, разделы
#   ./luma.sh backup       — снять ВСЕ системные разделы книги в ./backup/<время>/ (ничего не меняет)
#   ./luma.sh test-mod     — запустить мод вживую БЕЗ перепрошивки (сбрасывается перезагрузкой)
#   ./luma.sh log          — показать журнал мода
#   ./luma.sh light C W    — временно выставить подсветку (0..255) холодную и тёплую
set -euo pipefail
cd "$(dirname "$0")"

PARTS="uboot misc vnvm waveform logo recovery boot rootfs oem"
VOL=/Volumes/Luma
# остановленной программе — только ждать: второй сигнал во время выхода роняет ядро
# (драйвер e-ink дважды освобождает буфер экрана, kernel panic -> перезагрузка)
WAIT_EXIT='for i in $(seq 50); do pidof ylc-ebook >/dev/null || break; sleep 0.2; done'

need_device() {
	adb get-state >/dev/null 2>&1 || { echo "Книга не видна. Подключи кабель, разблокируй книгу, проверь 'adb devices'."; exit 1; }
}

case "${1:-}" in
check)
	adb devices -l
	need_device
	adb shell 'echo "модель:  $(tr -d "\0" </proc/device-tree/model)"; grep RK_BUILD_INFO /etc/os-release; uname -rv; ls -l /dev/block/by-name/'
	;;
backup)
	need_device
	dir="backup/$(date +%Y%m%d-%H%M%S)"; mkdir -p "$dir"
	adb shell 'cat /etc/os-release; uname -a; tr -d "\0" </proc/device-tree/model; echo; cat /proc/cmdline' > "$dir/info.txt"
	for p in $PARTS; do
		adb shell "[ -e /dev/block/by-name/$p ]" || { echo "$p: нет такого раздела, пропускаю"; continue; }
		if [ "$p" = oem ]; then
			# oem смонтирован на запись и меняется на ходу: снимок в памяти книги
			dev_md5=$(adb shell "dd if=/dev/block/by-name/$p of=/dev/shm/snap.img bs=64k 2>/dev/null && md5sum /dev/shm/snap.img" | cut -d' ' -f1)
			adb pull /dev/shm/snap.img "$dir/$p.img" >/dev/null
			adb shell rm -f /dev/shm/snap.img
		else
			adb exec-out "dd if=/dev/block/by-name/$p bs=64k 2>/dev/null" > "$dir/$p.img"
			dev_md5=$(adb shell "md5sum /dev/block/by-name/$p" | cut -d' ' -f1)
		fi
		local_md5=$(md5 -q "$dir/$p.img")
		if [ "$local_md5" = "$dev_md5" ]; then
			echo "$p: $(wc -c <"$dir/$p.img" | tr -d ' ') байт, сверено"
		else
			echo "$p: ОШИБКА — копия не совпала с разделом, повтори backup"; exit 1
		fi
	done
	echo "Готово: $dir. Сравни сборку с официальной: grep RK_BUILD_INFO $dir/info.txt"
	;;
test-mod)
	# книга при подключении кабеля уходит в режим флешки (диск Luma), программа в это время
	# остановлена: файлы кладём через диск, модуль — в память книги, мод стартует после отключения
	need_device
	[ -f luma/build/libluma.so ] || { echo "Сначала собери модуль: ./luma/build.sh"; exit 1; }
	R=/dev/shm/luma-test
	# прошивка уже с модом: подменяем только модуль (до перезагрузки)
	if adb shell 'grep -q "Luma mod" /opt/ylc-ebook/bin/ylc-daemon.sh'; then
		# программу останавливаем: занятую подмену не снять. /dev/shm смонтирован с noexec —
		# без exec модуль молча не загрузится, и программа пойдёт без мода
		adb shell "/etc/init.d/S99YlcDaemon stop >/dev/null 2>&1; $WAIT_EXIT; mkdir -p $R
			while mountpoint -q /opt/luma/libluma.so 2>/dev/null || grep -q ' /opt/luma/libluma.so ' /proc/mounts; do umount /opt/luma/libluma.so || break; done
			mount -o remount,exec /dev/shm"
		adb push luma/build/libluma.so $R/libluma.so >/dev/null
		adb shell "mount --bind $R/libluma.so /opt/luma/libluma.so" || exit 1
		echo "Модуль подменён до перезагрузки. Запуск: ./luma.sh live"
		exit 0
	fi
	[ -d "$VOL" ] || { echo "Нет диска $VOL: включи на книге режим USB."; exit 1; }
	tmp=$(mktemp -d)
	adb exec-out "cat /opt/ylc-ebook/bin/ylc-ebook" > "$tmp/ylc-ebook.orig"
	python3 -I build_firmware.py --patch-binary "$tmp/ylc-ebook.orig" "$tmp/ylc-ebook"
	# запускатель — штатный с книги + вставки мода (если уже с модом — перезагрузи книгу)
	adb exec-out "cat /opt/ylc-ebook/bin/ylc-daemon.sh" > "$tmp/ylc-daemon.orig"
	python3 -I build_firmware.py --patch-daemon "$tmp/ylc-daemon.orig" "$tmp/ylc-daemon.sh.mod"
	mkdir -p "$VOL/luma/fonts"
	cp fonts/*.ttf "$VOL/luma/fonts/"
	mkdir -p "$VOL/luma/icons" && cp luma/icons/*.bin "$VOL/luma/icons/"
	# настройки: только если их ещё нет; шрифты в тесте лежат в памяти книги
	if [ ! -f "$VOL/luma/luma.ini" ]; then
		sed 's|/opt/luma/fonts/|/userdata/luma/fonts/|' luma/luma.ini > "$VOL/luma/luma.ini"
	fi
	adb shell "mkdir -p $R"
	adb push "$tmp/ylc-ebook" "$tmp/ylc-daemon.sh.mod" luma/build/libluma.so luma/test-run.sh $R/ >/dev/null
	rm -rf "$tmp"
	adb shell "sh $R/test-run.sh"
	dot_clean -m "$VOL" 2>/dev/null || true
	diskutil eject "$VOL" >/dev/null && echo "Диск извлечён. Отключи кабель — программа запустится с модом."
	echo "Потом включи режим USB и запусти: ./luma.sh log"
	;;
live)
	# программа работает при подключённом кабеле: диск Luma отдаётся обратно книге
	need_device
	if [ -d "$VOL" ]; then
		dot_clean -m "$VOL" 2>/dev/null || true
		diskutil eject "$VOL" >/dev/null
	fi
	adb shell 'L=$(ls -d /sys/kernel/config/usb_gadget/*/functions/mass_storage.0/lun.0); echo > $L/file
		mountpoint -q /userdata || { fsck -y /dev/mmcblk0p10 >/dev/null 2>&1; mount -t vfat /dev/mmcblk0p10 /userdata; }
		killall show_image 2>/dev/null; pidof ylc-ebook >/dev/null || setsid /etc/init.d/S99YlcDaemon start </dev/null >/dev/null 2>&1'
	echo "Программа запущена, кабель можно не отключать. Вернуть диск: ./luma.sh usb"
	;;
usb)
	# обратно в режим флешки, программа останавливается (как штатно при подключении)
	need_device
	adb shell "L=\$(ls -d /sys/kernel/config/usb_gadget/*/functions/mass_storage.0/lun.0)
		/etc/init.d/S99YlcDaemon stop; $WAIT_EXIT; sync
		umount /userdata && echo /dev/mmcblk0p10 > \$L/file"
	;;
restart)
	# перезапуск программы в живом режиме (после замены модуля или настроек)
	need_device
	# книга сама переподключается по USB и отдаёт диск Маку — тогда сначала ./luma.sh live
	adb shell 'mountpoint -q /userdata' || { echo "Диск сейчас у Мака (режим USB) — запусти ./luma.sh live"; exit 1; }
	adb shell "/etc/init.d/S99YlcDaemon stop; $WAIT_EXIT; setsid /etc/init.d/S99YlcDaemon start </dev/null >/dev/null 2>&1"
	;;
log)
	if [ -d "$VOL/luma" ]; then
		cat "$VOL/luma/luma.log" "$VOL/luma/crash.log" 2>/dev/null | tail -n 200
	else
		need_device
		adb shell "cat /userdata/luma/luma.log /userdata/luma/crash.log 2>/dev/null | tail -n 200"
	fi
	;;
light)
	need_device
	[ $# -eq 3 ] || { echo "нужно: ./luma.sh light ХОЛОДНАЯ ТЁПЛАЯ (0..255)"; exit 1; }
	BL=/sys/class/backlight
	adb shell "echo $2 > $BL/backlight/brightness; echo $3 > $BL/backlight-warm/brightness"
	;;
*)
	sed -n '2,8p' "$0"
	;;
esac
