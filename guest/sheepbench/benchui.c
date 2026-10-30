/*
 *  benchui.c - SheepBench user interface: console, or a PDCurses window (SHEEPBENCHCURSES)
 *
 *	(C) 2026 RandoOnSteam (battlemageloveryt@gmail.com)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <MacTypes.h>
#include <Memory.h>
#include <Files.h>
#include "bench.h"

#ifdef SHEEPBENCHCURSES

#include <StandardFile.h>
#include <curses.h>

#define CURSESLINES 36
#define CURSESCOLUMNS 112
#define CURSESTOP 4
#define CURSESITEMQUICK 7
#define CURSESITEMJITOFF 8
#define CURSESITEMRUN 9
#define CURSESITEMRESULTS 10
#define CURSESITEMHOST 11
#define CURSESITEMSAVE 12
#define CURSESITEMCOMPARE 13
#define CURSESITEMQUIT 14
#define CURSESITEMS 15
#define CURSESKEYESCAPE 27
#define CURSESKEYCLOSE KEY_F(24)
#define CURSESMAXSAVED 512
#define CURSESHELPMENU " Up/Down move  Space/Return choose  R run  V results  H host  S save  C compare  Q quit "
#define CURSESHELPRUN " Running: Esc or Command-period stops between repetitions "
#define CURSESHELPVIEW " Up/Down/PgUp/PgDn scroll  Home/End  Esc or Return back "

typedef struct BenchCurses {
	WINDOW *log;
	int selected;
	Boolean canswitch;
	char status[160];
	char message[160];
} BenchCurses;

typedef struct BenchSaved {
	char suite[12];
	char test[80];
	double value;
} BenchSaved;

typedef struct BenchCompare {
	Bench *bench;
	BenchSaved *saved;
	int count;
	char name[64];
} BenchCompare;

typedef void (*BenchViewLine)(void *context, int index, char *text, int capacity);

static BenchCurses *BenchCursesState(Bench *bench)
{
	return (BenchCurses *)bench->ui;
}

static void BenchCursesBar(int row, const char *text)
{
	attron(A_REVERSE);
	mvhline(row, 0, ' ', COLS);
	mvaddnstr(row, 0, text, COLS);
	attroff(A_REVERSE);
}

static void BenchCursesProgress(Bench *bench)
{
	char text[80];
	double fraction;
	int passes;
	int width;
	int filled;
	int index;
	passes = 1;
	if (bench->jitoff)
		passes = 2;
	fraction = 0;
	if (bench->suitecount > 0 && bench->suiteindex > 0)
		fraction = ((double)bench->pass * bench->suitecount + bench->suiteindex - 0.5) / ((double)passes * bench->suitecount);
	if (fraction > 1)
		fraction = 1;
	width = COLS - 40;
	if (width < 10)
		width = 10;
	filled = (int)(fraction * width);
	move(2, 0);
	clrtoeol();
	mvaddch(2, 1, '[');
	for (index = 0; index < width; index++) {
		if (index < filled)
			mvaddch(2, 2 + index, '#');
		else
			mvaddch(2, 2 + index, '.');
	}
	mvaddch(2, 2 + width, ']');
	if (bench->suitename && bench->suiteindex > 0) {
		snprintf(text, sizeof(text), " suite %d/%d %s", bench->suiteindex, bench->suitecount, bench->suitename);
		if (bench->pass == BENCHPASSJITOFF)
			strcat(text, " [JIT off]");
		mvaddnstr(2, 4 + width, text, COLS - 5 - width);
	}
}

static void BenchCursesHeader(Bench *bench, const char *help)
{
	BenchCurses *ui;
	char title[200];
	const char *switching;
	char host[32];
	char jit[32];
	ui = BenchCursesState(bench);
	strcpy(host, "no host");
	strcpy(jit, "?");
	BenchHostValue(bench, "host", host, sizeof(host));
	BenchHostValue(bench, "jit", jit, sizeof(jit));
	switching = "not allowed";
	if (ui->canswitch)
		switching = "allowed";
	snprintf(title, sizeof(title), " SheepBench 2    host: %s    JIT: %s    switching: %s    timebase: %ld Hz", host, jit,
		switching, (long)bench->timebase);
	BenchCursesBar(0, title);
	move(1, 0);
	clrtoeol();
	mvaddnstr(1, 1, ui->status, COLS - 2);
	BenchCursesProgress(bench);
	mvhline(3, 0, ACS_HLINE, COLS);
	BenchCursesBar(LINES - 1, help);
}

static void BenchCursesSetStatus(Bench *bench, const char *text)
{
	BenchCurses *ui;
	ui = BenchCursesState(bench);
	strncpy(ui->status, text, sizeof(ui->status) - 1);
	ui->status[sizeof(ui->status) - 1] = 0;
}

Boolean BenchUiOpen(Bench *bench)
{
	BenchCurses *ui;
	ui = (BenchCurses *)NewPtrClear(sizeof(BenchCurses));
	if (ui == NULL)
		return false;
	bench->ui = ui;
	resize_term(CURSESLINES, CURSESCOLUMNS);
	if (initscr() == NULL)
		return false;
	PDC_set_title("SheepBench");
	PDC_set_function_key(FUNCTION_KEY_SHUT_DOWN, CURSESKEYCLOSE);
	cbreak();
	noecho();
	keypad(stdscr, TRUE);
	curs_set(0);
	mousemask(BUTTON1_CLICKED | BUTTON1_PRESSED, NULL);
	ui->canswitch = BenchHostIs(bench, "benchswitch", "1") && BenchHostIs(bench, "jit", "on");
	ui->selected = CURSESITEMRUN;
	BenchCursesSetStatus(bench, "Pick the suites and choose Run.");
	return true;
}

void BenchUiClose(Bench *bench)
{
	endwin();
	if (bench->ui)
		DisposePtr((Ptr)bench->ui);
	bench->ui = NULL;
}

void BenchUiLine(Bench *bench, const char *text)
{
	BenchCurses *ui;
	ui = BenchCursesState(bench);
	if (ui == NULL || ui->log == NULL)
		return;
	waddnstr(ui->log, text, COLS - 1);
	waddch(ui->log, '\n');
	touchwin(stdscr);
	wnoutrefresh(stdscr);
	wnoutrefresh(ui->log);
	doupdate();
}

void BenchUiStatus(Bench *bench, const char *text)
{
	BenchCurses *ui;
	ui = BenchCursesState(bench);
	if (ui == NULL || ui->log == NULL)
		return;
	BenchCursesSetStatus(bench, text);
	move(1, 0);
	clrtoeol();
	mvaddnstr(1, 1, ui->status, COLS - 2);
	BenchCursesProgress(bench);
	wnoutrefresh(stdscr);
	doupdate();
}

void BenchUiPoll(Bench *bench)
{
	int key;
	nodelay(stdscr, TRUE);
	key = getch();
	nodelay(stdscr, FALSE);
	if (key == CURSESKEYESCAPE || key == 'q' || key == 'Q' || key == CURSESKEYCLOSE)
		bench->cancelled = true;
}

static int BenchMenuRow(int item)
{
	if (item < CURSESITEMQUICK)
		return CURSESTOP + 2 + item;
	if (item < CURSESITEMRUN)
		return CURSESTOP + 11 + item - CURSESITEMQUICK;
	return CURSESTOP + 15 + item - CURSESITEMRUN;
}

static int BenchMenuItemAt(int row)
{
	int item;
	for (item = 0; item < CURSESITEMS; item++) {
		if (BenchMenuRow(item) == row)
			return item;
	}
	return -1;
}

static void BenchMenuCheck(int item, Boolean selected, Boolean checked, const char *name, const char *description)
{
	char text[160];
	const char *mark;
	mark = "[ ]";
	if (checked)
		mark = "[x]";
	snprintf(text, sizeof(text), "  %s %-10s %s", mark, name, description);
	if (selected)
		attron(A_REVERSE);
	mvaddnstr(BenchMenuRow(item), 2, text, COLS - 4);
	if (selected)
		attroff(A_REVERSE);
}

static void BenchMenuAction(int item, Boolean selected, const char *text)
{
	if (selected)
		attron(A_REVERSE);
	mvaddstr(BenchMenuRow(item), 4, text);
	if (selected)
		attroff(A_REVERSE);
}

static void BenchMenuDraw(Bench *bench, const BenchOptions *options)
{
	BenchCurses *ui;
	const char *jittext;
	int index;
	ui = BenchCursesState(bench);
	erase();
	BenchCursesHeader(bench, CURSESHELPMENU);
	attron(A_BOLD);
	mvaddstr(CURSESTOP + 1, 2, "Suites");
	mvaddstr(CURSESTOP + 10, 2, "Options");
	mvaddstr(CURSESTOP + 14, 2, "Actions");
	attroff(A_BOLD);
	for (index = 0; index < BenchSuiteCount(); index++)
		BenchMenuCheck(index, ui->selected == index, (options->suites & BenchSuiteFlag(index)) != 0, BenchSuiteName(index),
			BenchSuiteDescription(index));
	BenchMenuCheck(CURSESITEMQUICK, ui->selected == CURSESITEMQUICK, options->quick, "quick",
		"shorter repetitions and budgets (10 ms, 9 repetitions, 1 s)");
	jittext = "repeat everything with the JIT off and show the speedup";
	if (!ui->canswitch)
		jittext = "needs the pref benchswitch true and a JIT that was on at start-up";
	BenchMenuCheck(CURSESITEMJITOFF, ui->selected == CURSESITEMJITOFF, options->jitoff && ui->canswitch, "JIT off", jittext);
	BenchMenuAction(CURSESITEMRUN, ui->selected == CURSESITEMRUN, "Run");
	BenchMenuAction(CURSESITEMRESULTS, ui->selected == CURSESITEMRESULTS, "View results");
	BenchMenuAction(CURSESITEMHOST, ui->selected == CURSESITEMHOST, "Host information");
	BenchMenuAction(CURSESITEMSAVE, ui->selected == CURSESITEMSAVE, "Save report...");
	BenchMenuAction(CURSESITEMCOMPARE, ui->selected == CURSESITEMCOMPARE, "Compare with a saved report...");
	BenchMenuAction(CURSESITEMQUIT, ui->selected == CURSESITEMQUIT, "Quit");
	if (ui->message[0])
		mvaddnstr(BenchMenuRow(CURSESITEMQUIT) + 2, 2, ui->message, COLS - 4);
	refresh();
}

static void BenchView(Bench *bench, const char *header, int count, BenchViewLine line, void *context)
{
	char text[260];
	int top;
	int height;
	int index;
	int key;
	top = 0;
	height = LINES - CURSESTOP - 3;
	for (;;) {
		erase();
		BenchCursesHeader(bench, CURSESHELPVIEW);
		attron(A_BOLD);
		mvaddnstr(CURSESTOP, 1, header, COLS - 2);
		attroff(A_BOLD);
		for (index = 0; index < height && top + index < count; index++) {
			text[0] = 0;
			line(context, top + index, text, sizeof(text));
			mvaddnstr(CURSESTOP + 1 + index, 1, text, COLS - 2);
		}
		refresh();
		key = getch();
		if (key == KEY_UP && top > 0)
			top--;
		else if (key == KEY_DOWN && top + height < count)
			top++;
		else if (key == KEY_PPAGE) {
			top -= height;
			if (top < 0)
				top = 0;
		} else if (key == KEY_NPAGE) {
			if (top + height < count)
				top += height;
		} else if (key == KEY_HOME)
			top = 0;
		else if (key == KEY_END) {
			top = count - height;
			if (top < 0)
				top = 0;
		} else if (key == CURSESKEYESCAPE || key == 'q' || key == 'Q' || key == '\n' || key == '\r' || key == KEY_ENTER ||
			key == CURSESKEYCLOSE)
			return;
	}
}

static const char *BenchStatusText(int status)
{
	static const char *names[4] = {"not run", "ok", "skipped", "FAILED"};
	if (status < 0 || status > 3)
		return "";
	return names[status];
}

static Boolean BenchLowerIsBetter(int unit)
{
	return unit == BENCHUNITNSINSN || unit == BENCHUNITNS || unit == BENCHUNITUS;
}

static void BenchResultLine(void *context, int index, char *text, int capacity)
{
	Bench *bench;
	BenchRow *row;
	char value[24];
	char noise[24];
	char native[24];
	char off[24];
	char speedup[24];
	double nativevalue;
	bench = (Bench *)context;
	row = &bench->rows[index];
	BenchNumber(value, row->value[BENCHPASSMAIN]);
	BenchNumber(noise, row->noise[BENCHPASSMAIN]);
	BenchNumber(native, BENCHNOVALUE);
	BenchNumber(off, row->value[BENCHPASSJITOFF]);
	BenchNumber(speedup, BENCHNOVALUE);
	if (row->native > 0 && row->value[BENCHPASSMAIN] > 0 && row->work > 0) {
		nativevalue = BenchUnitValue(row->unit, row->native * 1e-9, row->work);
		if (BenchLowerIsBetter(row->unit))
			BenchNumber(native, nativevalue / row->value[BENCHPASSMAIN] * 100.0);
		else
			BenchNumber(native, row->value[BENCHPASSMAIN] / nativevalue * 100.0);
	}
	if (row->value[BENCHPASSMAIN] > 0 && row->value[BENCHPASSJITOFF] > 0) {
		if (BenchLowerIsBetter(row->unit))
			BenchNumber(speedup, row->value[BENCHPASSJITOFF] / row->value[BENCHPASSMAIN]);
		else
			BenchNumber(speedup, row->value[BENCHPASSMAIN] / row->value[BENCHPASSJITOFF]);
	}
	if (row->status == BENCHROWOK && row->value[BENCHPASSMAIN] <= 0)
		snprintf(text, capacity, "%-9s %-44.44s %10s", row->suite, row->test, "ok");
	else if (row->status == BENCHROWSKIPPED || row->status == BENCHROWFAILED)
		snprintf(text, capacity, "%-9s %-44.44s %s: %s", row->suite, row->test, BenchStatusText(row->status), row->why);
	else
		snprintf(text, capacity, "%-9s %-44.44s %10s %-7s %6s %8s %10s %7s", row->suite, row->test, value,
			BenchUnitName(row->unit), noise, native, off, speedup);
}

static void BenchShowResults(Bench *bench)
{
	char header[200];
	snprintf(header, sizeof(header), "%-9s %-44s %10s %-7s %6s %8s %10s %7s", "suite", "test", "value", "unit", "noise%",
		"native%", "JIT off", "speedup");
	BenchView(bench, header, bench->rowcount, BenchResultLine, bench);
}

static int BenchHostLineCount(Bench *bench)
{
	const char *text;
	int count;
	count = 0;
	for (text = bench->hostinfo; *text; text++) {
		if (*text == '\n')
			count++;
	}
	return count + 1;
}

static void BenchHostLine(void *context, int index, char *text, int capacity)
{
	Bench *bench;
	const char *line;
	const char *end;
	int length;
	bench = (Bench *)context;
	line = bench->hostinfo;
	while (index > 0 && line) {
		line = strchr(line, '\n');
		if (line)
			line++;
		index--;
	}
	if (line == NULL)
		return;
	end = strchr(line, '\n');
	length = (int)strlen(line);
	if (end)
		length = (int)(end - line);
	if (length >= capacity)
		length = capacity - 1;
	memcpy(text, line, length);
	text[length] = 0;
}

static void BenchShowHost(Bench *bench)
{
	if (!BenchHostRefresh(bench)) {
		strcpy(bench->hostinfo, "No Sheep Shears mailbox: this is not a SheepShaver that answers SheepBench.");
	}
	BenchView(bench, "Host information (Sheep Shears info command)", BenchHostLineCount(bench), BenchHostLine, bench);
}

static void BenchSave(Bench *bench)
{
	BenchCurses *ui;
	StandardFileReply reply;
	OSErr err;
	ui = BenchCursesState(bench);
	if (bench->rowcount == 0) {
		strcpy(ui->message, "Nothing to save yet: run SheepBench first.");
		return;
	}
	StandardPutFile("\pSave the SheepBench report as:", "\pSheepBench Report", &reply);
	touchwin(curscr);
	if (!reply.sfGood) {
		ui->message[0] = 0;
		return;
	}
	err = BenchWriteReport(bench, &reply.sfFile);
	if (err == noErr)
		snprintf(ui->message, sizeof(ui->message), "Saved the report as \"%.*s\".", reply.sfFile.name[0], reply.sfFile.name + 1);
	else
		snprintf(ui->message, sizeof(ui->message), "Could not save the report (error %d).", (int)err);
}

static char *BenchField(char **cursor)
{
	char *start;
	char *tab;
	start = *cursor;
	tab = strchr(start, '\t');
	if (tab) {
		*tab = 0;
		*cursor = tab + 1;
	} else
		*cursor = start + strlen(start);
	return start;
}

static int BenchReadSaved(const FSSpec *spec, BenchSaved *saved, int capacity)
{
	Handle buffer;
	char *text;
	char *line;
	char *end;
	char *cursor;
	char *value;
	short file;
	long length;
	int count;
	if (FSpOpenDF(spec, fsRdPerm, &file) != noErr)
		return -1;
	if (GetEOF(file, &length) != noErr || length <= 0 || length > 1048576L) {
		FSClose(file);
		return -1;
	}
	buffer = NewHandle(length + 1);
	if (buffer == NULL) {
		FSClose(file);
		return -1;
	}
	HLock(buffer);
	text = *buffer;
	FSRead(file, &length, text);
	FSClose(file);
	text[length] = 0;
	count = 0;
	line = strpbrk(text, "\r\n");
	while (line && count < capacity) {
		line++;
		end = strpbrk(line, "\r\n");
		if (end)
			*end = 0;
		cursor = line;
		strncpy(saved[count].suite, BenchField(&cursor), sizeof(saved[count].suite) - 1);
		strncpy(saved[count].test, BenchField(&cursor), sizeof(saved[count].test) - 1);
		BenchField(&cursor);
		value = BenchField(&cursor);
		saved[count].value = BENCHNOVALUE;
		if (value[0] >= '0' && value[0] <= '9')
			saved[count].value = atof(value);
		if (saved[count].suite[0])
			count++;
		line = end;
	}
	DisposeHandle(buffer);
	return count;
}

static BenchRow *BenchFindRow(Bench *bench, const BenchSaved *saved)
{
	int index;
	for (index = 0; index < bench->rowcount; index++) {
		if (strcmp(bench->rows[index].suite, saved->suite) == 0 && strcmp(bench->rows[index].test, saved->test) == 0)
			return &bench->rows[index];
	}
	return NULL;
}

static void BenchCompareLine(void *context, int index, char *text, int capacity)
{
	BenchCompare *compare;
	BenchSaved *saved;
	BenchRow *row;
	char before[24];
	char now[24];
	char change[24];
	double ratio;
	compare = (BenchCompare *)context;
	saved = &compare->saved[index];
	row = BenchFindRow(compare->bench, saved);
	BenchNumber(before, saved->value);
	BenchNumber(now, BENCHNOVALUE);
	strcpy(change, "-");
	if (row && row->value[BENCHPASSMAIN] > 0) {
		BenchNumber(now, row->value[BENCHPASSMAIN]);
		if (saved->value > 0) {
			ratio = row->value[BENCHPASSMAIN] / saved->value;
			if (BenchLowerIsBetter(row->unit))
				ratio = saved->value / row->value[BENCHPASSMAIN];
			snprintf(change, sizeof(change), "%+.1f%%", (ratio - 1.0) * 100.0);
		}
	}
	snprintf(text, capacity, "%-9s %-44.44s %10s %10s %9s", saved->suite, saved->test, before, now, change);
}

static void BenchCompareReports(Bench *bench)
{
	BenchCurses *ui;
	BenchCompare compare;
	StandardFileReply reply;
	SFTypeList types;
	char header[200];
	ui = BenchCursesState(bench);
	if (bench->rowcount == 0) {
		strcpy(ui->message, "Run SheepBench first: the saved report is compared with the current results.");
		return;
	}
	types[0] = 'TEXT';
	StandardGetFile(NULL, 1, types, &reply);
	touchwin(curscr);
	if (!reply.sfGood)
		return;
	compare.bench = bench;
	compare.saved = (BenchSaved *)NewPtrClear(sizeof(BenchSaved) * CURSESMAXSAVED);
	if (compare.saved == NULL) {
		strcpy(ui->message, "Not enough memory to compare.");
		return;
	}
	compare.count = BenchReadSaved(&reply.sfFile, compare.saved, CURSESMAXSAVED);
	if (compare.count <= 0)
		snprintf(ui->message, sizeof(ui->message), "\"%.*s\" is not a SheepBench report.", reply.sfFile.name[0],
			reply.sfFile.name + 1);
	else {
		snprintf(header, sizeof(header), "%-9s %-44s %10s %10s %9s   (+ is faster now)", "suite", "test", "saved", "now",
			"change");
		BenchView(bench, header, compare.count, BenchCompareLine, &compare);
		ui->message[0] = 0;
	}
	DisposePtr((Ptr)compare.saved);
}

static Boolean BenchActivate(Bench *bench, BenchOptions *options, int item, Boolean *run)
{
	BenchCurses *ui;
	ui = BenchCursesState(bench);
	*run = false;
	if (item >= 0 && item < BenchSuiteCount())
		options->suites ^= BenchSuiteFlag(item);
	else if (item == CURSESITEMQUICK)
		options->quick = !options->quick;
	else if (item == CURSESITEMJITOFF) {
		if (ui->canswitch)
			options->jitoff = !options->jitoff;
		else
			strcpy(ui->message, "The host does not allow switching the JIT: set benchswitch true and start with the JIT on.");
	} else if (item == CURSESITEMRUN) {
		if (options->suites == 0)
			strcpy(ui->message, "Pick at least one suite.");
		else
			*run = true;
	} else if (item == CURSESITEMRESULTS)
		BenchShowResults(bench);
	else if (item == CURSESITEMHOST)
		BenchShowHost(bench);
	else if (item == CURSESITEMSAVE)
		BenchSave(bench);
	else if (item == CURSESITEMCOMPARE)
		BenchCompareReports(bench);
	else if (item == CURSESITEMQUIT)
		return false;
	return true;
}

static int BenchHotkey(int key)
{
	if (key == 'r' || key == 'R')
		return CURSESITEMRUN;
	if (key == 'v' || key == 'V')
		return CURSESITEMRESULTS;
	if (key == 'h' || key == 'H')
		return CURSESITEMHOST;
	if (key == 's' || key == 'S')
		return CURSESITEMSAVE;
	if (key == 'c' || key == 'C')
		return CURSESITEMCOMPARE;
	if (key == 'q' || key == 'Q' || key == CURSESKEYCLOSE)
		return CURSESITEMQUIT;
	return -1;
}

Boolean BenchUiMenu(Bench *bench, BenchOptions *options)
{
	BenchCurses *ui;
	MEVENT mouse;
	Boolean run;
	int key;
	int item;
	ui = BenchCursesState(bench);
	if (ui->log) {
		delwin(ui->log);
		ui->log = NULL;
	}
	if (!ui->canswitch)
		options->jitoff = false;
	for (;;) {
		BenchMenuDraw(bench, options);
		key = getch();
		item = -1;
		if (key == KEY_UP) {
			ui->selected = (ui->selected + CURSESITEMS - 1) % CURSESITEMS;
			continue;
		}
		if (key == KEY_DOWN) {
			ui->selected = (ui->selected + 1) % CURSESITEMS;
			continue;
		}
		if (key == ' ' || key == '\n' || key == '\r' || key == KEY_ENTER)
			item = ui->selected;
		else if (key == KEY_MOUSE && nc_getmouse(&mouse) == OK) {
			item = BenchMenuItemAt(mouse.y);
			if (item >= 0)
				ui->selected = item;
		} else
			item = BenchHotkey(key);
		if (item < 0)
			continue;
		ui->message[0] = 0;
		if (!BenchActivate(bench, options, item, &run))
			return false;
		if (run)
			break;
	}
	erase();
	BenchCursesSetStatus(bench, "Starting...");
	bench->jitoff = options->jitoff;
	bench->suiteindex = 0;
	bench->pass = BENCHPASSMAIN;
	BenchCursesHeader(bench, CURSESHELPRUN);
	refresh();
	ui->log = subwin(stdscr, LINES - CURSESTOP - 1, COLS, CURSESTOP, 0);
	if (ui->log)
		scrollok(ui->log, TRUE);
	return true;
}

void BenchUiDone(Bench *bench)
{
	BenchCurses *ui;
	char text[160];
	ui = BenchCursesState(bench);
	if (bench->cancelled)
		snprintf(text, sizeof(text), "Stopped: %d failures. Press a key for the menu.", bench->failures);
	else
		snprintf(text, sizeof(text), "Done: %d failures, report written to \"SheepBench Report\". Press a key for the menu.",
			bench->failures);
	BenchCursesSetStatus(bench, text);
	BenchCursesHeader(bench, " Press any key for the menu ");
	touchwin(stdscr);
	refresh();
	flushinp();
	getch();
	snprintf(ui->message, sizeof(ui->message), "Last run: %d rows, %d failures.", bench->rowcount, bench->failures);
	BenchCursesSetStatus(bench, "Pick the suites and choose Run.");
}

#else

typedef struct BenchConsole {
	Boolean asked;
} BenchConsole;

Boolean BenchUiOpen(Bench *bench)
{
	bench->ui = NewPtrClear(sizeof(BenchConsole));
	return bench->ui != NULL;
}

void BenchUiClose(Bench *bench)
{
	if (bench->ui)
		DisposePtr((Ptr)bench->ui);
	bench->ui = NULL;
}

void BenchUiLine(Bench *bench, const char *text)
{
	printf("%s\n", text);
	fflush(stdout);
}

void BenchUiStatus(Bench *bench, const char *text)
{
}

void BenchUiPoll(Bench *bench)
{
}

static unsigned long BenchConsoleSuites(const char *text)
{
	unsigned long suites;
	int index;
	suites = 0;
	for (index = 0; index < BenchSuiteCount(); index++) {
		if (strstr(text, BenchSuiteName(index)))
			suites |= BenchSuiteFlag(index);
	}
	if (suites == 0)
		suites = BENCHSUITEALL;
	return suites;
}

static Boolean BenchConsoleYes(const char *question)
{
	char answer[80];
	printf("%s (y/N)? ", question);
	fflush(stdout);
	answer[0] = 0;
	fgets(answer, sizeof(answer), stdin);
	return answer[0] == 'y' || answer[0] == 'Y';
}

Boolean BenchUiMenu(Bench *bench, BenchOptions *options)
{
	BenchConsole *console;
	char answer[200];
	int index;
	console = (BenchConsole *)bench->ui;
	if (console->asked)
		return false;
	console->asked = true;
	printf("Suites:");
	for (index = 0; index < BenchSuiteCount(); index++)
		printf(" %s", BenchSuiteName(index));
	printf("\nWhich suites (Return for all)? ");
	fflush(stdout);
	answer[0] = 0;
	fgets(answer, sizeof(answer), stdin);
	options->suites = BenchConsoleSuites(answer);
	options->quick = BenchConsoleYes("Quick run");
	options->jitoff = false;
	if (BenchHostIs(bench, "benchswitch", "1") && BenchHostIs(bench, "jit", "on"))
		options->jitoff = BenchConsoleYes("Also run with the JIT off");
	bench->jitoff = options->jitoff;
	return true;
}

void BenchUiDone(Bench *bench)
{
	printf("Press Return to quit.\n");
	fflush(stdout);
	getchar();
}

#endif
