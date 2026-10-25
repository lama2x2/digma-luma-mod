/*
 * libluma — модуль для читалки Digma Luma (ylc-ebook, RK3506).
 * Подгружается через LD_PRELOAD и перехватывает несколько функций программы:
 *
 *   snprintf             — CSS, который программа отдаёт движку MuPDF: свои шрифты
 *                          (с настоящими жирным/курсивом), интервалы, отступы, свой CSS;
 *   system               — запись яркости подсветки: своя кривая вместо «позиция × 4»,
 *                          пишется в sysfs напрямую, без запуска шелла;
 *   lv_label_set_text*   — подписи интерфейса: «Автор — Название» вместо имени файла,
 *                          переименование шрифтов в меню.
 *
 * Плюс при старте переписывает таблицу полей (6 значений) в памяти программы.
 * Все настройки — в /userdata/luma/luma.ini (видна по USB как папка luma).
 * Модуль рассчитан на конкретную сборку ylc-ebook (2026-06-26) и проверяет это сам.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dlfcn.h>
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <iconv.h>
#ifdef __linux__
#include <dirent.h>
#include <link.h>
#include <pthread.h>
#endif
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <setjmp.h>
#include <signal.h>
#include <ucontext.h>
#include <time.h>
#include <unistd.h>

#ifndef LUMA_DIR
#define LUMA_DIR "/userdata/luma"
#endif
#define INI_PATH LUMA_DIR "/luma.ini"
#define LOG_PATH LUMA_DIR "/luma.log"
#define CACHE_PATH LUMA_DIR "/titles3.tsv" /* path, автор, название, серия, номер */
#define DEFAULT_INI "/opt/luma/luma.ini"
#define CSS_FMT_FONT "@font-face {font-family: 'MyFont';src:url('%s/%s.ttf')"
#define CSS_FMT_PLAIN "@page { margin: 0; }\nhtml, body, pre { margin: 0; padding: 0; line-height: %.2f;}"
/* в системе каналы подписаны наоборот: «backlight» светит жёлтым, «backlight-warm» — белым */
#define BL_COLD "/sys/devices/platform/backlight-warm/backlight/backlight-warm/brightness"
#define BL_WARM "/sys/devices/platform/backlight/backlight/backlight/brightness"
#define BL_COLD_PW "/sys/devices/platform/backlight-warm/backlight/backlight-warm/bl_power"
#define BL_WARM_PW "/sys/devices/platform/backlight/backlight/backlight/bl_power"
/* ШИМ подсветки (Rockchip PWM v4) и гейт его тактирования регистров pclk_pwm0 в CRU */
#define PWM_COLD_BASE 0xff933000u /* backlight-warm — белый */
#define PWM_WARM_BASE 0xff932000u /* backlight — жёлтый */
#define PWM_REG_ENABLE 0x004
#define PWM_REG_PERIOD 0x010
#define PWM_REG_DUTY 0x014
#define CRU_GATE_PAGE 0xff9b0000u
#define CRU_PCLK_PWM0 0x800 /* бит 15 */
#define BATTERY "/sys/class/power_supply/cw2015-battery/capacity"
#define CHARGER "/sys/class/power_supply/sc89601-charger/online"
#define SHOT_FLAG "/dev/shm/luma-test/shot"
#define SHOT_PATH "/dev/shm/luma-test/shot.bin"
#define SLEEP_SRC LUMA_DIR "/sleep.src" /* для какой книги нарисован экран сна */

/* адреса в ylc-ebook сборки 2026-06-26 (PIE, относительно базы) */
#define MARGIN_TABLE_VA 0xb0e310u
static const int32_t stock_margins[6] = {22, 26, 30, 34, 46, 60};

/* ---------- настройки ---------- */

struct font_map {
	char slot[48];    /* имя файла встроенного шрифта без .ttf, напр. Ebrima */
	char label[64];   /* как показывать в меню */
	char prefix[256]; /* путь без -Regular.ttf */
};

static struct {
	int debug;
	/* текст */
	double line_spacing[3]; /* что подставлять вместо 1.0 / 1.5 / 2.0 */
	int has_line_spacing;
	double size;           /* множитель размера, 1.0 = как в программе */
	char indent[32];       /* красная строка, CSS-длина */
	char para_spacing[32]; /* отступ между абзацами */
	char align[16];        /* justify / left / пусто = как в книге */
	char side_margin[32];  /* доп. поля слева/справа */
	int bold;              /* 1 = основной текст жирным начертанием */
	int force;             /* 1 = перебивать стили книги (!important) */
	int margins[6];
	int has_margins;
	/* шрифты */
	struct font_map fonts[24];
	int nfonts;
	/* подсветка: значение sysfs (0..255) для позиций ползунка 1..10 */
	int light_cold[11], light_warm[11];
	int has_light_cold, has_light_warm;
	/* mix: один ползунок — общая яркость (кривая brightness), другой — доля своего цвета */
	int light_mix;
	int light_total[11];
	int bright_cold; /* 1 = яркость на холодном ползунке, 0 = на тёплом */
	/* ticks: кривая brightness_ticks в тактах ШИМ (~10.7 нс) — тоньше штатной ступени (≈9 тактов) */
	int light_ticks;
	int light_tk[11];
	int pwm_period; /* тактов на период; штатно 2344 (40 кГц), больше — тоньше минимум */
	/* библиотека */
	char title_format[128]; /* {author} {title} {series} {num} {file}, [...] — необязательная часть */
	char author_style[16];  /* full / last / initials / last_initials */
	int titles_enabled;
	int max_author, max_title, max_series; /* обрезка в символах, 0 = без обрезки */
	double pulse_floor; /* сколько тактов «добавляет» растянутый драйвером импульс */
	int max_len;            /* вся подпись в списке, символов; 0 = без ограничения */
	char reader_format[128]; /* заголовок в читалке */
	int max_reader;
	int tile_width; /* подпись уже стольких пикселей — плитка: полназвания и фамилия */
	int tile_char10; /* средняя ширина буквы на плитке ×10, px */
	/* интерфейс */
	int menu_values;     /* в меню текста показывать настоящие интервалы */
	int status_battery;  /* «18:56 · 94%» */
	int status_progress; /* «5/816 · 1%» */
	/* кнопки и текст */
	int buttons_swap;   /* 1 = верхние кнопки листают вперёд (штатно — назад) */
	int buttons_toggle; /* 1 = две кнопки разом переключают раскладку */
	int text_global;    /* 1 = шрифт, размер, интервал и поля одни на все книги */
	int sleep_cover;    /* 1 = на экране сна обложка открытой книги */
	int thumbs;         /* 1 = миниатюры в меню из обложки книги, а не с экрана */
	int sort_books;     /* 1 = сортировка библиотеки по настоящим названиям и авторам */
	int text_anchor;    /* 1 = после смены размера, интервала, полей — страница с тем же текстом */
	int text_panel;     /* 1 = пункт меню «Размер шрифта» открывает панель «Текст» с − / + */
	char hide_fonts[512]; /* шрифты, которых нет в меню «Тип шрифта», через запятую */
} cfg;

static void logf_(const char *fmt, ...)
{
	if (!cfg.debug)
		return;
	/* журнал не растёт бесконечно: больше 512 КБ — в luma.old.log, пишем заново */
	static int writes;
	struct stat st;
	if (writes++ % 200 == 0 && stat(LOG_PATH, &st) == 0 && st.st_size > 512 * 1024)
		rename(LOG_PATH, LUMA_DIR "/luma.old.log");
	FILE *f = fopen(LOG_PATH, "a");
	if (!f)
		return;
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	fprintf(f, "%02d:%02d:%02d ", tm.tm_hour, tm.tm_min, tm.tm_sec);
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fclose(f);
}

static char *trim(char *s)
{
	while (isspace((unsigned char)*s))
		s++;
	char *e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = 0;
	return s;
}

static void copy(char *dst, size_t n, const char *src)
{
	snprintf(dst, n, "%s", src);
}

static int parse_ints(const char *s, int *out, int max)
{
	int n = 0;
	while (*s && n < max) {
		char *end;
		long v = strtol(s, &end, 10);
		if (end == s)
			break;
		out[n++] = (int)v;
		s = end;
		while (*s == ',' || isspace((unsigned char)*s))
			s++;
	}
	return n;
}

static void ini_set(const char *sec, const char *key, char *val)
{
	if (!strcmp(sec, "general")) {
		if (!strcmp(key, "debug"))
			cfg.debug = atoi(val);
	} else if (!strcmp(sec, "text")) {
		if (!strcmp(key, "global")) {
			cfg.text_global = atoi(val);
		} else if (!strcmp(key, "hide_fonts")) {
			copy(cfg.hide_fonts, sizeof cfg.hide_fonts, val);
		} else if (!strcmp(key, "panel")) {
			cfg.text_panel = atoi(val);
		} else if (!strcmp(key, "keep_place")) {
			cfg.text_anchor = atoi(val);
		} else if (!strcmp(key, "line_spacing")) {
			double a, b, c;
			if (sscanf(val, "%lf , %lf , %lf", &a, &b, &c) == 3) {
				cfg.line_spacing[0] = a;
				cfg.line_spacing[1] = b;
				cfg.line_spacing[2] = c;
				cfg.has_line_spacing = 1;
			}
		} else if (!strcmp(key, "size"))
			cfg.size = atof(val);
		else if (!strcmp(key, "indent"))
			copy(cfg.indent, sizeof cfg.indent, val);
		else if (!strcmp(key, "paragraph_spacing"))
			copy(cfg.para_spacing, sizeof cfg.para_spacing, val);
		else if (!strcmp(key, "align"))
			copy(cfg.align, sizeof cfg.align, val);
		else if (!strcmp(key, "side_margin"))
			copy(cfg.side_margin, sizeof cfg.side_margin, val);
		else if (!strcmp(key, "bold"))
			cfg.bold = atoi(val);
		else if (!strcmp(key, "override_book_styles"))
			cfg.force = atoi(val);
		else if (!strcmp(key, "margins"))
			cfg.has_margins = parse_ints(val, cfg.margins, 6) == 6;
	} else if (!strcmp(sec, "fonts")) {
		/* Ebrima = PT Serif | /userdata/luma/fonts/PTSerif */
		if (cfg.nfonts >= (int)(sizeof cfg.fonts / sizeof cfg.fonts[0]))
			return;
		char *bar = strchr(val, '|');
		if (!bar)
			return;
		*bar = 0;
		struct font_map *m = &cfg.fonts[cfg.nfonts++];
		copy(m->slot, sizeof m->slot, key);
		copy(m->label, sizeof m->label, trim(val));
		copy(m->prefix, sizeof m->prefix, trim(bar + 1));
	} else if (!strcmp(sec, "light")) {
		int v[10];
		if (!strcmp(key, "cold") && parse_ints(val, v, 10) == 10) {
			memcpy(cfg.light_cold + 1, v, sizeof v);
			cfg.has_light_cold = 1;
		} else if (!strcmp(key, "warm") && parse_ints(val, v, 10) == 10) {
			memcpy(cfg.light_warm + 1, v, sizeof v);
			cfg.has_light_warm = 1;
		} else if (!strcmp(key, "brightness") && parse_ints(val, v, 10) == 10) {
			memcpy(cfg.light_total + 1, v, sizeof v);

		} else if (!strcmp(key, "brightness_ticks") && parse_ints(val, v, 10) == 10) {
			memcpy(cfg.light_tk + 1, v, sizeof v);
			cfg.light_ticks = 1;
		} else if (!strcmp(key, "pulse_floor") && atof(val) >= 0) {
			cfg.pulse_floor = atof(val);
		} else if (!strcmp(key, "pwm_period")) {
			cfg.pwm_period = atoi(val);
		} else if (!strcmp(key, "mode")) {
			cfg.light_mix = !strcmp(val, "mix");
		} else if (!strcmp(key, "brightness_slider")) {
			cfg.bright_cold = !strcmp(val, "cold");
		}
	} else if (!strcmp(sec, "library")) {
		if (!strcmp(key, "thumbnails"))
			cfg.thumbs = atoi(val);
		if (!strcmp(key, "sort"))
			cfg.sort_books = atoi(val);
		if (!strcmp(key, "title_format"))
			copy(cfg.title_format, sizeof cfg.title_format, val);
		else if (!strcmp(key, "author_style"))
			copy(cfg.author_style, sizeof cfg.author_style, val);
		else if (!strcmp(key, "show_titles"))
			cfg.titles_enabled = atoi(val);
		else if (!strcmp(key, "max_author"))
			cfg.max_author = atoi(val);
		else if (!strcmp(key, "max_title"))
			cfg.max_title = atoi(val);
		else if (!strcmp(key, "max_series"))
			cfg.max_series = atoi(val);
		else if (!strcmp(key, "max_len"))
			cfg.max_len = atoi(val);
		else if (!strcmp(key, "reader_format"))
			copy(cfg.reader_format, sizeof cfg.reader_format, val);
		else if (!strcmp(key, "max_reader"))
			cfg.max_reader = atoi(val);
		else if (!strcmp(key, "tile_width"))
			cfg.tile_width = atoi(val);
		else if (!strcmp(key, "tile_char_width") && atof(val) > 0)
			cfg.tile_char10 = (int)(atof(val) * 10);
	} else if (!strcmp(sec, "sleep")) {
		if (!strcmp(key, "cover"))
			cfg.sleep_cover = atoi(val);
	} else if (!strcmp(sec, "buttons")) {
		if (!strcmp(key, "swap"))
			cfg.buttons_swap = atoi(val);
		else if (!strcmp(key, "toggle"))
			cfg.buttons_toggle = atoi(val);
	} else if (!strcmp(sec, "interface")) {
		if (!strcmp(key, "menu_values"))
			cfg.menu_values = atoi(val);
		else if (!strcmp(key, "battery_percent"))
			cfg.status_battery = atoi(val);
		else if (!strcmp(key, "progress_percent"))
			cfg.status_progress = atoi(val);
	}
}

static void load_ini(void)
{
	memset(&cfg, 0, sizeof cfg);
	cfg.size = 1.0;
	cfg.titles_enabled = 1;
	copy(cfg.title_format, sizeof cfg.title_format, "{author} — [({series}[ {num}]) ]{title}");
	copy(cfg.author_style, sizeof cfg.author_style, "last_initials");
	copy(cfg.reader_format, sizeof cfg.reader_format, "{title}");
	cfg.max_reader = 30;
	cfg.tile_width = 300;
	cfg.pwm_period = 2344;
	cfg.pulse_floor = 6;
	cfg.tile_char10 = 125;
	cfg.menu_values = cfg.status_battery = cfg.status_progress = 1;
	cfg.buttons_swap = cfg.buttons_toggle = cfg.text_global = cfg.sleep_cover = cfg.thumbs = cfg.sort_books = 1;
	cfg.text_anchor = 1;
	cfg.text_panel = 1;
	/* без кириллицы и почти одинаковые с оставшимися (Arial ≈ Roboto, Tahoma ≈ Verdana,
	   Noto Sans и Malgun ≈ Open Sans, Corbel ≈ Calibri) */
	copy(cfg.hide_fonts, sizeof cfg.hide_fonts, "OpenDyslexicAlta, OpenDyslexicMono, Arial, Tahoma, Noto Sans, Malgun Gothic, Corbel");
	static const int total[11] = {0, 1, 2, 3, 5, 8, 12, 18, 27, 40, 60};
	memcpy(cfg.light_total, total, sizeof total);

	FILE *f = fopen(INI_PATH, "r");
	if (!f) {
		/* первый запуск: кладём шаблон настроек в память книги */
		mkdir(LUMA_DIR, 0755);
		mkdir(LUMA_DIR "/fonts", 0755);
		FILE *src = fopen(DEFAULT_INI, "r"), *dst = fopen(INI_PATH, "w");
		if (src && dst) {
			char buf[4096];
			size_t n;
			while ((n = fread(buf, 1, sizeof buf, src)) > 0)
				fwrite(buf, 1, n, dst);
		}
		if (src)
			fclose(src);
		if (dst)
			fclose(dst);
		f = fopen(INI_PATH, "r");
		if (!f)
			return;
	}
	char line[512], sec[32] = "general";
	while (fgets(line, sizeof line, f)) {
		char *s = trim(line);
		if (!*s || *s == '#' || *s == ';')
			continue;
		if (*s == '[') {
			char *e = strchr(s, ']');
			if (e) {
				*e = 0;
				copy(sec, sizeof sec, trim(s + 1));
			}
			continue;
		}
		char *eq = strchr(s, '=');
		if (!eq)
			continue;
		*eq = 0;
		ini_set(sec, trim(s), trim(eq + 1));
	}
	fclose(f);
}

/* ---------- поля: таблица в памяти программы ---------- */

#ifdef __linux__
static int find_main_base(struct dl_phdr_info *info, size_t size, void *data)
{
	(void)size;
	if (info->dlpi_name == NULL || info->dlpi_name[0] == 0) {
		*(uintptr_t *)data = info->dlpi_addr;
		return 1;
	}
	return 0;
}

#endif

static void patch_margins(void)
{
	if (!cfg.has_margins)
		return;
#ifndef __linux__
	return;
#else
	uintptr_t base = 0;
	dl_iterate_phdr(find_main_base, &base);
	int32_t *tbl = (int32_t *)(base + MARGIN_TABLE_VA);
	if (memcmp(tbl, stock_margins, sizeof stock_margins) != 0) {
		logf_("поля: таблица не совпала со стоковой — другая версия программы, пропускаю");
		return;
	}
	long pg = sysconf(_SC_PAGESIZE);
	uintptr_t start = (uintptr_t)tbl & ~(uintptr_t)(pg - 1);
	if (mprotect((void *)start, pg * 2, PROT_READ | PROT_WRITE) != 0) {
		logf_("поля: mprotect: %s", strerror(errno));
		return;
	}
	for (int i = 0; i < 6; i++)
		tbl[i] = cfg.margins[i];
	mprotect((void *)start, pg * 2, PROT_READ);
	logf_("поля: %d %d %d %d %d %d", tbl[0], tbl[1], tbl[2], tbl[3], tbl[4], tbl[5]);
#endif
}

/* ---------- CSS ---------- */

static int (*real_vsnprintf)(char *, size_t, const char *, va_list);
static int (*real_system)(const char *);
static void (*real_label_set_text)(void *, const char *);
static void (*real_label_set_text_static)(void *, const char *);
static struct {
	void *(*get_screen)(const void *);
	int (*is_valid)(const void *);
	int (*get_width)(const void *);
	int32_t (*style_prop)(const void *, uint32_t part, uint32_t prop);
	void *(*parent)(const void *);
	int32_t (*text_width)(const char *, uint32_t, const void *font, int32_t letter_space);
	int32_t (*get_x)(const void *);
	const char *(*label_text)(const void *);
} lv;

/* ширина подписи, заданная программой в пикселях (до раскладки get_width даёт 0);
   0 = неизвестно, проценты или «по содержимому» */
static int label_width(void *obj)
{
	int w = lv.get_width ? lv.get_width(obj) : 0;
	if (w > 0 || !lv.style_prop)
		return w;
	/* подпись «по содержимому», родитель 100% — ищем ближайшего предка с шириной в пикселях */
	for (int i = 0; obj && i < 6; i++, obj = lv.parent ? lv.parent(obj) : NULL) {
		int32_t sw = lv.style_prop(obj, 0, 1 /* LV_STYLE_WIDTH */);
		if (sw > 0 && sw < (1 << 29))
			return sw;
	}
	return 0;
}

static void resolve(void)
{
	if (real_vsnprintf)
		return;
	real_vsnprintf = dlsym(RTLD_NEXT, "vsnprintf");
	real_system = dlsym(RTLD_NEXT, "system");
	real_label_set_text = dlsym(RTLD_NEXT, "lv_label_set_text");
	real_label_set_text_static = dlsym(RTLD_NEXT, "lv_label_set_text_static");
	lv.get_screen = dlsym(RTLD_DEFAULT, "lv_obj_get_screen");
	lv.is_valid = dlsym(RTLD_DEFAULT, "lv_obj_is_valid");
	lv.get_width = dlsym(RTLD_DEFAULT, "lv_obj_get_width");
	lv.style_prop = dlsym(RTLD_DEFAULT, "lv_obj_get_style_prop");
	lv.parent = dlsym(RTLD_DEFAULT, "lv_obj_get_parent");
	lv.text_width = dlsym(RTLD_DEFAULT, "lv_text_get_width");
	lv.get_x = dlsym(RTLD_DEFAULT, "lv_obj_get_x");
	lv.label_text = dlsym(RTLD_DEFAULT, "lv_label_get_text");
	if (!lv.get_screen)
		lv.is_valid = NULL;
}

static int file_exists(const char *p)
{
	return access(p, R_OK) == 0;
}

/* как шрифты подписаны в меню программы -> имя файла */
static const char *const font_names[][2] = {
	{"Courier New", "Cour"}, {"Franklin Gothic", "Framd"}, {"Kelson Sans", "Kelson-Sans-RU"},
	{"Malgun Gothic", "Malgun"}, {"Montserrat", "Montserrat-Light"}, {"Myriad Pro", "MyriadPro"},
	{"Noto Sans", "NotoSans"}, {"Open Sans", "OpenSans"}, {"Times New Roman", "Times"},
	{"Trebuchet", "Trebuc"},
};

static const char *slot_of_label(const char *text)
{
	for (unsigned i = 0; i < sizeof font_names / sizeof font_names[0]; i++)
		if (!strcmp(text, font_names[i][0]))
			return font_names[i][1];
	return text;
}

static const struct font_map *font_for_slot(const char *slot)
{
	for (int i = 0; i < cfg.nfonts; i++)
		if (!strcmp(cfg.fonts[i].slot, slot))
			return &cfg.fonts[i];
	return NULL;
}

struct sbuf {
	char *p;
	size_t n, len;
};

static void sb_add(struct sbuf *b, const char *fmt, ...)
{
	if (b->len >= b->n)
		return;
	va_list ap;
	va_start(ap, fmt);
	int r = real_vsnprintf(b->p + b->len, b->n - b->len, fmt, ap);
	va_end(ap);
	if (r > 0)
		b->len += (size_t)r < b->n - b->len ? (size_t)r : b->n - b->len - 1;
}

static void add_faces(struct sbuf *b, const char *prefix, int remapped)
{
	/* remapped: prefix-Regular.ttf и т.д.; встроенный: Name.ttf + Name-Bold.ttf, если есть */
	static const struct {
		const char *suffix_new, *suffix_old, *style;
	} faces[] = {
		{"-Regular", "", ""},
		{"-Italic", "-Italic", "font-style:italic;"},
		{"-Bold", "-Bold", "font-weight:bold;"},
		{"-BoldItalic", "-BoldItalic", "font-weight:bold;font-style:italic;"},
	};
	for (unsigned i = 0; i < sizeof faces / sizeof faces[0]; i++) {
		char path[320];
		const char *suffix = remapped ? faces[i].suffix_new : faces[i].suffix_old;
		int found = 0;
		for (int e = 0; e < 2 && !found; e++) {
			snprintf(path, sizeof path, "%s%s.%s", prefix, suffix, e ? "otf" : "ttf");
			found = file_exists(path);
		}
		if (!found)
			continue;
		sb_add(b, "@font-face{font-family:'MyFont';%ssrc:url('%s');}\n", faces[i].style, path);
	}
}

/* панель «Текст»: свои значения поверх luma.ini (0 / -1 = как в ini и меню программы).
   Хранятся в luma/tune.state и действуют во всех книгах */
#define TUNE_PATH LUMA_DIR "/tune.state"
static struct {
	double em;     /* размер шрифта, как в программе (пиксели), 0 = из меню программы */
	double lh;     /* интервал, 0 = из меню программы */
	double side;   /* поля по бокам, pt; -1 = из ini */
	double indent; /* красная строка, em; -1 = из ini */
	int loaded;
} tune = {0, 0, -1, -1, 0};
static float stock_em; /* что поставила программа (её размер из меню) */
static struct {
	double lh, side, indent; /* что получилось в последнем CSS — для показа на панели */
} tune_eff;
/* последний CSS, который построила программа: в тот же буфер мод пишет новый */
static struct {
	char *buf;
	size_t n;
	char dir[256], name[128];
	int has_name, valid;
	double lh_app;
} css_last;
static void *tune_item; /* подпись «Размер шрифта» в только что созданном меню читалки */

static void tune_save(void)
{
	FILE *f = fopen(TUNE_PATH ".tmp", "w");
	if (!f)
		return;
	fprintf(f, "em=%.1f\nline_height=%.2f\nside=%.0f\nindent=%.2f\n", tune.em, tune.lh, tune.side, tune.indent);
	fclose(f);
	rename(TUNE_PATH ".tmp", TUNE_PATH);
}

static void tune_load(void)
{
	if (tune.loaded)
		return;
	tune.loaded = 1;
	FILE *f = fopen(TUNE_PATH, "r");
	if (!f)
		return;
	char line[128];
	double v;
	while (fgets(line, sizeof line, f)) {
		if (sscanf(line, "em=%lf", &v) == 1)
			tune.em = v;
		else if (sscanf(line, "line_height=%lf", &v) == 1)
			tune.lh = v;
		else if (sscanf(line, "side=%lf", &v) == 1)
			tune.side = v;
		else if (sscanf(line, "indent=%lf", &v) == 1)
			tune.indent = v;
	}
	fclose(f);
}

static int build_css(char *out, size_t n, const char *dir, const char *name, double lh)
{
	struct sbuf b = {out, n, 0};
	out[0] = 0;
	if (cfg.has_line_spacing) {
		int idx = lh < 1.25 ? 0 : lh < 1.75 ? 1 : 2;
		lh = cfg.line_spacing[idx];
	}
	tune_load();
	if (tune.lh > 0)
		lh = tune.lh;
	double size = cfg.size;
	tune_eff.lh = lh;
	tune_eff.side = tune.side >= 0 ? tune.side : 0;
	tune_eff.indent = tune.indent >= 0 ? tune.indent : (cfg.indent[0] ? atof(cfg.indent) : 0);
	const char *imp = cfg.force ? " !important" : "";
	if (name) {
		const struct font_map *m = font_for_slot(name);
		if (m) {
			add_faces(&b, m->prefix, 1);
		} else {
			char prefix[320];
			snprintf(prefix, sizeof prefix, "%s/%s", dir, name);
			add_faces(&b, prefix, 0);
		}
		if (b.len == 0) /* ничего не нашли — как в оригинале */
			sb_add(&b, "@font-face {font-family: 'MyFont';src:url('%s/%s.ttf') format('truetype');}\n", dir, name);
	}
	sb_add(&b, "@page { margin: 0; }\n");
	sb_add(&b, "html, body, pre { %s margin: 0; padding: 0; line-height: %.2f;}\n",
	       name ? "font-family: 'MyFont';" : "", lh);
	sb_add(&b, "img { display: block !important; max-width: 100%%; height: auto; }\n");
	if (size > 0.3 && (size < 0.99 || size > 1.01))
		sb_add(&b, "html { font-size: %.0f%%; }\n", size * 100);
	if (tune.side > 0)
		sb_add(&b, "body { padding: 0 %.0fpt; }\n", tune.side);
	else if (tune.side < 0 && cfg.side_margin[0])
		sb_add(&b, "body { padding: 0 %s; }\n", cfg.side_margin);
	if (cfg.bold)
		sb_add(&b, "body, p { font-weight: bold%s; }\n", imp);
	if (cfg.force) {
		sb_add(&b, "p, div, li, blockquote { line-height: %.2f !important; }\n", lh);
		if (name)
			sb_add(&b, "p, div, li, span, blockquote { font-family: 'MyFont' !important; }\n");
	}
	if (tune.indent >= 0)
		sb_add(&b, "p { text-indent: %.2fem%s; }\n", tune.indent, imp);
	else if (cfg.indent[0])
		sb_add(&b, "p { text-indent: %s%s; }\n", cfg.indent, imp);
	if (cfg.para_spacing[0])
		sb_add(&b, "p { margin-top: 0%s; margin-bottom: %s%s; }\n", imp, cfg.para_spacing, imp);
	if (cfg.align[0])
		sb_add(&b, "p { text-align: %s%s; }\n", cfg.align, imp);
	/* свой CSS целиком */
	FILE *f = fopen(LUMA_DIR "/style.css", "r");
	if (f) {
		if (b.len < n - 1)
			b.len += fread(out + b.len, 1, n - 1 - b.len, f);
		out[b.len] = 0;
		fclose(f);
	}
	logf_("CSS (%zu из %zu байт):\n%s", b.len, n, out);
	return (int)b.len;
}

/* CSS строит сама программа (открытие книги, меню) — запоминаем, чтобы панель могла
   перестроить его с новыми значениями. Выбрали другой интервал в меню — свой интервал панели сбрасываем */
static void css_remember(char *str, size_t size, const char *dir, const char *name, double lh)
{
	tune_load();
	if (css_last.valid && fabs(lh - css_last.lh_app) > 0.001 && tune.lh > 0) {
		logf_("текст: в меню выбран другой интервал — свой (%.2f) сброшен", tune.lh);
		tune.lh = 0;
		tune_save();
	}
	css_last.buf = str;
	css_last.n = size;
	css_last.has_name = name != NULL;
	copy(css_last.dir, sizeof css_last.dir, dir ? dir : "");
	copy(css_last.name, sizeof css_last.name, name ? name : "");
	css_last.lh_app = lh;
	css_last.valid = 1;
}

int snprintf(char *str, size_t size, const char *fmt, ...)
{
	resolve();
	va_list ap;
	va_start(ap, fmt);
	int r;
	if (fmt && fmt[0] == '@' && str && size > 0) {
		if (!strncmp(fmt, CSS_FMT_FONT, sizeof CSS_FMT_FONT - 1)) {
			const char *dir = va_arg(ap, const char *);
			const char *name = va_arg(ap, const char *);
			double lh = va_arg(ap, double);
			va_end(ap);
			css_remember(str, size, dir, name, lh);
			return build_css(str, size, dir, name, lh);
		}
		if (!strncmp(fmt, CSS_FMT_PLAIN, sizeof CSS_FMT_PLAIN - 1)) {
			double lh = va_arg(ap, double);
			va_end(ap);
			css_remember(str, size, NULL, NULL, lh);
			return build_css(str, size, NULL, NULL, lh);
		}
	}
	r = real_vsnprintf(str, size, fmt, ap);
	va_end(ap);
	return r;
}

/* ---------- подсветка ---------- */

static int write_sysfs(const char *path, int v)
{
	int fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
	char buf[16];
	int n = snprintf(buf, sizeof buf, "%d\n", v);
	int r = write(fd, buf, n) == n ? 0 : -1;
	close(fd);
	return r;
}

/* режим mix: программа по-прежнему двигает «холодный» и «тёплый» ползунки, а мы
   считаем: ползунок яркости -> общая яркость по кривой, второй ползунок -> какая доля
   её уходит в его цвет (0 = вся в цвет ползунка яркости, 10 = вся в его цвет).
   Белый добавляется не сверху, а вместо части тёплого — яркость не растёт. */
static int lpos[2] = {-1, -1}; /* [0] холодная, [1] тёплая: позиция 0..10 */
static int lpow[2];            /* bl_power, который выставила программа (1 = выкл) */
static int lout[2] = {-1, -1}, lpw_out[2] = {-1, -1};

/* прямой доступ к регистрам ШИМ: драйвер ставит ступень, мы уточняем длительность импульса */
static volatile uint32_t *pwm_regs[2], *cru_gate;

static int pwm_map(void)
{
	static int tried, ok;
	if (tried)
		return ok;
	tried = 1;
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0)
		return 0;
	void *a = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, PWM_COLD_BASE);
	void *b = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, PWM_WARM_BASE);
	void *c = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, CRU_GATE_PAGE);
	close(fd);
	if (a == MAP_FAILED || b == MAP_FAILED || c == MAP_FAILED)
		return 0;
	pwm_regs[0] = a;
	pwm_regs[1] = b;
	cru_gate = (volatile uint32_t *)((char *)c + CRU_PCLK_PWM0);
	/* сверяемся: должен быть PWM v4 с периодом 25 мкс */
	*cru_gate = 1u << 31; /* снять гейт pclk_pwm0 (запись с маской) */
	uint32_t per = pwm_regs[0][PWM_REG_PERIOD / 4];
	ok = per > 1000 && per <= 100000 && cfg.pwm_period >= 1000 && cfg.pwm_period <= 100000;
	logf_("ШИМ: период %u тактов %s", per, ok ? "" : "— не похоже, отключаю точный режим");
	return ok;
}

static int pwm_want[2], pwm_per[2]; /* импульс и период канала, такты */

static void pwm_poke(int c)
{
	if (!pwm_want[c] || !pwm_map() || access("/dev/shm/luma-test/nopoke", F_OK) == 0)
		return;
	*cru_gate = 1u << 31;
	volatile uint32_t *r = pwm_regs[c];
	if (!(r[PWM_REG_ENABLE / 4] & 2) ||
	    (r[PWM_REG_DUTY / 4] == (uint32_t)pwm_want[c] && r[PWM_REG_PERIOD / 4] == (uint32_t)pwm_per[c]))
		return;
	r[PWM_REG_PERIOD / 4] = pwm_per[c];
	r[PWM_REG_DUTY / 4] = pwm_want[c];
	r[PWM_REG_ENABLE / 4] = (1u << 18) | (1u << 2); /* UPDATE_EN: применить */
}

static void mix_apply(void)
{
	static const char *const br[2] = {BL_COLD, BL_WARM}, *const pw[2] = {BL_COLD_PW, BL_WARM_PW};
	int b = cfg.bright_cold ? 0 : 1, t = !b;
	if (lpos[b] < 0)
		return;
	int total = lpow[b] || !lpos[b] ? 0 : cfg.light_ticks ? cfg.light_tk[lpos[b]] : cfg.light_total[lpos[b]];
	int tp = lpos[t] < 0 ? 0 : lpos[t];
	int v[2];
	v[t] = (total * tp + 5) / 10;
	if (cfg.light_ticks && total > 0) {
		/* драйвер растягивает короткий импульс: канал с d тактами светит как d + pulse_floor
		   (0 тактов — темно). Ползунок задаёт долю СВЕТА второго канала tp/10, подбираем
		   целые такты с ближайшей к ней долей — шаги на глаз ровные по всему ползунку */
		double want = tp / 10.0, best = 2;
		for (int d = 0; d <= total; d++) {
			double lt = d ? d + cfg.pulse_floor : 0, lb = d < total ? total - d + cfg.pulse_floor : 0;
			double e = fabs(lt / (lt + lb) - want);
			if (e < best - 1e-9) {
				best = e;
				v[t] = d;
			}
		}
	}
	v[b] = total - v[t];
	/* температура — только целыми тактами при общем периоде: из N тактов N+1 смесей
	   (0+N, 1+(N-1), ...); дробные доли на глаз скачут, поэтому их нет */
	int duty[2] = {0, 0}, per[2] = {cfg.pwm_period, cfg.pwm_period};
	if (cfg.light_ticks)
		for (int c = 0; c < 2; c++)
			duty[c] = v[c];
	for (int c = 0; c < 2; c++) {
		int p = v[c] > 0 ? 0 : 1;
		/* в тактах: драйверу — ближайшую ступень сверху (1 ступень ≈ 9.2 такта), остальное уточним */
		int level = cfg.light_ticks ? (v[c] * 255 + cfg.pwm_period - 1) / cfg.pwm_period : v[c];
		if (p && lpw_out[c] != p)
			write_sysfs(pw[c], lpw_out[c] = p);
		if (lout[c] != level)
			write_sysfs(br[c], lout[c] = level);
		if (!p && lpw_out[c] != p)
			write_sysfs(pw[c], lpw_out[c] = p);
		if (cfg.light_ticks) {
			pwm_want[c] = duty[c];
			pwm_per[c] = per[c];
			pwm_poke(c);
		}
	}
	if (cfg.light_ticks)
		logf_("свет: яркость %d тактов (поз. %d), доля %d/10 -> холодная %d/%d, тёплая %d/%d", total, lpos[b],
		      tp, duty[0], per[0], duty[1], per[1]);
	else
		logf_("свет: яркость %d (поз. %d), доля %d/10 -> холодная %d, тёплая %d", total, lpos[b], tp, v[0], v[1]);
}

/* отладочная печать программы: размер окна документа, поля, шрифт — в журнал мода */
int printf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	if (cfg.debug > 1 && fmt && (strstr(fmt, "margin") || strstr(fmt, "font_size") || strstr(fmt, "spacing"))) {
		char buf[300];
		va_list aq;
		va_copy(aq, ap);
		(real_vsnprintf ? real_vsnprintf : vsnprintf)(buf, sizeof buf, fmt, aq);
		va_end(aq);
		buf[strcspn(buf, "\n")] = 0;
		logf_("программа: %s", buf);
	}
	int r = vprintf(fmt, ap);
	va_end(ap);
	return r;
}

int system(const char *cmd)
{
	resolve();
	if (cfg.debug > 1 && cmd && !strstr(cmd, "/leds/work/")) /* светодиод мигает раз в 5 с */
		logf_("system: %s", cmd);
	int v;
	char path[160];
	if (cmd && cfg.light_mix && sscanf(cmd, "echo %d > %159s", &v, path) == 2) {
		int c = !strcmp(path, BL_COLD) || !strcmp(path, BL_COLD_PW) ? 0
			: !strcmp(path, BL_WARM) || !strcmp(path, BL_WARM_PW) ? 1 : -1;
		if (c >= 0) {
			if (strstr(path, "bl_power"))
				lpow[c] = v;
			else if (v >= 0 && v <= 40 && v % 4 == 0)
				lpos[c] = v / 4;
			else
				return real_system(cmd);
			mix_apply();
			return 0;
		}
	}
	if (cmd && sscanf(cmd, "echo %d > %159s", &v, path) == 2) {
		int is_cold = !strcmp(path, BL_COLD), is_warm = !strcmp(path, BL_WARM);
		if (is_cold || is_warm) {
			/* программа пишет позицию×4 (0..40) */
			int pos = v / 4, out = v;
			if (pos >= 0 && pos <= 10 && v % 4 == 0) {
				if (is_cold && cfg.has_light_cold)
					out = pos ? cfg.light_cold[pos] : 0;
				if (is_warm && cfg.has_light_warm)
					out = pos ? cfg.light_warm[pos] : 0;
			}
			if (out < 0)
				out = 0;
			if (out > 255)
				out = 255;
			logf_("подсветка %s: %d -> %d", is_cold ? "холодная" : "тёплая", v, out);
			if (write_sysfs(path, out) == 0)
				return 0;
		}
	}
	return real_system(cmd);
}

/* ---------- метаданные книг ---------- */

static struct zfuncs {
	int (*inflateInit2_)(void *, int, const char *, int);
	int (*inflate)(void *, int);
	int (*inflateEnd)(void *);
	const char *(*zlibVersion)(void);
} z;

/* минимальный z_stream для 32-битного ARM (zlib 1.2/1.3) */
typedef struct {
	const unsigned char *next_in;
	unsigned avail_in;
	unsigned long total_in;
	unsigned char *next_out;
	unsigned avail_out;
	unsigned long total_out;
	const char *msg;
	void *state;
	void *zalloc, *zfree, *opaque;
	int data_type;
	unsigned long adler, reserved;
} zstream;

static int zlib_ready(void)
{
	static int tried;
	if (z.inflate)
		return 1;
	if (tried)
		return 0;
	tried = 1;
#ifdef __linux__
	void *h = dlopen("libz.so.1", RTLD_NOW | RTLD_GLOBAL);
#else
	void *h = dlopen("libz.dylib", RTLD_NOW | RTLD_GLOBAL);
#endif
	if (!h)
		return 0;
	z.inflateInit2_ = dlsym(h, "inflateInit2_");
	z.inflate = dlsym(h, "inflate");
	z.inflateEnd = dlsym(h, "inflateEnd");
	z.zlibVersion = dlsym(h, "zlibVersion");
	return z.inflateInit2_ && z.inflate && z.inflateEnd && z.zlibVersion;
}

static uint16_t le16(const unsigned char *p) { return p[0] | p[1] << 8; }
static uint32_t le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* читает файл из zip (по точному имени или по суффиксу), не больше max байт */
static char *zip_read(FILE *f, const char *name, const char *suffix, size_t max, size_t *outlen)
{
	unsigned char tail[65557];
	if (fseek(f, 0, SEEK_END))
		return NULL;
	long fsz = ftell(f);
	long tl = fsz < (long)sizeof tail ? fsz : (long)sizeof tail;
	fseek(f, fsz - tl, SEEK_SET);
	if (fread(tail, 1, tl, f) != (size_t)tl)
		return NULL;
	long eocd = -1;
	for (long i = tl - 22; i >= 0; i--)
		if (le32(tail + i) == 0x06054b50) {
			eocd = i;
			break;
		}
	if (eocd < 0)
		return NULL;
	uint32_t cd_size = le32(tail + eocd + 12), cd_off = le32(tail + eocd + 16);
	if (cd_size > 8u << 20)
		return NULL;
	unsigned char *cd = malloc(cd_size);
	fseek(f, cd_off, SEEK_SET);
	if (!cd || fread(cd, 1, cd_size, f) != cd_size) {
		free(cd);
		return NULL;
	}
	char *result = NULL;
	for (uint32_t p = 0; p + 46 <= cd_size && le32(cd + p) == 0x02014b50;) {
		uint16_t method = le16(cd + p + 10), nlen = le16(cd + p + 28), xlen = le16(cd + p + 30), clen = le16(cd + p + 32);
		uint32_t csize = le32(cd + p + 20), lho = le32(cd + p + 42);
		const char *fn = (const char *)cd + p + 46;
		int match = name ? (nlen == strlen(name) && !memcmp(fn, name, nlen))
				 : (nlen >= strlen(suffix) && !strncasecmp(fn + nlen - strlen(suffix), suffix, strlen(suffix)));
		p += 46 + nlen + xlen + clen;
		if (!match)
			continue;
		unsigned char lh[30];
		fseek(f, lho, SEEK_SET);
		if (fread(lh, 1, 30, f) != 30 || le32(lh) != 0x04034b50)
			break;
		fseek(f, lho + 30 + le16(lh + 26) + le16(lh + 28), SEEK_SET);
		size_t want_in = csize < (4u << 20) ? csize : (4u << 20);
		unsigned char *in = malloc(want_in);
		if (!in || fread(in, 1, want_in, f) != want_in) {
			free(in);
			break;
		}
		if (method == 0) {
			size_t n = want_in < max ? want_in : max;
			result = malloc(n + 1);
			memcpy(result, in, n);
			result[n] = 0;
			*outlen = n;
		} else if (method == 8 && zlib_ready()) {
			result = malloc(max + 1);
			zstream s;
			memset(&s, 0, sizeof s);
			if (z.inflateInit2_(&s, -15, z.zlibVersion(), sizeof s) == 0) {
				s.next_in = in;
				s.avail_in = want_in;
				s.next_out = (unsigned char *)result;
				s.avail_out = max;
				z.inflate(&s, 2 /* Z_SYNC_FLUSH */);
				*outlen = max - s.avail_out;
				result[*outlen] = 0;
				z.inflateEnd(&s);
			} else {
				free(result);
				result = NULL;
			}
		}
		free(in);
		break;
	}
	free(cd);
	return result;
}

/* текст между <tag ...> и </tag>, где tag может иметь префикс вида dc: */
static int xml_text(const char *xml, const char *tag, char *out, size_t n, const char **after)
{
	char open1[64];
	snprintf(open1, sizeof open1, "<%s", tag);
	const char *p = xml;
	while ((p = strstr(p, open1))) {
		char c = p[strlen(open1)];
		if (c == '>' || c == ' ' || c == '\t' || c == '\n' || c == '\r')
			break;
		p++;
	}
	if (!p)
		return 0;
	const char *s = strchr(p, '>');
	if (!s)
		return 0;
	s++;
	char close[64];
	snprintf(close, sizeof close, "</%s>", tag);
	const char *e = strstr(s, close);
	if (!e)
		return 0;
	size_t o = 0;
	for (const char *q = s; q < e && o + 4 < n; q++) {
		if (*q == '<') { /* вложенные теги выбрасываем */
			const char *g = strchr(q, '>');
			if (!g || g > e)
				break;
			q = g;
			continue;
		}
		if (*q == '&') {
			const char *sc = strchr(q, ';');
			if (sc && sc - q < 10) {
				if (!strncmp(q, "&amp;", 5)) out[o++] = '&';
				else if (!strncmp(q, "&lt;", 4)) out[o++] = '<';
				else if (!strncmp(q, "&gt;", 4)) out[o++] = '>';
				else if (!strncmp(q, "&quot;", 6)) out[o++] = '"';
				else if (!strncmp(q, "&apos;", 6)) out[o++] = '\'';
				else if (q[1] == '#') {
					long cp = q[2] == 'x' ? strtol(q + 3, NULL, 16) : strtol(q + 2, NULL, 10);
					if (cp == 0x301) ; /* ударение — выкидываем */
					else if (cp < 0x80) out[o++] = (char)cp;
					else if (cp < 0x800) { out[o++] = 0xC0 | cp >> 6; out[o++] = 0x80 | (cp & 0x3F); }
					else { out[o++] = 0xE0 | cp >> 12; out[o++] = 0x80 | ((cp >> 6) & 0x3F); out[o++] = 0x80 | (cp & 0x3F); }
				}
				q = sc;
				continue;
			}
		}
		if ((unsigned char)q[0] == 0xCC && (unsigned char)q[1] == 0x81) { /* ударение */
			q++;
			continue;
		}
		if (isspace((unsigned char)*q)) {
			if (o && out[o - 1] != ' ')
				out[o++] = ' ';
			continue;
		}
		out[o++] = *q;
	}
	while (o && out[o - 1] == ' ')
		o--;
	out[o] = 0;
	if (after)
		*after = e + strlen(close);
	return o > 0;
}

/* значение атрибута attr="..." внутри тега, начинающегося в tag */
static int xml_attr(const char *tag, const char *attr, char *out, size_t n)
{
	const char *end = strchr(tag, '>');
	char key[40];
	snprintf(key, sizeof key, "%s=", attr);
	for (const char *p = tag; (p = strstr(p, key)) && (!end || p < end); p++) {
		if (p > tag && !isspace((unsigned char)p[-1]))
			continue;
		char q = p[strlen(key)];
		if (q != '"' && q != '\'')
			continue;
		const char *v = p + strlen(key) + 1, *ve = strchr(v, q);
		if (!ve)
			return 0;
		size_t l = (size_t)(ve - v) < n - 1 ? (size_t)(ve - v) : n - 1;
		memcpy(out, v, l);
		out[l] = 0;
		for (char *c = out; *c; c++)
			if (*c == '\t' || *c == '\n' || *c == '\r')
				*c = ' ';
		return l > 0;
	}
	return 0;
}

static char *to_utf8(char *buf, size_t len)
{
	/* fb2 часто в windows-1251 */
	char *decl_end = strstr(buf, "?>");
	char *enc = strstr(buf, "encoding=");
	if (!enc || (decl_end && enc > decl_end))
		return buf;
	char name[32] = {0};
	sscanf(enc + 10, "%31[^\"']", name);
	if (!strcasecmp(name, "utf-8") || !name[0])
		return buf;
	iconv_t cd = iconv_open("UTF-8", name);
	if (cd == (iconv_t)-1)
		return buf;
	size_t outsz = len * 2 + 1, inleft = len, outleft = outsz - 1;
	char *out = malloc(outsz), *ip = buf, *op = out;
	iconv(cd, &ip, &inleft, &op, &outleft);
	*op = 0;
	iconv_close(cd);
	free(buf);
	return out;
}

static void format_author(const char *style, const char *first, const char *middle, const char *last, char *out, size_t n)
{
	if (!strcmp(style, "last_initials") && last[0]) {
		/* Толстой Л. Н. */
		size_t o = (size_t)snprintf(out, n, "%s", last);
		const char *parts[2] = {first, middle};
		for (int i = 0; i < 2 && o + 6 < n; i++) {
			if (!parts[i][0])
				continue;
			int l = ((unsigned char)parts[i][0] >= 0xC0) ? 2 : 1;
			out[o++] = ' ';
			memcpy(out + o, parts[i], l);
			o += l;
			out[o++] = '.';
		}
		out[o] = 0;
	} else if (!strcmp(style, "last")) {
		snprintf(out, n, "%s", last[0] ? last : first);
	} else if (!strcmp(style, "initials") && last[0]) {
		/* первая буква имени (UTF-8) */
		char ini[8] = {0};
		if (first[0]) {
			int l = ((unsigned char)first[0] >= 0xC0) ? 2 : 1;
			memcpy(ini, first, l);
		}
		if (ini[0])
			snprintf(out, n, "%s. %s", ini, last);
		else
			snprintf(out, n, "%s", last);
	} else {
		snprintf(out, n, "%s%s%s%s%s", first, first[0] ? " " : "", middle, middle[0] ? " " : "", last);
	}
}

static int book_meta(const char *path, const char *style, char *title, size_t tn, char *author, size_t an,
		     char *series, size_t sn, char *num, size_t nn)
{
	title[0] = author[0] = series[0] = num[0] = 0;
	FILE *f = fopen(path, "rb");
	if (!f)
		return 0;
	unsigned char magic[4] = {0};
	fread(magic, 1, 4, f);
	char *xml = NULL;
	size_t len = 0;
	int epub = 0;
	if (!memcmp(magic, "PK\3\4", 4)) {
		xml = zip_read(f, NULL, ".fb2", 256 * 1024, &len);
		if (!xml) {
			char *cont = zip_read(f, "META-INF/container.xml", NULL, 64 * 1024, &len);
			char opf[256] = {0};
			if (cont) {
				char *fp = strstr(cont, "full-path=");
				if (fp)
					sscanf(fp + 11, "%255[^\"']", opf);
				free(cont);
			}
			if (opf[0]) {
				xml = zip_read(f, opf, NULL, 256 * 1024, &len);
				epub = 1;
			}
		}
	} else {
		xml = malloc(256 * 1024 + 1);
		fseek(f, 0, SEEK_SET);
		len = fread(xml, 1, 256 * 1024, f);
		xml[len] = 0;
	}
	fclose(f);
	if (!xml)
		return 0;
	xml = to_utf8(xml, len);
	if (epub) {
		xml_text(xml, "dc:title", title, tn, NULL);
		xml_text(xml, "dc:creator", author, an, NULL);
		/* серия в стиле calibre: <meta name="calibre:series" content="..."/> */
		const char *m = strstr(xml, "\"calibre:series\"");
		if (m) {
			const char *ts = m;
			while (ts > xml && *ts != '<')
				ts--;
			xml_attr(ts, "content", series, sn);
		}
		m = strstr(xml, "\"calibre:series_index\"");
		if (m) {
			const char *ts = m;
			while (ts > xml && *ts != '<')
				ts--;
			if (xml_attr(ts, "content", num, nn)) {
				char *dot = strstr(num, ".0"); /* 3.0 -> 3 */
				if (dot && !dot[2])
					*dot = 0;
			}
		}
	} else {
		char *ti = strstr(xml, "<title-info");
		if (ti) {
			char *end = strstr(ti, "</title-info>");
			if (end)
				*end = 0;
			xml_text(ti, "book-title", title, tn, NULL);
			const char *p = ti, *next;
			size_t o = 0;
			char block[512];
			/* все авторы через запятую */
			while (xml_text(p, "author", block, sizeof block, &next) || 0) {
				/* xml_text выкинул теги, поэтому берём поля из исходного куска */
				const char *as = strstr(p, "<author");
				char first[128] = "", middle[128] = "", last[128] = "", one[300];
				char piece[1024];
				size_t pl = next - as < (long)sizeof piece - 1 ? (size_t)(next - as) : sizeof piece - 1;
				memcpy(piece, as, pl);
				piece[pl] = 0;
				xml_text(piece, "first-name", first, sizeof first, NULL);
				xml_text(piece, "middle-name", middle, sizeof middle, NULL);
				xml_text(piece, "last-name", last, sizeof last, NULL);
				if (!first[0] && !last[0])
					xml_text(piece, "nickname", last, sizeof last, NULL);
				/* <last-name>Олегович Пелевин</last-name>: фамилия — последнее слово */
				char *sp = strrchr(last, ' ');
				if (sp && !middle[0] && first[0]) {
					*sp = 0;
					copy(middle, sizeof middle, last);
					memmove(last, sp + 1, strlen(sp + 1) + 1);
				}
				format_author(style, first, middle, last, one, sizeof one);
				if (one[0] && o + strlen(one) + 3 < an)
					o += snprintf(author + o, an - o, "%s%s", o ? ", " : "", one);
				p = next;
			}
			const char *sq = strstr(ti, "<sequence");
			if (sq) {
				xml_attr(sq, "name", series, sn);
				xml_attr(sq, "number", num, nn);
			}
		}
	}
	/* «Девятый [Каменистый]» -> «Девятый»; серия = название -> не показываем */
	size_t sl = strlen(series);
	if (sl && series[sl - 1] == ']') {
		char *br = strrchr(series, '[');
		if (br && br > series && br[-1] == ' ')
			br[-1] = 0;
	}
	if (series[0] && !strcasecmp(series, title))
		series[0] = num[0] = 0;
	if (!series[0])
		num[0] = 0;
	free(xml);
	return title[0] != 0;
}

/* ---------- кеш «имя файла -> подпись» ---------- */

struct entry {
	char *key;   /* как программа показывает книгу: имя файла (с расширением или без) */
	char *path;  /* полный путь */
	char *author, *title; /* NULL = ещё не считали, title "" = не удалось */
	char *series, *num;
	char *label, *reader;
	char *row; /* строка списка, посчитанная под ширину row_w шрифтом row_font */
	int row_w;
	const void *row_font;
};

static struct entry *tab;
static size_t tab_cap, tab_len;
static time_t db_mtime;
static time_t last_check;

static uint32_t hash(const char *s)
{
	uint32_t h = 2166136261u;
	while (*s)
		h = (h ^ (unsigned char)*s++) * 16777619u;
	return h;
}

static struct entry *tab_find(const char *key, int create)
{
	if (!tab_cap) {
		if (!create)
			return NULL;
		tab_cap = 1024;
		tab = calloc(tab_cap, sizeof *tab);
	}
	if (create && tab_len * 2 >= tab_cap) {
		struct entry *old = tab;
		size_t oc = tab_cap;
		tab_cap *= 2;
		tab = calloc(tab_cap, sizeof *tab);
		tab_len = 0;
		for (size_t i = 0; i < oc; i++)
			if (old[i].key) {
				struct entry *e = tab_find(old[i].key, 1);
				*e = old[i];
			}
		free(old);
	}
	for (size_t i = hash(key) & (tab_cap - 1);; i = (i + 1) & (tab_cap - 1)) {
		if (!tab[i].key) {
			if (!create)
				return NULL;
			tab[i].key = strdup(key);
			tab_len++;
			return &tab[i];
		}
		if (!strcmp(tab[i].key, key))
			return &tab[i];
	}
}

static void add_book(const char *path, const char *fname)
{
	char key[512];
	snprintf(key, sizeof key, "%s", fname);
	for (int pass = 0; pass < 3; pass++) {
		struct entry *e = tab_find(key, 1);
		if (!e->path)
			e->path = strdup(path);
		/* тот же ключ без расширения книги: .fb2.zip -> .fb2 -> без */
		static const char *const exts[] = {".zip", ".fb2", ".epub", ".pdf", ".txt", ".djvu", ".djv",
						   ".mobi", ".azw3", ".azw", ".docx", ".doc", ".rtf", ".cbz",
						   ".xps", ".html", ".htm", ".chm", ".odt"};
		char *dot = strrchr(key, '.');
		int known = 0;
		for (unsigned x = 0; dot && x < sizeof exts / sizeof exts[0]; x++)
			known |= !strcasecmp(dot, exts[x]);
		if (!known || dot == key)
			break;
		*dot = 0;
	}
}

static void load_cache_file(void)
{
	FILE *f = fopen(CACHE_PATH, "r");
	if (!f)
		return;
	char line[2048];
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\n")] = 0;
		char *tab1 = strchr(line, '\t');
		if (!tab1)
			continue;
		*tab1 = 0;
		/* path \t author \t title \t series \t num */
		char *fld[4] = {tab1 + 1};
		int ok = 1;
		for (int k = 1; k < 4 && ok; k++) {
			char *t = strchr(fld[k - 1], '\t');
			if (t) {
				*t = 0;
				fld[k] = t + 1;
			} else
				ok = 0;
		}
		if (!ok)
			continue;
		for (size_t i = 0; i < tab_cap; i++)
			if (tab[i].key && tab[i].path && !tab[i].title && !strcmp(tab[i].path, line)) {
				tab[i].author = strdup(fld[0]);
				tab[i].title = strdup(fld[1]);
				tab[i].series = strdup(fld[2]);
				tab[i].num = strdup(fld[3]);
			}
	}
	fclose(f);
}

static void refresh_books(void)
{
	time_t now = time(NULL);
	if (now - last_check < 3)
		return;
	last_check = now;
	static const char *dbs[] = {"/userdata/.system/ebooks.db", "/mnt/udisk/.system/ebooks.db"};
	int changed = 0;
	for (unsigned d = 0; d < 2; d++) {
		struct stat st;
		if (stat(dbs[d], &st) == 0 && st.st_mtime > db_mtime) {
			db_mtime = st.st_mtime;
			changed = 1;
		}
	}
	if (!changed)
		return;
	void *h = dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_GLOBAL);
	if (!h)
		return;
	int (*open_v2)(const char *, void **, int, const char *) = dlsym(h, "sqlite3_open_v2");
	int (*prepare)(void *, const char *, int, void **, const char **) = dlsym(h, "sqlite3_prepare_v2");
	int (*step)(void *) = dlsym(h, "sqlite3_step");
	const unsigned char *(*col_text)(void *, int) = dlsym(h, "sqlite3_column_text");
	int (*finalize)(void *) = dlsym(h, "sqlite3_finalize");
	int (*close_)(void *) = dlsym(h, "sqlite3_close");
	if (!open_v2 || !prepare || !step || !col_text || !finalize || !close_)
		return;
	for (unsigned d = 0; d < 2; d++) {
		void *db = NULL, *stmt = NULL;
		if (open_v2(dbs[d], &db, 0x00000001 /* READONLY */, NULL) != 0) {
			if (db)
				close_(db);
			continue;
		}
		if (prepare(db, "SELECT file_path, file_name FROM books", -1, &stmt, NULL) == 0) {
			while (step(stmt) == 100 /* SQLITE_ROW */) {
				const char *p = (const char *)col_text(stmt, 0), *n = (const char *)col_text(stmt, 1);
				if (p && n)
					add_book(p, n);
			}
			finalize(stmt);
		}
		close_(db);
	}
	load_cache_file();
	logf_("библиотека: в таблице %zu ключей", tab_len);
}

/* не больше max символов UTF-8, лишнее заменяется на «…» */
static void clip(const char *src, int max, char *out, size_t n)
{
	size_t o = 0;
	int chars = 0;
	const char *p = src;
	while (*p && o + 4 < n) {
		if (max > 0 && chars == max) {
			while (o && out[o - 1] == ' ')
				o--;
			memcpy(out + o, "…", 3);
			o += 3;
			break;
		}
		int l = 1;
		while ((p[l] & 0xC0) == 0x80)
			l++;
		memcpy(out + o, p, l);
		o += l;
		p += l;
		chars++;
	}
	out[o] = 0;
}

static int utf8_len(const char *s)
{
	int n = 0;
	for (; *s; s++)
		n += (*s & 0xC0) != 0x80;
	return n;
}

/* «Лев Николаевич Толстой, Иван Бунин» -> «Л. Н. Толстой, И. Бунин» */
static void initials(const char *src, char *out, size_t n)
{
	size_t o = 0;
	const char *p = src;
	while (*p && o + 8 < n) {
		const char *comma = strstr(p, ", ");
		const char *end = comma ? comma : p + strlen(p);
		const char *last = end;
		while (last > p && last[-1] != ' ')
			last--;
		for (const char *w = p; w < last && o + 8 < n;) {
			while (w < last && *w == ' ')
				w++;
			if (w >= last)
				break;
			int l = 1;
			while ((w[l] & 0xC0) == 0x80)
				l++;
			memcpy(out + o, w, l);
			o += l;
			out[o++] = '.';
			out[o++] = ' ';
			while (w < last && *w != ' ')
				w++;
		}
		size_t ll = (size_t)(end - last);
		if (o + ll + 3 >= n)
			break;
		memcpy(out + o, last, ll);
		o += ll;
		if (!comma)
			break;
		memcpy(out + o, ", ", 2);
		o += 2;
		p = comma + 2;
	}
	out[o] = 0;
}

/* сколько символов оставить каждому полю (0 = всё), surname = от автора только фамилия */
struct lim {
	int author, title, series, surname, no_series;
};

/* фамилия первого автора: «Толстой Л. Н., Бунин И.» -> «Толстой» */
static void surname(const char *a, char *out, size_t n)
{
	const char *s = a, *e;
	if (strncmp(cfg.author_style, "last", 4)) {
		/* «Лев Толстой»: последнее слово до запятой */
		const char *c = strstr(a, ", ");
		e = c ? c : a + strlen(a);
		s = e;
		while (s > a && s[-1] != ' ')
			s--;
	} else
		e = a + strcspn(a, " ,");
	snprintf(out, n, "%.*s", (int)(e - s), s);
}

/* подставляет {поля}; часть в [...] выкидывается, если любое поле в ней пустое.
   Возвращает 0, если в этом куске встретилось пустое поле. */
static int render(const char **pp, struct entry *e, const struct lim *l, char *out, size_t n, size_t *o)
{
	int ok = 1;
	const char *p = *pp;
	while (*p && *p != ']') {
		if (*p == '[') {
			size_t save = *o;
			p++;
			if (!render(&p, e, l, out, n, o))
				*o = save;
			if (*p == ']')
				p++;
			continue;
		}
		const char *v = NULL;
		int max = 0, skip = 0;
		if (!strncmp(p, "{author}", 8)) v = e->author, max = l->author, skip = 8;
		else if (!strncmp(p, "{title}", 7)) v = e->title, max = l->title, skip = 7;
		else if (!strncmp(p, "{series}", 8)) v = l->no_series ? "" : e->series, max = l->series, skip = 8;
		else if (!strncmp(p, "{num}", 5)) v = e->num, skip = 5;
		else if (!strncmp(p, "{file}", 6)) v = e->key, skip = 6;
		if (skip) {
			char buf[512], ini[512];
			v = v ? v : "";
			if (v == e->author && l->surname) {
				surname(v, ini, sizeof ini);
				v = ini;
			}
			/* длинного автора сначала сокращаем до инициалов, фамилию не режем */
			else if (v == e->author && max > 0 && utf8_len(v) > max && !strcmp(cfg.author_style, "full")) {
				initials(v, ini, sizeof ini);
				v = ini;
			}
			clip(v, max, buf, sizeof buf);
			if (!buf[0])
				ok = 0;
			*o += snprintf(out + *o, n - *o, "%s", buf);
			if (*o >= n)
				*o = n - 1;
			p += skip;
		} else if (*o + 1 < n)
			out[(*o)++] = *p++;
		else
			p++;
	}
	*pp = p;
	out[*o] = 0;
	return ok;
}

static void render_str(const char *fmt, struct entry *e, const struct lim *l, char *out, size_t n)
{
	size_t o = 0;
	render(&fmt, e, l, out, n, &o);
}

/* вся подпись не длиннее max: сначала без серии, потом автор — только фамилия,
   и обрезаются название и фамилия, чтобы было видно хотя бы начало обоих */
static void fit(const char *fmt, struct entry *e, int max, struct lim *l)
{
	char out[800];
	*l = (struct lim){0};
	render_str(fmt, e, l, out, sizeof out);
	if (utf8_len(out) <= max)
		return;
	l->no_series = 1;
	render_str(fmt, e, l, out, sizeof out);
	int A = utf8_len(e->author), T = utf8_len(e->title);
	int budget = max - (utf8_len(out) - A - T);
	if (A + T <= budget)
		return;
	char sn[256];
	surname(e->author, sn, sizeof sn);
	int S = utf8_len(sn);
	/* автору не меньше 2/5, но не больше, чем нужно; остальное — названию */
	int a = budget - T > budget * 2 / 5 ? budget - T : budget * 2 / 5;
	if (a < A) {
		l->surname = 1;
		if (S > a)
			l->author = a > 2 ? a - 1 : 1;
		else
			a = S;
	} else
		a = A;
	int t = budget - a;
	if (T > t)
		l->title = t > 2 ? t - 1 : 1;
}

static int thumbs_dirty; /* в список мода добавились книги — проверить миниатюры */

static const char *display_for(struct entry *e)
{
	if (e->label)
		return e->label;
	if (!e->title) {
		char title[256], author[256], series[256], num[32];
		if (!book_meta(e->path, cfg.author_style, title, sizeof title, author, sizeof author, series, sizeof series, num, sizeof num))
			title[0] = author[0] = series[0] = num[0] = 0;
		e->author = strdup(author);
		e->title = strdup(title);
		e->series = strdup(series);
		e->num = strdup(num);
		/* сохраняем, чтобы не разбирать книгу при каждом запуске */
		FILE *c = fopen(CACHE_PATH, "a");
		if (c) {
			fprintf(c, "%s\t%s\t%s\t%s\t%s\n", e->path, author, title, series, num);
			fclose(c);
		}
		thumbs_dirty = 1; /* новая книга: дорисовать миниатюру */
		for (size_t i = 0; i < tab_cap; i++)
			if (tab[i].key && tab[i].path && !tab[i].title && !strcmp(tab[i].path, e->path)) {
				tab[i].author = e->author;
				tab[i].title = e->title;
				tab[i].series = e->series;
				tab[i].num = e->num;
			}
	}
	if (!e->title[0])
		return NULL;
	char out[800];
	const char *fmt = e->author[0] ? cfg.title_format : "{title}[ ({series}[, {num}])]";
	struct lim l = {cfg.max_author, cfg.max_title, cfg.max_series, 0, 0};
	if (cfg.max_len > 0)
		fit(fmt, e, cfg.max_len, &l);
	render_str(fmt, e, &l, out, sizeof out);
	e->label = strdup(out);
	logf_("подпись: %s -> %s", e->key, out);
	return e->label;
}

/* заголовок в читалке: программа сама обрезает имя файла, показываем название */
static const char *reader_title(struct entry *e)
{
	if (!display_for(e))
		return NULL;
	if (!e->reader) {
		char out[800], cl[800];
		struct lim l = {0};
		render_str(cfg.reader_format, e, &l, out, sizeof out);
		clip(out, cfg.max_reader > 1 ? cfg.max_reader - 1 : 0, cl, sizeof cl);
		e->reader = strdup(cl);
		logf_("заголовок: %s -> %s", e->key, cl);
	}
	return e->reader;
}

static int read_int(const char *path)
{
	char buf[16] = {0};
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	int n = read(fd, buf, sizeof buf - 1);
	close(fd);
	return n > 0 ? atoi(buf) : -1;
}

/* ширина подписи появляется только после раскладки: проверяем в следующих кадрах,
   и если это узкая плитка главного экрана — оставляем одно название */
static struct {
	void *obj;
	struct entry *e;
	int tries;
	int prefix; /* подпись с обрезанным именем файла: заголовок читалки или строка списка (2) */
	const void *font; /* шрифт, которым строка списка уже посчитана */
} tiles[32];

/* шрифт подписи (LV_STYLE_TEXT_FONT = 90 в LVGL 9.2); проверяем, что похоже на lv_font_t */
static const void *label_font(void *obj)
{
	if (!lv.style_prop || !lv.text_width)
		return NULL;
	const int32_t *font = (const int32_t *)(intptr_t)lv.style_prop(obj, 0, 90);
	if ((uintptr_t)font < 0x10000 || !font[0] || font[3] < 8 || font[3] > 200) /* line_height */
		return NULL;
	return font;
}

static const void *tl_font;
static int tl_px(const char *s)
{
	return lv.text_width(s, strlen(s), tl_font, 0);
}

/* начало src (без «…», без хвостовых пробелов): ровно c символов */
static void cut(const char *src, int c, char *out, size_t n)
{
	size_t o = 0;
	for (const char *p = src; *p && c > 0 && o + 4 < n; c--) {
		int l = 1;
		while ((p[l] & 0xC0) == 0x80)
			l++;
		memcpy(out + o, p, l);
		o += l;
		p += l;
	}
	while (o && (out[o - 1] == ' ' || out[o - 1] == '.' || out[o - 1] == ','))
		o--;
	out[o] = 0;
}

/* самое длинное начало src, которое вместе с prefix влезает в px */
static void cut_px(const char *prefix, const char *src, int px, char *out, size_t n)
{
	char buf[512];
	for (int c = utf8_len(src); c > 0; c--) {
		cut(src, c, out, n);
		snprintf(buf, sizeof buf, "%s%s", prefix, out);
		if (tl_px(buf) <= px)
			return;
	}
	out[0] = 0;
}

/* плитка: «Фантастиче/Гаррисон» — название не шире половины, дальше фамилия;
   короткая фамилия отдаёт остаток названию */
static int tile_label(struct entry *e, void *obj, int w, char *out, size_t n)
{
	if (!display_for(e))
		return 0;
	char sn[128], t[256], a[128];
	surname(e->author, sn, sizeof sn);
	tl_font = label_font(obj);
	if (!tl_font) {
		int chars = w * 10 / cfg.tile_char10;
		cut(e->title, chars / 2, t, sizeof t);
		cut(sn, chars - utf8_len(t) - 1, a, sizeof a);
		snprintf(out, n, sn[0] ? "%s/%s" : "%s", t, a);
		return 1;
	}
	w -= 6; /* запас, чтобы LVGL не добавил свои «...» */
	snprintf(out, n, sn[0] ? "%s/%s" : "%s", e->title, sn);
	if (tl_px(out) <= w)
		return 1;
	if (!sn[0]) {
		cut_px("", e->title, w, out, n);
		return 1;
	}
	snprintf(a, sizeof a, "/%s", sn);
	int rest = w - tl_px(a);
	if (rest >= w / 2) {
		cut_px("", e->title, rest, t, sizeof t);
		snprintf(out, n, "%s%s", t, a);
		return 1;
	}
	cut_px("", e->title, w / 2, t, sizeof t);
	snprintf(out, n, "%s/", t);
	cut_px(out, sn, w, a, sizeof a);
	snprintf(out, n, "%s/%s", t, a);
	return 1;
}

static int list_w; /* ширина строки списка после раскладки (узнаём по первой строке) */
static const void *list_font; /* шрифт строк списка после раскладки */

/* подпись «по содержимому» (строка списка): её ширина — это ширина текущего текста.
   Тогда место для строки — от её левого края до правого края родителя */
static int list_avail(void *obj, int w)
{
	const void *font = label_font(obj);
	const char *cur = lv.label_text ? lv.label_text(obj) : NULL;
	if (!font || !cur || !lv.get_x || !lv.parent)
		return 0;
	int tw = lv.text_width(cur, strlen(cur), font, 0);
	if (tw - w > 2 || w - tw > 2)
		return 0;
	int x = lv.get_x(obj);
	for (void *p = lv.parent(obj); p; p = lv.parent(p)) {
		int pw = lv.get_width(p);
		if (pw > 0)
			return pw - x - 8;
	}
	return 0;
}

/* строка списка по шаблону, если влезает по ширине: целиком, потом с одной фамилией
   (серия остаётся); иначе как на плитке — «Назв/Фамилия» */
static int list_label(struct entry *e, void *obj, int w, char *out, size_t n)
{
	if (!display_for(e) || !e->author[0])
		return 0;
	/* в момент set_text у подписи ещё шрифт темы (уже настоящего) — меряем запомненным шрифтом строк списка */
	tl_font = list_font ? list_font : label_font(obj);
	if (!tl_font)
		return 0;
	static const struct lim tries[2] = {{0, 0, 0, 0, 0}, {0, 0, 0, 1, 0}};
	for (int i = 0; i < 2; i++) {
		render_str(cfg.title_format, e, &tries[i], out, n);
		if (tl_px(out) <= w - 36)
			return 1;
	}
	/* порядок всегда «автор, серия, название»: «Фамилия/(Серия 3) Назв» — серия с номером
	   и фамилия важнее, название получает остаток; на него почти не остаётся — от серии номер */
	char sn[128], pre[400], t[256];
	surname(e->author, sn, sizeof sn);
	int W = w - 36, min = W / 5; /* LVGL ставит «...» раньше ширины подписи */
	for (int k = 0; k < 3; k++) {
		if (k == 0 && e->series[0])
			snprintf(pre, sizeof pre, e->num[0] ? "%s/(%s %s) " : "%s/(%s) ", sn, e->series, e->num);
		else if (k == 1 && e->series[0] && e->num[0])
			snprintf(pre, sizeof pre, "%s/(%s) ", sn, e->num);
		else if (k < 2)
			continue;
		else
			snprintf(pre, sizeof pre, "%s/", sn);
		if (W - tl_px(pre) < min && k < 2)
			continue;
		cut_px(pre, e->title, W, t, sizeof t);
		snprintf(out, n, "%s%s", pre, t);
		if (tl_px(out) <= W)
			return 1;
	}
	cut_px("", pre, W, out, n);
	return 1;
}

/* строка списка из кеша записи; кеш годен только для той же ширины и того же шрифта */
static const char *list_row(struct entry *e, void *obj, int w)
{
	const void *f = list_font ? list_font : label_font(obj);
	if (!e->row || e->row_w != w || e->row_font != f) {
		char row[800];
		if (!list_label(e, obj, w, row, sizeof row))
			return NULL;
		free(e->row);
		e->row = strdup(row);
		e->row_w = w;
		e->row_font = f;
	}
	return e->row;
}

static void tile_check(void *obj, struct entry *e, int prefix)
{
	if (!lv.is_valid || !lv.get_width || cfg.tile_width <= 0)
		return;
	for (int i = 0; i < 32; i++)
		if (!tiles[i].obj || tiles[i].obj == obj) {
			tiles[i].obj = obj;
			tiles[i].e = e;
			tiles[i].tries = 20;
			tiles[i].prefix = prefix;
			tiles[i].font = NULL;
			return;
		}
}

static void tiles_poll(void)
{
	for (int i = 0; i < 32; i++) {
		if (!tiles[i].obj)
			continue;
		void *obj = tiles[i].obj;
		int w = lv.is_valid(obj) ? lv.get_width(obj) : -1;
		if (w == 0 && --tiles[i].tries > 0)
			continue;
		tiles[i].obj = NULL;
		int la = w > 0 && w < cfg.tile_width ? list_avail(obj, w) : 0;
		if (tiles[i].prefix) {
			if (cfg.debug > 1)
				logf_("книга %p: строка-префикс w %d la %d", obj, w, la);
			char d[800];
			int lw = la >= cfg.tile_width ? la : w >= cfg.tile_width ? w : 0;
			const void *lf = label_font(obj);
			if (lw && lf)
				list_font = lf; /* после раскладки шрифт строки уже настоящий */
			if (lw && lw != list_w) {
				list_w = lw;
				/* запоминаем, чтобы и первая страница после запуска рисовалась сразу */
				FILE *f = fopen(LUMA_DIR "/list.width", "w");
				if (f) {
					fprintf(f, "%d\n", lw);
					fclose(f);
				}
			}
			const char *cur = lv.label_text ? lv.label_text(obj) : NULL;
			const char *r = lw ? list_row(tiles[i].e, obj, lw) : NULL;
			if (r && (!cur || strcmp(cur, r))) {
				snprintf(d, sizeof d, "%s", r);
				if (cfg.debug > 1)
					logf_("строка списка переписана: «%s» -> «%s»", cur ? cur : "", d);
				real_label_set_text(obj, d);
			}
			continue;
		}
		if (la >= cfg.tile_width)
			w = la;
		if (w > 0 && w < cfg.tile_width) {
			char d[256];
			if (tile_label(tiles[i].e, obj, w, d, sizeof d))
				real_label_set_text(obj, d);
		} else if (w > 0) {
			char d[800];
			if (list_label(tiles[i].e, obj, w, d, sizeof d))
				real_label_set_text(obj, d);
		}
		if (cfg.debug > 1)
			logf_("книга %p: ширина подписи %d (la %d, prefix %d), шрифт %s", obj, w, la, tiles[i].prefix, label_font(obj) ? "есть" : "нет");
	}
}

/* пункты меню текста: что за заголовком «Интервалы между строк» — это интервалы */
static const char *menu_value(const char *text)
{
	static int ctx;
	static char spacing[3][48];
	static const char *const items[3] = {"Маленький", "Большой", "Очень большой"};
	if (!strcmp(text, "Интервалы между строк")) {
		ctx = 1;
		return NULL;
	}
	if (!strcmp(text, "Text"))
		return NULL;
	if (ctx == 1 && cfg.has_line_spacing)
		for (int i = 0; i < 3; i++)
			if (!strcmp(text, items[i])) {
				if (!spacing[i][0])
					snprintf(spacing[i], sizeof spacing[i], "%s · %g", items[i], cfg.line_spacing[i]);
				return spacing[i];
			}
	ctx = 0;
	return NULL;
}

static void battery(char *out, size_t n, const char *text)
{
	int cap = read_int(BATTERY);
	if (cap < 0)
		snprintf(out, n, "%s", text);
	else
		snprintf(out, n, "%s   %d%%%s", text, cap, read_int(CHARGER) == 1 ? "+" : "");
}

/* строка состояния: часы -> «часы  94%», «5/816» -> «5/816 · 1%» (только копируемые подписи).
   На главном экране справа от часов значок батареи — там процент пишется после даты. */
static const char *status_text(void *obj, const char *text)
{
	static char buf[64], date[16];
	static void *date_obj, *date_scr;
	int h, m, y, n = 0, len = strlen(text);
	if (cfg.status_battery && len == 10 && sscanf(text, "%2d-%2d-%4d%n", &h, &m, &y, &n) == 3 && n == len) {
		date_obj = obj;
		date_scr = lv.get_screen ? lv.get_screen(obj) : NULL;
		copy(date, sizeof date, text);
		battery(buf, sizeof buf, text);
		return buf;
	}
	if (cfg.status_battery && len <= 5 && sscanf(text, "%2d:%2d%n", &h, &m, &n) == 2 && n == len) {
		if (date_obj && lv.is_valid && lv.is_valid(date_obj) && lv.get_screen(obj) == date_scr) {
			char d[64];
			battery(d, sizeof d, date);
			real_label_set_text(date_obj, d);
			return NULL;
		}
		battery(buf, sizeof buf, text);
		return buf;
	}
	if (cfg.status_progress && sscanf(text, "%d/%d%n", &h, &m, &n) == 2 && n == len && m > 0 && h <= m &&
	    m != 30 /* счётчик закладок «0/30» */) {
		snprintf(buf, sizeof buf, "%s · %d%%", text, h * 100 / m);
		return buf;
	}
	return NULL;
}

static int font_hidden(const char *text)
{
	size_t n = strlen(text);
	for (const char *p = cfg.hide_fonts; *p;) {
		while (*p == ' ' || *p == ',')
			p++;
		const char *e = strchr(p, ',');
		size_t l = e ? (size_t)(e - p) : strlen(p);
		while (l && p[l - 1] == ' ')
			l--;
		if (l && l == n && !strncmp(p, text, n))
			return 1;
		p += e ? (size_t)(e - p) : strlen(p);
	}
	return 0;
}

static const char *relabel(void *obj, const char *text, int dynamic)
{
	if (!text || !text[0])
		return text;
	const char *slot = slot_of_label(text);
	for (int i = 0; i < cfg.nfonts; i++)
		if (!strcmp(slot, cfg.fonts[i].slot))
			return cfg.fonts[i].label;
	const char *r;
	if (cfg.sort_books && !strcmp(text, "No author books"))
		return "Без автора";
	if (font_hidden(text)) {
		/* пункт меню шрифтов — кнопка, у которой эта подпись внутри */
		void *(*parent)(const void *) = lv.parent;
		void (*add_flag)(void *, uint32_t) = dlsym(RTLD_DEFAULT, "lv_obj_add_flag");
		int (*has_flag)(const void *, uint32_t) = dlsym(RTLD_DEFAULT, "lv_obj_has_flag");
		void *o = obj;
		for (int i = 0; o && parent && has_flag && i < 3; i++, o = parent(o))
			if (has_flag(o, 1u << 1 /* CLICKABLE */)) {
				add_flag(o, 1u << 0 /* HIDDEN */);
				if (cfg.debug > 1)
					logf_("шрифты: «%s» спрятан", text);
				break;
			}
		return text;
	}
	if (cfg.text_panel && dynamic >= 0 && !strcmp(text, "Размер шрифта")) {
		tune_item = obj;
		return "Текст: размер, интервал";
	}
	if (cfg.menu_values && (r = menu_value(text)))
		return r;
	if (dynamic && (r = status_text(obj, text)))
		return r;
	if (!cfg.titles_enabled)
		return text;
	refresh_books();
	struct entry *e = tab_find(text, 0);
	if (e) {
		/* узкая плитка на главном экране: влезает ~11 символов — только название */
		int w = label_width(obj);
		if (cfg.debug > 1 && lv.style_prop) {
			char b[300];
			size_t o = 0;
			void *p = obj;
			for (int i = 0; p && i < 5 && o < sizeof b - 40; i++, p = lv.parent(p))
				o += snprintf(b + o, sizeof b - o, " [%d st%d]", (int)lv.get_width(p), (int)lv.style_prop(p, 0, 1));
			logf_("ширины %s:%s", e->key, b);
		}
		if (w <= 0)
			tile_check(obj, e, 0);
		int la = w > 0 && w < cfg.tile_width ? list_avail(obj, w) : 0;
		if (la >= cfg.tile_width)
			w = la;
		if (w >= cfg.tile_width) {
			const char *r = list_row(e, obj, w);
			if (r)
				return r;
		}
		/* ширина не своя, а от предка (подпись «по содержимому») — строка списка:
		   пока полная подпись, после раскладки — по ширине строки */
		int32_t pst = lv.parent && lv.parent(obj) ? lv.style_prop(lv.parent(obj), 0, 1 /* LV_STYLE_WIDTH */) : 0;
		/* у заголовка читалки родитель шириной в пикселях, у строк списка — 100% */
		if (w > 0 && w < cfg.tile_width && lv.get_width(obj) == 0 && !(pst > 0 && pst < (1 << 29))) {
			tile_check(obj, e, 2);
			/* ширину строки списка уже знаем — сразу готовый текст, без второй перерисовки */
			if (list_w > 0) {
				const char *r = list_row(e, obj, list_w);
				if (r)
					return r;
			}
			w = 0;
		}
		const char *d = w > 0 && w < cfg.tile_width ? reader_title(e) : display_for(e);
		if (cfg.debug > 1)
			logf_("книга %p: w %d la %d -> %s", obj, w, la, d ? d : "-");
		return d ? d : text;
	}
	/* заголовок в читалке: начало имени файла, обрезанное программой */
	size_t tl = strlen(text);
	if (tl >= 10 && !strchr(text, ' '))
		for (size_t i = 0; i < tab_cap; i++)
			if (tab[i].key && tab[i].path && strlen(tab[i].key) > tl && !strncmp(tab[i].key, text, tl)) {
				const char *d = reader_title(&tab[i]);
				if (d) {
					tile_check(obj, &tab[i], 1);
					return d;
				}
			}
	if (cfg.debug > 1)
		logf_("подпись (не книга): %s", text);
	return text;
}

/* снимок экрана для отладки: touch /dev/shm/luma-test/shot -> shot.bin (заголовок draw_buf + данные) */
static void shot(void)
{
	void *(*get_default)(void) = dlsym(RTLD_DEFAULT, "lv_display_get_default");
	void *(*get_buf)(void *) = dlsym(RTLD_DEFAULT, "lv_display_get_buf_active");
	if (!get_default || !get_buf)
		return;
	struct {
		uint32_t hdr[3];
		uint32_t data_size;
		uint8_t *data;
	} *db = get_buf(get_default());
	if (!db || !db->data)
		return;
	FILE *f = fopen(SHOT_PATH ".tmp", "wb");
	if (!f)
		return;
	fwrite(db, 16, 1, f);
	fwrite(db->data, 1, db->data_size, f);
	fclose(f);
	rename(SHOT_PATH ".tmp", SHOT_PATH);
}

static void sleep_cover_for(const char *path);
static void thumbs_start(void);

/* ---------- читалка: место в книге ----------
   Адреса внутри ylc-ebook сборки 26.06.2026 (MuPDF 1.24.11, код на основе pdfapp.c).
   Перед использованием сверяем таблицу операций MuPDF: другая сборка — ничего не трогаем.

   При смене размера, интервала, полей программа в фоновом потоке делает закладку
   (make_bookmark), перестраивает книгу и ищет закладку (lookup_bookmark). Закладка Digma —
   путь к абзацу, поэтому после перестройки открывается страница, где начинается абзац,
   и место уплывает назад. Мод подменяет обе функции в документе: закладка запоминает
   ещё и первые буквы страницы, а поиск открывает страницу, где эти буквы оказались.
   Если читатель после перестройки не листал, якорь остаётся прежним — от многих
   перестроек подряд место не уплывает. Всё идёт в потоке самой программы, без гонок. */
#define APP_PTR_VA    0xe94facu /* указатель на состояние читалки (pdfapp) */
#define MUPDF_OPS_VA  0xe471a4u /* таблица операций документа MuPDF, первая — init */
#define MUPDF_INIT_VA 0xd3828u
#define HT_MAKE_BOOKMARK_VA   0x2095ecu /* htdoc_make_bookmark (fb2, epub, html) */
#define HT_LOOKUP_BOOKMARK_VA 0x209550u /* htdoc_lookup_bookmark */
/* в открытии fb2/epub адреса этих функций берутся из констант рядом с кодом:
   ldr rN, [pc, #..]; add rN, pc, rN — подменяем константы, и каждый новый документ
   (в том числе открытый заново после смены CSS) сразу получает наши закладки */
#define HT_OPEN_MAKE_ADD_VA   0x20bf58u
#define HT_OPEN_MAKE_LIT_VA   0x20c090u
#define HT_OPEN_LOOKUP_ADD_VA 0x20bf64u
#define HT_OPEN_LOOKUP_LIT_VA 0x20c094u
/* MuPDF */
#define FZ_PUSH_TRY_VA      0x158b88u
#define FZ_DO_TRY_VA        0x158cc4u
#define FZ_DO_CATCH_VA      0x158cf8u
#define FZ_CHAPTER_PAGES_VA 0x13422cu /* fz_count_chapter_pages */
#define FZ_LOAD_PAGE_VA     0x134af4u /* fz_load_chapter_page */
#define FZ_BOUND_PAGE_VA    0x134db8u
#define FZ_RUN_CONTENTS_VA  0x134f1cu /* fz_run_page_contents */
#define FZ_DROP_PAGE_VA     0x135374u
#define FZ_NEW_STEXT_VA     0x1a3554u /* fz_new_stext_page */
#define FZ_STEXT_DEV_VA     0x1a39e8u /* fz_new_stext_device */
#define FZ_DROP_STEXT_VA    0x1a3620u
#define FZ_CLOSE_DEV_VA     0x12e97cu
#define FZ_DROP_DEV_VA      0x12ea98u
/* смещения */
#define APP_DOC       0         /* в pdfapp */
#define APP_MUTEX     48
#define DOC_MAKE_BOOKMARK   8   /* в fz_document, указатели на функции */
#define DOC_LOOKUP_BOOKMARK 9

typedef struct { float x0, y0, x1, y1; } fz_rect;
typedef struct { float a, b, c, d, e, f; } fz_matrix;
typedef struct { int chapter, page; } fz_location;

static struct {
	char **app;
	uintptr_t base;
	uintptr_t make, lookup; /* штатные функции закладок */
	sigjmp_buf *(*push_try)(void *);
	int (*do_try)(void *), (*do_catch)(void *);
	int (*chapter_pages)(void *, void *, int);
	void *(*load_page)(void *, void *, int, int);
	fz_rect (*bound_page)(void *, void *);
	void (*run_contents)(void *, void *, void *, fz_matrix, void *);
	void (*drop_page)(void *, void *);
	void *(*new_stext)(void *, fz_rect);
	void *(*stext_dev)(void *, void *, const void *);
	void (*drop_stext)(void *, void *);
	void (*close_dev)(void *, void *), (*drop_dev)(void *, void *);
} rd;

static intptr_t my_make_bookmark(void *ctx, void *doc, fz_location loc);
static fz_location my_lookup_bookmark(void *ctx, void *doc, intptr_t mark);

static void bm_patch_open(uintptr_t b)
{
#ifdef __linux__
	uint32_t *lm = (uint32_t *)(b + HT_OPEN_MAKE_LIT_VA), *ll = (uint32_t *)(b + HT_OPEN_LOOKUP_LIT_VA);
	uint32_t pm = b + HT_OPEN_MAKE_ADD_VA + 8, pl = b + HT_OPEN_LOOKUP_ADD_VA + 8;
	if (*lm + pm != b + HT_MAKE_BOOKMARK_VA || *ll + pl != b + HT_LOOKUP_BOOKMARK_VA) {
		logf_("читалка: открытие документа не совпало — закладки только у уже открытых");
		return;
	}
	long pg = sysconf(_SC_PAGESIZE);
	uintptr_t start = (uintptr_t)lm & ~(uintptr_t)(pg - 1);
	size_t len = ((uintptr_t)ll + 4 - start + pg - 1) & ~(uintptr_t)(pg - 1);
	if (mprotect((void *)start, len, PROT_READ | PROT_WRITE | PROT_EXEC)) {
		logf_("читалка: mprotect: %s", strerror(errno));
		return;
	}
	*lm = (uint32_t)(uintptr_t)my_make_bookmark - pm;
	*ll = (uint32_t)(uintptr_t)my_lookup_bookmark - pl;
	mprotect((void *)start, len, PROT_READ | PROT_EXEC);
	logf_("читалка: закладки по тексту для всех новых документов");
#endif
}

static int reader_init(void)
{
	static int done;
	if (done)
		return rd.app != NULL;
	done = 1;
#ifdef __linux__
	uintptr_t b = 0;
	dl_iterate_phdr(find_main_base, &b);
	if (!b || *(uint32_t *)(b + MUPDF_OPS_VA) != b + MUPDF_INIT_VA) {
		logf_("читалка: другая сборка программы — привязка к тексту выключена");
		return 0;
	}
#define AT(va) ((void *)(b + (va)))
	rd.base = b;
	rd.make = b + HT_MAKE_BOOKMARK_VA;
	rd.lookup = b + HT_LOOKUP_BOOKMARK_VA;
	rd.push_try = AT(FZ_PUSH_TRY_VA);
	rd.do_try = AT(FZ_DO_TRY_VA);
	rd.do_catch = AT(FZ_DO_CATCH_VA);
	rd.chapter_pages = AT(FZ_CHAPTER_PAGES_VA);
	rd.load_page = AT(FZ_LOAD_PAGE_VA);
	rd.bound_page = AT(FZ_BOUND_PAGE_VA);
	rd.run_contents = AT(FZ_RUN_CONTENTS_VA);
	rd.drop_page = AT(FZ_DROP_PAGE_VA);
	rd.new_stext = AT(FZ_NEW_STEXT_VA);
	rd.stext_dev = AT(FZ_STEXT_DEV_VA);
	rd.drop_stext = AT(FZ_DROP_STEXT_VA);
	rd.close_dev = AT(FZ_CLOSE_DEV_VA);
	rd.drop_dev = AT(FZ_DROP_DEV_VA);
#undef AT
	rd.app = (char **)(b + APP_PTR_VA);
	bm_patch_open(b);
	return 1;
#else
	return 0;
#endif
}

/* буквы страницы подряд, без пробелов и мягких переносов: так текст сравним при любой вёрстке */
static int stext_chars(const char *st, uint32_t *out, int max)
{
	int n = 0;
	for (const char *bl = st ? *(char **)(st + 20) : NULL; bl && n < max; bl = *(char **)(bl + 52)) {
		if (*(int *)bl != 0) /* не текст */
			continue;
		for (const char *ln = *(char **)(bl + 20); ln && n < max; ln = *(char **)(ln + 40))
			for (const char *ch = *(char **)(ln + 28); ch && n < max; ch = *(char **)(ch + 60)) {
				uint32_t c = *(const uint32_t *)ch;
				if (c > ' ' && c != 0xa0 && c != 0xad)
					out[n++] = c;
			}
	}
	return n;
}

/* текст любой страницы главы без показа на экране; -1 — нет такой страницы.
   Вызывать только из потока, который сейчас работает с документом: у MuPDF один контекст */
static int page_chars(void *ctx, void *doc, int chapter, int page, uint32_t *out, int max)
{
	void *volatile pg = NULL, *volatile st = NULL, *volatile dev = NULL;
	volatile int n = -1;
	static const fz_matrix ident = {1, 0, 0, 1, 0, 0};
	if (!sigsetjmp(*rd.push_try(ctx), 0)) {
		if (rd.do_try(ctx)) {
			pg = rd.load_page(ctx, doc, chapter, page);
			st = rd.new_stext(ctx, rd.bound_page(ctx, pg));
			dev = rd.stext_dev(ctx, st, NULL);
			rd.run_contents(ctx, pg, dev, ident, NULL);
			rd.close_dev(ctx, dev);
			n = stext_chars(st, out, max);
		}
	}
	if (rd.do_catch(ctx))
		n = -1;
	if (dev)
		rd.drop_dev(ctx, dev);
	if (st)
		rd.drop_stext(ctx, st);
	if (pg)
		rd.drop_page(ctx, pg);
	return n;
}

static int chapter_pages(void *ctx, void *doc, int chapter)
{
	volatile int n = 0;
	if (!sigsetjmp(*rd.push_try(ctx), 0))
		if (rd.do_try(ctx))
			n = rd.chapter_pages(ctx, doc, chapter);
	if (rd.do_catch(ctx))
		n = 0;
	return n;
}

#define ANCHOR 24   /* букв якоря */
static volatile int book_opens; /* растёт при каждом открытии книги (UPDATE last_open_time) */
#define PAGE_MAX 6000

static struct {
	pthread_mutex_t mu;
	void *doc;             /* документ, к которому относится якорь */
	int book;              /* номер открытия книги */
	uint32_t a[ANCHOR];    /* якорь: начало страницы, с которой меняли вёрстку */
	int alen, ach;
	double afrac;          /* где якорь в главе (доля) — если закладка Digma не нашлась */
	int lch, lpg, landed;  /* куда открыли после прошлой перестройки */
	int reopened;          /* документ открыт заново (интервал): страница Digma не в счёт */
	int pending;           /* закладку сделали, поиска ещё не было */
	struct timespec made;  /* когда делали закладку */
} anc = {.mu = PTHREAD_MUTEX_INITIALIZER};

static double since_ms(const struct timespec *t)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - t->tv_sec) * 1e3 + (now.tv_nsec - t->tv_nsec) / 1e6;
}

static void anchor_log(const char *what, int ch, int pg, int count)
{
	char t[ANCHOR * 4 + 1];
	int o = 0;
	for (int i = 0; i < anc.alen; i++) {
		uint32_t c = anc.a[i];
		if (c < 0x80)
			t[o++] = c;
		else if (c < 0x800) {
			t[o++] = 0xc0 | c >> 6;
			t[o++] = 0x80 | (c & 63);
		} else {
			t[o++] = 0xe0 | c >> 12;
			t[o++] = 0x80 | (c >> 6 & 63);
			t[o++] = 0x80 | (c & 63);
		}
	}
	t[o] = 0;
	logf_("место: %s, глава %d, страница %d из %d: «%s»", what, ch, pg, count, t);
}

/* страница главы, на которой начинается якорь; поиск от est в обе стороны */
static int find_anchor(void *ctx, void *doc, int est, int count, int *found)
{
	static uint32_t cur[PAGE_MAX + ANCHOR], next[ANCHOR];
	for (int d = 0; d <= 15; d++)
		for (int sgn = 0; sgn < (d ? 2 : 1); sgn++) {
			/* закладка Digma ведёт на начало абзаца — якорь чаще дальше, ищем сначала вперёд */
			int p = sgn ? est - d : est + d;
			if (p < 0 || p >= count)
				continue;
			int n = page_chars(ctx, doc, anc.ach, p, cur, PAGE_MAX);
			if (n < 0)
				continue;
			/* якорь мог начаться в конце страницы и перейти на следующую */
			int m = p + 1 < count ? page_chars(ctx, doc, anc.ach, p + 1, next, ANCHOR) : 0;
			for (int i = 0; i < m; i++)
				cur[n + i] = next[i];
			int tot = n + (m > 0 ? m : 0);
			for (int i = 0; i < n && i + anc.alen <= tot; i++)
				if (cur[i] == anc.a[0] && !memcmp(cur + i, anc.a, anc.alen * sizeof *anc.a)) {
					*found = p;
					return 1;
				}
		}
	return 0;
}

/* закладка перед перестройкой: запоминаем начало страницы, вёрстка ещё старая */
static intptr_t my_make_bookmark(void *ctx, void *doc, fz_location loc)
{
	pthread_mutex_lock(&anc.mu);
	int same = anc.alen && anc.doc == doc && anc.book == book_opens;
	/* смена интервала: программа открывает книгу заново, переходит на тот же номер страницы
	   ещё без наших закладок и потом перестраивает уже новый документ. Якорь — со старого */
	int reopen = anc.alen && anc.doc != doc && anc.book == book_opens && anc.pending && since_ms(&anc.made) < 10000;
	if (reopen) {
		anc.reopened = 1;
		if (cfg.debug > 1)
			logf_("место: книга открыта заново, якорь прежний, страница %d", loc.page);
	} else if (!(same && anc.landed && anc.lch == loc.chapter && anc.lpg == loc.page)) {
		/* читатель перелистнул (или первая перестройка) — новый якорь */
		int n = page_chars(ctx, doc, loc.chapter, loc.page, anc.a, ANCHOR);
		int count = chapter_pages(ctx, doc, loc.chapter);
		anc.alen = n > 0 ? n : 0;
		anc.ach = loc.chapter;
		anc.afrac = count > 0 ? (double)loc.page / count : 0;
		anc.landed = 0;
		anc.reopened = 0;
		if (anc.alen && cfg.debug > 1)
			anchor_log("якорь", loc.chapter, loc.page, count);
	}
	anc.doc = doc;
	anc.book = book_opens;
	anc.landed = 0;  /* поиск после перестройки поставит снова */
	anc.pending = 1;
	clock_gettime(CLOCK_MONOTONIC, &anc.made);
	pthread_mutex_unlock(&anc.mu);
	return ((intptr_t (*)(void *, void *, fz_location))rd.make)(ctx, doc, loc);
}

/* поиск закладки после перестройки: страница Digma — отправная точка, дальше ищем якорь */
static fz_location my_lookup_bookmark(void *ctx, void *doc, intptr_t mark)
{
	fz_location loc = ((fz_location (*)(void *, void *, intptr_t))rd.lookup)(ctx, doc, mark);
	pthread_mutex_lock(&anc.mu);
	/* только первый поиск после нашей закладки: следом программа ищет закладки читателя,
	   их не трогаем. Документ мог открыться заново — тогда закладку делали только что */
	if (cfg.text_anchor && anc.pending && anc.alen && anc.book == book_opens && (anc.doc == doc || since_ms(&anc.made) < 10000)) {
		int ch = loc.page >= 0 && loc.chapter == anc.ach ? loc.chapter : anc.ach;
		int count = chapter_pages(ctx, doc, ch), to = -1;
		int est = ch == loc.chapter && loc.page >= 0 && !anc.reopened ? loc.page : (int)(anc.afrac * count + 0.5);
		anc.reopened = 0;
		if (count > 0 && find_anchor(ctx, doc, est, count, &to)) {
			if (to != loc.page || ch != loc.chapter)
				logf_("место: после перестройки страница %d -> %d (из %d), по тексту", loc.page, to, count);
			loc.chapter = ch;
			loc.page = to;
		} else if (cfg.debug > 1)
			logf_("место: якорь не найден, остаёмся на %d (из %d)", loc.page, count);
		anc.doc = doc;
		anc.lch = loc.chapter;
		anc.lpg = loc.page;
		anc.landed = 1;
	}
	anc.pending = 0;
	pthread_mutex_unlock(&anc.mu);
	return loc;
}

/* открыли документ — ставим свои закладки (только в fb2/epub/html со штатными функциями Digma) */
static void reader_tick(void)
{
	if (!cfg.text_anchor || !reader_init() || !*rd.app)
		return;
	char *app = *rd.app;
	pthread_mutex_t *mu = (pthread_mutex_t *)(app + APP_MUTEX);
	if (pthread_mutex_trylock(mu))
		return;
	uintptr_t *doc = *(uintptr_t **)(app + APP_DOC);
	/* новый документ может занять память старого — смотрим на сами указатели */
	if (doc && doc[DOC_MAKE_BOOKMARK] == rd.make && doc[DOC_LOOKUP_BOOKMARK] == rd.lookup) {
		doc[DOC_MAKE_BOOKMARK] = (uintptr_t)my_make_bookmark;
		doc[DOC_LOOKUP_BOOKMARK] = (uintptr_t)my_lookup_bookmark;
		if (cfg.debug > 1)
			logf_("читалка: документ %p — закладки по тексту", (void *)doc);
	}
	pthread_mutex_unlock(mu);
}

/* ---------- панель «Текст» ----------
   Пункт «Размер шрифта» в меню читалки открывает свою панель: размер, интервал, поля по бокам,
   красная строка — каждая строка с − значение +. Панель не закрывается после нажатия:
   через долю секунды после последнего нажатия мод перестраивает CSS и программа открывает
   книгу заново (как после выбора в меню), место держится по тексту. Тап мимо панели — закрыть. */
#define OPS_SET_CSS_VA 0xd2fb0u /* операции документа: запомнить CSS (app+40) */
#define OPS_REOPEN_VA  0xd32e4u /* применить CSS: закладка, закрыть и открыть заново */
#define APP_CSS        40
#define APP_EM         36       /* размер шрифта (float) */
#define OPS_SET_FONT_SIZE_SLOT_VA 0xe47234u /* операции документа [36]: размер шрифта (int) */
#define OPS_SET_FONT_SIZE_VA      0xd3148u
#define LV_EVENT_ALL 0
#define LV_EVENT_CLICKED 7
#define LV_FLAG_HIDDEN (1u << 0)
#define LV_FLAG_CLICKABLE (1u << 1)
#define LV_FLAG_SCROLLABLE (1u << 4)

typedef struct { uint8_t blue, green, red; } lv_color_t;
typedef struct { void *cb, *user_data; uint32_t filter; } ev_dsc_t;

static struct {
	int ok;
	void *(*obj_create)(void *), *(*label_create)(void *), *(*button_create)(void *);
	void *(*layer_top)(void);
	void (*obj_delete)(void *);
	void (*set_size)(void *, int32_t, int32_t), (*set_pos)(void *, int32_t, int32_t);
	void (*remove_style_all)(void *);
	void (*add_flag)(void *, uint32_t), (*remove_flag)(void *, uint32_t);
	int (*has_flag)(const void *, uint32_t);
	void *(*add_event_cb)(void *, void (*)(void *), uint32_t, void *);
	uint32_t (*event_count)(const void *);
	ev_dsc_t *(*event_dsc)(const void *, uint32_t);
	int (*remove_event)(void *, uint32_t);
	uint32_t (*ev_code)(void *);
	void *(*ev_target)(void *), *(*ev_user)(void *);
	void (*ev_stop)(void *), (*ev_stop_bubbling)(void *);
	void (*bg_color)(void *, lv_color_t, uint32_t), (*bg_opa)(void *, uint8_t, uint32_t);
	void (*border_color)(void *, lv_color_t, uint32_t), (*border_width)(void *, int32_t, uint32_t);
	void (*radius)(void *, int32_t, uint32_t), (*text_color)(void *, lv_color_t, uint32_t);
	void (*text_font)(void *, const void *, uint32_t), (*text_align)(void *, int, uint32_t);
	void (*center)(void *);
	int32_t (*hor_res)(void *), (*ver_res)(void *);
	uint32_t (*child_count)(const void *);
	void *(*font_create)(const char *, int, uint32_t, int);
} ui;

static int ui_init(void)
{
	static int done;
	if (done)
		return ui.ok;
	done = 1;
#define S(f, n) if (!(*(void **)&ui.f = dlsym(RTLD_DEFAULT, n))) { logf_("панель: нет %s", n); return 0; }
	S(obj_create, "lv_obj_create") S(label_create, "lv_label_create") S(button_create, "lv_button_create")
	S(layer_top, "lv_layer_top") S(obj_delete, "lv_obj_delete")
	S(set_size, "lv_obj_set_size") S(set_pos, "lv_obj_set_pos") S(remove_style_all, "lv_obj_remove_style_all")
	S(add_flag, "lv_obj_add_flag") S(remove_flag, "lv_obj_remove_flag") S(has_flag, "lv_obj_has_flag")
	S(add_event_cb, "lv_obj_add_event_cb") S(event_count, "lv_obj_get_event_count")
	S(event_dsc, "lv_obj_get_event_dsc") S(remove_event, "lv_obj_remove_event")
	S(ev_code, "lv_event_get_code") S(ev_target, "lv_event_get_target") S(ev_user, "lv_event_get_user_data")
	S(ev_stop, "lv_event_stop_processing") S(ev_stop_bubbling, "lv_event_stop_bubbling")
	S(bg_color, "lv_obj_set_style_bg_color") S(bg_opa, "lv_obj_set_style_bg_opa")
	S(border_color, "lv_obj_set_style_border_color") S(border_width, "lv_obj_set_style_border_width")
	S(radius, "lv_obj_set_style_radius") S(text_color, "lv_obj_set_style_text_color")
	S(text_font, "lv_obj_set_style_text_font") S(text_align, "lv_obj_set_style_text_align")
	S(center, "lv_obj_center")
	S(hor_res, "lv_display_get_horizontal_resolution") S(ver_res, "lv_display_get_vertical_resolution")
	S(child_count, "lv_obj_get_child_count") S(font_create, "lv_freetype_font_create")
#undef S
	resolve();
	ui.ok = lv.is_valid && lv.parent && lv.get_screen;
	return ui.ok;
}

static const lv_color_t BLACK = {0, 0, 0}, WHITE = {255, 255, 255};

#define TUNE_ROWS 4
static const struct {
	const char *name;
	double step, lo, hi;
} tune_rows[TUNE_ROWS] = {
	{"Размер", 1, 12, 90},
	{"Интервал", 0.05, 0.8, 2.5},
	{"Поля по бокам", 4, 0, 120},
	{"Красная строка", 0.5, 0, 5},
};

static struct {
	void *backdrop;  /* панель открыта */
	void *menu;      /* спрятанное на время меню программы */
	void *value[TUNE_ROWS];
	double v[TUNE_ROWS];
	const void *font, *font_big;
	int dirty;
	struct timespec changed, applied;
} panel;

static double *tune_slot(int row)
{
	return row == 0 ? &tune.em : row == 1 ? &tune.lh : row == 2 ? &tune.side : &tune.indent;
}

static void panel_show_value(int row)
{
	char t[32];
	double v = panel.v[row];
	if (row == 0)
		snprintf(t, sizeof t, "%.0f", v);
	else if (row == 1)
		snprintf(t, sizeof t, "%.2f", v);
	else if (row == 2)
		snprintf(t, sizeof t, "%.0f", v);
	else
		snprintf(t, sizeof t, "%.1f", v);
	if (panel.value[row])
		real_label_set_text(panel.value[row], t);
}

/* применить: CSS в тот же буфер программы и «открыть заново», как после выбора в меню */
static void tune_apply(void)
{
	if (!reader_init() || !*rd.app)
		return;
	char *app = *rd.app;
	if (!*(void **)(app + APP_DOC))
		return;
	if (!css_last.valid || *(char **)(app + APP_CSS) != css_last.buf) {
		logf_("панель: CSS программы не там, где ждали (%p, у нас %p) — не применяю",
			*(void **)(app + APP_CSS), (void *)css_last.buf);
		return;
	}
	build_css(css_last.buf, css_last.n, css_last.has_name ? css_last.dir : NULL,
		css_last.has_name ? css_last.name : NULL, css_last.lh_app);
	tune_save();
	logf_("панель: размер %.0f, интервал %.2f, поля %.0f, красная строка %.1f",
		tune.em > 0 ? tune.em : stock_em, tune_eff.lh, tune_eff.side, tune_eff.indent);
	/* размер — тот же, что ставит меню программы: число в состоянии читалки */
	*(float *)(app + APP_EM) = tune.em > 0 ? tune.em : stock_em;
	((void (*)(void))(rd.base + OPS_REOPEN_VA))();
	clock_gettime(CLOCK_MONOTONIC, &panel.applied);
}

static void panel_close(void)
{
	if (panel.backdrop && lv.is_valid(panel.backdrop))
		ui.obj_delete(panel.backdrop);
	panel.backdrop = NULL;
	for (int i = 0; i < TUNE_ROWS; i++)
		panel.value[i] = NULL;
	/* меню программы возвращаем как было */
	if (panel.menu && lv.is_valid(panel.menu))
		ui.remove_flag(panel.menu, LV_FLAG_HIDDEN);
	panel.menu = NULL;
}

static void panel_button_cb(void *e)
{
	intptr_t a = (intptr_t)ui.ev_user(e);
	if (a == 100) { /* мимо панели */
		if (ui.ev_target(e) == panel.backdrop)
			panel_close();
		return;
	}
	if (a == 101) { /* как в ini */
		tune.em = tune.lh = 0;
		tune.side = tune.indent = -1;
		panel.dirty = 1;
		clock_gettime(CLOCK_MONOTONIC, &panel.changed);
		panel_close();
		return;
	}
	if (a == 102) {
		panel_close();
		return;
	}
	int row = a / 2, dir = a % 2 ? 1 : -1;
	if (row < 0 || row >= TUNE_ROWS)
		return;
	double v = panel.v[row] + dir * tune_rows[row].step;
	if (v < tune_rows[row].lo - 1e-9 || v > tune_rows[row].hi + 1e-9)
		return;
	v = round(v / tune_rows[row].step) * tune_rows[row].step;
	panel.v[row] = v;
	*tune_slot(row) = v;
	panel_show_value(row);
	panel.dirty = 1;
	clock_gettime(CLOCK_MONOTONIC, &panel.changed);
}

static void *ui_label(void *parent, const char *text, const void *font)
{
	void *l = ui.label_create(parent);
	ui.text_font(l, font, 0);
	ui.text_color(l, BLACK, 0);
	real_label_set_text(l, text);
	return l;
}

static void *ui_button(void *parent, int x, int y, int w, int h, const char *text, intptr_t act)
{
	void *b = ui.button_create(parent);
	ui.remove_style_all(b);
	ui.set_pos(b, x, y);
	ui.set_size(b, w, h);
	ui.bg_color(b, WHITE, 0);
	ui.bg_opa(b, 255, 0);
	ui.border_color(b, BLACK, 0);
	ui.border_width(b, 2, 0);
	ui.radius(b, 10, 0);
	void *l = ui_label(b, text, panel.font_big);
	ui.center(l);
	ui.add_event_cb(b, panel_button_cb, LV_EVENT_CLICKED, (void *)act);
	return b;
}

static void panel_open(void)
{
	if (panel.backdrop || !ui_init())
		return;
	if (!panel.font) {
		static const char *const fonts[] = {"/opt/ylc-ebook/fonts/NotoSans.ttf", "/opt/ylc-ebook/fonts/Arial.ttf"};
		for (unsigned i = 0; i < 2 && !panel.font; i++)
			if (file_exists(fonts[i])) {
				panel.font = ui.font_create(fonts[i], 0, 30, 0);
				panel.font_big = ui.font_create(fonts[i], 0, 44, 0);
			}
		if (!panel.font || !panel.font_big) {
			logf_("панель: не удалось открыть шрифт");
			return;
		}
	}
	if (stock_em <= 0 && reader_init() && *rd.app)
		stock_em = *(float *)(*rd.app + APP_EM);
	panel.v[0] = tune.em > 0 ? tune.em : stock_em > 0 ? stock_em : 30;
	panel.v[1] = tune_eff.lh > 0.5 ? tune_eff.lh : 1.3;
	panel.v[2] = tune_eff.side;
	panel.v[3] = tune_eff.indent;
	int W = ui.hor_res(NULL), H = ui.ver_res(NULL);
	int rowh = 92, ph = rowh * (TUNE_ROWS + 1) + 40;
	/* подложка во весь экран: тапы мимо панели не листают книгу, а закрывают панель */
	void *bd = ui.obj_create(ui.layer_top());
	ui.remove_style_all(bd);
	ui.set_pos(bd, 0, 0);
	ui.set_size(bd, W, H);
	ui.add_flag(bd, LV_FLAG_CLICKABLE);
	ui.remove_flag(bd, LV_FLAG_SCROLLABLE);
	ui.add_event_cb(bd, panel_button_cb, LV_EVENT_CLICKED, (void *)100);
	panel.backdrop = bd;
	void *p = ui.obj_create(bd);
	ui.remove_style_all(p);
	ui.set_pos(p, 0, H - ph);
	ui.set_size(p, W, ph);
	ui.bg_color(p, WHITE, 0);
	ui.bg_opa(p, 255, 0);
	ui.border_color(p, BLACK, 0);
	ui.border_width(p, 3, 0);
	ui.add_flag(p, LV_FLAG_CLICKABLE); /* тап по самой панели — не закрывать */
	ui.remove_flag(p, LV_FLAG_SCROLLABLE);
	int bw = 110, vw = 130, x_plus = W - 24 - bw, x_val = x_plus - vw, x_minus = x_val - bw;
	for (int i = 0; i < TUNE_ROWS; i++) {
		int y = 20 + i * rowh;
		void *name = ui_label(p, tune_rows[i].name, panel.font);
		ui.set_pos(name, 24, y + 22);
		ui_button(p, x_minus, y + 6, bw, rowh - 16, "−", i * 2);
		void *v = ui_label(p, "", panel.font);
		ui.set_size(v, vw, 40);
		ui.set_pos(v, x_val, y + 22);
		ui.text_align(v, 2 /* LV_TEXT_ALIGN_CENTER */, 0);
		panel.value[i] = v;
		panel_show_value(i);
		ui_button(p, x_plus, y + 6, bw, rowh - 16, "+", i * 2 + 1);
	}
	int y = 20 + TUNE_ROWS * rowh;
	ui_button(p, 24, y + 6, (W - 72) / 2, rowh - 16, "Как в ini", 101);
	ui_button(p, 48 + (W - 72) / 2, y + 6, (W - 72) / 2, rowh - 16, "Готово", 102);
	logf_("панель: открыта");
}

/* пункт меню «Размер шрифта» -> панель. Обработчик ставим первым и дальше не пускаем,
   остальные пункты меню работают как были */
static uint32_t item_code = LV_EVENT_CLICKED;

static void item_cb(void *e)
{
	if (ui.ev_code(e) != item_code)
		return;
	void *label = ui.ev_user(e), *t = ui.ev_target(e);
	if (!lv.is_valid(label))
		return;
	int hit = 0;
	for (void *o = label; o && !hit; o = lv.parent(o))
		hit = o == t;
	if (!hit)
		return;
	ui.ev_stop(e);
	ui.ev_stop_bubbling(e);
	/* меню программы прячем, чтобы видеть страницу: самый верхний предок, у которого есть соседи */
	void *menu = NULL;
	for (void *o = label; o && lv.parent(o); o = lv.parent(o))
		if (ui.child_count(lv.parent(o)) > 1)
			menu = o;
	if (menu && menu != lv.get_screen(label) && ui.child_count(lv.get_screen(label)) > 1) {
		ui.add_flag(menu, LV_FLAG_HIDDEN);
		panel.menu = menu;
	}
	panel_open();
}

static void item_hook(void)
{
	void *label = tune_item;
	tune_item = NULL;
	if (!ui_init() || !lv.is_valid(label))
		return;
	void *h = label;
	for (int i = 0; h && i < 4 && !ui.event_count(h); i++)
		h = lv.parent(h);
	if (!h || !ui.event_count(h)) {
		logf_("панель: у пункта меню нет обработчика");
		return;
	}
	ev_dsc_t d[8];
	int n = ui.event_count(h);
	if (n > 8)
		n = 8;
	for (int i = 0; i < n; i++)
		d[i] = *ui.event_dsc(h, i);
	for (int i = 0; i < n; i++)
		if (d[i].cb == (void *)item_cb)
			return; /* уже стоит */
	for (int i = n - 1; i >= 0; i--)
		ui.remove_event(h, i);
	ui.add_event_cb(h, item_cb, LV_EVENT_ALL, label);
	item_code = LV_EVENT_CLICKED;
	for (int i = 0; i < n; i++) {
		ui.add_event_cb(h, d[i].cb, d[i].filter, d[i].user_data);
		if (d[i].filter && d[i].filter != LV_EVENT_ALL && i == 0)
			item_code = d[i].filter;
	}
	if (cfg.debug > 1)
		logf_("панель: пункт меню %p, обработчик на %p (%s), событий %d, код %u", label, h,
			h == label ? "подпись" : "предок", n, (unsigned)item_code);
}

/* размер шрифта из меню и при открытии книги: свой размер панели главнее */
static void (*real_set_font_size)(int);

static void my_set_font_size(int px)
{
	stock_em = px;
	real_set_font_size(px);
	if (tune.em > 0 && rd.app && *rd.app)
		*(float *)(*rd.app + APP_EM) = tune.em;
}

static void font_size_hook(void)
{
	static int done;
	if (done || !reader_init())
		return;
	done = 1;
	tune_load();
	uint32_t *slot = (uint32_t *)(rd.base + OPS_SET_FONT_SIZE_SLOT_VA);
	if (*slot != rd.base + OPS_SET_FONT_SIZE_VA) {
		logf_("панель: размер шрифта не там, где ждали — размер панели не работает");
		return;
	}
	real_set_font_size = (void (*)(int))(uintptr_t)*slot;
	long pg = sysconf(_SC_PAGESIZE);
	uintptr_t start = (uintptr_t)slot & ~(uintptr_t)(pg - 1);
	if (mprotect((void *)start, pg, PROT_READ | PROT_WRITE)) {
		logf_("панель: mprotect: %s", strerror(errno));
		return;
	}
	/* таблица лежит рядом с изменяемыми данными программы — запись не отключаем */
	*slot = (uint32_t)(uintptr_t)my_set_font_size;
	if (*rd.app)
		stock_em = *(float *)(*rd.app + APP_EM);
}

/* из lv_timer_handler: поставить обработчик на новый пункт меню, применить изменения */
static void panel_tick(void)
{
	if (!cfg.text_panel)
		return;
	font_size_hook();
	if (tune_item)
		item_hook();
	if (panel.dirty && since_ms(&panel.changed) > 700 && since_ms(&panel.applied) > 2500) {
		if (!reader_init() || !*rd.app)
			return;
		pthread_mutex_t *mu = (pthread_mutex_t *)(*rd.app + APP_MUTEX);
		if (pthread_mutex_trylock(mu))
			return; /* программа ещё занята прошлой перестройкой */
		pthread_mutex_unlock(mu);
		panel.dirty = 0;
		tune_apply();
	}
}

uint32_t lv_timer_handler(void)
{
	static uint32_t (*real)(void);
	static time_t last;
	if (!real)
		real = dlsym(RTLD_NEXT, "lv_timer_handler");
	uint32_t r = real();
	tiles_poll();
	static unsigned watch;
	if (!(++watch & 7))
		reader_tick();
	panel_tick();
	/* драйвер мог переписать импульс (сон, смена режима) — возвращаем свой */
	static unsigned tick;
	if (cfg.light_ticks && !(++tick & 31)) {
		pwm_poke(0);
		pwm_poke(1);
	}
	time_t now = time(NULL);
	if (now != last) {
		last = now;
		if (access(SHOT_FLAG, F_OK) == 0) {
			unlink(SHOT_FLAG);
			shot();
		}
		if (cfg.thumbs && thumbs_dirty)
			thumbs_start();
		/* отладка экрана сна: путь книги в /dev/shm/luma-test/sleep */
		FILE *sf = fopen("/dev/shm/luma-test/sleep", "r");
		if (sf) {
			char p[512] = "";
			if (fgets(p, sizeof p, sf))
				p[strcspn(p, "\n")] = 0;
			fclose(sf);
			unlink("/dev/shm/luma-test/sleep");
			unlink(SLEEP_SRC);
			if (p[0])
				sleep_cover_for(p);
		}
	}
	return r;
}

/* ---------- LVGL ---------- */

/* свои иконки: файл с тем же именем в luma/icons подменяет штатный из /opt/ylc-ebook/image */
/* программа ставит строке списка шрифт уже после set_text: тут же, до отрисовки,
   пересчитываем строку настоящим шрифтом — иначе экран перерисуется второй раз */
static void rows_restyle(void *target)
{
	if (list_w <= 0 || !lv.is_valid || !lv.parent)
		return;
	for (int i = 0; i < 32; i++) {
		void *obj = tiles[i].obj;
		if (!obj || tiles[i].prefix != 2 || !lv.is_valid(obj))
			continue;
		int near = 0;
		for (void *p = obj; p && near < 4; p = lv.parent(p), near++)
			if (p == target)
				break;
		if (near == 4)
			continue;
		const void *f = label_font(obj);
		if (!f || f == tiles[i].font)
			continue;
		tiles[i].font = f;
		list_font = f;
		const char *r = list_row(tiles[i].e, obj, list_w);
		const char *cur = lv.label_text ? lv.label_text(obj) : NULL;
		if (r && (!cur || strcmp(cur, r))) {
			char d[800];
			snprintf(d, sizeof d, "%s", r);
			if (cfg.debug > 1)
				logf_("строка списка до отрисовки: «%s» -> «%s»", cur ? cur : "", d);
			real_label_set_text(obj, d);
		}
	}
}

void lv_obj_set_style_text_font(void *obj, const void *font, uint32_t selector)
{
	static void (*real)(void *, const void *, uint32_t);
	if (!real)
		real = dlsym(RTLD_NEXT, "lv_obj_set_style_text_font");
	real(obj, font, selector);
	rows_restyle(obj);
}

void lv_obj_add_style(void *obj, const void *style, uint32_t selector)
{
	static void (*real)(void *, const void *, uint32_t);
	if (!real)
		real = dlsym(RTLD_NEXT, "lv_obj_add_style");
	real(obj, style, selector);
	rows_restyle(obj);
}

void lv_image_set_src(void *obj, const void *src)
{
	static void (*real)(void *, const void *);
	static const char *const dirs[] = {"/userdata/luma/icons/", "/opt/luma/icons/"};
	if (!real)
		real = dlsym(RTLD_NEXT, "lv_image_set_src");
	const char *s = src;
	if (s && s[0] == 'A' && s[1] == ':') {
		const char *base = strrchr(s, '/');
		static char path[256];
		static int after_plus;
		const char *name = base ? base + 1 : NULL;
		/* луна у второго ползунка (идёт сразу за «+» первого) — это температура;
		   такая же луна на кнопке режима сверху остаётся */
		int plus = name && !strcmp(name, "icon_increase_32.bin");
		if (name && after_plus && !strcmp(name, "icon_warmlight_32.bin"))
			name = "icon_temperature_32.bin";
		after_plus = plus;
		if (name && strstr(s, "/image/"))
			for (int i = 0; i < 2; i++) {
				snprintf(path, sizeof path, "%s%s", dirs[i], name);
				if (file_exists(path)) {
					snprintf(path, sizeof path, "A:%s%s", dirs[i], name);
					if (cfg.debug > 1)
						logf_("иконка: %s -> %s", s, path);
					real(obj, path);
					return;
				}
			}
		if (cfg.debug > 1)
			logf_("иконка: %s", s);
	}
	real(obj, src);
}

void lv_label_set_text(void *obj, const char *text)
{
	resolve();
	real_label_set_text(obj, relabel(obj, text, 1));
}

void lv_label_set_text_static(void *obj, const char *text)
{
	resolve();
	real_label_set_text_static(obj, relabel(obj, text, 0));
}

void lv_label_set_text_fmt(void *obj, const char *fmt, ...)
{
	char buf[1024];
	resolve();
	va_list ap;
	va_start(ap, fmt);
	real_vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	real_label_set_text(obj, relabel(obj, buf, 1));
}

/* ---------- кнопки ---------- */

/* боковые кнопки (gpio-keys): ЛВ, ЛН, ПВ, ПН. Штатно верхние листают назад, нижние вперёд;
   мод меняет верх и низ местами. Две кнопки разом держать 3 с — переключить раскладку (запоминается) */
#define BTN_STATE LUMA_DIR "/buttons.state"
static const uint16_t btn_code[4] = {616, 617, 614, 615};
static int active;           /* мод работает только в ylc-ebook */
static unsigned char key_fd[256]; /* 0 — не смотрели, 1 — кнопки, 2 — другое */
static int btn_on = -1;      /* текущая раскладка: 1 = верх и низ поменяны */

struct kev {
	uint32_t sec, usec; /* ядро отдаёт 32-битной программе 16 байт на событие */
	uint16_t type, code;
	int32_t value;
};

static int btn_index(int code)
{
	for (int i = 0; i < 4; i++)
		if (btn_code[i] == code)
			return i;
	return -1;
}

static int btn_map(int idx, int on)
{
	return on ? btn_code[idx ^ 1] : btn_code[idx];
}

static int is_keys(int fd)
{
	char name[64] = "";
	/* EVIOCGNAME(len) = _IOC(_IOC_READ, 'E', 0x06, len) */
	if (ioctl(fd, (2u << 30) | (sizeof name << 16) | ('E' << 8) | 0x06, name) < 0)
		return 0;
	return !strcmp(name, "gpio-keys");
}

static void buttons_filter(struct kev *ev, int count)
{
	static int held, combo;
	static double both_since; /* когда зажали вторую кнопку сочетания */
	if (btn_on < 0) {
		int v = read_int(BTN_STATE);
		btn_on = v < 0 ? cfg.buttons_swap : v;
	}
	for (int i = 0; i < count; i++) {
		int idx = ev[i].type == 1 ? btn_index(ev[i].code) : -1;
		if (idx < 0)
			continue;
		int bit = 1 << idx;
		double t = ev[i].sec + ev[i].usec / 1e6;
		if (ev[i].value == 1 && held && !(held & bit) && cfg.buttons_toggle && !combo) {
			/* вторая кнопка при зажатой первой: для программы первая отпущена, дальше ждём 3 с */
			int first = __builtin_ctz(held);
			ev[i].code = btn_map(first, btn_on);
			ev[i].value = 0;
			combo = 1;
			both_since = t;
			held |= bit;
			continue;
		}
		if (ev[i].value)
			held |= bit;
		else {
			/* сочетание отпустили: держали обе кнопки не меньше 3 с — меняем раскладку */
			if (combo && both_since > 0 && t - both_since >= 3) {
				btn_on = !btn_on;
				FILE *f = fopen(BTN_STATE, "w");
				if (f) {
					fprintf(f, "%d\n", btn_on);
					fclose(f);
				}
				logf_("кнопки: раскладка %s", btn_on ? "верхние вперёд" : "штатная");
			} else if (combo && both_since > 0)
				logf_("кнопки: сочетание %.1f с — мало, раскладка прежняя", t - both_since);
			both_since = 0;
			held &= ~bit;
		}
		if (combo) {
			/* пока не отпущены все кнопки сочетания, программа их не видит */
			ev[i].type = ev[i].code = 0;
			ev[i].value = 0;
			if (!held)
				combo = 0;
			continue;
		}
		ev[i].code = btn_map(idx, btn_on);
	}
}

ssize_t read(int fd, void *buf, size_t n)
{
	static ssize_t (*real)(int, void *, size_t);
	if (!real)
		real = dlsym(RTLD_NEXT, "read");
	ssize_t r = real(fd, buf, n);
	if (!active || r < (ssize_t)sizeof(struct kev) || r % sizeof(struct kev) || fd < 0 || fd >= 256)
		return r;
	if (!key_fd[fd])
		key_fd[fd] = is_keys(fd) ? 1 : 2;
	if (key_fd[fd] == 1)
		buttons_filter(buf, r / sizeof(struct kev));
	return r;
}

int close(int fd)
{
	static int (*real)(int);
	if (!real)
		real = dlsym(RTLD_NEXT, "close");
	if (fd >= 0 && fd < 256)
		key_fd[fd] = 0;
	return real(fd);
}

/* ---------- общие настройки текста ---------- */

/* программа хранит шрифт, размер, интервал и поля у каждой книги (ebooks.db, таблица books).
   Мод запоминает их при каждом сохранении книги и подставляет при открытии любой */
#define TEXT_STATE LUMA_DIR "/text.state"
static const char *const text_cols[4] = {"last_font_size", "last_font_type", "last_line_spacing", "last_margin"};
static int text_val[4], text_ok = -1;
static void *book_stmt;

static void text_load(void)
{
	if (text_ok >= 0)
		return;
	text_ok = 0;
	FILE *f = fopen(TEXT_STATE, "r");
	if (f) {
		text_ok = fscanf(f, "%d %d %d %d", &text_val[0], &text_val[1], &text_val[2], &text_val[3]) == 4;
		fclose(f);
	}
}

/* ---------- сортировка библиотеки ---------- */

/* программа сортирует «Название» по имени файла, а «Автор» группирует по своему полю file_author
   («Виктор Олегович Пелевин» и «Виктор Пелевин» — разные авторы, порядок по имени). Мод даёт SQLite
   свои функции и переписывает запросы библиотеки: автор — «Фамилия Имя» из книги, внутри автора —
   серия, номер, название; «Название» — по настоящему названию */
struct sortinfo {
	char *path, *author, *title, *key;
};
static struct sortinfo sorts[1024];
static int nsorts;
static pthread_mutex_t sort_mu = PTHREAD_MUTEX_INITIALIZER;

/* «Виктор Олегович Пелевин, Другой» -> «Пелевин Виктор»; ударения (U+0301) убираем */
static void author_key(const char *full, char *out, size_t n)
{
	char a[256];
	size_t o = 0;
	for (const char *p = full; *p && *p != ',' && o + 1 < sizeof a; p++) {
		if ((unsigned char)p[0] == 0xCC && (unsigned char)p[1] == 0x81) {
			p++;
			continue;
		}
		a[o++] = *p;
	}
	a[o] = 0;
	char *t = trim(a), *last = strrchr(t, ' ');
	if (!last) {
		snprintf(out, n, "%s", t);
		return;
	}
	char *sp = strchr(t, ' ');
	snprintf(out, n, "%s %.*s", last + 1, (int)(sp - t), t);
}

static struct sortinfo *sort_of(const char *path)
{
	pthread_mutex_lock(&sort_mu);
	for (int i = 0; i < nsorts; i++)
		if (!strcmp(sorts[i].path, path)) {
			pthread_mutex_unlock(&sort_mu);
			return &sorts[i];
		}
	pthread_mutex_unlock(&sort_mu);
	char title[512], author[512], series[256], num[32], ak[256] = "", key[900];
	if (!book_meta(path, "full", title, sizeof title, author, sizeof author, series, sizeof series, num, sizeof num))
		return NULL;
	if (author[0])
		author_key(author, ak, sizeof ak);
	snprintf(key, sizeof key, "%s\1%08d\1%s", series, atoi(num), title);
	pthread_mutex_lock(&sort_mu);
	struct sortinfo *r = NULL;
	if (nsorts < 1024) {
		r = &sorts[nsorts];
		r->path = strdup(path);
		r->author = ak[0] ? strdup(ak) : NULL;
		r->title = strdup(title);
		r->key = strdup(key);
		nsorts++;
	}
	pthread_mutex_unlock(&sort_mu);
	return r;
}

static struct {
	const unsigned char *(*text)(void *);
	void (*result_text)(void *, const char *, int, void (*)(void *));
	void (*result_value)(void *, void *);
	int (*create)(void *, const char *, int, int, void *, void (*)(void *, int, void **), void *, void *);
} sq;

static void sql_luma(void *ctx, int argc, void **argv, int what)
{
	const char *path = (const char *)sq.text(argv[0]);
	struct sortinfo *si = path ? sort_of(path) : NULL;
	const char *v = !si ? NULL : what == 0 ? si->author : what == 1 ? si->key : si->title;
	if (v)
		sq.result_text(ctx, v, -1, (void (*)(void *))-1 /* SQLITE_TRANSIENT */);
	else if (argc > 1)
		sq.result_value(ctx, argv[1]);
	else
		sq.result_text(ctx, path ? path : "", -1, (void (*)(void *))-1);
}
static void sql_author(void *c, int n, void **v) { sql_luma(c, n, v, 0); }
static void sql_key(void *c, int n, void **v) { sql_luma(c, n, v, 1); }
static void sql_title(void *c, int n, void **v) { sql_luma(c, n, v, 2); }

static void sql_register(void *db)
{
	static void *dbs[8];
	for (int i = 0; i < 8; i++) {
		if (dbs[i] == db)
			return;
		if (!dbs[i]) {
			dbs[i] = db;
			break;
		}
	}
	if (!sq.create) {
		sq.text = dlsym(RTLD_DEFAULT, "sqlite3_value_text");
		sq.result_text = dlsym(RTLD_DEFAULT, "sqlite3_result_text");
		sq.result_value = dlsym(RTLD_DEFAULT, "sqlite3_result_value");
		sq.create = dlsym(RTLD_DEFAULT, "sqlite3_create_function");
	}
	if (!sq.create || !sq.text || !sq.result_text || !sq.result_value)
		return;
	/* SQLITE_UTF8 | SQLITE_DETERMINISTIC */
	sq.create(db, "luma_author", 2, 1 | 0x800, NULL, sql_author, NULL, NULL);
	sq.create(db, "luma_key", 1, 1 | 0x800, NULL, sql_key, NULL, NULL);
	sq.create(db, "luma_title", 1, 1 | 0x800, NULL, sql_title, NULL, NULL);
}

/* заменить все from на to; результат — malloc */
static char *replace_all(const char *s, const char *from, const char *to)
{
	size_t fl = strlen(from), tl = strlen(to), cnt = 0;
	for (const char *p = s; (p = strstr(p, from)); p += fl)
		cnt++;
	char *o = malloc(strlen(s) + cnt * (tl > fl ? tl - fl : 0) + 1), *w = o;
	if (!o)
		return NULL;
	for (const char *p = s, *q; ; p = q + fl) {
		q = strstr(p, from);
		size_t l = q ? (size_t)(q - p) : strlen(p);
		memcpy(w, p, l);
		w += l;
		if (!q)
			break;
		memcpy(w, to, tl);
		w += tl;
	}
	*w = 0;
	return o;
}

static char *sort_rewrite(const char *sql)
{
	if (strncmp(sql, "SELECT", 6) || !strstr(sql, "FROM books") || !strncmp(sql, "SELECT file_path, file_type", 27))
		return NULL;
	if (!strstr(sql, "file_author") && !strstr(sql, "ORDER BY file_name"))
		return NULL;
	int by_author = strstr(sql, "WHERE file_author") != NULL;
	char *a = replace_all(sql, "file_author", "luma_author(file_path, file_author)");
	if (!a)
		return NULL;
	char *b = replace_all(a, "ORDER BY file_name", by_author ? "ORDER BY luma_key(file_path)" : "ORDER BY luma_title(file_path)");
	free(a);
	return b;
}

/* «Название» программа сортирует сама: qsort со сравнением имён файлов через strcasecmp.
   Внутри qsort (только в этом потоке) два имени книг сравниваем по настоящим названиям */
static __thread int in_qsort;
static uint32_t utf8_next(const char **p);

void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
	static void (*real)(void *, size_t, size_t, int (*)(const void *, const void *));
	if (!real)
		real = dlsym(RTLD_NEXT, "qsort");
	in_qsort++;
	real(base, n, size, cmp);
	in_qsort--;
}

/* без учёта регистра и для кириллицы (А–Я, Ё в UTF-8) */
static uint32_t fold(uint32_t c)
{
	if (c >= 'A' && c <= 'Z')
		return c + 32;
	if (c >= 0x410 && c <= 0x42F)
		return c + 32;
	if (c == 0x401 || c == 0x451)
		return 0x435; /* ё как е */
	return c;
}

static int utf8_casecmp(const char *a, const char *b)
{
	while (*a && *b) {
		uint32_t x = fold(utf8_next(&a)), y = fold(utf8_next(&b));
		if (x != y)
			return x < y ? -1 : 1;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

int strcasecmp(const char *a, const char *b)
{
	static int (*real)(const char *, const char *);
	if (!real)
		real = dlsym(RTLD_NEXT, "strcasecmp");
	if (in_qsort && active && cfg.sort_books && a && b && tab) {
		struct entry *ea = tab_find(a, 0), *eb = ea ? tab_find(b, 0) : NULL;
		struct sortinfo *sa = ea && ea->path ? sort_of(ea->path) : NULL, *sb = sa && eb && eb->path ? sort_of(eb->path) : NULL;
		if (sa && sb && sa->title[0] && sb->title[0]) {
			int r = utf8_casecmp(sa->title, sb->title);
			return r ? r : real(a, b);
		}
	}
	return real(a, b);
}

int sqlite3_prepare_v2(void *db, const char *sql, int n, void **stmt, const char **tail)
{
	static int (*real)(void *, const char *, int, void **, const char **);
	if (!real)
		real = dlsym(RTLD_NEXT, "sqlite3_prepare_v2");
	char *rw = NULL;
	if (active && sql && cfg.sort_books) {
		/* длина может быть задана явно — работаем с копией, оканчивающейся нулём */
		char *cp = n >= 0 ? strndup(sql, n) : NULL;
		const char *q = cp ? cp : sql;
		if (strstr(q, "books"))
			sql_register(db);
		rw = sort_rewrite(q);
		if (cfg.debug > 1 && strstr(q, "FROM books"))
			logf_("sql: %s%s", q, rw ? "  -> переписан" : "");
		free(cp);
	}
	int r;
	if (rw) {
		r = real(db, rw, -1, stmt, NULL);
		if (r) {
			logf_("sql: переписанный запрос не принят, оставляю штатный: %s", rw);
			r = real(db, sql, n, stmt, tail);
		} else if (tail)
			*tail = sql + (n >= 0 ? (size_t)n : strlen(sql));
		free(rw);
	} else
		r = real(db, sql, n, stmt, tail);
	if (active && r == 0 && stmt && sql && !strncmp(sql, "SELECT * FROM books WHERE file_path", 35))
		book_stmt = *stmt;
	return r;
}

/* какая книга открыта: SELECT по пути даёт её id, потом программа пишет время открытия по id */
static char bound_path[512];
static struct {
	int id;
	char path[512];
} book_ids[64];

static void book_seen(int id)
{
	if (!bound_path[0])
		return;
	int k = 0;
	for (int i = 0; i < 64; i++)
		if (book_ids[i].id == id || !book_ids[i].id) {
			k = i;
			break;
		}
	book_ids[k].id = id;
	copy(book_ids[k].path, sizeof book_ids[k].path, bound_path);
}

static void sleep_cover_for(const char *path);

int sqlite3_bind_text(void *stmt, int i, const char *v, int n, void (*d)(void *))
{
	static int (*real)(void *, int, const char *, int, void (*)(void *));
	if (!real)
		real = dlsym(RTLD_NEXT, "sqlite3_bind_text");
	if (active && stmt == book_stmt && i == 1 && v)
		snprintf(bound_path, sizeof bound_path, "%.*s", n >= 0 ? n : (int)strlen(v), v);
	return real(stmt, i, v, n, d);
}

int sqlite3_column_int(void *stmt, int i)
{
	static int (*real)(void *, int);
	static const char *(*name)(void *, int);
	if (!real) {
		real = dlsym(RTLD_NEXT, "sqlite3_column_int");
		name = dlsym(RTLD_DEFAULT, "sqlite3_column_name");
	}
	int r = real(stmt, i);
	if (!active || stmt != book_stmt || !name)
		return r;
	const char *c = name(stmt, i);
	if (c && !strcmp(c, "id"))
		book_seen(r);
	if (!cfg.text_global)
		return r;
	text_load();
	if (text_ok && c)
		for (int k = 0; k < 4; k++)
			if (!strcmp(c, text_cols[k])) {
				if (cfg.debug > 1 && r != text_val[k])
					logf_("текст: %s %d -> %d (общие)", c, r, text_val[k]);
				return text_val[k];
			}
	return r;
}

char *sqlite3_mprintf(const char *fmt, ...)
{
	static char *(*vm)(const char *, va_list);
	if (!vm)
		vm = dlsym(RTLD_DEFAULT, "sqlite3_vmprintf");
	va_list ap;
	va_start(ap, fmt);
	if (active && cfg.text_global && fmt && !strncmp(fmt, "UPDATE books SET last_font_size = %d, last_font_type = %d, last_line_spacing = %d, last_margin = %d", 98)) {
		va_list cp;
		va_copy(cp, ap);
		int v[4];
		for (int k = 0; k < 4; k++)
			v[k] = va_arg(cp, int);
		va_end(cp);
		text_load();
		if (v[0] >= 0 && v[1] >= 0 && v[2] >= 0 && v[3] >= 0 && (!text_ok || memcmp(v, text_val, sizeof v))) {
			memcpy(text_val, v, sizeof v);
			text_ok = 1;
			FILE *f = fopen(TEXT_STATE, "w");
			if (f) {
				fprintf(f, "%d %d %d %d\n", v[0], v[1], v[2], v[3]);
				fclose(f);
			}
			logf_("текст: общие настройки размер %d, шрифт %d, интервал %d, поля %d", v[0], v[1], v[2], v[3]);
		}
	}
	if (active && fmt && !strcmp(fmt, "UPDATE books SET last_open_time = %lld WHERE id = %d"))
		book_opens++;
	if (active && cfg.sleep_cover && fmt && !strcmp(fmt, "UPDATE books SET last_open_time = %lld WHERE id = %d")) {
		va_list cp;
		va_copy(cp, ap);
		(void)va_arg(cp, long long);
		int id = va_arg(cp, int);
		va_end(cp);
		for (int i = 0; i < 64; i++)
			if (book_ids[i].id == id && book_ids[i].path[0]) {
				sleep_cover_for(book_ids[i].path);
				break;
			}
	}
	char *r = vm(fmt, ap);
	va_end(ap);
	return r;
}

/* ---------- обложка на экране сна ---------- */

/* программа показывает во сне /oem/sleep.pgm (1024×758, кадр повёрнут на 90° против часовой).
   Штатно это ссылка на sleep_default.pgm; мод рисует свой кадр — обложка целиком, под ней
   автор, название и серия — и переводит ссылку на него. Свою картинку («Сделать заставкой») не трогает */
#define SLEEP_LINK "/oem/sleep.pgm"
#define SLEEP_OURS "luma_sleep.pgm"
#define SLEEP_FONT "/opt/ylc-ebook/fonts/NotoSans.ttf"
#define SW 758
#define SH 1024

struct sleep_job {
	char path[512], author[256], title[512], series[300];
};

static char *read_book(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	unsigned char magic[4] = {0};
	fread(magic, 1, 4, f);
	char *x = NULL;
	if (!memcmp(magic, "PK\3\4", 4)) {
		x = zip_read(f, NULL, ".fb2", 64 << 20, len);
	} else if (!fseek(f, 0, SEEK_END)) {
		long sz = ftell(f);
		if (sz > 0 && sz < (64 << 20) && (x = malloc(sz + 1))) {
			fseek(f, 0, SEEK_SET);
			*len = fread(x, 1, sz, f);
			x[*len] = 0;
		}
	}
	fclose(f);
	return x;
}

static unsigned char *unbase64(const char *p, const char *end, size_t *out)
{
	unsigned char *o = malloc((end - p) / 4 * 3 + 4);
	if (!o)
		return NULL;
	uint32_t acc = 0;
	int bits = 0;
	size_t n = 0;
	for (; p < end; p++) {
		int c = *p, v;
		if (c >= 'A' && c <= 'Z')
			v = c - 'A';
		else if (c >= 'a' && c <= 'z')
			v = c - 'a' + 26;
		else if (c >= '0' && c <= '9')
			v = c - '0' + 52;
		else if (c == '+')
			v = 62;
		else if (c == '/')
			v = 63;
		else
			continue;
		acc = acc << 6 | v;
		if ((bits += 6) >= 8) {
			bits -= 8;
			o[n++] = acc >> bits;
		}
	}
	*out = n;
	return o;
}

/* картинка обложки fb2: <coverpage><image l:href="#id"/> -> <binary id="id">base64</binary> */
static unsigned char *cover_data(const char *x, size_t *n)
{
	const char *cp = strstr(x, "<coverpage");
	const char *ce = cp ? strstr(cp, "</coverpage") : NULL;
	const char *h = cp ? strstr(cp, "href=") : NULL;
	if (!h || (ce && h > ce))
		return NULL;
	char id[256];
	char q = h[5];
	if (sscanf(h + 6 + (h[6] == '#'), q == '\'' ? "%255[^']" : "%255[^\"]", id) != 1)
		return NULL;
	for (const char *b = x; (b = strstr(b, "<binary")); b++) {
		const char *gt = strchr(b, '>');
		if (!gt)
			break;
		const char *a = strstr(b, "id=");
		if (!a || a > gt || strncmp(a + 4, id, strlen(id)) || (a[4 + strlen(id)] != '"' && a[4 + strlen(id)] != '\''))
			continue;
		const char *e = strstr(gt, "</binary");
		return e ? unbase64(gt + 1, e, n) : NULL;
	}
	return NULL;
}

/* разжать JPEG (libturbojpeg) или PNG (lodepng из LVGL) в оттенки серого */
static unsigned char *decode_gray(const unsigned char *d, size_t n, int *w, int *h)
{
	if (n > 3 && d[0] == 0xFF && d[1] == 0xD8) {
		static void *tj;
		if (!tj)
			tj = dlopen("libturbojpeg.so.0", RTLD_NOW);
		void *(*init)(void) = tj ? dlsym(tj, "tjInitDecompress") : NULL;
		int (*hdr)(void *, const unsigned char *, unsigned long, int *, int *, int *, int *) = tj ? dlsym(tj, "tjDecompressHeader3") : NULL;
		int (*dec)(void *, const unsigned char *, unsigned long, unsigned char *, int, int, int, int, int) = tj ? dlsym(tj, "tjDecompress2") : NULL;
		int (*fin)(void *) = tj ? dlsym(tj, "tjDestroy") : NULL;
		if (!init || !hdr || !dec || !fin)
			return NULL;
		void *hd = init();
		int ss, cs;
		unsigned char *px = NULL;
		if (hd && !hdr(hd, d, n, w, h, &ss, &cs) && *w > 0 && *h > 0 && *w < 8000 && *h < 8000 &&
		    (px = malloc((size_t)*w * *h)) && dec(hd, d, n, px, *w, 0, *h, 6 /* TJPF_GRAY */, 0)) {
			free(px);
			px = NULL;
		}
		if (hd)
			fin(hd);
		return px;
	}
	if (n > 8 && !memcmp(d, "\x89PNG", 4)) {
		/* упрощённый API libpng16: сразу в серый, прозрачное — на белом
		   (lodepng из LVGL 9 отдаёт свой буфер изображения, а не пиксели) */
		static void *lp;
		if (!lp)
			lp = dlopen("libpng16.so.16", RTLD_NOW);
		int (*begin)(void *, const void *, size_t) = lp ? dlsym(lp, "png_image_begin_read_from_memory") : NULL;
		int (*finish)(void *, const void *, void *, int32_t, void *) = lp ? dlsym(lp, "png_image_finish_read") : NULL;
		void (*pfree)(void *) = lp ? dlsym(lp, "png_image_free") : NULL;
		if (!begin || !finish || !pfree)
			return NULL;
		struct {
			void *opaque;
			uint32_t version, width, height, format, flags, colormap_entries, warning_or_error;
			char message[64];
		} im;
		memset(&im, 0, sizeof im);
		im.version = 1; /* PNG_IMAGE_VERSION */
		if (!begin(&im, d, n))
			return NULL;
		im.format = 0; /* PNG_FORMAT_GRAY */
		static const unsigned char white[3] = {255, 255, 255};
		unsigned char *px = im.width < 8000 && im.height < 8000 ? malloc((size_t)im.width * im.height) : NULL;
		if (!px || !finish(&im, white, px, 0, NULL)) {
			free(px);
			pfree(&im);
			return NULL;
		}
		*w = im.width;
		*h = im.height;
		return px;
	}
	return NULL;
}

/* вписать src (sw×sh) в прямоугольник canvas: усреднение при уменьшении, билинейно при увеличении */
static void blit_fit(unsigned char *cv, int stride, const unsigned char *src, int sw, int sh, int bx, int by, int bw, int bh)
{
	double k = fmin((double)bw / sw, (double)bh / sh);
	int dw = sw * k, dh = sh * k, ox = bx + (bw - dw) / 2, oy = by + (bh - dh) / 2;
	for (int y = 0; y < dh; y++)
		for (int x = 0; x < dw; x++) {
			double fx0 = x / k, fx1 = (x + 1) / k, fy0 = y / k, fy1 = (y + 1) / k;
			int v;
			if (k < 1) {
				int x0 = fx0, x1 = fx1, y0 = fy0, y1 = fy1;
				if (x1 <= x0)
					x1 = x0 + 1;
				if (y1 <= y0)
					y1 = y0 + 1;
				unsigned sum = 0, cnt = 0;
				for (int yy = y0; yy < y1 && yy < sh; yy++)
					for (int xx = x0; xx < x1 && xx < sw; xx++)
						sum += src[yy * sw + xx], cnt++;
				v = cnt ? sum / cnt : 255;
			} else {
				double sx = fmin(fx0, sw - 1.001), sy = fmin(fy0, sh - 1.001);
				int ix = sx, iy = sy;
				double ax = sx - ix, ay = sy - iy;
				int ix1 = ix + 1 < sw ? ix + 1 : ix, iy1 = iy + 1 < sh ? iy + 1 : iy;
				v = (src[iy * sw + ix] * (1 - ax) + src[iy * sw + ix1] * ax) * (1 - ay) +
				    (src[iy1 * sw + ix] * (1 - ax) + src[iy1 * sw + ix1] * ax) * ay;
			}
			cv[(oy + y) * stride + ox + x] = v;
		}
}

/* текст через FreeType (на книге есть libfreetype.so.6). Поля структур — по заголовкам FreeType 2.x
   для 32-битного ARM; проверяем, что слот глифа ссылается на свой face и битмап в оттенках серого */
static struct {
	void *face;
	int (*load)(void *, unsigned long, int32_t);
	int (*px)(void *, unsigned, unsigned);
	int ok;
} ft;

static int ft_open(void)
{
	if (ft.ok)
		return ft.ok > 0;
	ft.ok = -1;
	void *h = dlopen("libfreetype.so.6", RTLD_NOW);
	int (*init)(void **) = h ? dlsym(h, "FT_Init_FreeType") : NULL;
	int (*nf)(void *, const char *, long, void **) = h ? dlsym(h, "FT_New_Face") : NULL;
	ft.load = h ? dlsym(h, "FT_Load_Char") : NULL;
	ft.px = h ? dlsym(h, "FT_Set_Pixel_Sizes") : NULL;
	void *lib;
	if (!init || !nf || !ft.load || !ft.px || init(&lib) || nf(lib, SLEEP_FONT, 0, &ft.face))
		return 0;
	ft.ok = 1;
	return 1;
}

static unsigned char *ft_glyph(uint32_t ch, int *w, int *rows, int *pitch, int *left, int *top, int *adv)
{
	if (ft.load(ft.face, ch, 4 /* FT_LOAD_RENDER */))
		return NULL;
	char *g = *(char **)((char *)ft.face + 84);
	if (!g || *(void **)(g + 4) != ft.face)
		return NULL;
	*rows = *(unsigned *)(g + 76);
	*w = *(unsigned *)(g + 80);
	static unsigned char none;
	if (!*rows || !*w) { /* пробел: картинки нет, только сдвиг */
		*rows = *w = 0;
		*left = *top = *pitch = 0;
		*adv = *(long *)(g + 64) >> 6;
		return &none;
	}
	if (*(unsigned char *)(g + 94) != 2 /* FT_PIXEL_MODE_GRAY */)
		return NULL;
	*pitch = *(int *)(g + 84);
	*left = *(int *)(g + 100);
	*top = *(int *)(g + 104);
	*adv = *(long *)(g + 64) >> 6;
	return *(unsigned char **)(g + 88);
}

static uint32_t utf8_next(const char **p)
{
	const unsigned char *s = (const unsigned char *)*p;
	uint32_t c = *s++;
	if (c >= 0xF0 && s[0] && s[1] && s[2])
		c = (c & 7) << 18 | (s[0] & 63) << 12 | (s[1] & 63) << 6 | (s[2] & 63), s += 3;
	else if (c >= 0xE0 && s[0] && s[1])
		c = (c & 15) << 12 | (s[0] & 63) << 6 | (s[1] & 63), s += 2;
	else if (c >= 0xC0 && s[0])
		c = (c & 31) << 6 | (s[0] & 63), s += 1;
	*p = (const char *)s;
	return c;
}

static int text_px(const char *s, int len)
{
	int x = 0, w, r, p, l, t, a;
	for (const char *q = s; q < s + len && *q;)
		if (ft_glyph(utf8_next(&q), &w, &r, &p, &l, &t, &a))
			x += a;
	return x;
}

static void draw_line(unsigned char *cv, const char *s, int len, int y, int size)
{
	int x = (SW - text_px(s, len)) / 2, w, rows, pitch, l, t, a;
	for (const char *q = s; q < s + len && *q;) {
		unsigned char *b = ft_glyph(utf8_next(&q), &w, &rows, &pitch, &l, &t, &a);
		if (!b)
			continue;
		for (int yy = 0; yy < rows; yy++)
			for (int xx = 0; xx < w; xx++) {
				int px = x + l + xx, py = y + size - t + yy;
				if (px < 0 || px >= SW || py < 0 || py >= SH)
					continue;
				int al = b[yy * pitch + xx];
				cv[py * SW + px] = cv[py * SW + px] * (255 - al) / 255;
			}
		x += a;
	}
}

/* разбить s на строки не шире maxw (по словам), не больше max строк; вернуть их число */
static int wrap(const char *s, int maxw, int max, const char **st, int *ln)
{
	int n = 0;
	while (*s && n < max) {
		while (*s == ' ')
			s++;
		const char *e = s, *best = NULL;
		while (*e) {
			const char *ne = e;
			while (*ne == ' ')
				ne++;
			while (*ne && *ne != ' ')
				ne++;
			if (text_px(s, ne - s) > maxw && best)
				break;
			best = ne;
			e = ne;
			if (text_px(s, ne - s) > maxw)
				break;
		}
		if (!best)
			break;
		st[n] = s;
		ln[n++] = best - s;
		s = best;
	}
	return n;
}

/* обложка книги в оттенках серого (или NULL) */
static unsigned char *book_cover(const char *path, int *w, int *h)
{
	size_t len = 0, cn = 0;
	char *x = read_book(path, &len);
	unsigned char *cd = x ? cover_data(x, &cn) : NULL;
	free(x);
	unsigned char *px = cd ? decode_gray(cd, cn, w, h) : NULL;
	free(cd);
	return px;
}

/* ---------- миниатюры обложек ---------- */

/* программа сохраняет миниатюру (180×240) с экрана, пока обложка ещё дорисовывается
   (0 → ~20% → ~60% → 100%), и в меню остаётся верхняя полоска. Мод рисует миниатюру сам из
   обложки в книге: <base>/.system/thumbnails/<путь книги>.png <- <base>/<путь книги> */
static int thumb_book(const char *thumb, char *book, size_t n)
{
	const char *t = strstr(thumb, "/.system/thumbnails/");
	size_t tl = strlen(thumb);
	if (!t || tl < 5 || strcmp(thumb + tl - 4, ".png"))
		return 0;
	snprintf(book, n, "%.*s/%.*s", (int)(t - thumb), thumb, (int)(tl - 4 - (t + 20 - thumb)), t + 20);
	return access(book, R_OK) == 0;
}

static int png_gray_write(const char *path, const unsigned char *px, int w, int h)
{
	static void *lp;
	if (!lp)
		lp = dlopen("libpng16.so.16", RTLD_NOW);
	int (*wr)(void *, const char *, int, const void *, int32_t, const void *) = lp ? dlsym(lp, "png_image_write_to_file") : NULL;
	if (!wr)
		return 0;
	struct {
		void *opaque;
		uint32_t version, width, height, format, flags, colormap_entries, warning_or_error;
		char message[64];
	} im;
	memset(&im, 0, sizeof im);
	im.version = 1;
	im.width = w;
	im.height = h;
	im.format = 0; /* PNG_FORMAT_GRAY */
	char tmp[600];
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	if (!wr(&im, tmp, 0, px, 0, NULL))
		return 0;
	return rename(tmp, path) == 0;
}

static int thumb_make(const char *thumb, int tw, int th)
{
	char book[600];
	if (!thumb_book(thumb, book, sizeof book))
		return 0;
	int w, h;
	unsigned char *px = book_cover(book, &w, &h);
	if (!px)
		return 0;
	unsigned char *cv = malloc((size_t)tw * th);
	int ok = 0;
	if (cv) {
		memset(cv, 255, (size_t)tw * th);
		blit_fit(cv, tw, px, w, h, 0, 0, tw, th);
		ok = png_gray_write(thumb, cv, tw, th);
	}
	free(cv);
	free(px);
	return ok;
}

unsigned lodepng_encode_file(const char *filename, const unsigned char *image, unsigned w, unsigned h, int colortype, unsigned bitdepth)
{
	static unsigned (*real)(const char *, const unsigned char *, unsigned, unsigned, int, unsigned);
	if (!real)
		real = dlsym(RTLD_NEXT, "lodepng_encode_file");
	if (active && cfg.thumbs && filename && w && h && w < 2000 && h < 2000 && thumb_make(filename, w, h)) {
		logf_("миниатюра из обложки: %s", filename);
		return 0;
	}
	return real(filename, image, w, h, colortype, bitdepth);
}

/* один раз пересоздать уже сохранённые (обрезанные) миниатюры */
#define THUMBS_DONE LUMA_DIR "/thumbs.v1"
static int thumbs_walk(const char *dir)
{
	DIR *d = opendir(dir);
	if (!d)
		return 0;
	int n = 0;
	struct dirent *de;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		char p[600];
		snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
		struct stat st;
		if (stat(p, &st))
			continue;
		if (S_ISDIR(st.st_mode))
			n += thumbs_walk(p);
		else if (strlen(p) > 4 && !strcmp(p + strlen(p) - 4, ".png") && thumb_make(p, 180, 240))
			n++;
	}
	closedir(d);
	return n;
}

static void mkdirs(char *p)
{
	for (char *s = strchr(p + 1, '/'); s; s = strchr(s + 1, '/')) {
		*s = 0;
		mkdir(p, 0777);
		*s = '/';
	}
}

/* у книг, которые ещё не открывали, программа миниатюр не делает — рисуем по списку книг мода */
static int thumbs_missing(void)
{
	FILE *c = fopen(CACHE_PATH, "r");
	if (!c)
		return 0;
	char line[2048], t[700];
	int n = 0;
	while (fgets(line, sizeof line, c)) {
		char *tab1 = strchr(line, '\t');
		if (!tab1 || strncmp(line, "/userdata/", 10))
			continue;
		*tab1 = 0;
		snprintf(t, sizeof t, "/userdata/.system/thumbnails/%s.png", line + 10);
		if (access(t, F_OK) == 0)
			continue;
		mkdirs(t);
		if (thumb_make(t, 180, 240))
			n++;
	}
	fclose(c);
	return n;
}

static struct sortinfo *sort_of(const char *path);

/* заранее разобрать авторов всех книг, чтобы вид «Автор» открывался сразу */
static void sort_warm(void)
{
	FILE *c = fopen(CACHE_PATH, "r");
	if (!c)
		return;
	char line[2048];
	while (fgets(line, sizeof line, c)) {
		char *t = strchr(line, '\t');
		if (t) {
			*t = 0;
			sort_of(line);
		}
	}
	fclose(c);
}

static int thumbs_busy;

static void *thumbs_thread(void *arg)
{
	if (access(THUMBS_DONE, F_OK)) {
		int n = thumbs_walk("/userdata/.system/thumbnails");
		FILE *f = fopen(THUMBS_DONE, "w");
		if (f)
			fclose(f);
		logf_("миниатюры: пересоздано %d", n);
	}
	int m = cfg.thumbs ? thumbs_missing() : 0;
	if (cfg.sort_books)
		sort_warm();
	if (m)
		logf_("миниатюры: новых %d", m);
	thumbs_busy = 0;
	return NULL;
}

static void thumbs_start(void)
{
	if (thumbs_busy)
		return;
	thumbs_busy = 1;
	thumbs_dirty = 0;
	pthread_t t;
	if (pthread_create(&t, NULL, thumbs_thread, NULL))
		thumbs_busy = 0;
	else
		pthread_detach(t);
}

static void sleep_draw(struct sleep_job *j);

static void *sleep_thread(void *arg)
{
	/* FreeType и общий кадр — по одному потоку за раз */
	static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
	pthread_mutex_lock(&mu);
	sleep_draw(arg);
	pthread_mutex_unlock(&mu);
	return NULL;
}

static void sleep_draw(struct sleep_job *j)
{
	unsigned char *cv = malloc(SW * SH), *rot = malloc(SW * SH + 32);
	size_t len = 0, cn = 0;
	char *x = read_book(j->path, &len);
	unsigned char *cd = x ? cover_data(x, &cn) : NULL;
	free(x);
	int w = 0, h = 0;
	unsigned char *px = cd ? decode_gray(cd, cn, &w, &h) : NULL;
	free(cd);
	if (!cv || !rot || !ft_open()) {
		logf_("сон: нет памяти или шрифта %s", SLEEP_FONT);
		goto out;
	}
	memset(cv, 255, SW * SH);
	/* на экране сна автор полностью: Имя Отчество Фамилия */
	{
		char t[512], a[256], se[256], nu[32];
		if (book_meta(j->path, "full", t, sizeof t, a, sizeof a, se, sizeof se, nu, sizeof nu) && a[0])
			copy(j->author, sizeof j->author, a);
	}
	/* подпись снизу: автор, название (до 3 строк), серия */
	static const int sz[3] = {30, 40, 28};
	const char *txt[3] = {j->author, j->title, j->series}, *st[8];
	int ln[8], size[8], n = 0, th = 0;
	for (int k = 0; k < 3; k++) {
		if (!txt[k][0])
			continue;
		ft.px(ft.face, 0, sz[k]);
		int m = wrap(txt[k], SW - 80, k == 1 ? 3 : 1, st + n, ln + n);
		for (int i = 0; i < m; i++)
			size[n + i] = sz[k], th += sz[k] * 13 / 10;
		n += m;
	}
	int ty = SH - 40 - th;
	if (px)
		blit_fit(cv, SW, px, w, h, 40, 40, SW - 80, ty - 30 - 40);
	for (int i = 0; i < n; i++) {
		ft.px(ft.face, 0, size[i]);
		draw_line(cv, st[i], ln[i], ty, size[i]);
		ty += size[i] * 13 / 10;
	}
	/* кадр в файле повёрнут: L(x, y) = P(SW-1-y, x), 1024×758 */
	int hl = snprintf((char *)rot, 32, "P5\n%d %d\n255\n", SH, SW);
	for (int y = 0; y < SW; y++)
		for (int xx = 0; xx < SH; xx++)
			rot[hl + y * SH + xx] = cv[xx * SW + (SW - 1 - y)];
	FILE *f = fopen("/oem/" SLEEP_OURS ".tmp", "wb");
	if (f) {
		fwrite(rot, 1, hl + SW * SH, f);
		fclose(f);
		rename("/oem/" SLEEP_OURS ".tmp", "/oem/" SLEEP_OURS);
		char cur[256] = "";
		ssize_t k = readlink(SLEEP_LINK, cur, sizeof cur - 1);
		cur[k > 0 ? k : 0] = 0;
		if (k > 0 && (!strcmp(cur, "sleep_default.pgm") || !strcmp(cur, "/oem/sleep_default.pgm"))) {
			unlink(SLEEP_LINK);
			symlink(SLEEP_OURS, SLEEP_LINK);
		}
		sync();
		logf_("сон: %s (обложка %dx%d%s), ссылка %s", j->title, w, h, px ? "" : " не найдена", k > 0 ? cur : "не ссылка");
		FILE *sf = fopen(SLEEP_SRC, "w");
		if (sf) {
			fputs(j->path, sf);
			fclose(sf);
		}
	}
out:
	free(px);
	free(cv);
	free(rot);
	free(j);
}

static void sleep_cover_for(const char *path)
{
	static char last[512];
	if (!last[0]) {
		FILE *f = fopen(SLEEP_SRC, "r");
		if (f) {
			if (!fgets(last, sizeof last, f))
				last[0] = 0;
			fclose(f);
		}
	}
	if (!strcmp(last, path))
		return;
	copy(last, sizeof last, path);
	struct entry *e = NULL;
	for (size_t i = 0; i < tab_cap && !e; i++)
		if (tab[i].key && tab[i].path && !strcmp(tab[i].path, path))
			e = &tab[i];
	struct sleep_job *j = calloc(1, sizeof *j);
	if (!j)
		return;
	copy(j->path, sizeof j->path, path);
	if (e && display_for(e)) {
		copy(j->author, sizeof j->author, e->author);
		copy(j->title, sizeof j->title, e->title);
		if (e->series[0])
			snprintf(j->series, sizeof j->series, e->num[0] ? "%s %s" : "%s", e->series, e->num);
	} else {
		const char *b = strrchr(path, '/');
		copy(j->title, sizeof j->title, b ? b + 1 : path);
	}
	pthread_t t;
	if (pthread_create(&t, NULL, sleep_thread, j))
		free(j);
	else
		pthread_detach(t);
}

/* ---------- старт ---------- */

/* падение программы: где именно (адреса в программе и в моде) — в журнал, дальше как обычно */
static void crash_handler(int sig, siginfo_t *si, void *uc_)
{
#ifdef __arm__
	ucontext_t *uc = uc_;
	uintptr_t pc = uc->uc_mcontext.arm_pc, lr = uc->uc_mcontext.arm_lr;
	Dl_info a = {0}, b = {0};
	dladdr((void *)pc, &a);
	dladdr((void *)lr, &b);
	logf_("ПАДЕНИЕ: сигнал %d, адрес %p, pc %s+%#lx, lr %s+%#lx", sig, si->si_addr,
		a.dli_fname ? a.dli_fname : "?", (unsigned long)(pc - (uintptr_t)a.dli_fbase),
		b.dli_fname ? b.dli_fname : "?", (unsigned long)(lr - (uintptr_t)b.dli_fbase));
#else
	(void)si; (void)uc_;
	logf_("ПАДЕНИЕ: сигнал %d", sig);
#endif
	signal(sig, SIG_DFL);
	raise(sig);
}

__attribute__((constructor)) static void luma_init(void)
{
	/* LD_PRELOAD наследуют все команды, которые программа запускает (system, popen) —
	   им мод не нужен: убираем переменную и в чужих процессах ничего не делаем */
	char exe[256];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
	exe[n > 0 ? n : 0] = 0;
	unsetenv("LD_PRELOAD");
	if (n > 0 && !strstr(exe, "ylc-ebook"))
		return;
	resolve();
	if (!real_label_set_text) {
		void *h = dlopen("liblvgl.so.9", RTLD_NOW | RTLD_NOLOAD);
		if (h) {
			real_label_set_text = dlsym(h, "lv_label_set_text");
			real_label_set_text_static = dlsym(h, "lv_label_set_text_static");
		}
	}
	load_ini();
	active = 1;
	list_w = read_int(LUMA_DIR "/list.width");
	if (list_w < 0)
		list_w = 0;
	if (cfg.thumbs || cfg.sort_books)
		thumbs_start();
	logf_("libluma запущен");
	struct sigaction sa = {0};
	sa.sa_sigaction = crash_handler;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);

	patch_margins();
}
