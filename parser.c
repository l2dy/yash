/* Yash: yet another shell */
/* © 2007 magicant */

/* This software can be redistributed and/or modified under the terms of
 * GNU General Public License, version 2 or (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRENTY. */


#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "yash.h"
#include <assert.h>


ssize_t parse_line(const char *line, SCMD **result);
static ssize_t parse_commands(const char **s, SCMD **result);
static int check_parse_result(SCMD *scmds, ssize_t count);
static const char *parse_scmd(const char *s, SCMD *scmd);
static const char *try_wordexp(const char *s, wordexp_t *p, int flags);
static const char *find_end_of_command_body(const char *s);
static const char *find_end_of_dquote(const char *s);
static const char *find_start_of_number(const char *s, const char *t);
static const char *parse_redir(const char *s, REDIR *redir);
static const char *parse_subexp(const char *s, SCMD *scmd);
static char *get_token(const char *s);
void redirsfree(REDIR *redirs, size_t count);
void scmdsfree(SCMD *scmds, size_t count);


/* コマンド入力を解析する
 * line:   コマンド入力。
 * result: 解析成功なら結果を含む新しい SCMD の配列へのポインタが代入される
 * 戻り値: 成功なら SCMD の個数、失敗なら -1 */
ssize_t parse_line(const char *line, SCMD **result) {
	ssize_t r = parse_commands(&line, result);

	if (r < 0)
		return r;
	if (*line) {
		error(0, 0, "unsupported syntax");
		return -1;
	}
	return r;
}

/* & や | や ; で区切られた一連のコマンドを解析する。
 * s:      解析する文字列へのポインタ。
 *         解析成功なら解析が終わったところへのポインタに書き換えられる。
 * result: 解析成功なら結果を含む新しい SCMD の配列へのポインタが代入される
 * 戻り値: 成功なら SCMD の個数、失敗なら -1 */
ssize_t parse_commands(const char **s, SCMD **result) {
	SCMD *scmds = xmalloc(sizeof(SCMD));
	ssize_t arylen = 1;
	ssize_t count = 0;
	const char *ss = *s;

	ss = parse_scmd(ss, scmds + count);
	if (!ss)
		goto error;
	count++;

	for (;;) {
		switch (*ss) {
			default:
			case '\n':  case '(':  case '{':  case '}':
				error(0, 0, "unsupported syntax");
				goto error;
			case '#':
				ss += strlen(ss);
				/* falls thru! */
			case '\0':  case ')':
				if (check_parse_result(scmds, count))
					goto error;
				*result = scmds;
				*s = ss;
				return count;
			case ';':
				scmds[count - 1].c_type = CT_END;
				ss++;
				goto next;
			case '&':
				if (*++ss == '&') {
					ss++;
					scmds[count - 1].c_type = CT_AND;
				} else {
					scmds[count - 1].c_type = CT_BG;
				}
				goto next;
			case '|':
				if (*++ss == '|') {
					ss++;
					scmds[count - 1].c_type = CT_OR;
				} else {
					scmds[count - 1].c_type = CT_PIPED;
				}
				/* falls thru! */
			next:
				if (count == arylen) {
					assert(arylen > 0);
					arylen *= 2;
					scmds = xrealloc(scmds, arylen * sizeof(SCMD));
				}
				ss = parse_scmd(ss, scmds + count);
				if (!ss)
					goto error;
				count++;
				break;
		}
	}

error:
	scmdsfree(scmds, count);
	free(scmds);
	return -1;
}

/* 解析結果が正しいかどうか検証する。
 * 戻り値: 結果が正しければ、0。結果が不正なら -1。 */
static int check_parse_result(SCMD *scmds, ssize_t count)
{
	assert(count > 0);
	for (ssize_t i = 0; i < count; i++) {
		assert((!!scmds[i].c_argv) != (!!scmds[i].c_subcmds));
		if (scmds[i].c_argv && scmds[i].c_argc == 0) {
			/* if (i + 1 != count)
				goto error;
			if (i > 0 && scmds[i-1].c_type == CT_PIPED)
				goto error; */
			if (!i && count >= 2)
				goto error;
			switch (scmds[i].c_type) {
				case CT_PIPED:
					goto error;
				case CT_END:
					if (i + 1 == count)
						break;
					/* falls thru */
				case CT_BG:
					if (scmds[i-1].c_type != CT_PIPED)
						goto error;
					break;
				default:
					break;
			}
			switch (scmds[i-1].c_type) {
				case CT_AND:  case CT_OR:
					goto error;
				default:
					break;
			}
		}
	}
	return 0;

error:
	error(0, 0, "syntax error");
	return -1;
}

/* コマンド入力から一つの SCMD を解析する。
 * s: コマンド入力
 * scmd: 結果がこれに入る
 * 戻り値: 成功ならつぎに解析すべき文字へのポインタ、失敗なら NULL。 */
static const char *parse_scmd(const char *s, SCMD *scmd)
{
	wordexp_t we;
	REDIR *redirs = NULL;
	size_t redircnt = 0, redirsize = 0;
	char *olds = NULL;

	s = skipwhites(s);
	if (*s == '(')
		return parse_subexp(s, scmd);
	s = olds = expand_alias(s);
	s = try_wordexp(s, &we, 0);
	if (!s)
		goto error;

	for (;;) {
		switch (*s) {
			case '\n':  case ';':  case '(':  case ')':  case '{':  case '}':
			case '\0':  case '#':  case '|':  case '&':
				scmd->c_type = CT_END;
				scmd->c_argc = we.we_wordc;
				scmd->c_argv = straryclone(we.we_wordv);
				scmd->c_subcmds = NULL;
				scmd->c_redir = redirs;
				scmd->c_redircnt = redircnt;
				scmd->c_name = xstrndup(olds, s - olds);
				wordfree(&we);
				return s;
			case '<':  case '>':  case '0':  case '1':  case '2':  case '3':
			case '4':  case '5':  case '6':  case '7':  case '8':  case '9':
				if (!redirs) {
					redirs = xmalloc(sizeof(REDIR));
					redirsize = 1;
				} else if (redircnt == redirsize) {
					assert(redirsize > 0);
					redirsize *= 2;
					redirs = xrealloc(redirs, redirsize * sizeof(REDIR));
				}
				s = parse_redir(s, &redirs[redircnt]);
				if (!s)
					goto error;
				redircnt++;
				break;
			default:
				assert(0);
		}
		s = try_wordexp(s, &we, WRDE_APPEND);
		if (!s) goto error;
	}

error:
	if (olds)
		free(olds);
	if (redirs) {
		redirsfree(redirs, redircnt);
		free(redirs);
	}
	return NULL;
}

/* wordexp を試みる。
 * 失敗すると、wordfree を行って NULL を返す。
 * s:      コマンド入力。文字列の内容は (見掛け上) 変更されない。
 * p:      これに結果が入れられる
 * flags:  wordexp するときのフラグ
 * 戻り値: 成功したら、find_end_of_command_body の結果の値。失敗したら NULL */
static const char *try_wordexp(const char *s, wordexp_t *p, int flags)
{
	char *eoc = (char *) find_end_of_command_body(s);
	char temp;
	int wordexpresult;

	if (!eoc)
		return NULL;
	if (!(flags & (WRDE_APPEND | WRDE_REUSE)))
		p->we_wordv = NULL;
	temp = *eoc;
	*eoc = '\0';
	wordexpresult = wordexp(s, p, flags);
	*eoc = temp;
	switch (wordexpresult) {
		case 0:  /* success */
			return eoc;
		case WRDE_BADCHAR:
			error(0, 0, "unsupported syntax");
			break;
		case WRDE_BADVAL:
			error(0, 0, "undefined variable");
			break;
		case WRDE_CMDSUB:
			error(0, 0, "command substitution disabled");
			break;
		case WRDE_NOSPACE:
			error(0, 0, "memory shortage");
			break;
		case WRDE_SYNTAX:
			error(0, 0, "syntax error");
			break;
		default:
			assert(0);
	}
	if (p->we_wordv)
		wordfree(p);
	return NULL;  /* error */
}

/* コマンドのうち、wordexp に渡す部分の直後の位置のアドレスを返す。
 * コマンドを全部 wordexp に渡せるならば s の終端の \0 のアドレスを返す。
 * コマンドの書式が不正・未対応だと分かったときはエラーを出力して NULL を返す。
 * s:      コマンドライン入力 */
static const char *find_end_of_command_body(const char *s)
{
	const char *olds = s;

	for (;;) {
		s += strcspn(s, "$\"'\\|&;()<>\n#");
		switch (*s) {
			case '$':
				if (*++s == '(') {
					SCMD *inner;
					ssize_t innercount;

					s++;
					innercount = parse_commands(&s, &inner);
					if (innercount < 0)
						return NULL;
					if (*s++ != ')') {
						error(0, 0, "invalid syntax: missing ')'");
						return NULL;
					}
					scmdsfree(inner, innercount);
					free(inner);
				}
				break;
			case '"':
				s = find_end_of_dquote(s + 1);
				if (!s)
					return NULL;
				break;
			case '\'':
				s = strchr(s + 1, '\'');
				if (!s) {
					error(0, 0, "unclosed string");
					return NULL;
				}
				s++;
				break;
			case '\\':
				if (!*++s) {
					error(0, 0, "invalid use of '\\'");
					return NULL;
				}
				s++;
				break;
			case '\n':
				error(0, 0, "invalid newline in command");
				return NULL;
			case '<':  case '>':
				return find_start_of_number(olds, s);
			default:
				return s;
		}
	}
}

/* " で囲まれた文字列の終わりのアドレスを返す
 * 文字列を閉じる " が見付からない場合は NULL を返す。
 * s:      文字列の始まりの " の次の文字のアドレス
 * 戻り値: 文字列を閉じる " の次の文字のアドレス、または NULL */
static const char *find_end_of_dquote(const char *s)
{
	for (;;) {
		s += strcspn(s, "$\\\"");
		switch (*s) {
			case '$':
				if (*++s == '(') {
					SCMD *inner;
					ssize_t innercount;

					s++;
					innercount = parse_commands(&s, &inner);
					if (innercount < 0)
						return NULL;
					if (*s++ != ')') {
						error(0, 0, "invalid syntax: missing ')'");
						return NULL;
					}
					scmdsfree(inner, innercount);
					free(inner);
				}
				break;
			case '\\':
				if (!*++s) {
					error(0, 0, "invalid use of '\\'");
					return NULL;
				}
				s++;
				break;
			case '"':
				return s + 1;
			case '\0':
				error(0, 0, "unclosed string");
				return NULL;
			default:
				assert(0);
		}
	}
}

/* 文字列に含まれる数値の先頭位置を返す。
 * s: 文字列全体の先頭のアドレス
 * t: 文字列のうち、数値 (0~9 のみからなる部分文字列) の次の文字のアドレス。
 * 戻り値: t の直前にある数値のうち最初の文字のアドレス。数値がなければ t。 */
static const char *find_start_of_number(const char *s, const char *t)
{
	const char *oldt = t;

	assert(s <= t);
	while (s < t) {
		char c = *--t;
		if (c == ' ')
			return t + 1;
		if (c < '0' || '9' < c)
			return oldt;
	}
	return t;
}

/* リダイレクトを解析する。
 * s:      解析するコマンド入力。
 * redir:  これに結果が入る。(成功した場合)
 * 戻り値: 成功したら次に解析すべき文字へのポインタ、失敗したら NULL */
static const char *parse_redir(const char *s, REDIR *redir)
{
	int fd = -1;
	int flags = 0;
	bool isfdcopy = false;
	char *file;
	ssize_t len;

	if ('0' <= *s && *s <= '9') {
		errno = 0;
		fd = (int) strtol(s, (char **) &s, 10);
		if (errno) {
			error(0, errno, "invalid file descriptor");
			return NULL;
		}
		assert(fd >= 0);
	}
	switch (*s) {
		case '<':
			if (fd < 0)
				fd = STDIN_FILENO;
			flags = O_RDONLY;
			s++;
			if (*s == '>') {
				flags = O_RDWR | O_CREAT;
				s++;
			}
			if (*s == '&') {
				isfdcopy = true;
				s++;
			}
			break;
		case '>':
			if (fd < 0)
				fd = STDOUT_FILENO;
			flags = O_WRONLY | O_CREAT;
			s++;
			if (*s == '>') {
				flags |= O_APPEND;
				s++;
			} else if (*s == '<') {
				/* FD を閉じることを示す特殊なリダイレクト */
				redir->rd_flags = 0;
				redir->rd_fd = fd;
				redir->rd_file = NULL;
				return s + 1;
			} else if (*s == '&') {
				isfdcopy = 1;
				s++;
			} else {
				flags |= O_TRUNC;
			}
			break;
		default:
			error(0, 0, "invalid redirection");
			return NULL;
	}
	while (*s == ' ')
		s++;
	file = get_token(s);
	if (!file) {
		error(0, 0, "invalid redirection (no file specified)");
		return NULL;
	}
	len = strlen(file);
	switch (file[0]) {
		case '"':  case '\'':
			assert(file[len - 1] == '"' || file[len - 1] == '\'');
			memmove(file, file + 1, len - 2);
			file[len - 2] = '\0';
			break;
	}
	if (isfdcopy) {
		/* ファイル名の先頭に "/dev/fd/" を挿入する */
		int len2;
		char *file2 = file;
		while (*file2 == '0') file2++;
		len2 = strlen(file2);
		if (len2 == 0) {
			len2++;
			file2--;
			assert(*file2 == '0');
		}
		file = xrealloc(file, len2 + 9);
		memmove(file + 8, file2, len2 + 1);
		strncpy(file, "/dev/fd/", 8);
		flags &= ~O_CREAT;
	}
	if (file[0] == '~') {
		char *newfile = expand_tilde(file);
		if (newfile) {
			free(file);
			file = newfile;
		}
	}
	redir->rd_flags = flags;
	redir->rd_fd = fd;
	redir->rd_file = file;
	return s + len;
}

/* 括弧 ( ) で囲まれた、サブシェルで実行されるコマンドと
 * それに続くリダイレクトを解析します。
 * s:    括弧 ( で始まる文字列。
 * scmd: これに結果が入る。
 * 戻り値: 成功すると、次に解析すべき文字のポインタを返す。失敗すると NULL。 */
static const char *parse_subexp(const char *s, SCMD *scmd)
{
	SCMD *innerscmds;
	ssize_t innercount;
	REDIR *redirs = NULL;
	size_t redircnt = 0, redirsize = 0;
	const char *olds = s;

	assert(s && *s == '(');
	s++;
	innercount = parse_commands(&s, &innerscmds);
	if (innercount < 0)
		return NULL;
	if (*s != ')') {
		error(0, 0, "invalid syntax: missing ')'");
		return NULL;
	}
	s++;
	for (;;) {
		s = skipwhites(s);
		switch (*s) {
			default:
				scmd->c_type = CT_END;
				scmd->c_argc = innercount;
				scmd->c_argv = NULL;
				scmd->c_subcmds = innerscmds;
				scmd->c_redir = redirs;
				scmd->c_redircnt = redircnt;
				scmd->c_name = xstrndup(olds, strlen(olds) - strlen(s));
				return s;
			case '<':  case '>':  case '0':  case '1':  case '2':  case '3':
			case '4':  case '5':  case '6':  case '7':  case '8':  case '9':
				if (!redirs) {
					redirs = xmalloc(sizeof(REDIR));
					redirsize = 1;
				} else if (redircnt == redirsize) {
					assert(redirsize > 0);
					redirsize *= 2;
					redirs = xrealloc(redirs, redirsize * sizeof(REDIR));
				}
				s = parse_redir(s, &redirs[redircnt]);
				if (!s)
					goto error;
				redircnt++;
				break;
		}
	}

error:
	if (redirs) {
		redirsfree(redirs, redircnt);
		free(redirs);
	}
	scmdsfree(innerscmds, innercount);
	free(innerscmds);
	return NULL;
}

/* 指定した文字列からトークンを取得する。
 * s の先頭に空白があってはならない。
 * 戻り値: 新しく malloc した、s にあるトークンのコピー。
 *         トークンがなければ/エラーが起きたら NULL。 */
static char *get_token(const char *s)
{
	const char *end;
	ssize_t len;

	assert(s);
	assert(*s != ' ');
	if (!*s)
		return NULL;
	switch (*s) {
		case '"':
			end = strchr(s + 1, '"');
			if (!end) {
				error(0, 0, "unclosed string");
				return NULL;
			}
			end += 1;
			break;
		case '\'':
			end = strchr(s + 1, '\'');
			if (!end) {
				error(0, 0, "unclosed string");
				return NULL;
			}
			end += 1;
			break;
		default:
			end = s + strcspn(s, " \"'\\|&;<>(){}\n#");
			break;
	}
	
	len = end - s;
	if (!len)
		return NULL;
	return xstrndup(s, len);
}

/* 指定した配列にある各 REDIR の内部のメモリを解放する。
 * 配列そのものは解放しない。
 * redirs: REDIR の配列へのポインタ
 * count:  配列の要素数 */
void redirsfree(REDIR *redirs, size_t count)
{
	for (size_t i = 0; i < count; i++)
		free(redirs[i].rd_file);
}

/* 指定した配列にある各 SCMD の内部のメモリを解放する。
 * 配列そのものは解放しない。
 * scmds: SCMD の配列へのポインタ
 * count: 配列の要素数 */
void scmdsfree(SCMD *scmds, size_t count)
{
	for (ssize_t i = 0; i < count; i++) {
		SCMD *scmd = scmds + i;
		char **argv = scmd->c_argv;

		if (argv) {
			for (char **a = argv; *a; a++)
				free(*a);
			free(argv);
		} else {
			scmdsfree(scmd->c_subcmds, scmd->c_argc);
		}
		redirsfree(scmd->c_redir, scmd->c_redircnt);
		free(scmd->c_name);
	}
}

