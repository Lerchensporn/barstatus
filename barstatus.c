#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <signal.h>
#include <alsa/asoundlib.h>
#include <mpd/client.h>
#include <libnotify/notify.h>
#include <xcb/xcb.h>
#include <err.h>

#define SOCKET_PATH_TPL "/tmp/bspwm:%i-socket"
#define SOCKET_ENV_VAR  "BSPWM_SOCKET"

enum {
	MSG_SUCCESS,
	MSG_FAILURE,
	MSG_SYNTAX,
	MSG_UNKNOWN,
	MSG_SUBSCRIBE,
	MSG_LENGTH
};

#define COLOR_DARK     "#555555"
#define COLOR_CRITICAL "#FF0000"

#define COLOR_DEFAULT_FG "#FFA3A6AB"
#define COLOR_DEFAULT_BG "#FF34322E"
#define FORMAT_RESET     "%%{F-}%%{B-}%%{U-}"

#define FORMAT_FOCUSED_FREE     "%%{F#FFF6F9FF}%%{B#FF6D561C}"
#define FORMAT_FOCUSED_URGENT   "%%{F#FF34322E}%%{B#FFF9A299}"
#define FORMAT_FOCUSED_OCCUPIED "%%{F#FFF6F9FF}%%{B#FF6D561C}"
#define FORMAT_FREE             "%%{F#FF6F7277}%%{B#FF34322E}"
#define FORMAT_URGENT           "%%{F#FFF9A299}%%{B#FF34322E}"
#define FORMAT_OCCUPIED         "%%{F#FFA2A6AB}%%{B#FF34322E}"
#define FORMAT_LAYOUT           "%%{F#FFA3A6AB}%%{B#FF34322E}"

#define BATLOW 5
#define BATFULL 50

static bool running = true;
static FILE *out;

static pthread_mutex_t mutex;

static struct {
	char wm_string[1024];
	char mpd[64];
	char volume[16];
	char date[32];
	char battery[32];
	bool modified;
} bar_data;

/* Update the bar. Called by the threads. */
static void print_to_bar()
{
	pthread_mutex_lock(&mutex);

	fprintf(out, "%%{" FORMAT_RESET "}%%{l}%s %%{r} %s  %s  %s  %s\n",
		bar_data.wm_string, bar_data.mpd, bar_data.volume, bar_data.battery, bar_data.date);
	fflush(out);
	bar_data.modified = false;

	pthread_mutex_unlock(&mutex);
}

static void escape_quotes_nl(char *str)
{
	int i, k;
	char *tmp;

	tmp = strdup(str);
	for (i = 0, k = 0; str[i]; ++i) {
		if (str[i] != '"' && str[i] != '\n') {
			tmp[k++] = str[i];
		}
	}

	strncpy(str, tmp, k);
	str[k] = '\0';
	free(tmp);
}

static void * tfunc_mpd(void *data)
{
	char mpdbuf[sizeof bar_data.mpd];
	char *icon;
	struct mpd_connection *conn;
	bool first_run = true;
	enum mpd_idle idle;

	conn = mpd_connection_new(NULL, 0, 0);
	if (!conn) {
		return 0;
	}

	while (running) {
		struct mpd_song *song;
		struct mpd_status *status;

		if (!first_run) {
			idle = mpd_run_idle_mask(conn, MPD_IDLE_PLAYER);
			if (!idle) {
				puts(mpd_connection_get_error_message(conn));
				sleep(1);
				break;
			} else if (!(idle & MPD_IDLE_PLAYER)) {
				continue;
			}
		}

		first_run = false;

		song = mpd_run_current_song(conn);
		if (!song) {
			continue;
		}

		status = mpd_run_status(conn);
		if (!status) {
			mpd_song_free(song);
			continue;
		}

		switch (mpd_status_get_state(status)) {
		case MPD_STATE_PLAY:
			icon = "\uE09A";
			break;
		case MPD_STATE_PAUSE:
			icon = "\uE09B";
			break;
		default:
			icon = NULL;
		}

		if (icon) {
			snprintf(mpdbuf, sizeof mpdbuf, "%s %s - %s", icon,
				mpd_song_get_tag(song, MPD_TAG_ARTIST, 0), mpd_song_get_tag(song, MPD_TAG_TITLE, 0));
			escape_quotes_nl(mpdbuf);
		} else {
			mpdbuf[0] = 0;
		}

		mpd_status_free(status);
		mpd_song_free(song);

		if (strcmp(bar_data.mpd, mpdbuf)) {
			pthread_mutex_lock(&mutex);
			strcpy(bar_data.mpd, mpdbuf);
			pthread_mutex_unlock(&mutex);
			print_to_bar();
		}
	}

	mpd_connection_free(conn);
}

static void update_date()
{
	time_t now;
	char datebuf[sizeof bar_data.date];

	now = time(NULL);
	strftime(datebuf, sizeof datebuf, "\uE015 %d.%m.%Y %H:%M ", localtime(&now));
	if (strcmp(bar_data.date, datebuf)) {
		bar_data.modified = true;
		strcpy(bar_data.date, datebuf);
	}
}

static void update_battery()
{
	static int is_battery_ok = 1;
	FILE *fp;
	int percentage, online;
	char batbuf[4];
	char buffer[sizeof bar_data.battery];

	fp = fopen("/sys/class/power_supply/ADP1/online", "r");
	if (!fp) {
		return;
	}
	fread(batbuf, 1, sizeof batbuf, fp);
	fclose(fp);

	online = (batbuf[0] == '1');

	fp = fopen("/sys/class/power_supply/BAT0/capacity", "r");
	if (!fp) {
		return;
	}
	fread(batbuf, 1, sizeof batbuf, fp);
	fclose(fp);

	percentage = atoi(batbuf);
	if (percentage > 100) {
		percentage = 100;
	}
	snprintf(batbuf, sizeof batbuf, "%d", percentage);
	if (online) {
		snprintf(buffer, sizeof buffer, "\uE041 %s", batbuf);
		is_battery_ok = 1;
	} else if (percentage <= BATLOW) {
		snprintf(buffer, sizeof buffer, "\uE031 %s", batbuf);

		if (is_battery_ok) {
			NotifyNotification *msg;
			msg = notify_notification_new("Battery low!", "Battery low!", "dialog-critical");
			notify_notification_show(msg, NULL);
		}

		is_battery_ok = 0;
	} else if (percentage < BATFULL) {
		snprintf(buffer, sizeof buffer, "\uE032 %s", batbuf);
		is_battery_ok = 1;
	} else {
		snprintf(buffer, sizeof buffer, "\uE033 %s", batbuf);
		is_battery_ok = 1;
	}

	if (strcmp(bar_data.battery, buffer)) {
		bar_data.modified = true;
		strcpy(bar_data.battery, buffer);
	}
}

/* Thread to update the time in a fixed interval. */
static void * tfunc_timer(void *data)
{
	while (running) {
		pthread_mutex_lock(&mutex);
		update_date();
		update_battery();
		pthread_mutex_unlock(&mutex);
		if (bar_data.modified) {
			print_to_bar();
		}
		sleep(2);
	}
}

static int alsa_elem_callback(snd_mixer_elem_t *elem, unsigned int mask)
{
	static long old_volume = -1, old_unmuted = -1;
	long min, max, volume;
	int unmuted;

	snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
	snd_mixer_selem_get_playback_volume(elem, 0, &volume);
	snd_mixer_selem_get_playback_switch(elem, SND_MIXER_SCHN_FRONT_LEFT, &unmuted);

	volume = 100 * (volume - min) / (max - min);

	if (old_volume != volume || old_unmuted != unmuted) {
		pthread_mutex_lock(&mutex);
		snprintf(bar_data.volume, sizeof bar_data.volume, "%s %d", unmuted ? "\uE04E" : "\uE04F", volume);
		pthread_mutex_unlock(&mutex);
		old_volume = volume;
		old_unmuted = unmuted;
		print_to_bar();
	}
}

static void * tfunc_alsa(void *data)
{
	struct pollfd *pollfds = NULL;
	int nfds = 0;
	int n;
	int err;
	snd_mixer_elem_t *elem;
	snd_mixer_t *mixer;
	snd_mixer_selem_id_t *sid;
	unsigned short revents;

	if (snd_mixer_open(&mixer, 0)) {
		return 0;
	}
	if (snd_mixer_attach(mixer, "default")) {
		snd_mixer_close(mixer);
		return 0;
	}
	if (snd_mixer_selem_register(mixer, NULL, NULL)) {
		snd_mixer_close(mixer);
		return 0;
	}
	if (snd_mixer_load(mixer)) {
		snd_mixer_close(mixer);
		return 0;
	}
	snd_mixer_selem_id_alloca(&sid);
	snd_mixer_selem_id_set_index(sid, 0);
	snd_mixer_selem_id_set_name(sid, "Master");
	elem = snd_mixer_find_selem(mixer, sid);
	if (!elem) {
		snd_mixer_close(mixer);
		return 0;
	}

	alsa_elem_callback(elem, 0);
	snd_mixer_elem_set_callback(elem, alsa_elem_callback);

	nfds = snd_mixer_poll_descriptors_count(mixer);
	pollfds = malloc(nfds * sizeof *pollfds);

	err = snd_mixer_poll_descriptors(mixer, pollfds, nfds);
	if (err < 0) {
		printf("Error");
		return NULL;
	}

	while (running) {
		n = poll(pollfds, nfds, -1);
		if (n < 0) {
			if (errno == EINTR) {
		puts("POLL");
				pollfds[0].revents = 0;
			} else {
				printf("poll error");
				break;
			}
		}

		if (n > 0) {
			err = snd_mixer_poll_descriptors_revents(mixer, pollfds, nfds, &revents);
			if (err < 0) {
				printf("cannot get poll events");
				break;
			}
			if (revents & (POLLERR | POLLNVAL)) {
				printf("poll error");
				break;
			} else if (revents & POLLIN) {
				snd_mixer_handle_events(mixer);
			}
		}
	}

	free(pollfds);
	snd_mixer_detach(mixer, "default");
	snd_mixer_close(mixer);
}

/* Translates the window manager info to bar markup. */
static void parse_wm(const char *buffer)
{
	const char *end;
	const char *start;
	int len, catlen;
	char line[64];

	if (!*buffer) {
		return;
	}

	pthread_mutex_lock(&mutex);

	start = buffer;
	len = 0;
	bar_data.wm_string[0] = 0;
	do {
		end = strchr(start, ':');
		if (!end) {
			end = &start[strlen(start)];
		}
		switch (*start) {
		case 'W': // active monitor
			break;
		case 'w': // inactive monitor
			break;
		case 'O': // focused occupied desktop
			len += snprintf(bar_data.wm_string + len, sizeof bar_data.wm_string - len,
				FORMAT_FOCUSED_OCCUPIED " %.*s ", end - start - 1, &start[1]);
			break;
		case 'F': // focused free desktop
			len += snprintf(bar_data.wm_string + len, sizeof bar_data.wm_string - len,
				FORMAT_FOCUSED_FREE " %.*s ", end - start - 1, &start[1]);
			break;
		case 'U': // focused urgent desktop
			len += snprintf(bar_data.wm_string + len, sizeof bar_data.wm_string - len,
				FORMAT_FOCUSED_URGENT " %.*s ", end - start - 1, &start[1]);
			break;
		case 'o': // occupied desktop
			len += snprintf(bar_data.wm_string + len, sizeof bar_data.wm_string - len,
				FORMAT_OCCUPIED " %.*s ", end - start - 1, &start[1]);
			break;
		case 'f': // free desktop
//			len += snprintf(bar_data.wm_string + len, sizeof bar_data.wm_string - len,
//				FORMAT_FREE " %.*s ", end - start - 1, &start[1]);
			break;
		case 'u': // urgent desktop
			len += snprintf(bar_data.wm_string + len, sizeof bar_data.wm_string - len,
				FORMAT_URGENT " %.*s ", end - start - 1, &start[1]);
			break;
		case 'L': // layout
			len += snprintf(bar_data.wm_string + len, sizeof bar_data.wm_string - len,
				FORMAT_LAYOUT "   %.*s ", end - start - 1, &start[1]);
			break;
		default:
			break;
		}

		if (len >= sizeof bar_data.wm_string) {
			snprintf(bar_data.wm_string, sizeof bar_data.wm_string, "Error");
			printf("Error: Workspace buffer too small.\n");
			fflush(stdout);
			break;
		}

		start = end + 1;
	} while (*end);

	pthread_mutex_unlock(&mutex);
}

/* Thread that listens for window manager changes. */
static void * tfunc_wm(void *data)
{
	int fd;
	struct sockaddr_un sock_address;
	char msg[BUFSIZ], rsp[BUFSIZ];
	sock_address.sun_family = AF_UNIX;
	char *sp;
	const char cmd[] = "control\0--subscribe";
	int ret = 0, nb;

	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
		err(1, "Failed to create the socket.\n");
	}

	sp = getenv(SOCKET_ENV_VAR);
	if (sp != NULL) {
		snprintf(sock_address.sun_path, sizeof sock_address.sun_path, "%s", sp);
	} else {
		char *host = NULL;
		int dn = 0, sn = 0;
		if (xcb_parse_display(NULL, &host, &dn, &sn) != 0) {
			snprintf(sock_address.sun_path, sizeof sock_address.sun_path, SOCKET_PATH_TPL, dn);
		}
		free(host);
	}

	if (connect(fd, (struct sockaddr *) &sock_address, sizeof sock_address) == -1) {
		err(1, "Failed to connect to the socket.\n");
	}

	if (send(fd, cmd, sizeof cmd, 0) == -1) {
		err(1, "Failed to send the data.\n");
	}

	while (running && (nb = recv(fd, rsp, sizeof rsp, 0)) > 0) {
		if (nb == 1 && rsp[0] < MSG_LENGTH) {
			ret = rsp[0];
			if (ret == MSG_UNKNOWN) {
				warn("Unknown command.\n");
			} else if (ret == MSG_SYNTAX) {
				warn("Invalid syntax.\n");
			}
		} else {
			int end = MIN(nb, (int) sizeof(rsp) - 1);
			rsp[end--] = '\0';
			while (end >= 0 && isspace(rsp[end])) {
				rsp[end--] = '\0';
			}
			parse_wm(rsp);
			print_to_bar();
		}
	}
	close(fd);
}

int main()
{
	pthread_t thread_wm, thread_mpd, thread_timer, thread_alsa;
	int pin[2];
	pid_t pid;

	if (pipe(pin)) {
		perror("pipe");
		return EXIT_FAILURE;
	}

	pid = fork();
	if (pid < 0) {
		close(pin[0]);
		close(pin[1]);
		perror("fork");
		return EXIT_FAILURE;
	} else if (!pid) {
		close(pin[1]);
		dup2(pin[0], STDIN_FILENO);
		return execlp("bar", "bar", "-gx14", "-f",
			"-*-terminus-*-*-*-*-*-*-*-*-*-*-*-*,-*-stlarch-*-*-*-*-*-*-*-*-*-*-*-*",
			"-F" COLOR_DEFAULT_FG, "-B" COLOR_DEFAULT_BG, NULL);
	}

	close(pin[0]);
	out = fdopen(pin[1], "w");

	notify_init("barstatus");

	pthread_create(&thread_wm, NULL, tfunc_wm, NULL);
	pthread_create(&thread_mpd, NULL, tfunc_mpd, NULL);
	pthread_create(&thread_timer, NULL, tfunc_timer, NULL);
	pthread_create(&thread_alsa, NULL, tfunc_alsa, NULL);

	pause();

	running = false;

	pthread_cancel(thread_wm);
	pthread_cancel(thread_mpd);
	pthread_cancel(thread_timer);
	pthread_cancel(thread_alsa);

	pthread_join(thread_wm, NULL);
	pthread_join(thread_mpd, NULL);
	pthread_join(thread_timer, NULL);
	pthread_join(thread_alsa, NULL);

	fclose(out);
	close(pin[1]);

	return EXIT_SUCCESS;
}

