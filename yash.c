/* Yash: yet another shell */
/* © 2007 magicant */

/* This software can be redistributed and/or modified under the terms of
 * GNU General Public License, version 2 or (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRENTY. */


#define _GNU_SOURCE
#include <ctype.h>
#include <error.h>
#include <errno.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/types.h>
#include "yash.h"
#include <assert.h>

#ifndef NDEBUG
#include <fcntl.h>
#endif


void setsigaction(void);
void resetsigaction(void);
void exec_file(const char *path, bool suppresserror);
void exec_file_exp(const char *path, bool suppresserror);
static void set_shlvl(int change);
static void init_env(void);
void init_interactive(void);
void finalize_interactive(void);
void interactive_loop(void);
int main(int argc, char **argv);
void print_help(void);
void print_version(void);
void yash_exit(int exitcode);

#ifndef NDEBUG
void print_scmds(SCMD *scmds, int count, int indent);
#endif

/* このプロセスがログインシェルなら非 0 */
bool is_loginshell;
/* 対話的シェルなら非 0 */
bool is_interactive;

/* 対話的シェルで無視するシグナルのリスト */
const int ignsignals[] = {
	SIGINT, SIGTERM, SIGTSTP, SIGTTIN, SIGTTOU, 0,
};

void debug_sig(int signal)
{
	error(0, 0, "DEBUG: received signal %d. pid=%ld", signal, (long) getpid());
}

/* シグナルハンドラを初期化する */
void setsigaction(void)
{
	struct sigaction action;
	const int *signals;

	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;
#if 0
	action.sa_handler = debug_sig;
	fprintf(stderr, "DEBUG: setting all sigaction\n");
	for (int i = 1; i < 32; i++)
		if (sigaction(i, &action, NULL) < 0) ;
#endif

	action.sa_handler = SIG_IGN;
	if (sigaction(SIGQUIT, &action, NULL) < 0)
		error(0, errno, "sigaction: signal=SIGQUIT");
	if (is_interactive) {
		for (signals = ignsignals; *signals; signals++)
			if (sigaction(*signals, &action, NULL) < 0)
				error(0, errno, "sigaction: signal=%d", *signals);
	}

	action.sa_handler = SIG_DFL;
	if (sigaction(SIGCHLD, &action, NULL) < 0)
		error(0, errno, "sigaction: signal=SIGCHLD");

	action.sa_handler = sig_hup;
	action.sa_flags = SA_RESETHAND;
	if (sigaction(SIGHUP, &action, NULL) < 0)
		error(0, errno, "sigaction: signal=SIGHUP");
}

/* シグナルハンドラを元に戻す */
void resetsigaction(void)
{
	struct sigaction action;
	const int *signals;

	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;
	action.sa_handler = SIG_DFL;

	if (sigaction(SIGQUIT, &action, NULL) < 0)
		error(0, errno, "sigaction: signal=SIGQUIT");
	for (signals = ignsignals; *signals; signals++)
		if (sigaction(*signals, &action, NULL) < 0)
			error(0, errno, "sigaction: signal=%d", *signals);
	if (sigaction(SIGHUP, &action, NULL) < 0)
		error(0, errno, "sigaction: signal=SIGHUP");
}

/* 指定したファイルをシェルスクリプトとして実行する
 * suppresserror: true なら、ファイルが読み込めなくてもエラーを出さない */
void exec_file(const char *path, bool suppresserror)
{
	FILE *file = fopen(path, "r");
	char *line = NULL;
	size_t len = 0;
	ssize_t size;

	if (!file) {
		if (!suppresserror)
			error(0, errno, "%s", path);
		return;
	}
	while ((size = getline(&line, &len, file)) >= 0) {
		SCMD *scmds;
		ssize_t count;
		
		if (line[size - 1] == '\n')
			line[size - 1] = '\0';
		count = parse_line(line, &scmds);
		if (count < 0)
			continue;
		exec_list(scmds, count);
		scmdsfree(scmds, count);
		free(scmds);
	}
	free(line);
	fclose(file);
}

/* ファイルをシェルスクリプトとして実行する。
 * path: ファイルのパス。'~' で始まるならホームディレクトリを展開して
 *       ファイルを探す。 */
void exec_file_exp(const char *path, bool suppresserror)
{
	if (path[0] == '~') {
		char *newpath = expand_tilde(path);
		if (!newpath)
			return;
		exec_file(newpath, suppresserror);
		free(newpath);
	} else {
		exec_file(path, suppresserror);
	}
}

/* 環境変数 SHLVL に change を加える */
static void set_shlvl(int change)
{
	char *shlvl = getenv(ENV_SHLVL);
	int level = shlvl ? atoi(shlvl) : 0;
	char newshlvl[16];

	level += change;
	if (level < 0)
		level = 0;
	if (snprintf(newshlvl, sizeof newshlvl, "%d", level) >= 0) {
		if (setenv(ENV_SHLVL, newshlvl, true /* overwrite */) < 0)
			error(0, 0, "failed to set env SHLVL");
	}
}

/* 実行環境を初期化する */
static void init_env(void)
{
	char *path = getcwd(NULL, 0);

	if (path) {
		char *spwd = collapse_homedir(path);

		if (setenv(ENV_PWD, path, true /* overwrite */) < 0)
			error(0, 0, "failed to set env PWD");
		if (spwd) {
			if (setenv(ENV_SPWD, spwd, true /* overwrite */) < 0)
				error(0, 0, "failed to set env SPWD");
			free(spwd);
		}
		free(path);
	}
}

static pid_t orig_pgrp = 0;
static bool noprofile = false, norc = false; 
static char *rcfile = "~/.yashrc";

/* 対話モードの初期化を行う */
void init_interactive(void)
{
	static bool initialized = false;

	if (is_interactive) {
		orig_pgrp = getpgrp();
		setpgrp();   /* シェル自身は独立した pgrp に属する */
		set_shlvl(1);
		if (!initialized) {
			if (is_loginshell) {
				if (!noprofile)
					exec_file_exp("~/.yash_profile", true /* suppress error */);
			} else if (!norc) {
				exec_file_exp(rcfile, true /* suppress error */);
			}
			initialized = true;
		}
		initialize_readline();
	}
}

/* 対話モードの終了処理を行う */
void finalize_interactive(void)
{
	if (is_interactive) {
		finalize_readline();
		set_shlvl(-1);
		if (orig_pgrp > 0 && tcsetpgrp(STDIN_FILENO, orig_pgrp) < 0)
			error(0, errno, "cannot reset foreground process group");
		if (orig_pgrp > 0 && setpgid(0, orig_pgrp) < 0 && errno != EPERM)
			error(0, errno, "cannot reset process group");
	}
}

/* 対話的動作を行う。この関数は返らない。 */
void interactive_loop(void)
{
	char *line;
	SCMD *scmds;
	ssize_t count;

	assert(is_interactive);
	for (;;) {
		switch (yash_readline(&line)) {
			case -2:  /* EOF */
				printf("\n");
				wait_all(-2 /* non-blocking */);
				print_all_job_status(
						true /* changed only */, false /* not verbose */);
				if (job_count()) {
					error(0, 0, "There are undone jobs!"
							"  Use `exit -f' to exit forcibly.");
					continue;
				}
				goto exit;
			case -1:
				continue;
			case 0:  default:
				break;
		}
		/* printf("parsing \"%s\"\n", line); */
		count = parse_line(line, &scmds);
		if (count < 0)
			continue;

		/* printf("count=%d\n", count); */
		/* print_scmds(scmds, count, 0); */
		exec_list(scmds, count);
		scmdsfree(scmds, count);
		free(scmds);
		free(line);
	}
exit:
	yash_exit(laststatus);
}

#ifndef NDEBUG

void print_scmds(SCMD *scmds, int count, int indent)
{
	void print_indent(int i) {
		printf("%*s", i, "");
	}
	int i, j;

	for (i = 0; i < count; i++) {
		print_indent(indent);
		printf("SCMD[%d] : ", i);
		switch (scmds[i].c_type) {
			case CT_END:   printf("END\n");   break;
			case CT_PIPED: printf("PIPED\n"); break;
			case CT_BG:    printf("BG\n");    break;
			case CT_AND:   printf("AND\n");   break;
			case CT_OR:    printf("OR\n");    break;
		}
		if (scmds[i].c_argv)
			for (j = 0; j < scmds[i].c_argc; j++) {
				print_indent(indent);
				printf("  Arg   %d : %s\n", j, scmds[i].c_argv[j]);
			}

		for (j = 0; j < scmds[i].c_redircnt; j++) {
			print_indent(indent);
			printf("  Redir %d : fd=%d file=\"%s\" ", j,
					scmds[i].c_redir[j].rd_fd, scmds[i].c_redir[j].rd_file);
			if (scmds[i].c_redir[j].rd_flags & O_RDWR)        printf("RDWR");
			else if (scmds[i].c_redir[j].rd_flags & O_WRONLY) printf("WRONLY");
			else                                               printf("RDONLY");
			if (scmds[i].c_redir[j].rd_flags & O_CREAT)  printf(" CREAT");
			if (scmds[i].c_redir[j].rd_flags & O_APPEND) printf(" APPEND");
			if (scmds[i].c_redir[j].rd_flags & O_TRUNC)  printf(" TRUNC");
			printf("\n");
		}
		print_indent(indent);
		printf("  Name    : %s\n", scmds[i].c_name);
		
		if (scmds[i].c_subcmds)
			print_scmds(scmds[i].c_subcmds, scmds[i].c_argc, indent + 8);
	}
}

#endif

static struct option long_opts[] = {
	{ "help", 0, NULL, '?', },
	{ "version", 0, NULL, 'v' + 256, },
	{ "rcfile", 1, NULL, 'r', },
	{ "noprofile", 0, NULL, 'N' + 256, },
	{ "norc", 0, NULL, 'n' + 256, },
	{ "login", 0, NULL, 'l', },
	{ "interactive", 0, NULL, 'i', },
	{ NULL, 0, NULL, 0, },
};

int main(int argc, char **argv)
{
	bool help = false, version = false;
	int opt, index;
	char *directcommand = NULL;
	static const char *short_opts = "c:il";

	is_loginshell = argv[0][0] == '-';
	is_interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
	joblistlen = 2;
	joblist = xcalloc(joblistlen, sizeof(JOB));
	setlocale(LC_ALL, "");

	optind = 0;
	opterr = 1;
	while ((opt = getopt_long(argc, argv, short_opts, long_opts, &index)) >= 0){
		switch (opt) {
			case 0:
				break;
			case 'c':
				directcommand = optarg;
				break;
			case 'i':
				is_interactive = true;
				break;
			case 'l':
				is_loginshell = true;
				break;
			case 'n' + 256:
				norc = 1;
				break;
			case 'N' + 256:
				noprofile = true;
				break;
			case 'r':
				rcfile = optarg;
				break;
			case 'v' + 256:
				version = true;
				break;
			case '?':
				help = true;
				break;
			default:
				return EXIT_FAILURE;
		}
	}
	if (help) {
		print_help();
		return EXIT_SUCCESS;
	} else if (version) {
		print_version();
		return EXIT_SUCCESS;
	}

	setsigaction();
	init_env();

	if (directcommand) {
		SCMD *scmds;
		ssize_t count;
		
		is_interactive = 0;
		count = parse_line(directcommand, &scmds);
		if (count < 0)
			return EXIT_SUCCESS;
		exec_list(scmds, count);
		scmdsfree(scmds, count);
		free(scmds);
		return laststatus;
	}

	if (is_interactive) {
		init_interactive();
		interactive_loop();
	}
	return EXIT_SUCCESS;
}

void print_help(void)
{
	printf("Usage:  yash [-il] [-c command] [long options] [file]\n");
	printf("Long options:\n");
	for (size_t index = 0; long_opts[index].name; index++)
		printf("\t--%s\n", long_opts[index].name);
}

void print_version(void)
{
	printf("Yet another shell, version " YASH_VERSION
			" (compiled " __DATE__ " " __TIME__ ")\n"
			YASH_COPYRIGHT "\n");
}

/* 終了前の手続きを行って、終了する。*/
void yash_exit(int exitcode) {
	wait_all(-2 /* non-blocking */);
	print_all_job_status(false /* all jobs */, false /* not verbose */);
	if (is_loginshell)
		exec_file("~/.yash_logout", true /* suppress error */);
	finalize_interactive();
	if (huponexit)
		send_sighup_to_all_jobs();
	exit(exitcode);
}
