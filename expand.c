/* Yash: yet another shell */
/* expand.c: word expansion */
/* (C) 2007-2008 magicant */

/* This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.  */


#include "common.h"
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>
#include "option.h"
#include "util.h"
#include "strbuf.h"
#include "plist.h"
#include "wfnmatch.h"
#include "path.h"
#include "input.h"
#include "parser.h"
#include "variable.h"
#include "expand.h"
#include "exec.h"
#include "yash.h"


/* characters that have special meanings in brace expansion and glob. */
#define ESCAPED_CHARS L"\\{,}"

/* characters that can be escaped with a backslash inside doublequotes. */
#define ESCAPABLE_CHARS L"$`\"\\"

static bool expand_word_and_split(
	const wordunit_T *restrict w, plist_T *restrict list)
    __attribute__((nonnull(2)));

static bool expand_word(
	const wordunit_T *restrict w, tildetype_T tilde,
	plist_T *restrict valuelist, plist_T *restrict splitlist)
    __attribute__((nonnull(3)));

static wchar_t *expand_tilde(const wchar_t **ss,
	bool hasnextwordunit, tildetype_T tt)
    __attribute__((nonnull,malloc,warn_unused_result));

static void **expand_param(const paramexp_T *p, bool indq, tildetype_T tilde)
    __attribute__((nonnull,malloc,warn_unused_result));
static void print_subst_as_error(const paramexp_T *p)
    __attribute__((nonnull));
static void match_each(
	void **slist, const wchar_t *pattern, paramexptype_T type)
    __attribute__((nonnull));
static void match_head_each(void **slist, const wchar_t *pattern, bool longest)
    __attribute__((nonnull));
static void match_tail_longest_each(void **slist, const wchar_t *pattern)
    __attribute__((nonnull));
static void match_tail_shortest_each(void **slist, const wchar_t *pattern)
    __attribute__((nonnull));
static void subst_each(void **slist, const wchar_t *pattern,
	const wchar_t *subst, paramexptype_T type)
    __attribute__((nonnull));
static void subst_whole_each(
	void **slist, const wchar_t *pattern, const wchar_t *subst)
    __attribute__((nonnull));
static void subst_head_each(
	void **slist, const wchar_t *pattern, const wchar_t *subst)
    __attribute__((nonnull));
static void subst_tail_each(
	void **slist, const wchar_t *pattern, const wchar_t *subst)
    __attribute__((nonnull));
static void subst_generic_each(void **slist,
	const wchar_t *pattern, const wchar_t *subst, bool substall)
    __attribute__((nonnull));
static void subst_length_each(void **slist)
    __attribute__((nonnull));

static void expand_brace_each(void **restrict values, void **restrict splits,
	plist_T *restrict valuelist, plist_T *restrict splitlist)
    __attribute__((nonnull));
static void expand_brace(wchar_t *restrict word, char *restrict split,
	plist_T *restrict valuelist, plist_T *restrict splitlist)
    __attribute__((nonnull));
static bool has_leading_zero(const wchar_t *s, bool *sign)
    __attribute__((nonnull));
static bool tryexpand_brace_sequence(
	wchar_t *word, char *split, wchar_t *startc,
	plist_T *restrict valuelist, plist_T *restrict splitlist)
    __attribute__((nonnull));

static void fieldsplit(wchar_t *restrict s, char *restrict split,
	const wchar_t *restrict ifs, plist_T *restrict dest)
    __attribute__((nonnull));
static void fieldsplit_all(void **restrict valuelist, void **restrict splitlist,
	plist_T *restrict dest)
    __attribute__((nonnull));

static inline void add_sq(
	const wchar_t *restrict *ss, xwcsbuf_T *restrict buf, bool escape)
    __attribute__((nonnull));
static wchar_t *reescape(const wchar_t *s)
    __attribute__((nonnull,malloc,warn_unused_result));
static void **reescape_full_array(void **wcsarray)
    __attribute__((nonnull));
static wchar_t *escaped_wcspbrk(const wchar_t *s, const wchar_t *accept)
    __attribute__((nonnull));

static void do_glob_each(void **restrict patterns, plist_T *restrict list)
    __attribute__((nonnull));
static enum wglbflags get_wglbflags(void)
    __attribute__((pure));


/********** Entry Points **********/

/* Expands a command line.
 * `args' is a NULL-terminated array of pointers to `const wordunit_T'
 * to expand.
 * The number of resulting words is assigned to `*argcp' and a newly malloced
 * array of the words is assigned to `*argvp'. The array is NULL-terminated and
 * its elements are newly malloced wide strings.
 * If successful, true is returned and `*argcp' and `*argvp' are assigned to.
 * If unsuccessful, false is returned and the values of `*argcp' and `*argvp'
 * are unspecified.
 * On error in a non-interactive shell, the shell exits. */
bool expand_line(void *const *restrict args,
    int *restrict argcp, void ***restrict argvp)
{
    plist_T list1, list2;
    pl_init(&list1);

    /* four expansions, brace expansions and field splitting (args -> list1) */
    while (*args) {
	if (!expand_word_and_split(*args, &list1)) {
	    if (!is_interactive)
		exit_shell_with_status(EXIT_EXPERROR);
	    recfree(pl_toary(&list1), free);
	    return false;
	}
	args++;
    }

    /* glob (list1 -> list2) */
    if (shopt_noglob) {
	list2 = list1;
    } else {
	pl_init(&list2);
	do_glob_each(pl_toary(&list1), &list2);
    }

    *argcp = list2.length;
    *argvp = pl_toary(&list2);
    return true;
}

/* Does four expansions, brace expansion and field splitting in a word.
 * Returns true iff successful. Resulting words are added to `list'.
 * Tilde expansion is performed with `tt_single'. */
bool expand_word_and_split(const wordunit_T *restrict w, plist_T *restrict list)
{
    plist_T valuelist1, valuelist2, splitlist1, splitlist2;
    pl_init(&valuelist1);
    pl_init(&splitlist1);

    /* four expansions (w -> list1) */
    if (!expand_word(w, tt_single, &valuelist1, &splitlist1)) {
	recfree(pl_toary(&valuelist1), free);
	recfree(pl_toary(&splitlist1), free);
	return false;
    }

    /* brace expansion (list1 -> list2) */
    if (shopt_braceexpand) {
	pl_init(&valuelist2);
	pl_init(&splitlist2);
	expand_brace_each(valuelist1.contents, splitlist1.contents,
		&valuelist2, &splitlist2);
	pl_destroy(&valuelist1);
	pl_destroy(&splitlist1);
    } else {
	valuelist2 = valuelist1;
	splitlist2 = splitlist1;
    }

    /* field splitting (list2 -> list) */
    fieldsplit_all(pl_toary(&valuelist2), pl_toary(&splitlist2), list);
    return true;
}

/* Expands a single word: four expansions and quote removal.
 * This function doesn't perform brace expansion, field splitting, globbing and
 * unescaping.
 * If successful, the resulting word is returned as a newly malloced string.
 * On error, an error message is printed and NULL is returned.
 * On error in a non-interactive shell, the shell exits. */
wchar_t *expand_single(const wordunit_T *arg, tildetype_T tilde)
{
    wchar_t *result;
    plist_T list;
    pl_init(&list);

    if (!expand_word(arg, tilde, &list, NULL)) {
	if (!is_interactive)
	    exit_shell_with_status(EXIT_EXPERROR);
	recfree(pl_toary(&list), free);
	return NULL;
    }
    if (list.length != 1) {
	/* concatenate multiple words to a single */
	const wchar_t *ifs = getvar(VAR_IFS);
	wchar_t padding[] = { ifs ? ifs[0] : L' ', L'\0' };
	result = joinwcsarray(list.contents, padding);
	recfree(pl_toary(&list), free);
    } else {
	result = list.contents[0];
	pl_destroy(&list);
    }
    return result;
}

/* Expands a single word: four expansions, glob, quote removal and unescape.
 * This function doesn't perform brace expansion and field splitting.
 * If the result of glob is more than one,
 *   - returns the pre-glob pattern string if `posixly_correct' is true, or
 *   - treats as an error if `posixly_correct' is false.
 * If `shopt_noglob' is true, glob is not performed.
 * `shopt_nullglob' is ignored.
 * If successful, the resulting word is returned as a newly malloced string.
 * On error, an error message is printed and NULL is returned.
 * On error in a non-interactive shell, the shell exits. */
char *expand_single_with_glob(const wordunit_T *arg, tildetype_T tilde)
{
    wchar_t *exp = expand_single(arg, tilde);
    char *result;

    /* glob */
    if (shopt_noglob || !pattern_has_special_char(exp)) {
noglob:
	result = realloc_wcstombs(unescapefree(exp));
	if (!result)
	    xerror(0, Ngt("expanded word contains characters that "
			"cannot be converted to wide characters and "
			"is replaced with null string"));
    } else {
	plist_T list;
	pl_init(&list);
	wglob(exp, get_wglbflags(), &list);
	if (list.length == 1) {
	    free(exp);
	    result = list.contents[0];
	    pl_destroy(&list);
	} else {
	    recfree(pl_toary(&list), free);
	    if (posixly_correct) {
		goto noglob;
	    } else {
		exp = unescapefree(exp);
		xerror(0, Ngt("%ls: not single file"), exp);
		free(exp);
		result = NULL;
	    }
	}
    }
    return result;
}

/* Does expansions in a string: parameter/arithmetic/command expansions.
 * Brace expansion, field splitting and glob are not performed.
 * If `esc' is true, backslashes preceding $, `, \ are removed. Otherwise,
 * no quotations are removed.
 * The result is a newly malloced string.
 * On error, an error message is printed and NULL is returned. */
wchar_t *expand_string(const wordunit_T *w, bool esc)
{
    bool ok = true;
    xwcsbuf_T buf;
    const wchar_t *ss;
    wchar_t *s;
    void **array;

    wb_init(&buf);
    while (w) {
	switch (w->wu_type) {
	case WT_STRING:
	    ss = w->wu_string;
	    while (*ss) {
		if (esc && ss[0] == L'\\' && ss[1] != L'\0'
			&& wcschr(L"$`\\", ss[1])) {
		    ss++;
		    if (*ss)
			wb_wccat(&buf, *ss);
		} else {
		    wb_wccat(&buf, *ss);
		}
		ss++;
	    }
	    break;
	case WT_PARAM:
	    array = expand_param(w->wu_param, false, tt_none);
	    if (array) {
		if (array[0]) {
		    for (void **a = array; *a; a++)
			*a = unescapefree(*a);
		    if (!array[1]) {
			wb_cat(&buf, array[0]);
		    } else {
			const wchar_t *ifs = getvar(VAR_IFS);
			wchar_t padding[] = { ifs ? ifs[0] : L' ', L'\0' };
			wb_catfree(&buf, joinwcsarray(array, padding));
		    }
		}
		recfree(array, free);
	    } else {
		ok = false;
	    }
	    break;
	case WT_CMDSUB:
	    s = exec_command_substitution(w->wu_cmdsub);
	    if (s) {
		wb_catfree(&buf, s);
	    } else {
		ok = false;
	    }
	    break;
	case WT_ARITH:
	    ok = false;  // TODO expand: expand_word: arithmetic expansion
	    xerror(0, "arithmetic expansion not implemented");
	    break;
	}
	w = w->next;
    }
    if (ok) {
	return wb_towcs(&buf);
    } else {
	if (!is_interactive)
	    exit_shell_with_status(EXIT_EXPERROR);
	wb_destroy(&buf);
	return NULL;
    }
}


/********** Four Expansions **********/

/* Does four expansions in a single word.
 * `w' is the word in which expansions occur.
 * `tilde' is type of tilde expansion that occurs.
 * The expanded word is added to `valuelist' as a newly malloced wide string.
 * The splittability string is added to `splitlist' if `splitlist' is non-NULL.
 * Single- or double-quoted characters are unquoted and backslashed.
 * In most case, one string is added to each of `valuelist' and `splitlist'.
 * If the word contains "$@", the result may be any number of strings.
 * The return value is true iff successful. */
bool expand_word(
	const wordunit_T *restrict w, tildetype_T tilde,
	plist_T *restrict valuelist, plist_T *restrict splitlist)
{
    bool ok = true;
    bool indq = false;     /* in a doublequote? */
    bool first = true;     /* is the first word unit? */
    bool force = false;    /* don't ignore empty string? */
    bool suppress = false; /* ignore empty string anyway? */
    size_t initlen = valuelist->length;
    xwcsbuf_T buf;
    xstrbuf_T sbuf;
    const wchar_t *ss;
    wchar_t *s;
    void **array;

    wb_init(&buf);
    if (splitlist)
	sb_init(&sbuf);

#define FILL_SBUF(c) \
    ((void) (splitlist && sb_ccat_repeat(&sbuf, c, buf.length - sbuf.length)))
#define FILL_SBUF_SPLITTABLE    FILL_SBUF(1)
#define FILL_SBUF_UNSPLITTABLE  FILL_SBUF(0)

    while (w) {
	switch (w->wu_type) {
	case WT_STRING:
	    ss = w->wu_string;
	    if (first && tilde != tt_none) {
		s = expand_tilde(&ss, w->next, tilde);
		if (s) {
		    wb_catfree(&buf, escapefree(s, ESCAPED_CHARS));
		    FILL_SBUF_UNSPLITTABLE;
		}
	    }
	    while (*ss) {
		switch (*ss) {
		case L'"':
		    indq = !indq;
		    force = true;
		    break;
		case L'\'':
		    if (indq)
			goto default_case;
		    force = true;
		    add_sq(&ss, &buf, true);
		    FILL_SBUF_UNSPLITTABLE;
		    break;
		case L'\\':
		    if (indq && !wcschr(ESCAPABLE_CHARS, ss[1])) {
			goto default_case;
		    } else {
			wb_wccat(&buf, L'\\');
			if (*++ss)
			    wb_wccat(&buf, *ss++);
			FILL_SBUF_UNSPLITTABLE;
			continue;
		    }
		case L':':
		    if (!indq && tilde == tt_multi) {
			wb_wccat(&buf, L':');
			ss++;
			s = expand_tilde(&ss, w->next, tilde);
			if (s)
			    wb_catfree(&buf, escapefree(s, ESCAPED_CHARS));
			FILL_SBUF_UNSPLITTABLE;
			continue;
		    }
		    /* falls thru! */
		default:  default_case:
		    if (indq)
			wb_wccat(&buf, L'\\');
		    wb_wccat(&buf, *ss);
		    FILL_SBUF_UNSPLITTABLE;
		    break;
		}
		ss++;
	    }
	    break;
	case WT_PARAM:
	    array = expand_param(w->wu_param, indq, tilde);
	    if (array) {
		if (!array[0]) {
		    suppress = true;
		} else {
		    force = true;
		    for (void **a = array; ; ) {
			wb_catfree(&buf, *a);
			FILL_SBUF_SPLITTABLE;
			a++;
			if (!*a)
			    break;
			pl_add(valuelist, wb_towcs(&buf));
			wb_init(&buf);
			if (splitlist) {
			    pl_add(splitlist, sb_tostr(&sbuf));
			    sb_init(&sbuf);
			}
		    }
		}
		free(array);
	    } else {
		ok = false;
	    }
	    break;
	case WT_CMDSUB:
	    s = exec_command_substitution(w->wu_cmdsub);
	    if (s) {
		wb_catfree(&buf, escapefree(s, indq ? NULL : ESCAPED_CHARS));
		FILL_SBUF_SPLITTABLE;
	    } else {
		ok = false;
	    }
	    break;
	case WT_ARITH:
	    ok = false;  // TODO expand: expand_word: arithmetic expansion
	    //FILL_SBUF_SPLITTABLE;
	    xerror(0, "arithmetic expansion not implemented");
	    break;
	}
	w = w->next;
	first = false;
    }

    /* A quoted empty word is added to the list here.
     * It is indicated by the `force' flag, which is set when a quote is found.
     * An exception is "$@", which is not added if there is no positional
     * parameter even if it is quoted. This is indicated by the `suppress' flag.
     */
    if (buf.length > 0 || (initlen == valuelist->length && force && !suppress)){
	pl_add(valuelist, wb_towcs(&buf));
	if (splitlist)
	    pl_add(splitlist, sb_tostr(&sbuf));
    } else {
	wb_destroy(&buf);
	if (splitlist)
	    sb_destroy(&sbuf);
    }
    return ok;

#undef FILL_SBUF
#undef FILL_SBUF_SPLITTABLE
#undef FILL_SBUF_UNSPLITTABLE
}

/* Does tilde expansion.
 * `ss' is a pointer to a pointer to the tilde character. The pointer is
 * increased so that it points to the character right after the expanded string.
 * The string pointed by the pointer pointed by `ss' should be contents of a
 * word unit of type WT_STRING. Iff there is a next word unit, `hasnextwordunit'
 * should be true.
 * If `**ss' is not L'~' or expansion fails, this function has no side effects
 * and returns NULL. If successful, `*ss' is incremented and the result is
 * returned as a newly malloced string. */
wchar_t *expand_tilde(const wchar_t **ss, bool hasnextwordunit, tildetype_T tt)
{
    const wchar_t *s = *ss;
    if (*s != L'~')
	return NULL;
    s++;

    const wchar_t *end = wcspbrk(s, tt == tt_single ? L"/" : L"/:");
    wchar_t *username;
    const wchar_t *home;
    size_t usernamelen;

    if (end) {
	usernamelen = end - s;
    } else {
	if (hasnextwordunit)
	    return NULL;
	usernamelen = wcslen(s);
    }
    username = xwcsndup(s, usernamelen);
    if (username[0] == L'\0') {
	/* empty user name: use $HOME */
	home = getvar(VAR_HOME);
	goto finish;
    } else if (wcspbrk(username, L"\"'\\") != 0) {
	/* don't expand if the user name is quoted */
	free(username);
	return NULL;
    }
    if (!posixly_correct) {
	if (wcscmp(username, L"+") == 0) {
	    home = getvar(VAR_PWD);
	    goto finish;
	} else if (wcscmp(username, L"-") == 0) {
	    home = getvar(VAR_OLDPWD);
	    goto finish;
	}  // TODO expand: expand_tilde: directory stack
    }
    home = get_home_directory(username, false);
finish:
    free(username);
    if (!home)
	return NULL;
    *ss = s + usernamelen;
    return xwcsdup(home);
}

/* Does a parameter expansion.
 * `tilde' is type of tilde expansion that is done in the nested expansion.
 * If successful, the result is returned as a newly malloced array of pointers
 * to newly malloced wide strings cast to (void *). The array is
 * NULL-terminated.
 * Characters in the strings that are contained in `ESCAPED_CHARS' are
 * backslashed.
 * If "$@" is expanded or the result is an array, the returned array may
 * contain any number of strings. Otherwise, the array contains one string.
 * If "$*" is expanded, the result is a single concatenated string.
 * On error, NULL is returned. */
void **expand_param(const paramexp_T *p, bool indq, tildetype_T tilde)
{
    void **list;  /* array of (wchar_t *) cast to (void *) */
    bool concat;  /* concatenate? */
    bool unset;   /* parameter is not set? */
    wchar_t *match, *subst;

    /* get the value of parameter or nested expansion */
    if (p->pe_type & PT_NEST) {
	plist_T plist;
	pl_init(&plist);
	if (!expand_word(p->pe_nest, tilde, &plist, NULL)) {
	    recfree(pl_toary(&plist), free);
	    return NULL;
	}
	list = pl_toary(&plist);
	concat = true;
	unset = false;
	for (size_t i = 0; list[i]; i++)
	    list[i] = unescapefree(list[i]);
    } else {
	list = get_variable(p->pe_name, &concat);
	if (list) {
	    unset = false;
	} else {
	    /* if parameter is not set, return empty string or error */
	    if (shopt_nounset) {
		xerror(0, Ngt("%s: parameter not set"), p->pe_name);
		return NULL;
	    } else {
		plist_T plist;
		list = pl_toary(pl_add(pl_init(&plist), xwcsdup(L"")));
		unset = true;
	    }
	}
    }

    /* here, the contents of `list' are not backslashed. */

    /* if `PT_COLON' is true, empty string is treated as unset */
    if ((p->pe_type & PT_COLON)
	    && (!list[0] || (!((char *) list[0])[0] && !list[1])))
	unset = true;

    /* PT_PLUS, PT_MINUS, PT_ASSIGN, PT_ERROR, PT_MATCH, PT_SUBST */
    switch (p->pe_type & PT_MASK) {
    case PT_PLUS:
	if (!unset)
	    goto subst;
	break;
    case PT_MINUS:
	if (unset) {
	    plist_T plist;
subst:
	    recfree(list, free);
	    pl_init(&plist);
	    if (expand_word(p->pe_subst, tt_single, &plist, NULL)) {
		list = pl_toary(&plist);
		return indq ? reescape_full_array(list) : list;
	    } else {
		recfree(pl_toary(&plist), free);
		return NULL;
	    }
	}
	break;
    case PT_ASSIGN:
	if (unset) {
	    recfree(list, free);
	    if (p->pe_type & PT_NEST) {
		xerror(0, Ngt("invalid assignment in parameter expansion"));
		return NULL;
	    } else if (!is_name(p->pe_name)) {
		xerror(0, Ngt("cannot assign to `%s' in parameter expansion"),
			p->pe_name);
		return NULL;
	    }
	    subst = expand_single(p->pe_subst, tt_single);
	    if (!subst)
		return NULL;
	    subst = unescapefree(subst);
	    if (!set_variable(p->pe_name, xwcsdup(subst), false, false)) {
		free(subst);
		return NULL;
	    }
	    list = xmalloc(2 * sizeof *list);
	    list[0] = subst;
	    list[1] = NULL;
	}
	break;
    case PT_ERROR:
	if (unset) {
	    recfree(list, free);
	    print_subst_as_error(p);
	    return NULL;
	}
	break;
    case PT_MATCH:
	match = expand_single(p->pe_match, tt_single);
	if (!match) {
	    recfree(list, free);
	    return NULL;
	}
	match_each(list, match, p->pe_type);
	free(match);
	break;
    case PT_SUBST:
	match = expand_single(p->pe_match, tt_single);
	subst = expand_single(p->pe_subst, tt_single);
	if (!match || !subst) {
	    free(match);
	    free(subst);
	    recfree(list, free);
	    return NULL;
	}
	subst = unescapefree(subst);
	subst_each(list, match, subst, p->pe_type);
	free(match);
	free(subst);
	break;
    }

    /* concatenate `list' elements */
    if (concat) {
	const wchar_t *ifs = getvar(VAR_IFS);
	wchar_t padding[] = { ifs ? ifs[0] : L' ', L'\0' };
	wchar_t *chain = joinwcsarray(list, padding);
	recfree(list, free);
	list = xmalloc(2 * sizeof *list);
	list[0] = chain;
	list[1] = NULL;
    }

    /* PT_NUMBER */
    if (p->pe_type & PT_NUMBER)
	subst_length_each(list);

    /* backslash */
    for (size_t i = 0; list[i]; i++)
	list[i] = escapefree(list[i], indq ? NULL : ESCAPED_CHARS);

    /* treat the case the value is empty */
    if (!indq && list[0] && !list[1] && !((wchar_t *) list[0])[0]) {
	free(list[0]);
	list[0] = NULL;
    }

    return list;
}

/* Expands `p->pe_subst' and prints it as an error message. */
void print_subst_as_error(const paramexp_T *p)
{
    if (p->pe_subst) {
	wchar_t *subst = expand_single(p->pe_subst, tt_single);
	if (subst) {
	    subst = unescapefree(subst);
	    if (p->pe_type & PT_NEST)
		xerror(0, "%ls", subst);
	    else
		xerror(0, "%s: %ls", p->pe_name, subst);
	    free(subst);
	}
    } else {
	if (p->pe_type & PT_NEST)
	    xerror(0, Ngt("parameter null"));
	else
	    xerror(0, (p->pe_type & PT_COLON)
		    ? Ngt("%s: parameter null or not set")
		    : Ngt("%s: parameter not set"),
		    p->pe_name);
    }
}

/* Matches each string in the array to the specified pattern and removes the
 * matching portions.
 * `slist' is a NULL-terminated array of pointers to `free'able wide strings.
 * `type' may contain PT_MATCHHEAD, PT_MATCHTAIL and PT_MATCHLONGEST.
 * Elements of `slist' may be modified and `realloc'ed in this function. */
void match_each(void **slist, const wchar_t *pattern, paramexptype_T type)
{
    if (type & PT_MATCHHEAD) {
	match_head_each(slist, pattern, type & PT_MATCHLONGEST);
    } else {
	assert(type & PT_MATCHTAIL);
	if (type & PT_MATCHLONGEST)
	    match_tail_longest_each(slist, pattern);
	else
	    match_tail_shortest_each(slist, pattern);
    }
}

/* Does `match_each' for 'PT_MATCHHEAD'. */
void match_head_each(void **slist, const wchar_t *pattern, bool longest)
{
    enum wfnmtype type = longest ? WFNM_LONGEST : WFNM_SHORTEST;
    enum wfnmflags flags = shopt_nocaseglob ? WFNM_CASEFOLD : 0;
    size_t minlen = shortest_match_length(pattern, flags);
    wchar_t *s;
    while ((s = *slist)) {
	size_t match = wfnmatchl(pattern, s, flags, type, minlen);
	if (match == WFNM_ERROR) {
	    break;
	} else if (match != WFNM_NOMATCH) {
	    if (match > 0)
		wmemmove(s, s + match, wcslen(s + match) + 1);
	}
	slist++;
    }
}

/* Does `match_each' for 'PT_MATCHTAIL' where `PT_MATCHLONGEST' is true.*/
void match_tail_longest_each(void **slist, const wchar_t *pattern)
{
    enum wfnmflags flags = shopt_nocaseglob ? WFNM_CASEFOLD : 0;
    size_t minlen = shortest_match_length(pattern, flags);
    wchar_t *s;
    while ((s = *slist)) {
	size_t len = wcslen(s);
	size_t index = 0;
	while (minlen + index <= len) {
	    size_t match = wfnmatchl(
		    pattern, s + index, flags, WFNM_WHOLE, minlen);
	    if (match == WFNM_ERROR) {
		return;
	    } else if (match != WFNM_NOMATCH) {
		s[index] = L'\0';
		break;
	    }
	    index++;
	}
	slist++;
    }
}

/* Does `match_each' for 'PT_MATCHTAIL' where `PT_MATCHLONGEST' is false. */
void match_tail_shortest_each(void **slist, const wchar_t *pattern)
{
    enum wfnmflags flags = shopt_nocaseglob ? WFNM_CASEFOLD : 0;
    size_t minlen = shortest_match_length(pattern, flags);
    wchar_t *s;
    while ((s = *slist)) {
	size_t len = wcslen(s);
	size_t index = len - minlen;
	do {
	    size_t match = wfnmatchl(
		    pattern, s + index, flags, WFNM_WHOLE, minlen);
	    if (match == WFNM_ERROR) {
		return;
	    } else if (match != WFNM_NOMATCH) {
		s[index] = L'\0';
		break;
	    }
	} while (index--);
	slist++;
    }
}

/* Matches each string in the array to the specified pattern and substitutes the
 * matching portions with `subst'.
 * `slist' is a NULL-terminated array of pointers to `free'able wide strings.
 * `type' may contain PT_MATCHHEAD, PT_MATCHTAIL and PT_SUBSTALL.
 * Elements of `slist' may be modified and `realloc'ed in this function. */
void subst_each(void **slist, const wchar_t *pattern, const wchar_t *subst,
	paramexptype_T type)
{
    if (type & PT_MATCHHEAD) {
	if (type & PT_MATCHTAIL)
	    subst_whole_each(slist, pattern, subst);
	else
	    subst_head_each(slist, pattern, subst);
    } else if (type & PT_MATCHTAIL) {
	subst_tail_each(slist, pattern, subst);
    } else {
	subst_generic_each(slist, pattern, subst, type & PT_SUBSTALL);
    }
}

/* Does `subst_each' where `PT_MATCHHEAD' and `PT_MATCHTAIL' is true. */
void subst_whole_each(
	void **slist, const wchar_t *pattern, const wchar_t *subst)
{
    enum wfnmflags flags = shopt_nocaseglob ? WFNM_CASEFOLD : 0;
    size_t minlen = shortest_match_length(pattern, flags);
    wchar_t *s;
    while ((s = *slist)) {
	size_t match = wfnmatchl(pattern, s, flags, WFNM_WHOLE, minlen);
	if (match == WFNM_ERROR) {
	    break;
	} else if (match != WFNM_NOMATCH) {
	    free(s);
	    *slist = xwcsdup(subst);
	}
	slist++;
    }
}

/* Does `subst_each' where `PT_MATCHHEAD' is true but `PT_MATCHTAIL' is not. */
void subst_head_each(
	void **slist, const wchar_t *pattern, const wchar_t *subst)
{
    enum wfnmflags flags = shopt_nocaseglob ? WFNM_CASEFOLD : 0;
    size_t minlen = shortest_match_length(pattern, flags);
    wchar_t *s;
    while ((s = *slist)) {
	size_t match = wfnmatchl(pattern, s, flags, WFNM_LONGEST, minlen);
	if (match == WFNM_ERROR) {
	    break;
	} else if (match != WFNM_NOMATCH) {
	    xwcsbuf_T buf;
	    wb_init(&buf);
	    wb_cat(&buf, subst);
	    wb_cat(&buf, s + match);
	    free(s);
	    *slist = wb_towcs(&buf);
	}
	slist++;
    }
}

/* Does `subst_each' where `PT_MATCHTAIL' is true but `PT_MATCHHEAD' is not. */
void subst_tail_each(
	void **slist, const wchar_t *pattern, const wchar_t *subst)
{
    enum wfnmflags flags = shopt_nocaseglob ? WFNM_CASEFOLD : 0;
    size_t minlen = shortest_match_length(pattern, flags);
    wchar_t *s;
    while ((s = *slist)) {
	size_t len = wcslen(s);
	size_t index = 0;
	while (minlen + index <= len) {
	    size_t match = wfnmatchl(
		    pattern, s + index, flags, WFNM_WHOLE, minlen);
	    if (match == WFNM_ERROR) {
		return;
	    } else if (match != WFNM_NOMATCH) {
		xwcsbuf_T buf;
		wb_init(&buf);
		wb_ncat(&buf, s, index);
		wb_cat(&buf, subst);
		free(s);
		*slist = wb_towcs(&buf);
		break;
	    }
	    index++;
	}
	slist++;
    }
}

/* Does `subst_each' where neither `PT_MATCHHEAD' nor `PT_MATCHTAIL' is true. */
void subst_generic_each(void **slist,
	const wchar_t *pattern, const wchar_t *subst, bool substall)
{
    enum wfnmflags flags = shopt_nocaseglob ? WFNM_CASEFOLD : 0;
    size_t minlen = shortest_match_length(pattern, flags);
    wchar_t *s;
    while ((s = *slist)) {
	size_t index = 0;
	xwcsbuf_T buf;
	wb_init(&buf);
	while (s[index]) {
	    size_t match = wfnmatchl(
		    pattern, s + index, flags, WFNM_LONGEST, minlen);
	    if (match == WFNM_ERROR) {
		return;
	    } else if (match != WFNM_NOMATCH && match > 0) {
		wb_cat(&buf, subst);
		index += match;
		if (!substall) {
		    wb_cat(&buf, s + index);
		    break;
		} else {
		    continue;
		}
	    } else {
		wb_wccat(&buf, s[index]);
	    }
	    index++;
	}
	free(s);
	*slist = wb_towcs(&buf);
	slist++;
    }
}

/* Substitutes each string in the array with a string that contains the number
 * of characters in the original string.
 * `slist' is a NULL-terminated array of pointers to `free'able wide strings.
 * The strings are `realloc'ed and modified in this function. */
void subst_length_each(void **slist)
{
    wchar_t *s;
    while ((s = *slist)) {
	*slist = malloc_wprintf(L"%zu", wcslen(s));
	free(s);
	slist++;
    }
}


/********** Brace Expansions **********/

/* Does brace expansion in each element of the specified array.
 * `values' is an array of pointers to `free'able wide strings to be expanded.
 * `splits' is an array of pointers to `free'able splittability strings.
 * Both the arrays must be NULL-terminated and their elements are freed in this
 * function. The arrays themselves are not freed.
 * Newly malloced results are added to `valuelist' and `splitlist'. */
void expand_brace_each(void **restrict values, void **restrict splits,
	plist_T *restrict valuelist, plist_T *restrict splitlist)
{
    while (*values) {
	expand_brace(*values, *splits, valuelist, splitlist);
	values++;
	splits++;
    }
}

/* Does brace expansion in a single word.
 * `word' is freed in this function. `split' is a splittability string
 * corresponding to `word' and is also freed.
 * Newly malloced results are added to `valuelist' and `splitlist'. */
void expand_brace(wchar_t *restrict const word, char *restrict const split,
	plist_T *restrict valuelist, plist_T *restrict splitlist)
{
    plist_T elemlist;
    wchar_t *c;
    unsigned nest;

    c = word;
start:
    c = escaped_wcspbrk(c, L"{");
    if (!c || !*++c) {
	/* don't expand if there is no L'{' or L'{' is at the end of string */
	pl_add(valuelist, word);
	pl_add(splitlist, split);
	return;
    } else if (tryexpand_brace_sequence(word, split, c, valuelist, splitlist)) {
	return;
    }

    /* add each expanded elements to `elemlist' */
    pl_init(&elemlist);
    pl_add(&elemlist, c);
    nest = 0;
    while ((c = escaped_wcspbrk(c, L"{,}"))) {
	switch (*c++) {
	    case L'{':
		nest++;
		break;
	    case L',':
		if (nest == 0)
		    pl_add(&elemlist, c);
		break;
	    case L'}':
		if (nest > 0) {
		    nest--;
		    break;
		} else if (elemlist.length == 1) {
		    goto restart;
		} else {
		    pl_add(&elemlist, c);
		    goto done;
		}
	}
    }
restart:
    /* if there is no L',' or L'}' corresponding to L'{',
     * find the next L'{' and try again */
    c = elemlist.contents[0];
    pl_destroy(&elemlist);
    goto start;

done:;
#define idx(p)  ((wchar_t *) (p) - word)
#define wtos(p) (split + idx(p))
    size_t lastelemindex = elemlist.length - 1;
    size_t headlength = idx(elemlist.contents[0]) - 1;
    size_t lastlen = wcslen(elemlist.contents[lastelemindex]);
    for (size_t i = 0; i < lastelemindex; i++) {
	xwcsbuf_T buf;
	xstrbuf_T sbuf;
	wb_init(&buf);
	sb_init(&sbuf);

	wb_ncat(&buf, word, headlength);
	sb_ncat_force(&sbuf, split, headlength);

	wb_ncat(&buf, elemlist.contents[i],
		(wchar_t *) elemlist.contents[i + 1] -
		(wchar_t *) elemlist.contents[i    ] - 1);
	sb_ncat_force(&sbuf, wtos(elemlist.contents[i]),
		(wchar_t *) elemlist.contents[i + 1] -
		(wchar_t *) elemlist.contents[i    ] - 1);

	wb_ncat_force(&buf, elemlist.contents[lastelemindex], lastlen);
	sb_ncat_force(&sbuf, wtos(elemlist.contents[lastelemindex]), lastlen);
	assert(buf.length == sbuf.length);

	/* expand the remaining portion recursively */
	expand_brace(wb_towcs(&buf), sb_tostr(&sbuf), valuelist, splitlist);
    }
    pl_destroy(&elemlist);
    free(word);
    free(split);
#undef idx
#undef wtos
}

/* Tries numeric brace expansion like "{01..05}".
 * If unsuccessful, this function has no side effects and returns false.
 * If successful, `word' and `split' are freed and the full expansion results
 * are added to `valuelist' and `splitlist'.
 * `startc' is a pointer to the character immediately succeeding L'{' in `word'.
 */
bool tryexpand_brace_sequence(
	wchar_t *word, char *split, wchar_t *startc,
	plist_T *restrict valuelist, plist_T *restrict splitlist)
{
    long start, end, value;
    wchar_t *dotexpect, *braceexpect, *c;
    int startlen, endlen, len, wordlen;
    bool sign = false;

    assert(startc[-1] == L'{');
    c = startc;

    /* parse the starting point */
    dotexpect = wcschr(c, L'.');
    if (!dotexpect || c == dotexpect)
	return false;
    startlen = has_leading_zero(c, &sign) ? (dotexpect - c) : 0;
    errno = 0;
    start = wcstol(c, &c, 0);
    if (errno || c != dotexpect || c[1] != L'.')
	return false;

    c += 2;

    /* parse the ending point */
    braceexpect = wcschr(c, L'}');
    if (!braceexpect || c == braceexpect)
	return false;
    endlen = has_leading_zero(c, &sign) ? (braceexpect - c) : 0;
    errno = 0;
    end = wcstol(c, &c, 0);
    if (errno || c != braceexpect)
	return false;

    /* expand the sequence */
    value = start;
    len = (startlen > endlen) ? startlen : endlen;
    wordlen = wcslen(word);
    for (;;) {
	xwcsbuf_T buf;
	xstrbuf_T sbuf;
	wb_init(&buf);
	sb_init(&sbuf);

	wb_ncat(&buf, word, startc - 1 - word);
	sb_ncat_force(&sbuf, split, startc - 1 - word);

	int plen = wb_wprintf(&buf, sign ? L"%0+*ld" : L"%0*ld", len, value);
	if (plen > 0)
	    sb_ccat_repeat(&sbuf, 0, plen);

	wb_cat(&buf, braceexpect + 1);
	sb_ncat_force(&sbuf,
		split + (braceexpect + 1 - word),
		wordlen - (braceexpect + 1 - word));
	assert(buf.length == sbuf.length || plen < 0);

	/* expand the remaining portion recursively */
	expand_brace(wb_towcs(&buf), sb_tostr(&sbuf), valuelist, splitlist);

	if (value == end)
	    break;
	if (start < end)
	    value++;
	else
	    value--;
    }
    free(word);
    free(split);
    return true;
}

/* Checks if the specified numeral starts with a L'0'.
 * Leading spaces are ignored.
 * If the numeral has a plus sign L'+', true is assigned to `*sign'.
 * If not, false is assigned. */
bool has_leading_zero(const wchar_t *s, bool *sign)
{
    while (iswspace(*s))
	s++;
    if (*s == L'+') {
	*sign = true;
	s++;
    } else if (*s == L'-') {
	s++;
    }
    return *s == L'0';
}


/********** Word Splitting **********/

/* Performs word splitting.
 * `str' is the word to split and freed in this function.
 * `split' is the splittability string corresponding to `str' and also freed.
 * The results are added to `dest' as newly malloced wide strings.
 * `ifs' must not be NULL.
 * The word is split at non-backslashed characters that are contained in `ifs'
 * and whose corresponding character in the splittability string is non-zero. */
void fieldsplit(wchar_t *restrict s, char *restrict split,
	const wchar_t *restrict ifs, plist_T *restrict dest)
{
    size_t index = 0, startindex = 0;
    size_t savedestlen = dest->length;

    while (s[index]) {
	if (s[index] == L'\\') {
	    index++;
	    if (!s[index])
		break;
	    index++;
	} else if (split[index] && wcschr(ifs, s[index])) {
	    /* the character is in `ifs', so do splitting */
	    bool splitonnonspace = false, nonspace = false;
	    if (startindex < index)
		pl_add(dest, xwcsndup(s + startindex, index - startindex));
	    else
		splitonnonspace = true;
	    do {
		if (!iswspace(s[index])) {
		    if (splitonnonspace)
			pl_add(dest, xwcsdup(L""));
		    splitonnonspace = true;
		    nonspace = true;
		}
		index++;
		if (!s[index]) {
		    if (nonspace && startindex < index)
			pl_add(dest, xwcsdup(L""));
		    break;
		}
	    } while (split[index] && wcschr(ifs, s[index]));
	    startindex = index;
	} else {
	    index++;
	}
    }
    if (savedestlen == dest->length) {
	/* if there were no splittings, simply add the original string */
	assert(startindex == 0);
	pl_add(dest, s);
    } else {
	if (startindex < index)
	    pl_add(dest, xwcsndup(s + startindex, index - startindex));
	free(s);
    }
    free(split);
}

/* Performs word splitting.
 * `valuelist' is a NULL-terminated array of pointers to wide strings to split.
 * `splitlist' is an array of pointers to corresponding splittability strings.
 * `valuelist' and `splitlist' are `recfree'ed in this function.
 * The results are added to `dest'. */
void fieldsplit_all(void **restrict valuelist, void **restrict splitlist,
	plist_T *restrict dest)
{
    void **s = valuelist, **t = splitlist;
    const wchar_t *ifs = getvar(VAR_IFS);
    if (!ifs)
	ifs = L" \t\n";
    while (*s) {
	fieldsplit(*s, *t, ifs, dest);
	s++, t++;
    }
    free(valuelist);
    free(splitlist);
}


/********** Escaping **********/

/* Unquote singlequotes in a string and adds it to the specified buffer.
 * `ss' is a pointer to a pointer to the opening quote.
 * `ss' is incremented so that it points to the closing quote.
 * If `escape' is true, all the characters added are backslashed. */
void add_sq(const wchar_t *restrict *ss, xwcsbuf_T *restrict buf, bool escape)
{
    (*ss)++;
    for (;;) {
	switch (**ss) {
	    case L'\0':
		assert(false);
	    case L'\'':
		return;
	    default:
		if (escape)
		    wb_wccat(buf, L'\\');
		wb_wccat(buf, **ss);
		break;
	}
	(*ss)++;
    }
}

/* Backslashes characters in `s' that are contained in `t'.
 * Returns a newly malloced wide string.
 * `t' may be NULL, in which case all the characters are backslashed. */
wchar_t *escape(const wchar_t *restrict s, const wchar_t *restrict t)
{
    xwcsbuf_T buf;
    wb_init(&buf);
    while (*s) {
	if (!t || wcschr(t, *s))
	    wb_wccat(&buf, L'\\');
	wb_wccat(&buf, *s);
	s++;
    }
    return wb_towcs(&buf);
}

/* Same as `escape', except that the first argument is freed. */
wchar_t *escapefree(wchar_t *restrict s, const wchar_t *restrict t)
{
    if (t && !wcspbrk(s, t)) {
	return s;
    } else {
	wchar_t *result = escape(s, t);
	free(s);
	return result;
    }
}

/* Removes backslash escapes. The result is a newly malloced string. */
wchar_t *unescape(const wchar_t *s)
{
    xwcsbuf_T buf;
    wb_init(&buf);
    while (*s) {
	if (*s == L'\\') {
	    if (*(s+1) == L'\0')
		break;
	    else
		s++;
	}
	wb_wccat(&buf, *s);
	s++;
    }
    return wb_towcs(&buf);
}

/* Same as `unescape', except that the first argument is freed. */
wchar_t *unescapefree(wchar_t *s)
{
    if (!wcschr(s, L'\\')) {
	return s;
    } else {
	wchar_t *result = unescape(s);
	free(s);
	return result;
    }
}

/* Backslashes all the characters in the specified string.
 * Some characters of the string may be already backslashed;
 * they are not re-backslashed. The result is a newly malloced string. */
wchar_t *reescape(const wchar_t *s)
{
    xwcsbuf_T buf;

    wb_init(&buf);
    while (*s) {
	wb_wccat(&buf, L'\\');
	if (*s == L'\\')
	    s++;
	if (*s)
	    wb_wccat(&buf, *s++);
    }
    return wb_towcs(&buf);
}

/* Applies `reescape' to each element of the specified array.
 * All the original elements are freed and substituted with newly malloced
 * results. */
void **reescape_full_array(void **const wcsarray)
{
    void **ary = wcsarray;
    while (*ary) {
	wchar_t *esc = reescape(*ary);
	free(*ary);
	*ary = esc;
	ary++;
    }
    return wcsarray;
}

/* Removes quotes (', ", \). The result is a newly malloced string. */
wchar_t *unquote(const wchar_t *s)
{
    bool indq = false;
    xwcsbuf_T buf;
    wb_init(&buf);
    for (;;) {
	switch (*s) {
	case L'\0':
	    goto done;
	case L'\'':
	    if (indq)
		goto default_case;
	    add_sq(&s, &buf, false);
	    break;
	case L'"':
	    indq = !indq;
	    break;
	case L'\\':
	    if (s[1] != L'\0' && (!indq || wcschr(ESCAPABLE_CHARS, s[1]))) {
		wb_wccat(&buf, s[1]);
		s += 2;
		continue;
	    }
	    /* falls thru! */
	default:  default_case:
	    wb_wccat(&buf, *s);
	    break;
	}
	s++;
    }
done:
    return wb_towcs(&buf);
}

/* Like `wcspbrk', but ignores backslashed characters in `s'. */
wchar_t *escaped_wcspbrk(const wchar_t *s, const wchar_t *accept)
{
    while (*s) {
	if (*s == L'\\') {
	    s++;
	    if (!*s)
		break;
	    s++;
	    continue;
	}
	if (wcschr(accept, *s))
	    return (wchar_t *) s;
	s++;
    }
    return NULL;
}


/********** File Name Expansion (Glob) **********/

/* Makes a option value from the current shell settings. */
enum wglbflags get_wglbflags(void)
{
    enum wglbflags flags = 0;
    if (shopt_nocaseglob)   flags |= WGLB_CASEFOLD;
    if (shopt_dotglob)      flags |= WGLB_PERIOD;
    if (shopt_markdirs)     flags |= WGLB_MARK;
    if (shopt_extendedglob) flags |= WGLB_RECDIR;
    return flags;
}

/* Performs file name expansion to each pattern.
 * `patterns' is a NULL-terminated array of pointers to wide strings cast to
 * (void *). `patterns' is `recfree'd in this function.
 * The results are added to `list' as newly malloced wide strings. */
void do_glob_each(void **restrict patterns, plist_T *restrict list)
{
    void **const savepatterns = patterns;
    enum wglbflags flags = get_wglbflags();

    while (*patterns) {
	wchar_t *pat = *patterns;
	if (pattern_has_special_char(pat)) {
	    size_t oldlen = list->length;
	    wglob(pat, flags, list);
	    if (!shopt_nullglob && oldlen == list->length)
		goto addpattern;
	    free(pat);
	} else {
	    /* if the pattern doesn't contain characters like L'*' and L'?',
	     * we don't need to glob. */
addpattern:
	    pl_add(list, unescapefree(pat));
	}
	patterns++;
    }
    free(savepatterns);
}


/********** Auxiliary functions **********/

/* Performs parameter expansions, command substitutions in "$(...)" form and
 * arithmetic expansions in the specified string.
 * If `name' is non-NULL, it is printed in error messages on error.
 * Returns a newly malloced string if successful. Otherwise NULL is returned.
 * This function is reentrant. */
wchar_t *parse_and_expand_string(const wchar_t *s, const char *name)
{
    struct input_wcs_info winfo = {
	.src = s,
    };
    parseinfo_T info = {
	.print_errmsg = true,
	.enable_verbose = false,
	.filename = name,
	.lineno = 1,
	.input = input_wcs,
	.inputinfo = &winfo,
    };
    wordunit_T *word;
    wchar_t *result;

    if (!parse_string(&info, &word))
	return NULL;
    result = expand_string(word, false);
    wordfree(word);
    return result;
}


/* vim: set ts=8 sts=4 sw=4 noet: */
