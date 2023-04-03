/*
 * main.c
 *
 *  Created on: 2023年3月27日
 *      Author: yui
 */


#include <unistd.h>
//#include <mqueue.h>
#include <getopt.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <libconfig.h>

#define MQ_MODE        0666
#define MAX_LINE_SIZE  4096
#ifdef WIN32
#define LIST_FILE   "mq.list"
#define CONFIG_FILE "mq.conf"
struct mq_attr {
   long mq_flags;       /* Flags: 0 or O_NONBLOCK */
   long mq_maxmsg;      /* Max. # of messages on queue */
   long mq_msgsize;     /* Max. message size (bytes) */
   long mq_curmsgs;     /* # of messages currently in queue */
};
typedef int mqd_t;
struct mq_attr;
int mq_open(const char * name, int flags, mode_t mode, struct mq_attr *attr) { return 0;}
int mq_close(mqd_t mq) { return 0; }
int mq_unlink (const char *name) { return 0; }
int mq_getattr(mqd_t mq, struct mq_attr*attr) { return 0; }
#else
#define LIST_FILE   "/run/mq.list"
#define CONFIG_FILE "/etc/mq/mq.conf"
#include <mqueue.h>
#endif

enum CMD {
	create,
	delete,
	list,
	info,
	help
};

inline static char *get_program_name(char *prog_path) {
	char *ret = strrchr(prog_path, '/');
	if (ret == NULL)
		return prog_path;
	return ret + 1;
}

static void usage(int argc, char *argv[], int ret) {
	printf("tool to manipulate message queues.\n"
		   "Usage: %s <command>\n"
		   "commands:\n"
		   "  -c --create NAME        create a messqge queue with its name as 'NAME'\n"
		   "  -d --delete [NAME]      delete a specified message queue\n"
		   "  --info NAME             show details of a specified message queue\n"
		   "  -l --list               list all message queues\n"
		   "  -h --help               display this message\n"
		   "option for create command:\n"
		   "  -s --size SIZE          specify the max message-size of a message queue\n"
		   "  --max                   specify the maximum messages of this message queue\n"
		   "  --config CONFIG_FILE    specify config file\n"
		   "option for list and delete command:\n"
		   "  -a --all                select all message queues\n",
		   get_program_name(argv[0]));
	exit(ret);
}

/*
 * Automatically append a '\n' to the written string.
 * Return how many byte successfully appended in the file, otherwise, return -1;
 */
int file_append_line(const char *path, const char *txt) {
	int ret = -1;
	FILE *fp = fopen(path, "a+");
	if (fp == NULL)
		return ret;
	ret = fprintf(fp, "%s\n", txt);
	fclose(fp);
	return ret;
}

/* Make sure maximum length of each line of the file must be less than MAX_LINE_SIZE,
 * otherwise, all those contents greater than MAX_LINE_SIZE in that line will be ignored.
 * Return how many lines deleted on success, otherwise, return -1;
 */
int file_delete_line(const char *path, const char *pattern, int flags) {
	int lines = 0;
	regex_t reg;
	char tmpfile[256] = { 0 };
	char buf[MAX_LINE_SIZE] = { 0 };
	sprintf(tmpfile, "%s~", path);
	FILE *fp = fopen(path, "r");
	FILE *tmpfp = fopen(tmpfile, "w");
	if (fp == NULL || tmpfp == NULL)
		goto err0;
	if (regcomp(&reg, pattern, flags))
		goto err0;
	while (fgets(buf, sizeof(buf), fp)) {
		if(!regexec(&reg, buf, 0, NULL, 0)) {
			lines++;
		}
		else {
			fprintf(tmpfp, "%s", buf);
		}
		memset(buf, 0, sizeof(buf));
	}
	regfree(&reg);
err0:
	if (fp)
		fclose(fp);
	if (tmpfp)
		fclose(tmpfp);
	if (lines <= 0) {
		unlink(tmpfile);
	}
	else {
		unlink(path);
		rename(tmpfile, path);
	}
	return lines;
}

static inline int is_trimed_char(char c) {
	return (c == '\n' || c == '\r' || c == ' ' || c == '\t');
}

char *str_right_trim(char *s) {
	int i = strlen(s) - 1;
	for (; i >= 0; i--) {
		if (!is_trimed_char(s[i]))
			break;
		s[i] = '\0';
	}
	return s;
}

static void mq_info(const char *name) {
	struct mq_attr attr = { 0 };
	mqd_t mq = mq_open(name, O_RDONLY, 0, NULL);
	if (mq == -1) {
		printf("failed to open %s: %s\n", name, strerror(errno));
		return;
	}
	if (!mq_getattr(mq, &attr)) {
		printf("%-15s\t%5d\t%5d\t%5d\t%s\n", name, attr.mq_maxmsg, attr.mq_msgsize, attr.mq_curmsgs, attr.mq_flags ? "NOBLOCK" : "BLOCK");
	}
	mq_close(mq);
}

static void mq_create(const char *name, int size, int max) {
	if (name == NULL || name[0] == '\0')
		return;
	struct mq_attr attr = {
		.mq_maxmsg = max,
		.mq_msgsize = size
	};
	struct mq_attr *a = &attr;
	if (size <= 0 || max <= 0)
		a = NULL;
	mode_t omask = umask(0);
	printf("try to create mq: %-10s ---> ", name);
	mqd_t mq = mq_open(name, O_RDWR|O_CREAT|O_EXCL, MQ_MODE, a);
	umask(omask);
	if (mq == -1) {
		if (errno == EEXIST)
			printf("already exists.\n", name);
		else
			printf("%s\n", strerror(errno));
		return;
	}
	mq_close(mq);
	mq_info(name);
	file_append_line(LIST_FILE, name);
}

static void mq_create_by_config(const char *config_file) {
	if (access(config_file, R_OK))
		return;
	config_t config = { 0 };
	config_init(&config);
	if (config_read_file(&config, config_file) != CONFIG_TRUE)
		return;
	config_setting_t *mqs = config_lookup(&config,"mqs");
	if (mqs == NULL)
		return;
	int idx = 0;
	config_setting_t *elem = NULL;
	while ((elem = config_setting_get_elem(mqs, idx++))) {
		const char *name = NULL;
		int size = 0, max = 0;
		if (config_setting_lookup_string(elem, "name", &name) != CONFIG_TRUE)
			continue;
		if (config_setting_lookup_int(elem, "size", &size) != CONFIG_TRUE)
			continue;
		if (config_setting_lookup_int(elem, "maxmsgs", &max) != CONFIG_TRUE)
			continue;
		mq_create(name, size, max);
	}
	config_destroy(&config);
}

static void mq_delete(const char *path, const char *pattern, int flags) {
	int lines = 0;
	regex_t reg;
	char tmpfile[256] = { 0 };
	char buf[MAX_LINE_SIZE] = { 0 };
	sprintf(tmpfile, "%s~", path);
	FILE *fp = fopen(path, "r");
	FILE *tmpfp = fopen(tmpfile, "w");
	if (fp == NULL || tmpfp == NULL)
		goto err0;
	if (regcomp(&reg, pattern, flags))
		goto err0;
	while (fgets(buf, sizeof(buf), fp)) {
		str_right_trim(buf);
		if(!regexec(&reg, buf, 0, NULL, 0)) {
			if (!mq_unlink(buf)) {
				printf("mq: %s deleted\n", buf);
				lines++;
			}
			else {
				fprintf(tmpfp, "%s\n", buf);
			}
		}
		else {
			fprintf(tmpfp, "%s\n", buf);
		}
		memset(buf, 0, sizeof(buf));
	}
	regfree(&reg);
err0:
	if (fp)
		fclose(fp);
	if (tmpfp)
		fclose(tmpfp);
	if (lines <= 0) {
		unlink(tmpfile);
	}
	else {
		unlink(path);
		rename(tmpfile, path);
	}
	return;
}

static void mq_list(void) {
	FILE *fp = fopen(LIST_FILE, "r");
	char name[256] = { 0 };
	//printf("%-15s\tmaxmsgs\tmsgsize\tcurmsgs\tflag\n", "   name");
	while (fgets(name, sizeof(name), fp)) {
		str_right_trim(name);
		mq_info(name);
		memset(name, 0, sizeof(name));
	}
	fclose(fp);
}

int main(int argc, char *argv[]) {
	int opt = 0;
	int cmd = help, size = 0, all = 0, max = 0;
	const char *name = NULL;
	const char *conf = NULL;
	struct option opts[] = {
		{"create", optional_argument, NULL, 'c'},
		{"delete", optional_argument, NULL, 'd'},
		{"size", required_argument, NULL, 's'},
		{"all", no_argument, NULL, 'a'},
		{"list", no_argument, NULL, 'l'},
		{"info", required_argument, NULL, 'i'},
		{"config", required_argument, NULL, 'g' },
		{"max", required_argument, NULL, 'm' },
		{"help", no_argument, NULL, 'h'},
	};
	while ((opt = getopt_long(argc, argv, "c::d::s:lah", opts, NULL)) != -1) {
		switch (opt) {
		case 'c':
			cmd = create;
			name = optarg;
			break;
		case 'd':
			cmd = delete;
			name = optarg;
			break;
		case 'a':
			all = 1;
			break;
		case 's':
			size = atoi(optarg);
			break;
		case 'l':
			mq_list();
			return 0;
		case 'i':
			mq_info(optarg);
			return 0;
		case 'g':
			conf = optarg;
			break;
		case 'm':
			max = atoi(optarg);
			break;
		case 'h':
			usage(argc, argv, 0);
			break;
		default:
			usage(argc, argv, 1);
		}
	}
	if (optind < argc)
		conf = argv[optind];
	if (conf == NULL)
		conf = CONFIG_FILE;

	if (cmd == create) {
		if (name == NULL) {
			mq_create_by_config(conf);
		}
		else {
			mq_create(name, size, 10);
		}
	}
	else if (cmd == delete) {
		if (all) {
			mq_delete(LIST_FILE, ".*", 0);
		}
		else if (name == NULL) {
			printf("You must specify the name of the message queue you want to delete, \n"
				   "or specify 'all' to delete all message queues.\n");
		}
		else {
			mq_delete(LIST_FILE, name, 0);
		}
	}
	else {
		usage(argc, argv, 0);
	}
	return 0;
}
