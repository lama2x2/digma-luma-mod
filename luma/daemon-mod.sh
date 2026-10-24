# Вставки Luma-мода в штатный запускатель /opt/ylc-ebook/bin/ylc-daemon.sh.
# Сам запускатель — файл Digma, в репозитории его нет: build_firmware.py берёт его
# из официального update.img (или с книги для живого теста) и вписывает эти куски.
# @setup — перед основным циклом «while true; do»
# @run   — вместо строки запуска «/opt/ylc-ebook/bin/ylc-ebook» в цикле

# @setup
# ---- Luma mod ----
LUMA_DIR=/userdata/luma
LUMA_LIB=/opt/luma/libluma.so
mkdir -p "$LUMA_DIR" 2>/dev/null

# свой скрипт пользователя: один раз при старте, в фоне, не дольше 20 секунд
if [ -f "$LUMA_DIR/autorun.sh" ] && [ ! -f "$LUMA_DIR/disable" ]; then
    ( timeout 20 sh "$LUMA_DIR/autorun.sh" > "$LUMA_DIR/autorun.log" 2>&1 ) &
fi

luma_fails=0
uptime_s() { cut -d. -f1 /proc/uptime; }

# @run
    # мод выключен файлом disable или программа трижды подряд падала в первые 30 секунд
    if [ -f "$LUMA_DIR/disable" ] || [ $luma_fails -ge 3 ] || [ ! -f "$LUMA_LIB" ]; then
        /opt/ylc-ebook/bin/ylc-ebook
    else
        started=$(uptime_s)
        LD_PRELOAD=$LUMA_LIB /opt/ylc-ebook/bin/ylc-ebook
        rc=$?
        if [ $(( $(uptime_s) - started )) -lt 30 ]; then
            luma_fails=$((luma_fails + 1))
            echo "$(date '+%F %T') ylc-ebook с модом завершилась за <30 с (код $rc), подряд: $luma_fails" >> "$LUMA_DIR/crash.log"
            [ $luma_fails -ge 3 ] && echo "$(date '+%F %T') мод отключён до перезагрузки" >> "$LUMA_DIR/crash.log"
        else
            luma_fails=0
        fi
    fi
