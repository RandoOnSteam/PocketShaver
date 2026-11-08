/*
 *  sflbootgui.c - Original boot configuration window
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
#include "gui.h"
#include "guictl.h"
#include "file.h"
#include "thread.h"
#include "sflui.h"
#include "sflbootgui.h"
//#define SFL_USEDARKMODE
#if defined(WSWIN) && defined(SFL_USEDARKMODE)
#include "darkmode.h"
#endif

#define SFLBOOTPAGES 4
#define SFLBOOTPAGEGENERAL 0
#define SFLBOOTPAGEGRAPHICS 1
#define SFLBOOTPAGENETWORK 2
#define SFLBOOTPAGEADVANCED 3
#define SFLBOOTITEMS 96
#define SFLBOOTMAXOPTIONS 8
#define SFLBOOTMAXBUTTONS 4
#define SFLBOOTMAXRADIOS 16
#define SFLBOOTVALUESIZE 64
#define SFLBOOTPATHSIZE 1024
#define SFLBOOTMAXDISKS 128
#define SFLBOOTMAXRESOLUTIONS 10
#define SFLBOOTMAXDISKSIZEMB 2047

#define SFLBOOTHEADING 1
#define SFLBOOTHELP 2
#define SFLBOOTCHECK 3
#define SFLBOOTPATH 4
#define SFLBOOTFIELD 5
#define SFLBOOTCOMBO 6
#define SFLBOOTRADIO 7
#define SFLBOOTLIST 8
#define SFLBOOTBUTTONS 9
#define SFLBOOTBANNER 10

#define SFLBOOTKINDSTRING 0
#define SFLBOOTKINDINT 1
#define SFLBOOTKINDBOOL 2
#define SFLBOOTKINDSPECIAL 3

#define SFLBOOTWIDTH 720
#define SFLBOOTHEIGHT 760
#define SFLBOOTMARGIN 12
#define SFLBOOTTABHEIGHT 32
#define SFLBOOTBOOTHEIGHT 36
#define SFLBOOTROWHEIGHT 26
#define SFLBOOTROWGAP 4
#define SFLBOOTHEADINGHEIGHT 26
#define SFLBOOTHEADINGGAP 10
#define SFLBOOTTEXTLINE 19
#define SFLBOOTCHARWIDTH 7
#define SFLBOOTLABELWIDTH 200
#define SFLBOOTFIELDWIDTH 120
#define SFLBOOTCOMBOWIDTH 240
#define SFLBOOTBROWSEWIDTH 90
#define SFLBOOTBUTTONWIDTH 130
#define SFLBOOTCLOSEWIDTH 26
#define SFLBOOTLISTHEIGHT 130
#define SFLBOOTLISTCOLUMN 620
#define SFLBOOTDISKNAMECOLUMN 450
#define SFLBOOTDISKFLAGCOLUMN 80
#define SFLBOOTTEXTPAD 2
#define SFLBOOTLABELOFFSET 4
#define SFLBOOTBANNERGAP 8
#define SFLBOOTSHEETWIDTH 620
#define SFLBOOTSHEETHEIGHT 600
#define SFLBOOTSHEETINSET 16
#define SFLBOOTFORMWIDTH 540
#define SFLBOOTFORMHEIGHT 230
#define SFLBOOTHELPGREY 96
#define SFLBOOTMINROMTEXT 8
#define SFLBOOTMINWIDTH 512
#define SFLBOOTMINHEIGHT 384
#define SFLBOOTMAXWIDTH 4096
#define SFLBOOTMAXHEIGHT 4096
#define SFLBOOTMEGABYTE 1048576L
#define SFLBOOTIDLEMS 10

#if defined(WSWINGUI)
#define SFLBOOTCOMBOHEIGHT 220
#else
#define SFLBOOTCOMBOHEIGHT SFLBOOTROWHEIGHT
#endif

#define SFLBOOTIDTAB 10
#define SFLBOOTIDBOOT 11
#define SFLBOOTIDBUTTONBASE 100
#define SFLBOOTIDRADIOBASE 300
#define SFLBOOTIDCONTROLBASE 500
#define SFLBOOTIDROM (SFLBOOTIDBUTTONBASE + 0)
#define SFLBOOTIDFOLDER (SFLBOOTIDBUTTONBASE + 2)
#define SFLBOOTIDDISKADD (SFLBOOTIDBUTTONBASE + 3)
#define SFLBOOTIDDISKCREATE (SFLBOOTIDBUTTONBASE + 4)
#define SFLBOOTIDDISKREMOVE (SFLBOOTIDBUTTONBASE + 5)
#define SFLBOOTIDREDIRADD (SFLBOOTIDBUTTONBASE + 6)
#define SFLBOOTIDREDIRREMOVE (SFLBOOTIDBUTTONBASE + 7)
#define SFLBOOTIDINSTRUCTIONS (SFLBOOTIDBUTTONBASE + 8)
#define SFLBOOTIDLICENSES (SFLBOOTIDBUTTONBASE + 9)
#define SFLBOOTIDSETUPCLOSE (SFLBOOTIDBUTTONBASE + 10)
#define SFLBOOTIDNETWORKCLOSE (SFLBOOTIDBUTTONBASE + 11)
#define SFLBOOTIDSHEETREAD (SFLBOOTIDBUTTONBASE + 12)
#define SFLBOOTIDDISKMOUNT (SFLBOOTIDBUTTONBASE + 13)
#define SFLBOOTIDDISKTYPE (SFLBOOTIDBUTTONBASE + 14)
#define SFLBOOTIDDISKRELOAD (SFLBOOTIDBUTTONBASE + 15)
#define SFLBOOTIDDISKOPEN (SFLBOOTIDBUTTONBASE + 16)
#define SFLBOOTIDRESADD (SFLBOOTIDBUTTONBASE + 17)
#define SFLBOOTIDRESEDIT (SFLBOOTIDBUTTONBASE + 18)
#define SFLBOOTIDRESREMOVE (SFLBOOTIDBUTTONBASE + 19)
#define SFLBOOTIDBOOTSTRAP (SFLBOOTIDBUTTONBASE + 20)
#define SFLBOOTIDBOOTSTRAPDONE (SFLBOOTIDBUTTONBASE + 21)
#define SFLBOOTIDCOMPATIBILITY (SFLBOOTIDBUTTONBASE + 22)
#define SFLBOOTIDCOMPATIBILITY2 (SFLBOOTIDBUTTONBASE + 23)
#define SFLBOOTIDRELATIVE (SFLBOOTIDBUTTONBASE + 24)
#define SFLBOOTIDRELATIVE2 (SFLBOOTIDBUTTONBASE + 25)
#define SFLBOOTIDSETUPGUIDE (SFLBOOTIDBUTTONBASE + 26)
#define SFLBOOTIDBOOTSTRAP2 (SFLBOOTIDBUTTONBASE + 27)
#define SFLBOOTIDCLIPBOARD (SFLBOOTIDBUTTONBASE + 28)
#define SFLBOOTIDDONE 1
#define SFLBOOTIDACCEPT 1
#define SFLBOOTIDCANCEL 2

typedef struct SFLBOOTITEM {
	int page;
	int type;
	int kind;
	int invert;
	int hidden;
	int optioncount;
	int selected;
	int radionumber;
	int buttoncount;
	int folder;
	int documenty;
	int wrappedcolumns;
	int haslabel;
	int hascontrol;
	const char *pref;
	const char *caption;
	const char *options;
	char *wrapped;
	char customvalue[SFLBOOTVALUESIZE];
	WSCTLSTYLEDLABEL label;
	WSCTLLINE line;
	WSCTLCHECKBOX check;
	WSCTLCOMBOBOX combo;
	WSCTLLIST list;
	WSCTLBUTTON buttons[SFLBOOTMAXBUTTONS];
	WSCTLRADIOBUTTON radios[SFLBOOTMAXOPTIONS];
} SFLBOOTITEM;

typedef struct SFLBOOTDISK {
	char *path;
	int mounted;
	int cd;
} SFLBOOTDISK;

typedef struct SFLBOOTGUI {
	WSAPP *app;
	WSWINDOW window;
	WSWNDCLASS windowclass;
	WSCTLTAB tabs;
	WSCTLSCROLLPANEL panels[SFLBOOTPAGES];
	WSCTLBUTTON boot;
	WSDND drop;
	WSFONT headingfont;
	WSFONT bodyfont;
	SFLBOOTITEM items[SFLBOOTITEMS];
	SFLBOOTDISK disks[SFLBOOTMAXDISKS];
	SFLBOOTITEM *radioitems[SFLBOOTMAXRADIOS];
	int radiooptions[SFLBOOTMAXRADIOS];
	int count;
	int radiocount;
	int diskcount;
	int dpi;
	int basedpi;
	int setupread;
	int resolutionsedited;
	int networkchanged;
	int bootstrapcompleted;
	int page;
	int controlid;
	int finished;
	int accepted;
	int ready;
	int dropcreated;
	int fontscreated;
	int failed;
#if defined(SFL_USEDARKMODE)
	WSDARKMODE darkmode;
#endif /* SFL_USEDARKMODE */
} SFLBOOTGUI;

typedef struct SFLBOOTSHEET {
	WSWINDOW window;
	WSCTLTEXT text;
	WSSCROLLEDWINDOW scrolled;
	WSCTLBUTTON done;
	int closed;
} SFLBOOTSHEET;

typedef struct SFLBOOTFORM {
	WSWINDOW window;
	WSCTLSTYLEDLABEL label;
	WSCTLLINE line;
	WSCTLSTYLEDLABEL secondlabel;
	WSCTLLINE secondline;
	WSCTLBUTTON accept;
	WSCTLBUTTON cancel;
	char *value;
	char *secondvalue;
	int hassecond;
	int accepted;
	int closed;
} SFLBOOTFORM;

static void SflBootLayout(SFLBOOTGUI *pThis);
static void SflBootRefreshDynamic(SFLBOOTGUI *pThis);

static int SflBootPx(SFLBOOTGUI *pThis, int value)
{
	return value * pThis->dpi / pThis->basedpi;
}

#define SFLBOOTPX(value) SflBootPx(pThis, (value))

static SFLBOOTITEM *SflBootFind(SFLBOOTGUI *pThis, const char *name)
{
	int index;
	for (index = 0; index < pThis->count; ++index)
		if (pThis->items[index].pref != NULL && strcmp(pThis->items[index].pref, name) == 0)
			return &pThis->items[index];
	return NULL;
}

static int SflBootOption(const char *options, int wanted, char *label, char *value)
{
	const char *position;
	const char *separator;
	const char *equals;
	size_t length;
	int index;
	position = options;
	for (index = 0; index < wanted; ++index) {
		position = strchr(position, '|');
		if (position == NULL)
			return 0;
		++position;
	}
	separator = strchr(position, '|');
	if (separator != NULL)
		length = (size_t)(separator - position);
	else
		length = strlen(position);
	equals = strchr(position, '=');
	if (equals == NULL || equals >= position + length)
		return 0;
	memcpy(label, position, (size_t)(equals - position));
	label[equals - position] = 0;
	memcpy(value, equals + 1, length - (size_t)(equals - position) - 1);
	value[length - (size_t)(equals - position) - 1] = 0;
	return 1;
}

static void SflBootCurrentValue(SFLBOOTITEM *item, char *value)
{
	const char *text;
	if (item->kind == SFLBOOTKINDINT) {
		sprintf(value, "%d", SflBootPrefInt(item->pref));
	} else if (item->kind == SFLBOOTKINDBOOL) {
		sprintf(value, "%d", SflEmulatorPrefBool(item->pref) != 0);
	} else if (item->kind == SFLBOOTKINDSPECIAL) {
		SflBootSpecialValue(item->pref, value, SFLBOOTVALUESIZE);
	} else {
		text = SflEmulatorPrefString(item->pref, 0);
		value[0] = 0;
		if (text != NULL)
			strncat(value, text, SFLBOOTVALUESIZE - 1);
	}
}

static void SflBootSelectedValue(SFLBOOTITEM *item, char *value)
{
	char label[SFLBOOTVALUESIZE];
	if (item->selected >= item->optioncount)
		strcpy(value, item->customvalue);
	else
		SflBootOption(item->options, item->selected, label, value);
}

static void SflBootStoreValue(SFLBOOTITEM *item, const char *value)
{
	if (item->kind == SFLBOOTKINDINT)
		SflBootSetInt(item->pref, atoi(value));
	else if (item->kind == SFLBOOTKINDBOOL)
		SflBootSetBool(item->pref, atoi(value));
	else if (item->kind == SFLBOOTKINDSPECIAL)
		SflBootSetSpecial(item->pref, value);
	else
		SflBootSetFirstString(item->pref, value);
}

static void SflBootAddLabel(SFLBOOTGUI *pThis, SFLBOOTITEM *item, const char *text)
{
	WSRECT bounds;
	WSRect_Construct(&bounds, 0, 0, 100, SFLBOOTROWHEIGHT);
	if (!WSCtlStyledLabel_Create(&item->label, &pThis->window, &bounds, text, 0, (size_t)pThis->controlid++)) {
		pThis->failed = 1;
		return;
	}
	item->haslabel = 1;
	if (pThis->fontscreated)
		WSCtlStyledLabel_SetFont(&item->label, &pThis->bodyfont);
	WSCtlScrollPanel_Add(&pThis->panels[item->page], WSCtl_GetHandle((WSCTL *)&item->label));
}

static SFLBOOTITEM *SflBootAddItem(SFLBOOTGUI *pThis, int page, int type, const char *pref, const char *caption)
{
	SFLBOOTITEM *item;
	WSASSERT(pThis->count < SFLBOOTITEMS);
	item = &pThis->items[pThis->count++];
	item->page = page;
	item->type = type;
	item->pref = pref;
	item->caption = caption;
	item->wrappedcolumns = -1;
	return item;
}

static SFLBOOTITEM *SflBootAddHeading(SFLBOOTGUI *pThis, int page, const char *text)
{
	SFLBOOTITEM *item;
	item = SflBootAddItem(pThis, page, SFLBOOTHEADING, NULL, text);
	SflBootAddLabel(pThis, item, text);
	if (item->haslabel) {
		WSCtlStyledLabel_SetBold(&item->label);
		if (pThis->fontscreated)
			WSCtlStyledLabel_SetFont(&item->label, &pThis->headingfont);
	}
	return item;
}

static SFLBOOTITEM *SflBootAddHelp(SFLBOOTGUI *pThis, int page, const char *pref, const char *text)
{
	SFLBOOTITEM *item;
	item = SflBootAddItem(pThis, page, SFLBOOTHELP, pref, text);
	item->wrapped = (char *)WSALLOC(char, strlen(text) + SFLBOOTPATHSIZE);
	if (item->wrapped == NULL) {
		pThis->failed = 1;
		return item;
	}
	strcpy(item->wrapped, text);
	SflBootAddLabel(pThis, item, item->wrapped);
	if (item->haslabel) {
		WSCtlStyledLabel_SetBackgroundColor(&item->label, WSCtlScrollPanel_GetBackgroundColor());
		WSCtlStyledLabel_SetTextColor(&item->label, WSRGB(SFLBOOTHELPGREY, SFLBOOTHELPGREY, SFLBOOTHELPGREY));
	}
	return item;
}

static void SflBootAddButton(SFLBOOTGUI *pThis, SFLBOOTITEM *item, const char *text, WSEVENTBUTTON_CALLBACK((*callback)), size_t id)
{
	WSRECT bounds;
	WSCTLBUTTON *button;
	WSASSERT(item->buttoncount < SFLBOOTMAXBUTTONS);
	button = &item->buttons[item->buttoncount];
	WSRect_Construct(&bounds, 0, 0, SFLBOOTBUTTONWIDTH, SFLBOOTROWHEIGHT);
	if (!WSCtlButton_Create(button, &pThis->window, &bounds, text, callback, id)) {
		pThis->failed = 1;
		return;
	}
	item->buttoncount++;
	WSCtlScrollPanel_Add(&pThis->panels[item->page], WSCtl_GetHandle((WSCTL *)button));
}

static void SflBootAddCheckHandler(SFLBOOTGUI *pThis, int page, const char *pref, const char *caption, int invert, WSEVENTCHECKBUTTON_CALLBACK((*callback)), size_t id)
{
	SFLBOOTITEM *item;
	WSRECT bounds;
	item = SflBootAddItem(pThis, page, SFLBOOTCHECK, pref, caption);
	item->kind = SFLBOOTKINDBOOL;
	item->invert = invert;
	WSRect_Construct(&bounds, 0, 0, 100, SFLBOOTROWHEIGHT);
	if (!WSCtlCheckBox_Create(&item->check, &pThis->window, &bounds, caption, callback, id)) {
		pThis->failed = 1;
		return;
	}
	item->hascontrol = 1;
	WSCtlCheckBox_SetChecked(&item->check, (SflEmulatorPrefBool(pref) != 0) != invert);
	WSCtlScrollPanel_Add(&pThis->panels[page], WSCtl_GetHandle((WSCTL *)&item->check));
}

static void SflBootAddCheck(SFLBOOTGUI *pThis, int page, const char *pref, const char *caption, int invert)
{
	SflBootAddCheckHandler(pThis, page, pref, caption, invert, NULL, (size_t)pThis->controlid++);
}

static void SflBootAddLineControl(SFLBOOTGUI *pThis, SFLBOOTITEM *item, const char *value)
{
	WSRECT bounds;
	WSRect_Construct(&bounds, 0, 0, 100, SFLBOOTROWHEIGHT);
	if (!WSCtlLine_Create(&item->line, &pThis->window, &bounds)) {
		pThis->failed = 1;
		return;
	}
	item->hascontrol = 1;
	WSCtlLine_SetText(&item->line, value, strlen(value));
	WSCtlScrollPanel_Add(&pThis->panels[item->page], WSCtlLine_GetHandle(&item->line));
}

static void SflBootAddPath(SFLBOOTGUI *pThis, int page, const char *pref, const char *caption, int folder, WSEVENTBUTTON_CALLBACK((*browse)), size_t id)
{
	SFLBOOTITEM *item;
	const char *value;
	item = SflBootAddItem(pThis, page, SFLBOOTPATH, pref, caption);
	item->folder = folder;
	value = SflEmulatorPrefString(pref, 0);
	if (value == NULL)
		value = "";
	SflBootAddLabel(pThis, item, caption);
	SflBootAddLineControl(pThis, item, value);
	SflBootAddButton(pThis, item, "Browse...", browse, id);
}

static void SflBootAddField(SFLBOOTGUI *pThis, int page, const char *pref, const char *caption)
{
	SFLBOOTITEM *item;
	char value[SFLBOOTVALUESIZE];
	item = SflBootAddItem(pThis, page, SFLBOOTFIELD, pref, caption);
	item->kind = SFLBOOTKINDINT;
	SflBootCurrentValue(item, value);
	SflBootAddLabel(pThis, item, caption);
	SflBootAddLineControl(pThis, item, value);
}

static void SflBootLoadOptions(SFLBOOTITEM *item, char *current)
{
	char label[SFLBOOTVALUESIZE];
	char value[SFLBOOTVALUESIZE];
	item->selected = -1;
	item->optioncount = 0;
	SflBootCurrentValue(item, current);
	while (item->optioncount < SFLBOOTMAXOPTIONS && SflBootOption(item->options, item->optioncount, label, value)) {
		if (strcmp(value, current) == 0)
			item->selected = item->optioncount;
		item->optioncount++;
	}
}

static void SflBootAddCombo(SFLBOOTGUI *pThis, int page, const char *pref, const char *caption, const char *options, int kind)
{
	SFLBOOTITEM *item;
	WSRECT bounds;
	char label[SFLBOOTVALUESIZE];
	char value[SFLBOOTVALUESIZE];
	char current[SFLBOOTVALUESIZE];
	int index;
	item = SflBootAddItem(pThis, page, SFLBOOTCOMBO, pref, caption);
	item->kind = kind;
	item->options = options;
	SflBootAddLabel(pThis, item, caption);
	WSRect_Construct(&bounds, 0, 0, SFLBOOTCOMBOWIDTH, SFLBOOTCOMBOHEIGHT);
	if (!WSCtlComboBox_Create(&item->combo, &pThis->window, &bounds, (size_t)pThis->controlid++)) {
		pThis->failed = 1;
		return;
	}
	item->hascontrol = 1;
	SflBootLoadOptions(item, current);
	for (index = 0; index < item->optioncount; ++index) {
		SflBootOption(options, index, label, value);
		WSCtlComboBox_AddString(&item->combo, label);
	}
	if (item->selected < 0 && current[0] != 0) {
		strcpy(item->customvalue, current);
		WSCtlComboBox_AddString(&item->combo, current);
		item->selected = item->optioncount;
	}
	if (item->selected < 0)
		item->selected = 0;
	WSCtlComboBox_SetCurrentIndex(&item->combo, (size_t)item->selected);
	WSCtlScrollPanel_Add(&pThis->panels[page], WSCtl_GetHandle((WSCTL *)&item->combo));
}

static void SflBootRadioClicked(SFLBOOTGUI *pThis, int number);

#define SFLBOOTRADIOHANDLER(number) \
	WSEVENTRADIOBUTTON_CALLBACK(SflBootRadio##number) \
	{ \
		WSEVENTRADIOBUTTON event; \
		WSEventRadioButton_Construct(&event); \
		SflBootRadioClicked((SFLBOOTGUI *)WSEventRadioButton_GetWindowCon(&event), number); \
		return WSEVENTRADIOBUTTON_HANDLED; \
	}

SFLBOOTRADIOHANDLER(0)
SFLBOOTRADIOHANDLER(1)
SFLBOOTRADIOHANDLER(2)
SFLBOOTRADIOHANDLER(3)
SFLBOOTRADIOHANDLER(4)
SFLBOOTRADIOHANDLER(5)
SFLBOOTRADIOHANDLER(6)
SFLBOOTRADIOHANDLER(7)
SFLBOOTRADIOHANDLER(8)
SFLBOOTRADIOHANDLER(9)
SFLBOOTRADIOHANDLER(10)
SFLBOOTRADIOHANDLER(11)
SFLBOOTRADIOHANDLER(12)
SFLBOOTRADIOHANDLER(13)
SFLBOOTRADIOHANDLER(14)
SFLBOOTRADIOHANDLER(15)

static void SflBootAttachRadioCallback(SFLBOOTGUI *pThis, WSCTLRADIOBUTTON *radio, WSRECT *bounds, const char *label, int number)
{
	WSEVENTRADIOBUTTON_CALLBACK((*callback));
	switch (number) {
	case 0: callback = SflBootRadio0; break;
	case 1: callback = SflBootRadio1; break;
	case 2: callback = SflBootRadio2; break;
	case 3: callback = SflBootRadio3; break;
	case 4: callback = SflBootRadio4; break;
	case 5: callback = SflBootRadio5; break;
	case 6: callback = SflBootRadio6; break;
	case 7: callback = SflBootRadio7; break;
	case 8: callback = SflBootRadio8; break;
	case 9: callback = SflBootRadio9; break;
	case 10: callback = SflBootRadio10; break;
	case 11: callback = SflBootRadio11; break;
	case 12: callback = SflBootRadio12; break;
	case 13: callback = SflBootRadio13; break;
	case 14: callback = SflBootRadio14; break;
	default: callback = SflBootRadio15; break;
	}
	if (!WSCtlRadioButton_Create(radio, &pThis->window, bounds, label, callback, (size_t)(SFLBOOTIDRADIOBASE + number)))
		pThis->failed = 1;
}

static void SflBootAddRadio(SFLBOOTGUI *pThis, int page, const char *pref, const char *caption, const char *options, int kind)
{
	SFLBOOTITEM *item;
	WSRECT bounds;
	char label[SFLBOOTVALUESIZE];
	char value[SFLBOOTVALUESIZE];
	char current[SFLBOOTVALUESIZE];
	int index;
	item = SflBootAddItem(pThis, page, SFLBOOTRADIO, pref, caption);
	item->kind = kind;
	item->options = options;
	item->radionumber = pThis->radiocount;
	if (caption != NULL)
		SflBootAddLabel(pThis, item, caption);
	SflBootLoadOptions(item, current);
	if (item->selected < 0)
		item->selected = 0;
	WSRect_Construct(&bounds, 0, 0, 100, SFLBOOTROWHEIGHT);
	for (index = 0; index < item->optioncount; ++index) {
		WSASSERT(pThis->radiocount < SFLBOOTMAXRADIOS);
		SflBootOption(options, index, label, value);
		SflBootAttachRadioCallback(pThis, &item->radios[index], &bounds, label, pThis->radiocount);
		if (pThis->failed)
			return;
		if (index > 0)
			WSCtlRadioButton_SetGroup(&item->radios[index], &item->radios[0]);
		pThis->radioitems[pThis->radiocount] = item;
		pThis->radiooptions[pThis->radiocount] = index;
		pThis->radiocount++;
		WSCtlScrollPanel_Add(&pThis->panels[page], WSCtl_GetHandle((WSCTL *)&item->radios[index]));
	}
	item->hascontrol = 1;
	WSCtlRadioButton_SetChecked(&item->radios[item->selected], TRUE);
}

static void SflBootSyncRadios(SFLBOOTGUI *pThis)
{
	SFLBOOTITEM *item;
	int number;
	for (number = 0; number < pThis->radiocount; ++number) {
		item = pThis->radioitems[number];
		WSCtlRadioButton_SetChecked(&item->radios[pThis->radiooptions[number]], pThis->radiooptions[number] == item->selected);
	}
}

static void SflBootUpdatePortField(SFLBOOTGUI *pThis)
{
	char value[SFLBOOTVALUESIZE];
	SflBootSelectedValue(SflBootFind(pThis, "network"), value);
	if (strcmp(value, "udptunnel") == 0)
		WSCtl_Enable((WSCTL *)&SflBootFind(pThis, "udpport")->line);
	else
		WSCtl_Disable((WSCTL *)&SflBootFind(pThis, "udpport")->line);
}

static void SflBootRadioClicked(SFLBOOTGUI *pThis, int number)
{
	SFLBOOTITEM *item;
	if (!pThis->ready)
		return;
	item = pThis->radioitems[number];
#if defined(WSGTK)
	if (!WSCtlRadioButton_IsChecked(&item->radios[pThis->radiooptions[number]]))
		return;
#endif
	if (item->selected == pThis->radiooptions[number]) {
		SflBootSyncRadios(pThis);
		return;
	}
	item->selected = pThis->radiooptions[number];
	SflBootSyncRadios(pThis);
	if (strcmp(item->pref, "network") == 0) {
		pThis->networkchanged = 1;
		SflBootUpdatePortField(pThis);
	}
	SflBootRefreshDynamic(pThis);
}

static void SflBootAppendList(SFLBOOTITEM *item, const char *text)
{
	WSCTLLISTITEM entry;
	WSCtlListItem_Construct(&entry, 0);
	WSCtlListItem_SetText(&entry, text);
	WSCtlList_AppendItem(&item->list, &entry);
	WSCtlListItem_Destroy(&entry);
}

static char *SflBootListText(SFLBOOTITEM *item, int index)
{
	WSCTLLISTITEM entry;
	char *text;
	text = NULL;
	WSCtlListItem_Construct(&entry, 0);
	WSCtlList_GetItem(&item->list, index, &entry);
	WSCtlList_AllocItemText(&item->list, &entry, &text);
	WSCtlListItem_Destroy(&entry);
	return text;
}

static SFLBOOTITEM *SflBootAddListControl(SFLBOOTGUI *pThis, int page, const char *pref, const char *caption, int width)
{
	SFLBOOTITEM *item;
	WSRECT bounds;
	item = SflBootAddItem(pThis, page, SFLBOOTLIST, pref, caption);
	WSRect_Construct(&bounds, 0, 0, 100, SFLBOOTLISTHEIGHT);
	if (!WSCtlList_Create(&item->list, &pThis->window, &bounds, NULL, NULL, (size_t)pThis->controlid++, WSCTLLIST_STYLE_REPORT | WSCTLLIST_STYLE_FULLROWSELECT)) {
		pThis->failed = 1;
		return item;
	}
	item->hascontrol = 1;
	WSCtlList_InsertColumn(&item->list, 0, caption, WSCTLLIST_ALIGN_LEFT, SFLBOOTPX(width));
	WSCtlScrollPanel_AddList(&pThis->panels[page], &item->list);
	return item;
}

static void SflBootAddList(SFLBOOTGUI *pThis, int page, const char *pref, const char *caption)
{
	SFLBOOTITEM *item;
	const char *value;
	int index;
	item = SflBootAddListControl(pThis, page, pref, caption, SFLBOOTLISTCOLUMN);
	if (!item->hascontrol)
		return;
	index = 0;
	while ((value = SflEmulatorPrefString(pref, index++)) != NULL)
		SflBootAppendList(item, value);
}

static const char *SflBootBaseName(const char *path)
{
	const char *slash;
	const char *back;
	slash = strrchr(path, '/');
	back = strrchr(path, '\\');
	if (back != NULL && (slash == NULL || back > slash))
		slash = back;
	if (slash != NULL)
		return slash + 1;
	return path;
}

static int SflBootExtensionIn(const char *path, const char *extensions)
{
	const char *dot;
	char lowered[SFLBOOTVALUESIZE];
	char wanted[SFLBOOTVALUESIZE];
	size_t index;
	dot = strrchr(path, '.');
	if (dot == NULL || strlen(dot + 1) + 2 >= sizeof(lowered))
		return 0;
	for (index = 0; dot[index + 1] != 0; ++index) {
		lowered[index] = dot[index + 1];
		if (lowered[index] >= 'A' && lowered[index] <= 'Z')
			lowered[index] = (char)(lowered[index] - 'A' + 'a');
	}
	lowered[index] = 0;
	sprintf(wanted, "|%s|", lowered);
	return strstr(extensions, wanted) != NULL;
}

static void SflBootDiskAppend(SFLBOOTGUI *pThis, const char *path, int mounted, int cd)
{
	SFLBOOTDISK *disk;
	int index;
	if (pThis->diskcount >= SFLBOOTMAXDISKS)
		return;
	for (index = 0; index < pThis->diskcount; ++index)
		if (strcmp(SflBootBaseName(pThis->disks[index].path), SflBootBaseName(path)) == 0)
			return;
	disk = &pThis->disks[pThis->diskcount];
	disk->path = WSALLOC(char, strlen(path) + 1);
	if (disk->path == NULL)
		return;
	strcpy(disk->path, path);
	disk->mounted = mounted;
	disk->cd = cd;
	pThis->diskcount++;
}

static void SflBootDiskRemoveAt(SFLBOOTGUI *pThis, int position)
{
	int index;
	WSFREE(pThis->disks[position].path);
	for (index = position; index + 1 < pThis->diskcount; ++index)
		pThis->disks[index] = pThis->disks[index + 1];
	pThis->diskcount--;
}

static void SflBootDiskSort(SFLBOOTGUI *pThis)
{
	SFLBOOTDISK moved;
	int index;
	int previous;
	for (index = 1; index < pThis->diskcount; ++index) {
		moved = pThis->disks[index];
		previous = index;
		while (previous > 0 && moved.mounted && !pThis->disks[previous - 1].mounted) {
			pThis->disks[previous] = pThis->disks[previous - 1];
			previous--;
		}
		pThis->disks[previous] = moved;
	}
}

static void SflBootDiskRender(SFLBOOTGUI *pThis)
{
	SFLBOOTITEM *item;
	const char *mounted;
	const char *type;
	int index;
	item = SflBootFind(pThis, "disk");
	SflBootDiskSort(pThis);
	WSCtlList_DeleteAll(&item->list);
	for (index = 0; index < pThis->diskcount; ++index) {
		SflBootAppendList(item, pThis->disks[index].path);
		mounted = "No";
		if (pThis->disks[index].mounted)
			mounted = "Yes";
		type = "Hard disk";
		if (pThis->disks[index].cd)
			type = "CD-ROM";
		WSCtlList_SetItemText(&item->list, index, 1, mounted);
		WSCtlList_SetItemText(&item->list, index, 2, type);
	}
	SflBootFind(pThis, "diskempty")->hidden = pThis->diskcount != 0;
}

static void SflBootDiskLoad(SFLBOOTGUI *pThis)
{
	const char *value;
	int index;
	int cd;
	index = 0;
	while ((value = SflEmulatorPrefString("disk", index++)) != NULL)
		SflBootDiskAppend(pThis, value, 1, 0);
	index = 0;
	while ((value = SflEmulatorPrefString("cdrom", index++)) != NULL)
		SflBootDiskAppend(pThis, value, 1, 1);
	index = 0;
	while ((value = SflEmulatorPrefString("diskunmounted", index++)) != NULL) {
		cd = strncmp(value, "cd:", 3) == 0;
		SflBootDiskAppend(pThis, value + 3, 0, cd);
	}
}

static void SflBootDiskApply(SFLBOOTGUI *pThis)
{
	SFLBOOTDISK *disk;
	char *encoded;
	int disks;
	int cds;
	int unmounted;
	int index;
	SflBootRemove("disk");
	SflBootRemove("cdrom");
	SflBootRemove("diskunmounted");
	disks = 0;
	cds = 0;
	unmounted = 0;
	for (index = 0; index < pThis->diskcount; ++index) {
		disk = &pThis->disks[index];
		if (disk->mounted && disk->cd) {
			SflBootSetString("cdrom", disk->path, cds++);
		} else if (disk->mounted) {
			SflBootSetString("disk", disk->path, disks++);
		} else {
			encoded = WSALLOC(char, strlen(disk->path) + 4);
			if (encoded == NULL)
				continue;
			if (disk->cd)
				sprintf(encoded, "cd:%s", disk->path);
			else
				sprintf(encoded, "hd:%s", disk->path);
			SflBootSetString("diskunmounted", encoded, unmounted++);
			WSFREE(encoded);
		}
	}
}

static void SflBootAddDiskList(SFLBOOTGUI *pThis, int page)
{
	SFLBOOTITEM *item;
	item = SflBootAddListControl(pThis, page, "disk", "Disk image", SFLBOOTDISKNAMECOLUMN);
	if (!item->hascontrol)
		return;
	WSCtlList_InsertColumn(&item->list, 1, "Mounted", WSCTLLIST_ALIGN_LEFT, SFLBOOTPX(SFLBOOTDISKFLAGCOLUMN));
	WSCtlList_InsertColumn(&item->list, 2, "Type", WSCTLLIST_ALIGN_LEFT, SFLBOOTPX(SFLBOOTDISKFLAGCOLUMN));
	SflBootDiskLoad(pThis);
}

static void SflBootShowLabel(SFLBOOTITEM *item, int visible)
{
	if (!item->haslabel)
		return;
	if (visible)
		WSCtlStyledLabel_Show(&item->label);
	else
		WSCtlStyledLabel_Hide(&item->label);
}

static int SflBootMapRect(SFLBOOTGUI *pThis, SFLBOOTITEM *item, WSRECT *bounds)
{
	WSCTLSCROLLPANEL *panel;
	panel = &pThis->panels[item->page];
	WSCtlScrollPanel_ToParent(panel, bounds);
	return !item->hidden && item->page == pThis->page && WSCtlScrollPanel_IsRectShown(panel, bounds);
}

static void SflBootPlaceLabel(SFLBOOTGUI *pThis, SFLBOOTITEM *item, int x, int y, int width, int height)
{
	WSRECT bounds;
	int visible;
	if (!item->haslabel)
		return;
	WSRect_Construct(&bounds, x, y, width, height);
	visible = SflBootMapRect(pThis, item, &bounds);
	WSCtlStyledLabel_Move(&item->label, &bounds);
	SflBootShowLabel(item, visible);
}

static void SflBootPlaceLine(SFLBOOTGUI *pThis, SFLBOOTITEM *item, int x, int y, int width)
{
	WSRECT bounds;
	int visible;
	WSRect_Construct(&bounds, x, y, width, SFLBOOTPX(SFLBOOTROWHEIGHT));
	visible = SflBootMapRect(pThis, item, &bounds);
	WSCtlLine_Move(&item->line, &bounds);
	if (visible)
		WSCtlLine_Show(&item->line);
	else
		WSCtlLine_Hide(&item->line);
}

static void SflBootPlaceButton(SFLBOOTGUI *pThis, SFLBOOTITEM *item, int index, int x, int y, int width)
{
	WSRECT bounds;
	int visible;
	WSRect_Construct(&bounds, x, y, width, SFLBOOTPX(SFLBOOTROWHEIGHT));
	visible = SflBootMapRect(pThis, item, &bounds);
	WSCtlButton_Move(&item->buttons[index], &bounds);
	if (visible)
		WSCtlButton_Show(&item->buttons[index]);
	else
		WSCtlButton_Hide(&item->buttons[index]);
}

static void SflBootPlaceCheck(SFLBOOTGUI *pThis, SFLBOOTITEM *item, int x, int y, int width)
{
	WSRECT bounds;
	int visible;
	WSRect_Construct(&bounds, x, y, width, SFLBOOTPX(SFLBOOTROWHEIGHT));
	visible = SflBootMapRect(pThis, item, &bounds);
	WSCtlCheckBox_Move(&item->check, &bounds);
	if (visible)
		WSCtlCheckBox_Show(&item->check);
	else
		WSCtlCheckBox_Hide(&item->check);
}

static void SflBootPlaceCombo(SFLBOOTGUI *pThis, SFLBOOTITEM *item, int x, int y)
{
	WSRECT bounds;
	int visible;
	WSRect_Construct(&bounds, x, y, SFLBOOTPX(SFLBOOTCOMBOWIDTH), SFLBOOTPX(SFLBOOTCOMBOHEIGHT));
	visible = SflBootMapRect(pThis, item, &bounds);
	WSCtlComboBox_Move(&item->combo, &bounds);
	if (visible)
		WSCtlComboBox_Show(&item->combo);
	else
		WSCtlComboBox_Hide(&item->combo);
}

static void SflBootPlaceRadio(SFLBOOTGUI *pThis, SFLBOOTITEM *item, int index, int x, int y, int width)
{
	WSRECT bounds;
	int visible;
	WSRect_Construct(&bounds, x, y, width, SFLBOOTPX(SFLBOOTROWHEIGHT));
	visible = SflBootMapRect(pThis, item, &bounds);
	WSCtlRadioButton_Move(&item->radios[index], &bounds);
	if (visible)
		WSCtlRadioButton_Show(&item->radios[index]);
	else
		WSCtlRadioButton_Hide(&item->radios[index]);
}

static void SflBootPlaceList(SFLBOOTGUI *pThis, SFLBOOTITEM *item, int x, int y, int width)
{
	WSRECT bounds;
	int visible;
	WSRect_Construct(&bounds, x, y, width, SFLBOOTPX(SFLBOOTLISTHEIGHT));
	visible = SflBootMapRect(pThis, item, &bounds);
	WSCtlList_Move(&item->list, &bounds);
	if (visible)
		WSCtlList_Show(&item->list);
	else
		WSCtlList_Hide(&item->list);
}

static int SflBootWrapText(SFLBOOTITEM *item, int columns)
{
	char *lastspace;
	char *position;
	int width;
	int lines;
	if (columns < SFLBOOTMINROMTEXT)
		columns = SFLBOOTMINROMTEXT;
	strcpy(item->wrapped, item->caption);
	lines = 1;
	width = 0;
	lastspace = NULL;
	for (position = item->wrapped; *position != 0; ++position) {
		if (*position == '\n') {
			width = 0;
			lastspace = NULL;
			lines++;
			continue;
		}
		if (*position == ' ')
			lastspace = position;
		width++;
		if (width > columns && lastspace != NULL) {
			*lastspace = '\n';
			width = (int)(position - lastspace);
			lastspace = NULL;
			lines++;
		}
	}
	if (columns != item->wrappedcolumns) {
		item->wrappedcolumns = columns;
		WSCtlStyledLabel_SetText(&item->label, item->wrapped);
	}
	return lines;
}

static void SflBootRefreshHelp(SFLBOOTITEM *item, const char *text)
{
	item->caption = text;
	item->wrappedcolumns = -1;
}

static int SflBootPlaceRadios(SFLBOOTGUI *pThis, SFLBOOTITEM *item, int y, int width)
{
	int index;
	int cellwidth;
	if (item->haslabel) {
		SflBootPlaceLabel(pThis, item, SFLBOOTPX(SFLBOOTMARGIN), y, width, SFLBOOTPX(SFLBOOTTEXTLINE) + SFLBOOTPX(SFLBOOTTEXTPAD));
		y += SFLBOOTPX(SFLBOOTTEXTLINE) + SFLBOOTPX(SFLBOOTROWGAP);
	}
	cellwidth = width / item->optioncount;
	for (index = 0; index < item->optioncount; ++index)
		SflBootPlaceRadio(pThis, item, index, SFLBOOTPX(SFLBOOTMARGIN) + index * cellwidth, y, cellwidth);
	return y + SFLBOOTPX(SFLBOOTROWHEIGHT) + SFLBOOTPX(SFLBOOTROWGAP);
}

static int SflBootPlaceItemAt(SFLBOOTGUI *pThis, SFLBOOTITEM *item, int y, int width)
{
	int lines;
	int index;
	int x;
	item->documenty = y;
	switch (item->type) {
	case SFLBOOTHEADING:
		y += SFLBOOTPX(SFLBOOTHEADINGGAP);
		SflBootPlaceLabel(pThis, item, SFLBOOTPX(SFLBOOTMARGIN), y, width, SFLBOOTPX(SFLBOOTHEADINGHEIGHT));
		return y + SFLBOOTPX(SFLBOOTHEADINGHEIGHT);
	case SFLBOOTHELP:
		lines = SflBootWrapText(item, width / SFLBOOTPX(SFLBOOTCHARWIDTH));
		SflBootPlaceLabel(pThis, item, SFLBOOTPX(SFLBOOTMARGIN), y, width, lines * SFLBOOTPX(SFLBOOTTEXTLINE) + SFLBOOTPX(SFLBOOTTEXTPAD));
		return y + lines * SFLBOOTPX(SFLBOOTTEXTLINE) + SFLBOOTPX(SFLBOOTROWGAP) + SFLBOOTPX(SFLBOOTTEXTPAD);
	case SFLBOOTBANNER:
		lines = SflBootWrapText(item, (width - SFLBOOTPX(SFLBOOTCLOSEWIDTH)) / SFLBOOTPX(SFLBOOTCHARWIDTH));
		SflBootPlaceLabel(pThis, item, SFLBOOTPX(SFLBOOTMARGIN), y, width - SFLBOOTPX(SFLBOOTCLOSEWIDTH) - SFLBOOTPX(SFLBOOTROWGAP), lines * SFLBOOTPX(SFLBOOTTEXTLINE) + SFLBOOTPX(SFLBOOTTEXTPAD));
		SflBootPlaceButton(pThis, item, 0, SFLBOOTPX(SFLBOOTMARGIN) + width - SFLBOOTPX(SFLBOOTCLOSEWIDTH), y, SFLBOOTPX(SFLBOOTCLOSEWIDTH));
		return y + lines * SFLBOOTPX(SFLBOOTTEXTLINE) + SFLBOOTPX(SFLBOOTROWGAP) + SFLBOOTPX(SFLBOOTBANNERGAP);
	case SFLBOOTCHECK:
		SflBootPlaceCheck(pThis, item, SFLBOOTPX(SFLBOOTMARGIN), y, width);
		return y + SFLBOOTPX(SFLBOOTROWHEIGHT) + SFLBOOTPX(SFLBOOTROWGAP);
	case SFLBOOTPATH:
		SflBootPlaceLabel(pThis, item, SFLBOOTPX(SFLBOOTMARGIN), y, width, SFLBOOTPX(SFLBOOTTEXTLINE) + SFLBOOTPX(SFLBOOTTEXTPAD));
		y += SFLBOOTPX(SFLBOOTTEXTLINE) + SFLBOOTPX(SFLBOOTROWGAP);
		SflBootPlaceLine(pThis, item, SFLBOOTPX(SFLBOOTMARGIN), y, width - SFLBOOTPX(SFLBOOTBROWSEWIDTH) - SFLBOOTPX(SFLBOOTROWGAP));
		SflBootPlaceButton(pThis, item, 0, SFLBOOTPX(SFLBOOTMARGIN) + width - SFLBOOTPX(SFLBOOTBROWSEWIDTH), y, SFLBOOTPX(SFLBOOTBROWSEWIDTH));
		return y + SFLBOOTPX(SFLBOOTROWHEIGHT) + SFLBOOTPX(SFLBOOTROWGAP);
	case SFLBOOTFIELD:
		SflBootPlaceLabel(pThis, item, SFLBOOTPX(SFLBOOTMARGIN), y + SFLBOOTPX(SFLBOOTLABELOFFSET), SFLBOOTPX(SFLBOOTLABELWIDTH), SFLBOOTPX(SFLBOOTTEXTLINE) + SFLBOOTPX(SFLBOOTTEXTPAD));
		SflBootPlaceLine(pThis, item, SFLBOOTPX(SFLBOOTMARGIN) + SFLBOOTPX(SFLBOOTLABELWIDTH), y, SFLBOOTPX(SFLBOOTFIELDWIDTH));
		return y + SFLBOOTPX(SFLBOOTROWHEIGHT) + SFLBOOTPX(SFLBOOTROWGAP);
	case SFLBOOTCOMBO:
		SflBootPlaceLabel(pThis, item, SFLBOOTPX(SFLBOOTMARGIN), y + SFLBOOTPX(SFLBOOTLABELOFFSET), SFLBOOTPX(SFLBOOTLABELWIDTH), SFLBOOTPX(SFLBOOTTEXTLINE) + SFLBOOTPX(SFLBOOTTEXTPAD));
		SflBootPlaceCombo(pThis, item, SFLBOOTPX(SFLBOOTMARGIN) + SFLBOOTPX(SFLBOOTLABELWIDTH), y);
		return y + SFLBOOTPX(SFLBOOTROWHEIGHT) + SFLBOOTPX(SFLBOOTROWGAP);
	case SFLBOOTRADIO:
		return SflBootPlaceRadios(pThis, item, y, width);
	case SFLBOOTLIST:
		SflBootPlaceList(pThis, item, SFLBOOTPX(SFLBOOTMARGIN), y, width);
		return y + SFLBOOTPX(SFLBOOTLISTHEIGHT) + SFLBOOTPX(SFLBOOTROWGAP);
	case SFLBOOTBUTTONS:
		x = SFLBOOTPX(SFLBOOTMARGIN);
		for (index = 0; index < item->buttoncount; ++index) {
			SflBootPlaceButton(pThis, item, index, x, y, SFLBOOTPX(SFLBOOTBUTTONWIDTH));
			x += SFLBOOTPX(SFLBOOTBUTTONWIDTH) + SFLBOOTPX(SFLBOOTROWGAP);
		}
		return y + SFLBOOTPX(SFLBOOTROWHEIGHT) + SFLBOOTPX(SFLBOOTROWGAP);
	default:
		return y;
	}
}

static int SflBootPlaceItem(SFLBOOTGUI *pThis, SFLBOOTITEM *item, int y, int width)
{
	int next;
	next = SflBootPlaceItemAt(pThis, item, y, width);
	if (item->hidden)
		return y;
	return next;
}

static void SflBootLayout(SFLBOOTGUI *pThis)
{
	WSRECT client;
	WSRECT bounds;
#if defined(WSWINGUI)
	WSRECT tabpage;
#endif
	int positions[SFLBOOTPAGES];
	int contentwidth[SFLBOOTPAGES];
	int width;
	int height;
	int tabheight;
	int index;
	SFLBOOTITEM *item;
	if (!pThis->ready)
		return;
	WSWindow_GetClientRect(&pThis->window, &client);
	width = WSRect_GetWidth(&client);
	height = WSRect_GetHeight(&client);
	tabheight = SFLBOOTPX(SFLBOOTTABHEIGHT);
	WSRect_Construct(&bounds, SFLBOOTPX(SFLBOOTMARGIN), SFLBOOTPX(SFLBOOTMARGIN), width - 2 * SFLBOOTPX(SFLBOOTMARGIN), tabheight);
	WSCtlTab_Move(&pThis->tabs, &bounds);
#if defined(WSWINGUI)
	WSCtlTab_GetClientRect(&pThis->tabs, &tabpage);
	tabheight = tabpage.top;
	WSRect_Construct(&bounds, SFLBOOTPX(SFLBOOTMARGIN), SFLBOOTPX(SFLBOOTMARGIN), width - 2 * SFLBOOTPX(SFLBOOTMARGIN), tabheight);
	WSCtlTab_Move(&pThis->tabs, &bounds);
#endif
	WSRect_Construct(&bounds, SFLBOOTPX(SFLBOOTMARGIN), height - SFLBOOTPX(SFLBOOTBOOTHEIGHT) - SFLBOOTPX(SFLBOOTMARGIN), width - 2 * SFLBOOTPX(SFLBOOTMARGIN), SFLBOOTPX(SFLBOOTBOOTHEIGHT));
	WSCtlButton_Move(&pThis->boot, &bounds);
	WSRect_Construct(&bounds, SFLBOOTPX(SFLBOOTMARGIN), SFLBOOTPX(SFLBOOTMARGIN) + tabheight, width - 2 * SFLBOOTPX(SFLBOOTMARGIN), height - 3 * SFLBOOTPX(SFLBOOTMARGIN) - tabheight - SFLBOOTPX(SFLBOOTBOOTHEIGHT));
	for (index = 0; index < SFLBOOTPAGES; ++index) {
		WSCtlScrollPanel_Move(&pThis->panels[index], &bounds);
		contentwidth[index] = WSCtlScrollPanel_GetContentWidth(&pThis->panels[index]) - 2 * SFLBOOTPX(SFLBOOTMARGIN);
		positions[index] = SFLBOOTPX(SFLBOOTMARGIN);
	}
	for (index = 0; index < pThis->count; ++index) {
		item = &pThis->items[index];
		positions[item->page] = SflBootPlaceItem(pThis, item, positions[item->page], contentwidth[item->page]);
	}
	for (index = 0; index < SFLBOOTPAGES; ++index) {
		WSCtlScrollPanel_SetContentHeight(&pThis->panels[index], positions[index] + SFLBOOTPX(SFLBOOTMARGIN));
		if (index == pThis->page)
			WSCtlScrollPanel_Show(&pThis->panels[index]);
		else
			WSCtlScrollPanel_Hide(&pThis->panels[index]);
	}
	WSWindow_Invalidate(&pThis->window);
}

static void SflBootScrollCallback(void *con)
{
	SflBootLayout((SFLBOOTGUI *)con);
}

static void SflBootShowPage(SFLBOOTGUI *pThis, int page, int y)
{
	pThis->page = page;
	WSCtlTab_SetCurrent(&pThis->tabs, (size_t)page);
	SflBootLayout(pThis);
	WSCtlScrollPanel_ScrollTo(&pThis->panels[page], y);
}

static void SflBootError(SFLBOOTGUI *pThis, SFLBOOTITEM *item, const char *text)
{
	SflBootShowPage(pThis, SFLBOOTPAGEGENERAL, item->documenty);
	WSMessageBox(&pThis->window, "Unable to boot", text, WSMESSAGEBOX_OK);
}

static void SflBootApplyList(SFLBOOTITEM *item)
{
	char *text;
	int index;
	SflBootRemove(item->pref);
	for (index = 0; index < WSCtlList_GetItemCount(&item->list); ++index) {
		text = SflBootListText(item, index);
		SflBootSetString(item->pref, text, index);
		WSFREE(text);
	}
}

static void SflBootApplyItem(SFLBOOTITEM *item, int resolutionsedited)
{
	char *text;
	char value[SFLBOOTVALUESIZE];
	if (!item->hascontrol)
		return;
	switch (item->type) {
	case SFLBOOTCHECK:
		SflBootSetBool(item->pref, (WSCtlCheckBox_IsChecked(&item->check) != 0) != item->invert);
		break;
	case SFLBOOTPATH:
		text = NULL;
		WSCtlLine_AllocText(&item->line, &text);
		SflBootSetFirstString(item->pref, text);
		WSFREE(text);
		break;
	case SFLBOOTFIELD:
		text = NULL;
		WSCtlLine_AllocText(&item->line, &text);
		SflBootSetInt(item->pref, atoi(text));
		WSFREE(text);
		break;
	case SFLBOOTCOMBO:
		item->selected = (int)WSCtlComboBox_GetCurrentIndex(&item->combo);
		SflBootSelectedValue(item, value);
		SflBootStoreValue(item, value);
		break;
	case SFLBOOTRADIO:
		SflBootSelectedValue(item, value);
		SflBootStoreValue(item, value);
		break;
	case SFLBOOTLIST:
		if (strcmp(item->pref, "disk") != 0 && (strcmp(item->pref, "resolution") != 0 || resolutionsedited))
			SflBootApplyList(item);
		break;
	default:
		break;
	}
}

static int SflBootRomIsValid(SFLBOOTGUI *pThis)
{
	char *path;
	int valid;
	path = NULL;
	WSCtlLine_AllocText(&SflBootFind(pThis, "rom")->line, &path);
	valid = SflEmulatorRomValid(path);
	WSFREE(path);
	return valid;
}

static void SflBootUpdateRomStatus(SFLBOOTGUI *pThis)
{
	SFLBOOTITEM *status;
	SFLBOOTITEM *welcome;
	int valid;
	int shown;
	valid = SflBootRomIsValid(pThis);
	status = SflBootFind(pThis, "romstatus");
	if (valid)
		SflBootRefreshHelp(status, "A valid ROM was found.");
	else
		SflBootRefreshHelp(status, "No valid ROM. Select a Macintosh ROM file.");
	shown = 0;
	if (SflBootIsSheepShaver()) {
		shown = !valid || pThis->bootstrapcompleted;
		SflBootFind(pThis, "bootstraptext")->hidden = !shown;
		SflBootFind(pThis, "bootstrapbuttons")->hidden = !shown || pThis->bootstrapcompleted;
		SflBootFind(pThis, "bootstrapdone")->hidden = !pThis->bootstrapcompleted;
		SflBootFind(pThis, "advbootstrapheading")->hidden = !valid;
		SflBootFind(pThis, "advbootstraptext")->hidden = !valid;
		SflBootFind(pThis, "advbootstrapbuttons")->hidden = !valid;
		if (valid)
			SflBootFind(pThis, "bootstraperror")->hidden = 1;
	}
	welcome = SflBootFind(pThis, "welcomeheading");
	welcome->hidden = shown;
	SflBootFind(pThis, "welcometext")->hidden = shown;
}

static void SflBootRefreshDynamic(SFLBOOTGUI *pThis)
{
	SFLBOOTITEM *relative;
	char value[SFLBOOTVALUESIZE];
	if (WSCtlCheckBox_IsChecked(&SflBootFind(pThis, "clipsharing")->check))
		SflBootRefreshHelp(SflBootFind(pThis, "clipinfo"), "Clipboard content is automatically copied between Classic Mac OS and the host after each copy action, in either OS.");
	else
		SflBootRefreshHelp(SflBootFind(pThis, "clipinfo"), "Clipboard content will not be shared.");
	relative = SflBootFind(pThis, "relmouse");
	SflBootSelectedValue(relative, value);
	SflBootFind(pThis, "init_grab")->hidden = strcmp(value, "always") == 0;
	if (strcmp(value, "always") == 0)
		SflBootRefreshHelp(SflBootFind(pThis, "relinfo"), "Some games and apps require relative mouse mode to function.");
	else
		SflBootRefreshHelp(SflBootFind(pThis, "relinfo"), "Some games and apps require relative mouse mode to function. Relative mouse mode can be toggled on and off during emulation by pressing control + F5.");
	SflBootFind(pThis, "networkwarning")->hidden = !pThis->networkchanged;
	SflBootLayout(pThis);
}

static void SflBootBoot(SFLBOOTGUI *pThis)
{
	int index;
	for (index = 0; index < pThis->count; ++index)
		SflBootApplyItem(&pThis->items[index], pThis->resolutionsedited);
	SflBootDiskApply(pThis);
	SflBootUpdateRomStatus(pThis);
	if (!SflBootIsNativeBoot() && !SflEmulatorRomValid(SflEmulatorPrefString("rom", 0))) {
		if (SflBootIsSheepShaver()) {
			SflBootFind(pThis, "bootstraperror")->hidden = 0;
			SflBootLayout(pThis);
			SflBootError(pThis, SflBootFind(pThis, "bootstraptext"), "You need to bootstrap the emulator or select a valid Macintosh ROM file before booting.");
		} else {
			SflBootLayout(pThis);
			SflBootError(pThis, SflBootFind(pThis, "rom"), "Select a valid Macintosh ROM file before booting.");
		}
		return;
	}
	if (SflEmulatorPrefString("disk", 0) == NULL && SflEmulatorPrefString("cdrom", 0) == NULL) {
		SflBootFind(pThis, "diskerror")->hidden = 0;
		SflBootLayout(pThis);
		SflBootError(pThis, SflBootFind(pThis, "disk"), "Must select to mount at least one disk file.");
		return;
	}
	if (!SflBootSave()) {
		WSMessageBox(&pThis->window, "Save failed", "The preferences file could not be written. Check its permissions and available disk space.", WSMESSAGEBOX_OK);
		return;
	}
	pThis->accepted = 1;
	pThis->finished = 1;
}

static void SflBootSheetClose(SFLBOOTSHEET *sheet)
{
	sheet->closed = 1;
	WSCtlButton_Destroy(&sheet->done);
	WSCtlText_Destroy(&sheet->text);
	WSScrolledWindow_Destroy(&sheet->scrolled);
	WSWindow_Destroy(&sheet->window);
}

WSEVENTBUTTON_CALLBACK(SflBootSheetDone)
{
	WSEVENTBUTTON event;
	WSEventButton_Construct(&event);
	SflBootSheetClose((SFLBOOTSHEET *)WSEventButton_GetWindowCon(&event));
	return WSEVENTBUTTON_HANDLED;
}

WSCLOSEEVENT_CALLBACK(SflBootSheetDismiss)
{
	WSCLOSEEVENT event;
	WSCloseEvent_Construct(&event);
	SflBootSheetClose((SFLBOOTSHEET *)WSCloseEvent_GetWindowCon(&event));
	return WSCLOSEEVENT_HANDLED;
}

BEGIN_WINDOW_MESSAGES(SflBootSheetMessages)
	IMPLEMENT_CLOSE_MESSAGE(SflBootSheetDismiss)
	IMPLEMENT_CTLBUTTON_CLICKED_MESSAGE(SflBootSheetDone, SFLBOOTIDDONE)
#if defined(SFL_USEDARKMODE)
	END_WINDOW_MESSAGES_DARKMODE(SflBootSheetMessages)
#else
	END_WINDOW_MESSAGES(SflBootSheetMessages)
#endif /* SFL_USEDARKMODE */

static void SflBootSheet(SFLBOOTGUI *pThis, const char *title, const char *text)
{
	SFLBOOTSHEET sheet;
	WSWNDCLASS windowclass;
	WSRECT bounds;
	int inset;
	int width;
	int buttonheight;
	memset(&sheet, 0, sizeof(sheet));
	if (!WSWndClass_Create(&windowclass, pThis->app, NULL, SflBootSheetMessages, "SflBootSheet"))
		return;
	inset = SFLBOOTPX(SFLBOOTSHEETINSET);
	width = SFLBOOTPX(SFLBOOTSHEETWIDTH);
	buttonheight = SFLBOOTPX(SFLBOOTBOOTHEIGHT);
	WSRect_Construct(&bounds, WSDEFAULTPOSITION, WSDEFAULTPOSITION, width, SFLBOOTPX(SFLBOOTSHEETHEIGHT));
	if (WSWindow_Create(&sheet.window, pThis->app, &bounds, title, NULL, &windowclass)) {
		WSWindow_SetCon(&sheet.window, &sheet);
		WSWindow_SetSize(&sheet.window, width, SFLBOOTPX(SFLBOOTSHEETHEIGHT));
		WSRect_Construct(&bounds, inset, inset, width - 3 * inset, SFLBOOTPX(SFLBOOTSHEETHEIGHT) - 2 * buttonheight - 4 * inset);
		if (WSCtlText_CreateScrolled(&sheet.text, &sheet.scrolled, &sheet.window, &bounds, &windowclass)) {
			WSCtlText_SetText(&sheet.text, text, strlen(text));
			WSCtlText_SetEditable(&sheet.text, FALSE);
			WSRect_Construct(&bounds, inset, SFLBOOTPX(SFLBOOTSHEETHEIGHT) - 2 * buttonheight - 2 * inset, width - 3 * inset, buttonheight);
			if (WSCtlButton_Create(&sheet.done, &sheet.window, &bounds, "Done", SflBootSheetDone, SFLBOOTIDDONE))
				WSWindow_Modal(&sheet.window, &pThis->window, pThis->app);
			else {
				WSCtlText_Destroy(&sheet.text);
				WSScrolledWindow_Destroy(&sheet.scrolled);
			}
		}
		if (!sheet.closed)
			WSWindow_Destroy(&sheet.window);
	}
	WSWndClass_Destroy(&windowclass, pThis->app);
}

static void SflBootFormClose(SFLBOOTFORM *form, int accept)
{
	if (accept) {
		WSCtlLine_AllocText(&form->line, &form->value);
		if (form->hassecond)
			WSCtlLine_AllocText(&form->secondline, &form->secondvalue);
	}
	form->accepted = accept;
	form->closed = 1;
	WSCtlButton_Destroy(&form->cancel);
	WSCtlButton_Destroy(&form->accept);
	if (form->hassecond) {
		WSCtlLine_Destroy(&form->secondline);
		WSCtlStyledLabel_Destroy(&form->secondlabel);
	}
	WSCtlLine_Destroy(&form->line);
	WSCtlStyledLabel_Destroy(&form->label);
	WSWindow_Destroy(&form->window);
}

WSEVENTBUTTON_CALLBACK(SflBootFormAccept)
{
	WSEVENTBUTTON event;
	WSEventButton_Construct(&event);
	SflBootFormClose((SFLBOOTFORM *)WSEventButton_GetWindowCon(&event), 1);
	return WSEVENTBUTTON_HANDLED;
}

WSEVENTBUTTON_CALLBACK(SflBootFormCancel)
{
	WSEVENTBUTTON event;
	WSEventButton_Construct(&event);
	SflBootFormClose((SFLBOOTFORM *)WSEventButton_GetWindowCon(&event), 0);
	return WSEVENTBUTTON_HANDLED;
}

WSCLOSEEVENT_CALLBACK(SflBootFormDismiss)
{
	WSCLOSEEVENT event;
	WSCloseEvent_Construct(&event);
	SflBootFormClose((SFLBOOTFORM *)WSCloseEvent_GetWindowCon(&event), 0);
	return WSCLOSEEVENT_HANDLED;
}

BEGIN_WINDOW_MESSAGES(SflBootFormMessages)
	IMPLEMENT_CLOSE_MESSAGE(SflBootFormDismiss)
	IMPLEMENT_CTLBUTTON_CLICKED_MESSAGE(SflBootFormAccept, SFLBOOTIDACCEPT)
	IMPLEMENT_CTLBUTTON_CLICKED_MESSAGE(SflBootFormCancel, SFLBOOTIDCANCEL)
#if defined(SFL_USEDARKMODE)
	END_WINDOW_MESSAGES_DARKMODE(SflBootFormMessages)
#else
	END_WINDOW_MESSAGES(SflBootFormMessages)
#endif /* SFL_USEDARKMODE */


static int SflBootForm(SFLBOOTGUI *pThis, const char *title, const char *label, const char *initial, const char *secondlabel, const char *secondinitial, char **value, char **secondvalue)
{
	SFLBOOTFORM form;
	WSWNDCLASS windowclass;
	WSRECT bounds;
	int inset;
	int width;
	int height;
	int row;
	int y;
	memset(&form, 0, sizeof(form));
	if (!WSWndClass_Create(&windowclass, pThis->app, NULL, SflBootFormMessages, "SflBootForm"))
		return 0;
	inset = SFLBOOTPX(SFLBOOTSHEETINSET);
	width = SFLBOOTPX(SFLBOOTFORMWIDTH);
	height = SFLBOOTPX(SFLBOOTFORMHEIGHT);
	row = SFLBOOTPX(SFLBOOTROWHEIGHT);
	WSRect_Construct(&bounds, WSDEFAULTPOSITION, WSDEFAULTPOSITION, width, height);
	if (!WSWindow_Create(&form.window, pThis->app, &bounds, title, NULL, &windowclass)) {
		WSWndClass_Destroy(&windowclass, pThis->app);
		return 0;
	}
	WSWindow_SetCon(&form.window, &form);
	WSWindow_SetSize(&form.window, width, height);
	y = inset;
	WSRect_Construct(&bounds, inset, y, width - 3 * inset, row);
	WSCtlStyledLabel_Create(&form.label, &form.window, &bounds, label, 0, 3);
	if (pThis->fontscreated)
		WSCtlStyledLabel_SetFont(&form.label, &pThis->bodyfont);
	y += row;
	WSRect_Construct(&bounds, inset, y, width - 3 * inset, row);
	WSCtlLine_Create(&form.line, &form.window, &bounds);
	WSCtlLine_SetText(&form.line, initial, strlen(initial));
	y += row;
	if (secondlabel != NULL) {
		form.hassecond = 1;
		WSRect_Construct(&bounds, inset, y, width - 3 * inset, row);
		WSCtlStyledLabel_Create(&form.secondlabel, &form.window, &bounds, secondlabel, 0, 4);
		if (pThis->fontscreated)
			WSCtlStyledLabel_SetFont(&form.secondlabel, &pThis->bodyfont);
		y += row;
		WSRect_Construct(&bounds, inset, y, width - 3 * inset, row);
		WSCtlLine_Create(&form.secondline, &form.window, &bounds);
		WSCtlLine_SetText(&form.secondline, secondinitial, strlen(secondinitial));
	}
	WSRect_Construct(&bounds, width - 2 * SFLBOOTPX(SFLBOOTBROWSEWIDTH) - 3 * inset, height - SFLBOOTPX(70) - inset, SFLBOOTPX(SFLBOOTBROWSEWIDTH), SFLBOOTPX(SFLBOOTROWHEIGHT));
	WSCtlButton_Create(&form.accept, &form.window, &bounds, "OK", SflBootFormAccept, SFLBOOTIDACCEPT);
	WSRect_Construct(&bounds, width - SFLBOOTPX(SFLBOOTBROWSEWIDTH) - 2 * inset, height - SFLBOOTPX(70) - inset, SFLBOOTPX(SFLBOOTBROWSEWIDTH), SFLBOOTPX(SFLBOOTROWHEIGHT));
	WSCtlButton_Create(&form.cancel, &form.window, &bounds, "Cancel", SflBootFormCancel, SFLBOOTIDCANCEL);
	WSWindow_Modal(&form.window, &pThis->window, pThis->app);
	WSWndClass_Destroy(&windowclass, pThis->app);
	*value = form.value;
	if (secondvalue != NULL)
		*secondvalue = form.secondvalue;
	return form.accepted;
}

static void SflBootBrowsePath(SFLBOOTGUI *pThis, const char *pref)
{
	SFLBOOTITEM *item;
	WSCHOOSERFILEITERATOR iterator;
	char *path;
	item = SflBootFind(pThis, pref);
	if (item->folder)
		WSChooserFolder(&iterator, &pThis->window);
	else
		WSChooserFile(&iterator, &pThis->window);
	if (!WSChooserFileIsEnd(&iterator)) {
		WSChooserFileIterator_AllocValue(&iterator, &path);
		WSCtlLine_SetText(&item->line, path, strlen(path));
		WSFREE(path);
	}
	WSChooserFileFinish(&iterator);
}

static void SflBootAddDisks(SFLBOOTGUI *pThis)
{
	WSCHOOSERFILEITERATOR iterator;
	char *path;
	int rejected;
	rejected = 0;
	WSChooserFile(&iterator, &pThis->window);
	while (!WSChooserFileIsEnd(&iterator)) {
		WSChooserFileIterator_AllocValue(&iterator, &path);
		if (SflBootExtensionIn(path, SflBootDiskExtensions()))
			SflBootDiskAppend(pThis, path, 1, SflBootExtensionIn(path, SflBootCdExtensions()));
		else
			rejected = 1;
		WSFREE(path);
		WSChooserFileIterator_Increment(&iterator);
	}
	WSChooserFileFinish(&iterator);
	SflBootDiskRender(pThis);
	SflBootLayout(pThis);
	if (rejected)
		WSMessageBox(&pThis->window, "Import disk file", "The file has an unsupported suffix. The supported suffixes are .dsk, .dmg, .cdr, .iso, .cue, .toast, .img, .chd, .qcow2.", WSMESSAGEBOX_OK);
}

static int SflBootSelectedDisk(SFLBOOTGUI *pThis)
{
	return WSCtlList_GetNextItem(&SflBootFind(pThis, "disk")->list, -1, WSCTLLIST_STATE_SELECTED, WSCTLLIST_GETNEXTITEM_ALL);
}

static void SflBootDiskRemove(SFLBOOTGUI *pThis)
{
	int selected;
	selected = SflBootSelectedDisk(pThis);
	if (selected < 0)
		return;
	SflBootDiskRemoveAt(pThis, selected);
	SflBootDiskRender(pThis);
	SflBootLayout(pThis);
}

static void SflBootDiskToggleMount(SFLBOOTGUI *pThis)
{
	int selected;
	selected = SflBootSelectedDisk(pThis);
	if (selected < 0)
		return;
	pThis->disks[selected].mounted = !pThis->disks[selected].mounted;
	if (pThis->disks[selected].mounted)
		SflBootFind(pThis, "diskerror")->hidden = 1;
	SflBootDiskRender(pThis);
	SflBootLayout(pThis);
}

static void SflBootDiskToggleType(SFLBOOTGUI *pThis)
{
	int selected;
	selected = SflBootSelectedDisk(pThis);
	if (selected < 0)
		return;
	pThis->disks[selected].cd = !pThis->disks[selected].cd;
	SflBootDiskRender(pThis);
}

static void SflBootDiskReload(SFLBOOTGUI *pThis)
{
	WSFOLDER folder;
	WSFOLDERFILEINFO info;
	char directory[SFLBOOTPATHSIZE];
	char joined[SFLBOOTPATHSIZE];
	int index;
	index = 0;
	while (index < pThis->diskcount) {
		if (WSFileExists(pThis->disks[index].path))
			index++;
		else
			SflBootDiskRemoveAt(pThis, index);
	}
	SflBootShareFolder(directory, sizeof(directory));
	WSFolder_Create(&folder, directory);
	if (WSFolder_IsCreated(&folder)) {
		while (WSFolder_Next(&folder, &info)) {
			if (!SflBootExtensionIn(info.sName, SflBootDiskExtensions()))
				continue;
			if (strlen(directory) + strlen(info.sName) + 2 > sizeof(joined))
				continue;
			sprintf(joined, "%s%s%s", directory, WSPATHSEPARATOR, info.sName);
			SflBootDiskAppend(pThis, joined, 0, SflBootExtensionIn(info.sName, SflBootCdExtensions()));
		}
		WSFolder_Destroy(&folder);
	}
	SflBootDiskRender(pThis);
	SflBootLayout(pThis);
}

static void SflBootRemoveSelected(SFLBOOTGUI *pThis, const char *pref)
{
	SFLBOOTITEM *item;
	int selected;
	item = SflBootFind(pThis, pref);
	selected = WSCtlList_GetNextItem(&item->list, -1, WSCTLLIST_STATE_SELECTED, WSCTLLIST_GETNEXTITEM_ALL);
	if (selected >= 0)
		WSCtlList_DeleteItem(&item->list, selected);
	if (strcmp(pref, "resolution") == 0)
		pThis->resolutionsedited = 1;
}

static void SflBootEditResolution(SFLBOOTGUI *pThis, int edit)
{
	SFLBOOTITEM *item;
	char *value;
	char *current;
	const char *initial;
	char normalized[SFLBOOTVALUESIZE];
	int selected;
	int width;
	int height;
	item = SflBootFind(pThis, "resolution");
	selected = -1;
	current = NULL;
	if (edit) {
		selected = WSCtlList_GetNextItem(&item->list, -1, WSCTLLIST_STATE_SELECTED, WSCTLLIST_GETNEXTITEM_ALL);
		if (selected < 0)
			return;
		current = SflBootListText(item, selected);
	} else if (WSCtlList_GetItemCount(&item->list) >= SFLBOOTMAXRESOLUTIONS) {
		WSMessageBox(&pThis->window, "Monitor resolutions", "Mac OS allows a maximum of 10 monitor resolutions to be available simultaneously.", WSMESSAGEBOX_OK);
		return;
	}
	initial = "800x600";
	if (current != NULL)
		initial = current;
	value = NULL;
	if (SflBootForm(pThis, "Monitor resolution", "Width x height", initial, NULL, NULL, &value, NULL)) {
		if (sscanf(value, "%dx%d", &width, &height) == 2 && width >= SFLBOOTMINWIDTH && height >= SFLBOOTMINHEIGHT && width <= SFLBOOTMAXWIDTH && height <= SFLBOOTMAXHEIGHT) {
			sprintf(normalized, "%dx%d", width, height);
			if (edit)
				WSCtlList_SetItemText(&item->list, selected, 0, normalized);
			else
				SflBootAppendList(item, normalized);
			pThis->resolutionsedited = 1;
		} else {
			WSMessageBox(&pThis->window, "Monitor resolution", "Enter a resolution such as 800x600, between 512x384 and 4096x4096.", WSMESSAGEBOX_OK);
		}
		WSFREE(value);
	}
	if (current != NULL)
		WSFREE(current);
}

static void SflBootSelectInstallDisc(SFLBOOTGUI *pThis)
{
	WSCHOOSERFILEITERATOR iterator;
	SFLBOOTITEM *rom;
	char destination[SFLBOOTPATHSIZE];
	char *path;
	int extracted;
	WSChooserFile(&iterator, &pThis->window);
	if (WSChooserFileIsEnd(&iterator)) {
		WSChooserFileFinish(&iterator);
		return;
	}
	WSChooserFileIterator_AllocValue(&iterator, &path);
	WSChooserFileFinish(&iterator);
	extracted = SflBootExtractRom(path, destination, sizeof(destination));
	WSFREE(path);
	if (!extracted) {
		WSMessageBox(&pThis->window, "Mac OS install disc image not compatible", "The provided file is not a compatible Mac OS install disc image for bootstrapping. Check 'Bootstrap compatibility list' for guidance.", WSMESSAGEBOX_OK);
		return;
	}
	rom = SflBootFind(pThis, "rom");
	WSCtlLine_SetText(&rom->line, destination, strlen(destination));
	pThis->bootstrapcompleted = 1;
	SflBootRefreshHelp(SflBootFind(pThis, "bootstraptext"), "Congratulations, the emulator is successfully bootstrapped!\n\nYou can now install and run Mac OS up to version 9.0.4. For network support, Mac OS 9.0 - 9.0.4 is required.");
	SflBootFind(pThis, "bootstraperror")->hidden = 1;
	SflBootUpdateRomStatus(pThis);
	SflBootLayout(pThis);
}

static void SflBootBootstrapDone(SFLBOOTGUI *pThis)
{
	pThis->bootstrapcompleted = 0;
	SflBootRefreshHelp(SflBootFind(pThis, "bootstraptext"), SflBootBootstrapText());
	SflBootUpdateRomStatus(pThis);
	SflBootLayout(pThis);
}

static void SflBootShowInstructions(SFLBOOTGUI *pThis)
{
	pThis->setupread = 1;
	SflBootSheet(pThis, "Setup instructions", SflBootInstructionsText());
}

static void SflBootDismissSetup(SFLBOOTGUI *pThis)
{
	if (pThis->setupread) {
		WSMessageBox(&pThis->window, "Information", "Setup instructions will still be accessible from the Resources section at the bottom of this tab.", WSMESSAGEBOX_OK);
	} else if (WSMessageBox(&pThis->window, "Warning", "Steps 5 and 8 of the setup instructions are non-trivial and must be followed for successful setup.\nIf setup instructions are dismissed from here, they can still be accessed from the Resources section at the bottom of this tab.", WSMESSAGEBOX_OKCANCEL) != WSMESSAGEBOX_IDOK) {
		return;
	}
	SflBootSetBool("setupdismissed", 1);
	SflBootFind(pThis, "setupbanner")->hidden = 1;
	SflBootFind(pThis, "setupread")->hidden = 1;
	SflBootLayout(pThis);
}

static void SflBootCreateDisk(SFLBOOTGUI *pThis)
{
	FILE *file;
	char directory[SFLBOOTPATHSIZE];
	char joined[SFLBOOTPATHSIZE];
	char *name;
	char *sizetext;
	long megabytes;
	int success;
	name = NULL;
	sizetext = NULL;
	if (!SflBootForm(pThis, "Create new disk", "Name", "DiskName.dsk", "Size in MB (recommended minimum is around 500)", "500", &name, &sizetext))
		return;
	SflBootShareFolder(directory, sizeof(directory));
	megabytes = atol(sizetext);
	WSFREE(sizetext);
	if (megabytes <= 0 || megabytes > SFLBOOTMAXDISKSIZEMB) {
		WSMessageBox(&pThis->window, "Create new disk", "An invalid file size was given.", WSMESSAGEBOX_OK);
		WSFREE(name);
		return;
	}
	if (name[0] == 0 || strlen(directory) + strlen(name) + 8 > sizeof(joined)) {
		WSMessageBox(&pThis->window, "Create new disk", "Something went wrong when trying to create the file.", WSMESSAGEBOX_OK);
		WSFREE(name);
		return;
	}
	if (SflBootExtensionIn(name, "|dsk|"))
		sprintf(joined, "%s%s%s", directory, WSPATHSEPARATOR, name);
	else
		sprintf(joined, "%s%s%s.dsk", directory, WSPATHSEPARATOR, name);
	WSFREE(name);
	if (WSFileExists(joined)) {
		WSMessageBox(&pThis->window, "Create new disk", "A file with that name already exists.", WSMESSAGEBOX_OK);
		return;
	}
	success = 0;
	file = fopen(joined, "wb");
	if (file != NULL) {
		success = fseek(file, megabytes * SFLBOOTMEGABYTE - 1, SEEK_SET) == 0 && fputc(0, file) != EOF;
		if (fclose(file) != 0)
			success = 0;
	}
	if (!success) {
		WSMessageBox(&pThis->window, "Create new disk", "Something went wrong when trying to create the file.", WSMESSAGEBOX_OK);
		return;
	}
	SflBootDiskAppend(pThis, joined, 1, 0);
	SflBootDiskRender(pThis);
	SflBootLayout(pThis);
}

static void SflBootAddRedirect(SFLBOOTGUI *pThis)
{
	char *rule;
	if (!SflBootForm(pThis, "Port forwarding", "protocol:host-port:guest-address:guest-port", "tcp:8080:10.0.2.15:80", NULL, NULL, &rule, NULL))
		return;
	if (SflBootValidateRedir(rule))
		SflBootAppendList(SflBootFind(pThis, "redir"), rule);
	else
		WSMessageBox(&pThis->window, "Invalid rule", "Use tcp or udp, valid ports, and a guest IPv4 address.", WSMESSAGEBOX_OK);
	WSFREE(rule);
}

static void SflBootDismiss(SFLBOOTGUI *pThis, const char *pref, const char *linked)
{
	SflBootFind(pThis, pref)->hidden = 1;
	if (linked != NULL)
		SflBootFind(pThis, linked)->hidden = 1;
	SflBootLayout(pThis);
}

#define SFLBOOTBUTTONHANDLER(name, action) \
	WSEVENTBUTTON_CALLBACK(name) \
	{ \
		WSEVENTBUTTON event; \
		SFLBOOTGUI *pThis; \
		WSEventButton_Construct(&event); \
		pThis = (SFLBOOTGUI *)WSEventButton_GetWindowCon(&event); \
		action; \
		return WSEVENTBUTTON_HANDLED; \
	}

SFLBOOTBUTTONHANDLER(SflBootOnBoot, SflBootBoot(pThis))
SFLBOOTBUTTONHANDLER(SflBootOnBrowseRom, { SflBootBrowsePath(pThis, "rom"); SflBootUpdateRomStatus(pThis); SflBootLayout(pThis); })
SFLBOOTBUTTONHANDLER(SflBootOnBrowseFolder, SflBootBrowsePath(pThis, "extfs"))
SFLBOOTBUTTONHANDLER(SflBootOnDiskAdd, SflBootAddDisks(pThis))
SFLBOOTBUTTONHANDLER(SflBootOnDiskCreate, SflBootCreateDisk(pThis))
SFLBOOTBUTTONHANDLER(SflBootOnDiskRemove, SflBootDiskRemove(pThis))
SFLBOOTBUTTONHANDLER(SflBootOnDiskMount, SflBootDiskToggleMount(pThis))
SFLBOOTBUTTONHANDLER(SflBootOnDiskType, SflBootDiskToggleType(pThis))
SFLBOOTBUTTONHANDLER(SflBootOnDiskReload, SflBootDiskReload(pThis))
SFLBOOTBUTTONHANDLER(SflBootOnDiskOpen, SflBootOpenShareFolder())
SFLBOOTBUTTONHANDLER(SflBootOnResolutionAdd, SflBootEditResolution(pThis, 0))
SFLBOOTBUTTONHANDLER(SflBootOnResolutionEdit, SflBootEditResolution(pThis, 1))
SFLBOOTBUTTONHANDLER(SflBootOnResolutionRemove, SflBootRemoveSelected(pThis, "resolution"))
SFLBOOTBUTTONHANDLER(SflBootOnBootstrap, SflBootSelectInstallDisc(pThis))
SFLBOOTBUTTONHANDLER(SflBootOnBootstrapDone, SflBootBootstrapDone(pThis))
SFLBOOTBUTTONHANDLER(SflBootOnCompatibility, SflBootSheet(pThis, "Bootstrap compatibility list", SflBootCompatibilityText()))
SFLBOOTBUTTONHANDLER(SflBootOnRelative, SflBootSheet(pThis, "Relative mouse mode", SflBootRelativeMouseText()))
SFLBOOTBUTTONHANDLER(SflBootOnRedirAdd, SflBootAddRedirect(pThis))
SFLBOOTBUTTONHANDLER(SflBootOnRedirRemove, SflBootRemoveSelected(pThis, "redir"))
SFLBOOTBUTTONHANDLER(SflBootOnInstructions, SflBootShowInstructions(pThis))
SFLBOOTBUTTONHANDLER(SflBootOnLicenses, SflBootSheet(pThis, "Licenses", SflBootLicenseText()))
SFLBOOTBUTTONHANDLER(SflBootOnSetupClose, SflBootDismissSetup(pThis))
SFLBOOTBUTTONHANDLER(SflBootOnNetworkClose, SflBootDismiss(pThis, "networkbanner", NULL))

WSDND_DROP_CALLBACK(SflBootOnDrop)
{
	WSEVENTDND event;
	WSDNDDROPFILEITERATOR iterator;
	SFLBOOTGUI *pThis;
	char *path;
	size_t length;
	WSEventDnD_Construct(&event);
	pThis = (SFLBOOTGUI *)WSEventDnD_GetWindowCon(&event);
	if (WSEventDnD_GetFileDrop(&event, &iterator)) {
		while (!WSEventDnD_IsFileDropEnd(&event, &iterator)) {
			WSDnDFileDropIterator_Value(&iterator, &path, &length);
			if (SflBootExtensionIn(path, SflBootDiskExtensions()))
				SflBootDiskAppend(pThis, path, 1, SflBootExtensionIn(path, SflBootCdExtensions()));
			WSFREE(path);
			WSDnDFileDropIterator_Increment(&iterator);
		}
		WSEventDnD_FinishFileDrop(&event, &iterator);
		SflBootDiskRender(pThis);
		SflBootLayout(pThis);
	}
	return WSEVENTDND_HANDLED;
}

WSEVENTCHECKBUTTON_CALLBACK(SflBootOnCheck)
{
	WSEVENTCHECKBUTTON event;
	SFLBOOTGUI *pThis;
	WSEventCheckButton_Construct(&event);
	WSCtlCheckBoxHandleClicked(&event);
	pThis = (SFLBOOTGUI *)WSEventCheckButton_GetWindowCon(&event);
	if (pThis->ready)
		SflBootRefreshDynamic(pThis);
	return WSEVENTCHECKBUTTON_HANDLED;
}

WSCLOSEEVENT_CALLBACK(SflBootOnClose)
{
	WSCLOSEEVENT event;
	WSCloseEvent_Construct(&event);
	((SFLBOOTGUI *)WSCloseEvent_GetWindowCon(&event))->finished = 1;
	return WSCLOSEEVENT_HANDLED;
}

WSEVENTTABSELECTION_CALLBACK(SflBootOnTab)
{
	WSEVENTTABSELECTION event;
	SFLBOOTGUI *pThis;
	WSEventTabSelection_Construct(&event);
	pThis = (SFLBOOTGUI *)WSEventTabSelection_GetWindowCon(&event);
	pThis->page = (int)WSCtlTab_GetCurrent(&pThis->tabs);
	SflBootLayout(pThis);
	WSCtlScrollPanel_ScrollTo(&pThis->panels[pThis->page], 0);
	return WSEVENTTABSELECTION_HANDLED;
}

WSSIZEEVENT_CALLBACK(SflBootOnSize)
{
	WSSIZEEVENT event;
	SFLBOOTGUI *pThis;
	WSSizeEvent_Construct(&event);
	pThis = (SFLBOOTGUI *)WSSizeEvent_GetWindowCon(&event);
	if (pThis != NULL && pThis->ready)
		SflBootLayout(pThis);
	return WSSIZEEVENT_UNHANDLED;
}

#if defined(WSWINGUI)
WSCTLLABELCOLOREVENT_CALLBACK(SflBootOnLabelColor)
{
	WSCTLLABELCOLOREVENT event;
	SFLBOOTGUI *pThis;
	int index;
	WSCtlLabelColorEvent_Construct(&event);
	pThis = (SFLBOOTGUI *)WSCtlLabelColorEvent_GetWindowCon(&event);
	if (pThis == NULL)
		return WSCTLLABELCOLOREVENT_UNHANDLED;
	for (index = 0; index < pThis->count; ++index)
		if (pThis->items[index].type == SFLBOOTHELP && pThis->items[index].haslabel
			&& *(void **)WSCtlLabelColorEvent_GetCtlLabel(&event) == (void *)WSCtl_GetHandle((WSCTL *)&pThis->items[index].label))
			return WSCtlStyledLabel_HandleColor(&pThis->items[index].label, &event);
	return WSCTLLABELCOLOREVENT_UNHANDLED;
}
#endif

#if defined(WSMACCLASSICORCARBON)
WSPAINTEVENT_CALLBACK(SflBootOnPaint)
{
	WSPAINTEVENT event;
	WSPAINTDC dc;
	SFLBOOTGUI *pThis;
	SFLBOOTITEM *item;
	int index;
	WSPaintEvent_Construct(&event);
	pThis = (SFLBOOTGUI *)WSPaintEvent_GetWindowCon(&event);
	WSPaintDC_Create(&dc, &event);
	for (index = 0; index < pThis->count; ++index) {
		item = &pThis->items[index];
		if (item->haslabel && item->page == pThis->page && !item->hidden)
			WSCtlStyledLabel_HandlePaint(&item->label, (WSDC *)&dc);
		if (item->type == SFLBOOTCOMBO && item->page == pThis->page && item->hascontrol)
			WSCtlComboBox_HandlePaint(&item->combo, (WSDC *)&dc);
		if (item->type == SFLBOOTLIST && item->page == pThis->page && item->hascontrol)
			WSCtlList_HandlePaint(&item->list);
	}
	WSCtlTab_HandlePaint(&pThis->tabs);
	WSPaintDC_Destroy(&dc, &event);
	return WSPAINTEVENT_HANDLED;
}

static void SflBootForwardMouse(SFLBOOTITEM *item, WSMOUSEEVENT *event)
{
	int index;
	switch (item->type) {
#if defined(WSMACCLASSIC)
	case SFLBOOTCHECK:
		WSCtlCheckBox_HandleMouseDown(&item->check, event);
		break;
	case SFLBOOTRADIO:
		for (index = 0; index < item->optioncount; ++index)
			WSCtlRadioButton_HandleMouseDown(&item->radios[index], event);
		break;
	case SFLBOOTBUTTONS:
	case SFLBOOTBANNER:
		for (index = 0; index < item->buttoncount; ++index)
			WSCtlButton_HandleMouseDown(&item->buttons[index], event);
		break;
	case SFLBOOTPATH:
		WSCtlButton_HandleMouseDown(&item->buttons[0], event);
		WSCtlLine_HandleMouseDown(&item->line, event);
		break;
	case SFLBOOTFIELD:
		WSCtlLine_HandleMouseDown(&item->line, event);
		break;
#else
	case SFLBOOTPATH:
	case SFLBOOTFIELD:
		WSCtlLine_HandleMouseDown(&item->line, event);
		break;
#endif
	case SFLBOOTCOMBO:
		WSCtlComboBox_HandleMouseDown(&item->combo, WSMouseEvent_GetX(event), WSMouseEvent_GetY(event));
		break;
	case SFLBOOTLIST:
		WSCtlList_HandleMouseDown(&item->list, event);
		break;
	default:
		break;
	}
	(void)index;
}

WSMOUSEEVENT_CALLBACK(SflBootOnMouseDown)
{
	WSMOUSEEVENT event;
	SFLBOOTGUI *pThis;
	int index;
	WSMouseEvent_Construct(&event);
	pThis = (SFLBOOTGUI *)WSMouseEvent_GetWindowCon(&event);
	WSCtlTab_HandleMouseDown(&pThis->tabs, &event);
#if defined(WSMACCLASSIC)
	WSCtlButton_HandleMouseDown(&pThis->boot, &event);
#endif
	WSCtlScrollPanel_HandleMouseDown(&pThis->panels[pThis->page], &event);
	for (index = 0; index < pThis->count; ++index)
		if (pThis->items[index].page == pThis->page && !pThis->items[index].hidden && pThis->items[index].hascontrol)
			SflBootForwardMouse(&pThis->items[index], &event);
	return WSMOUSEEVENT_HANDLED;
}

WSMOUSEWHEELEVENT_CALLBACK(SflBootOnWheel)
{
	WSMOUSEWHEELEVENT event;
	SFLBOOTGUI *pThis;
	WSMouseWheelEvent_Construct(&event);
	pThis = (SFLBOOTGUI *)WSMouseWheelEvent_GetWindowCon(&event);
	WSCtlScrollPanel_HandleMouseWheel(&pThis->panels[pThis->page], &event);
	return WSMOUSEWHEELEVENT_HANDLED;
}
#endif

BEGIN_WINDOW_MESSAGES(SflBootMessages)
	IMPLEMENT_CLOSE_MESSAGE(SflBootOnClose)
	IMPLEMENT_SIZECHANGED_MESSAGE(SflBootOnSize)
	IMPLEMENT_CTLTAB_CHANGED_MESSAGE(SflBootOnTab, SFLBOOTIDTAB)
	IMPLEMENT_CTLBUTTON_CLICKED_MESSAGE(SflBootOnBoot, SFLBOOTIDBOOT)
	IMPLEMENT_CTLBUTTON_CLICKED_MESSAGE(SflBootOnBrowseRom, SFLBOOTIDROM)
	IMPLEMENT_CTLBUTTON_CLICKED_MESSAGE(SflBootOnBrowseFolder, SFLBOOTIDFOLDER)
	IMPLEMENT_CTLBUTTON_CLICKED_MESSAGE(SflBootOnDiskAdd, SFLBOOTIDDISKADD)
	IMPLEMENT_CTLBUTTON_CLICKED_MESSAGE(SflBootOnDiskCreate, SFLBOOTIDDISKCREATE)
	IMPLEMENT_CTLBUTTON_CLICKED_MESSAGE(SflBootOnDiskRemove, SFLBOOTIDDISKREMOVE)
	IMPLEMENT_CTLBUTTON_CLICKED_MESSAGE(SflBootOnRedirAdd, SFLBOOTIDREDIRADD)
	IMPLEMENT_CTLBUTTON_CLICKED_MESSAGE(SflBootOnRedirRemove, SFLBOOTIDREDIRREMOVE)
	IMPLEMENT_CTLBUTTON_CLICKED_MESSAGE(SflBootOnInstructions, SFLBOOTIDINSTRUCTIONS)
	IMPLEMENT_CTLBUTTON_CLICKED_MESSAGE(SflBootOnLicenses, SFLBOOTIDLICENSES)
	IMPLEMENT_CTLBUTTON_CLICKED_MESSAGE(SflBootOnSetupClose, SFLBOOTIDSETUPCLOSE)
	IMPLEMENT_CTLBUTTON_CLICKED_MESSAGE(SflBootOnNetworkClose, SFLBOOTIDNETWORKCLOSE)
	IMPLEMENT_CTLBUTTON_CLICKED_MESSAGE(SflBootOnInstructions, SFLBOOTIDSHEETREAD)
	IMPLEMENT_CTLBUTTON_CLICKED_MESSAGE(SflBootOnInstructions, SFLBOOTIDSETUPGUIDE)
	IMPLEMENT_CTLBUTTON_CLICKED_MESSAGE(SflBootOnDiskMount, SFLBOOTIDDISKMOUNT)
	IMPLEMENT_CTLBUTTON_CLICKED_MESSAGE(SflBootOnDiskType, SFLBOOTIDDISKTYPE)
	IMPLEMENT_CTLBUTTON_CLICKED_MESSAGE(SflBootOnDiskReload, SFLBOOTIDDISKRELOAD)
	IMPLEMENT_CTLBUTTON_CLICKED_MESSAGE(SflBootOnDiskOpen, SFLBOOTIDDISKOPEN)
	IMPLEMENT_CTLBUTTON_CLICKED_MESSAGE(SflBootOnResolutionAdd, SFLBOOTIDRESADD)
	IMPLEMENT_CTLBUTTON_CLICKED_MESSAGE(SflBootOnResolutionEdit, SFLBOOTIDRESEDIT)
	IMPLEMENT_CTLBUTTON_CLICKED_MESSAGE(SflBootOnResolutionRemove, SFLBOOTIDRESREMOVE)
	IMPLEMENT_CTLBUTTON_CLICKED_MESSAGE(SflBootOnBootstrap, SFLBOOTIDBOOTSTRAP)
	IMPLEMENT_CTLBUTTON_CLICKED_MESSAGE(SflBootOnBootstrap, SFLBOOTIDBOOTSTRAP2)
	IMPLEMENT_CTLBUTTON_CLICKED_MESSAGE(SflBootOnBootstrapDone, SFLBOOTIDBOOTSTRAPDONE)
	IMPLEMENT_CTLBUTTON_CLICKED_MESSAGE(SflBootOnCompatibility, SFLBOOTIDCOMPATIBILITY)
	IMPLEMENT_CTLBUTTON_CLICKED_MESSAGE(SflBootOnCompatibility, SFLBOOTIDCOMPATIBILITY2)
	IMPLEMENT_CTLBUTTON_CLICKED_MESSAGE(SflBootOnRelative, SFLBOOTIDRELATIVE)
	IMPLEMENT_CTLBUTTON_CLICKED_MESSAGE(SflBootOnRelative, SFLBOOTIDRELATIVE2)
	IMPLEMENT_CTLRADIOBUTTON_TOGGLED_MESSAGE(SflBootRadio0, SFLBOOTIDRADIOBASE + 0)
	IMPLEMENT_CTLRADIOBUTTON_TOGGLED_MESSAGE(SflBootRadio1, SFLBOOTIDRADIOBASE + 1)
	IMPLEMENT_CTLRADIOBUTTON_TOGGLED_MESSAGE(SflBootRadio2, SFLBOOTIDRADIOBASE + 2)
	IMPLEMENT_CTLRADIOBUTTON_TOGGLED_MESSAGE(SflBootRadio3, SFLBOOTIDRADIOBASE + 3)
	IMPLEMENT_CTLRADIOBUTTON_TOGGLED_MESSAGE(SflBootRadio4, SFLBOOTIDRADIOBASE + 4)
	IMPLEMENT_CTLRADIOBUTTON_TOGGLED_MESSAGE(SflBootRadio5, SFLBOOTIDRADIOBASE + 5)
	IMPLEMENT_CTLRADIOBUTTON_TOGGLED_MESSAGE(SflBootRadio6, SFLBOOTIDRADIOBASE + 6)
	IMPLEMENT_CTLRADIOBUTTON_TOGGLED_MESSAGE(SflBootRadio7, SFLBOOTIDRADIOBASE + 7)
	IMPLEMENT_CTLRADIOBUTTON_TOGGLED_MESSAGE(SflBootRadio8, SFLBOOTIDRADIOBASE + 8)
	IMPLEMENT_CTLRADIOBUTTON_TOGGLED_MESSAGE(SflBootRadio9, SFLBOOTIDRADIOBASE + 9)
	IMPLEMENT_CTLRADIOBUTTON_TOGGLED_MESSAGE(SflBootRadio10, SFLBOOTIDRADIOBASE + 10)
	IMPLEMENT_CTLRADIOBUTTON_TOGGLED_MESSAGE(SflBootRadio11, SFLBOOTIDRADIOBASE + 11)
	IMPLEMENT_CTLRADIOBUTTON_TOGGLED_MESSAGE(SflBootRadio12, SFLBOOTIDRADIOBASE + 12)
	IMPLEMENT_CTLRADIOBUTTON_TOGGLED_MESSAGE(SflBootRadio13, SFLBOOTIDRADIOBASE + 13)
	IMPLEMENT_CTLRADIOBUTTON_TOGGLED_MESSAGE(SflBootRadio14, SFLBOOTIDRADIOBASE + 14)
	IMPLEMENT_CTLRADIOBUTTON_TOGGLED_MESSAGE(SflBootRadio15, SFLBOOTIDRADIOBASE + 15)
	IMPLEMENT_CTLCHECKBOX_CLICKED_MESSAGE(SflBootOnCheck, 0)
#if defined(WSWINGUI)
	IMPLEMENT_CTLLABEL_COLOR_MESSAGE(SflBootOnLabelColor)
#endif
#if defined(WSMACCLASSICORCARBON)
	IMPLEMENT_PAINT_MESSAGE(SflBootOnPaint)
	IMPLEMENT_MOUSEDOWN_MESSAGE(SflBootOnMouseDown)
	IMPLEMENT_MOUSEWHEEL_MESSAGE(SflBootOnWheel)
#endif
#if defined(SFL_USEDARKMODE)
	END_WINDOW_MESSAGES_DARKMODE(SflBootMessages)
#else
	END_WINDOW_MESSAGES(SflBootMessages)
#endif /* SFL_USEDARKMODE */

static void SflBootLoadDefaultResolutions(SFLBOOTITEM *item)
{
	if (WSCtlList_GetItemCount(&item->list) != 0)
		return;
	SflBootAppendList(item, "640x480");
	SflBootAppendList(item, "800x600");
	SflBootAppendList(item, "1024x768");
	SflBootAppendList(item, "1152x870");
	SflBootAppendList(item, "1280x1024");
	SflBootAppendList(item, "1600x1200");
}

static void SflBootBuildGeneral(SFLBOOTGUI *pThis)
{
	SFLBOOTITEM *item;
	int dismissed;
	dismissed = SflEmulatorPrefBool("setupdismissed");
	item = SflBootAddHelp(pThis, SFLBOOTPAGEGENERAL, "setupbanner", "Read initial setup instructions if you want to install Classic Mac OS from scratch. Contains crucial tips on how to not get stuck in installation progress and get audio working, after installation.\n\nThe instructions can still be accessed from the Resources section at the bottom of this tab, after dismissal.");
	item->type = SFLBOOTBANNER;
	item->hidden = dismissed;
	SflBootAddButton(pThis, item, "X", SflBootOnSetupClose, SFLBOOTIDSETUPCLOSE);
	item = SflBootAddItem(pThis, SFLBOOTPAGEGENERAL, SFLBOOTBUTTONS, "setupread", NULL);
	item->hidden = dismissed;
	SflBootAddButton(pThis, item, "Read instructions", SflBootOnInstructions, SFLBOOTIDSHEETREAD);
	if (SflBootIsSheepShaver()) {
		SflBootAddHelp(pThis, SFLBOOTPAGEGENERAL, "bootstraptext", SflBootBootstrapText());
		item = SflBootAddItem(pThis, SFLBOOTPAGEGENERAL, SFLBOOTBUTTONS, "bootstrapbuttons", NULL);
		SflBootAddButton(pThis, item, "Select install disc", SflBootOnBootstrap, SFLBOOTIDBOOTSTRAP);
		SflBootAddButton(pThis, item, "Compatibility list", SflBootOnCompatibility, SFLBOOTIDCOMPATIBILITY);
		item = SflBootAddItem(pThis, SFLBOOTPAGEGENERAL, SFLBOOTBUTTONS, "bootstrapdone", NULL);
		SflBootAddButton(pThis, item, "Done", SflBootOnBootstrapDone, SFLBOOTIDBOOTSTRAPDONE);
		item = SflBootAddHelp(pThis, SFLBOOTPAGEGENERAL, "bootstraperror", "You need to bootstrap the emulator or select a valid ROM file");
		item->hidden = 1;
	}
	item = SflBootAddHeading(pThis, SFLBOOTPAGEGENERAL, "Welcome!");
	item->pref = "welcomeheading";
	SflBootAddHelp(pThis, SFLBOOTPAGEGENERAL, "welcometext", "These settings appear each time the emulator starts. To go straight to Mac OS, turn on Skip startup screen in the Advanced tab.");
	SflBootAddHeading(pThis, SFLBOOTPAGEGENERAL, "ROM");
	SflBootAddPath(pThis, SFLBOOTPAGEGENERAL, "rom", "Macintosh ROM file", 0, SflBootOnBrowseRom, SFLBOOTIDROM);
	SflBootAddHelp(pThis, SFLBOOTPAGEGENERAL, "romstatus", "");
	SflBootAddHeading(pThis, SFLBOOTPAGEGENERAL, "Disks");
	item = SflBootAddItem(pThis, SFLBOOTPAGEGENERAL, SFLBOOTBUTTONS, NULL, NULL);
	SflBootAddButton(pThis, item, "Import...", SflBootOnDiskAdd, SFLBOOTIDDISKADD);
	SflBootAddButton(pThis, item, "Create...", SflBootOnDiskCreate, SFLBOOTIDDISKCREATE);
	SflBootAddButton(pThis, item, "Reload", SflBootOnDiskReload, SFLBOOTIDDISKRELOAD);
	SflBootAddButton(pThis, item, "Open folder", SflBootOnDiskOpen, SFLBOOTIDDISKOPEN);
	SflBootAddDiskList(pThis, SFLBOOTPAGEGENERAL);
	item = SflBootAddItem(pThis, SFLBOOTPAGEGENERAL, SFLBOOTBUTTONS, NULL, NULL);
	SflBootAddButton(pThis, item, "Mount / Unmount", SflBootOnDiskMount, SFLBOOTIDDISKMOUNT);
	SflBootAddButton(pThis, item, "Hard disk / CD", SflBootOnDiskType, SFLBOOTIDDISKTYPE);
	SflBootAddButton(pThis, item, "Remove", SflBootOnDiskRemove, SFLBOOTIDDISKREMOVE);
	SflBootAddHelp(pThis, SFLBOOTPAGEGENERAL, "diskempty", "No disk files found.\nClick Create or Import to add a disk file. Alternatively, add a file manually to the share folder and click Reload. Click Open folder to open the share folder.");
	item = SflBootAddHelp(pThis, SFLBOOTPAGEGENERAL, "diskerror", "Must select to mount at least one disk file");
	item->hidden = 1;
	SflBootAddHeading(pThis, SFLBOOTPAGEGENERAL, "Shared folder");
	SflBootAddCheck(pThis, SFLBOOTPAGEGENERAL, "enableextfs", "Enable shared folder", 0);
	SflBootAddPath(pThis, SFLBOOTPAGEGENERAL, "extfs", "Shared folder", 1, SflBootOnBrowseFolder, SFLBOOTIDFOLDER);
	SflBootAddHeading(pThis, SFLBOOTPAGEGENERAL, "Right click");
	SflBootAddRadio(pThis, SFLBOOTPAGEGENERAL, "rightclick", NULL, "control + click=control|command + click=command", SFLBOOTKINDSTRING);
	SflBootAddHelp(pThis, SFLBOOTPAGEGENERAL, NULL, "Controls what a right mouse click does.");
	SflBootAddHeading(pThis, SFLBOOTPAGEGENERAL, "Audio");
	SflBootAddCheck(pThis, SFLBOOTPAGEGENERAL, "nosound", "Audio enabled", 1);
	SflBootAddHelp(pThis, SFLBOOTPAGEGENERAL, NULL, "Having trouble getting audio to work? Read the setup guide.");
	item = SflBootAddItem(pThis, SFLBOOTPAGEGENERAL, SFLBOOTBUTTONS, NULL, NULL);
	SflBootAddButton(pThis, item, "Setup guide", SflBootOnInstructions, SFLBOOTIDSETUPGUIDE);
	if (SflBootIsSheepShaver()) {
		SflBootAddHeading(pThis, SFLBOOTPAGEGENERAL, "Mac OS X");
		SflBootAddCombo(pThis, SFLBOOTPAGEGENERAL, "osxboot", "Boot device", "Automatic=|Hard disk=disk|CD-ROM=cdrom", SFLBOOTKINDSTRING);
	}
	SflBootAddHeading(pThis, SFLBOOTPAGEGENERAL, "Resources");
	item = SflBootAddItem(pThis, SFLBOOTPAGEGENERAL, SFLBOOTBUTTONS, NULL, NULL);
	SflBootAddButton(pThis, item, "Setup instructions", SflBootOnInstructions, SFLBOOTIDINSTRUCTIONS);
	if (SflBootIsSheepShaver())
		SflBootAddButton(pThis, item, "Compatibility list", SflBootOnCompatibility, SFLBOOTIDCOMPATIBILITY2);
	SflBootAddButton(pThis, item, "Relative mouse", SflBootOnRelative, SFLBOOTIDRELATIVE);
	SflBootAddButton(pThis, item, "Licenses", SflBootOnLicenses, SFLBOOTIDLICENSES);
}

static void SflBootBuildGraphics(SFLBOOTGUI *pThis)
{
	SFLBOOTITEM *item;
	SflBootAddHeading(pThis, SFLBOOTPAGEGRAPHICS, "Display");
	SflBootAddRadio(pThis, SFLBOOTPAGEGRAPHICS, "screenmode", NULL, "Windowed=win|Full screen=dga", SFLBOOTKINDSPECIAL);
	SflBootAddHelp(pThis, SFLBOOTPAGEGRAPHICS, NULL, "Choose whether emulation runs in a window or full screen.");
	SflBootAddCombo(pThis, SFLBOOTPAGEGRAPHICS, "screensize", "Startup resolution", "Desktop=0/0|640 x 480=640/480|800 x 600=800/600|1024 x 768=1024/768|1280 x 720=1280/720|1920 x 1080=1920/1080", SFLBOOTKINDSPECIAL);
	if (SflBootIsSheepShaver()) {
		SflBootAddHeading(pThis, SFLBOOTPAGEGRAPHICS, "Monitor resolutions");
		SflBootAddList(pThis, SFLBOOTPAGEGRAPHICS, "resolution", "Width x height");
		item = SflBootFind(pThis, "resolution");
		if (item->hascontrol)
			SflBootLoadDefaultResolutions(item);
		item = SflBootAddItem(pThis, SFLBOOTPAGEGRAPHICS, SFLBOOTBUTTONS, NULL, NULL);
		SflBootAddButton(pThis, item, "Add...", SflBootOnResolutionAdd, SFLBOOTIDRESADD);
		SflBootAddButton(pThis, item, "Edit...", SflBootOnResolutionEdit, SFLBOOTIDRESEDIT);
		SflBootAddButton(pThis, item, "Remove", SflBootOnResolutionRemove, SFLBOOTIDRESREMOVE);
		SflBootAddHelp(pThis, SFLBOOTPAGEGRAPHICS, NULL, "Resolutions made available to Mac OS. Can be edited. Changing the active resolution is still done with the Monitors app, inside Mac OS. Mac OS allows a maximum of 10 monitor resolutions.");
		SflBootAddHeading(pThis, SFLBOOTPAGEGRAPHICS, "Graphics acceleration (experimental)");
		SflBootAddCheck(pThis, SFLBOOTPAGEGRAPHICS, "nqdaccel", "QuickDraw 2D", 0);
		SflBootAddCheck(pThis, SFLBOOTPAGEGRAPHICS, "raveaccel", "QuickDraw 3D + RAVE", 0);
		SflBootAddCheck(pThis, SFLBOOTPAGEGRAPHICS, "glaccel", "OpenGL", 0);
		SflBootAddCheck(pThis, SFLBOOTPAGEGRAPHICS, "dspaccel", "DrawSprocket", 0);
		SflBootAddHelp(pThis, SFLBOOTPAGEGRAPHICS, NULL, "Each accelerator is independent - mix and match per app to find what runs best.");
	}
	SflBootAddHeading(pThis, SFLBOOTPAGEGRAPHICS, "Rendering filter");
	SflBootAddRadio(pThis, SFLBOOTPAGEGRAPHICS, "scale_nearest", NULL, "Bilinear=0|Nearest neighbor=1", SFLBOOTKINDBOOL);
	SflBootAddHelp(pThis, SFLBOOTPAGEGRAPHICS, NULL, "Bilinear produces a smooth image through interpolation. Nearest neighbor gives a sharp, \"retro\" pixelated look. Takes effect on next resolution change or restart.");
	SflBootAddCheck(pThis, SFLBOOTPAGEGRAPHICS, "scale_integer", "Integer scaling", 0);
	SflBootAddHeading(pThis, SFLBOOTPAGEGRAPHICS, "Gamma ramp");
	SflBootAddRadio(pThis, SFLBOOTPAGEGRAPHICS, "gammaramp", NULL, "Linear=off|OS defined=on|Full screen only=fullscreen", SFLBOOTKINDSTRING);
	SflBootAddHelp(pThis, SFLBOOTPAGEGRAPHICS, NULL, "Linear gamma ramp generally produces a less color distorted image. Higher screen brightness can compensate for a potentially dark picture and, in some instances, produce a higher color dynamic. Takes effect on next resolution change or restart.");
	SflBootAddHeading(pThis, SFLBOOTPAGEGRAPHICS, "Frame rate setting");
	SflBootAddCombo(pThis, SFLBOOTPAGEGRAPHICS, "frameskip", "Refresh", "Every frame=1|Every 2nd frame=2|Every 4th frame=4|Every 6th frame=6|Every 8th frame=8", SFLBOOTKINDINT);
}

static void SflBootBuildNetwork(SFLBOOTGUI *pThis)
{
	SFLBOOTITEM *item;
	item = SflBootAddHelp(pThis, SFLBOOTPAGENETWORK, "networkbanner", "Network requires Mac OS 9.0 - 9.0.4");
	item->type = SFLBOOTBANNER;
	SflBootAddButton(pThis, item, "X", SflBootOnNetworkClose, SFLBOOTIDNETWORKCLOSE);
	SflBootAddHeading(pThis, SFLBOOTPAGENETWORK, "Mode");
	SflBootAddRadio(pThis, SFLBOOTPAGENETWORK, "network", NULL, "Off=off|Internet=slirp|UDP tunnel=udptunnel", SFLBOOTKINDSPECIAL);
	SflBootAddHelp(pThis, SFLBOOTPAGENETWORK, NULL, "Internet gives access to the internet, using SLiRP, but without the ability to host. UDP tunnel connects to other emulators on the same network, but without access to the internet.");
	item = SflBootAddHelp(pThis, SFLBOOTPAGENETWORK, "networkwarning", "It might take a while before Mac OS assigns the new IP address. You can force this by restarting or changing configuration inside the TCP/IP app.");
	item->hidden = 1;
	SflBootAddField(pThis, SFLBOOTPAGENETWORK, "udpport", "UDP tunnel port");
	SflBootAddHeading(pThis, SFLBOOTPAGENETWORK, "Port forwarding");
	SflBootAddList(pThis, SFLBOOTPAGENETWORK, "redir", "Rules");
	item = SflBootAddItem(pThis, SFLBOOTPAGENETWORK, SFLBOOTBUTTONS, NULL, NULL);
	SflBootAddButton(pThis, item, "Add...", SflBootOnRedirAdd, SFLBOOTIDREDIRADD);
	SflBootAddButton(pThis, item, "Remove", SflBootOnRedirRemove, SFLBOOTIDREDIRREMOVE);
}

static void SflBootBuildAdvanced(SFLBOOTGUI *pThis)
{
	SFLBOOTITEM *item;
	SflBootAddHeading(pThis, SFLBOOTPAGEADVANCED, "RAM setting");
	SflBootAddCombo(pThis, SFLBOOTPAGEADVANCED, "ramsize", "RAM", "16 MB=16777216|32 MB=33554432|64 MB=67108864|128 MB=134217728|256 MB=268435456|512 MB=536870912|768 MB=805306368|1 GB=1073741824", SFLBOOTKINDINT);
	SflBootAddHeading(pThis, SFLBOOTPAGEADVANCED, "Clipboard sharing");
	SflBootAddCheckHandler(pThis, SFLBOOTPAGEADVANCED, "clipsharing", "Enable clipboard sharing", 0, SflBootOnCheck, SFLBOOTIDCLIPBOARD);
	SflBootAddHelp(pThis, SFLBOOTPAGEADVANCED, "clipinfo", "");
	SflBootAddHeading(pThis, SFLBOOTPAGEADVANCED, "Relative mouse mode");
	SflBootAddRadio(pThis, SFLBOOTPAGEADVANCED, "relmouse", NULL, "Manual=manual|Automatic=auto|Always on=always", SFLBOOTKINDSTRING);
	SflBootAddHelp(pThis, SFLBOOTPAGEADVANCED, "relinfo", "");
	item = SflBootAddItem(pThis, SFLBOOTPAGEADVANCED, SFLBOOTBUTTONS, NULL, NULL);
	SflBootAddButton(pThis, item, "Read more", SflBootOnRelative, SFLBOOTIDRELATIVE2);
	SflBootAddCheck(pThis, SFLBOOTPAGEADVANCED, "init_grab", "Boot with relative mouse mode on", 0);
	SflBootAddHeading(pThis, SFLBOOTPAGEADVANCED, "Performance metrics");
	SflBootAddCheck(pThis, SFLBOOTPAGEADVANCED, "showfps", "Show FPS counter", 0);
	SflBootAddCheck(pThis, SFLBOOTPAGEADVANCED, "shownetrate", "Show network transfer rate", 0);
	SflBootAddHeading(pThis, SFLBOOTPAGEADVANCED, "UI options");
	SflBootAddCheck(pThis, SFLBOOTPAGEADVANCED, "skiposxstartup", "Skip startup screen", 0);
	SflBootAddHelp(pThis, SFLBOOTPAGEADVANCED, NULL, "Start the emulator with --settings to show this window again.");
	SflBootAddCheck(pThis, SFLBOOTPAGEADVANCED, "notifyip", "Report IP address assignment", 0);
	SflBootAddCheck(pThis, SFLBOOTPAGEADVANCED, "notifyclip", "Notify of clipboard sharing events", 0);
	SflBootAddHeading(pThis, SFLBOOTPAGEADVANCED, "CPU emulation");
	if (SflBootIsSheepShaver()) {
		SflBootAddCheck(pThis, SFLBOOTPAGEADVANCED, "ignoreillegal", "Ignore illegal CPU instructions", 0);
		SflBootAddCheck(pThis, SFLBOOTPAGEADVANCED, "altivec", "AltiVec", 0);
		SflBootAddHelp(pThis, SFLBOOTPAGEADVANCED, NULL, "Experimental. Advertises a G4 processor with AltiVec so vector-aware software can use faster code paths. Toggle this off (reverting to a G3) if you run into conflicts with graphics acceleration.");
	}
	SflBootAddCheck(pThis, SFLBOOTPAGEADVANCED, "jit", "JIT compiler", 0);
	SflBootAddHelp(pThis, SFLBOOTPAGEADVANCED, NULL, "Experimental. Translates guest code into native code as it runs, for a large CPU speedup. Takes effect after the app is fully restarted.");
	if (SflBootIsSheepShaver()) {
		SflBootAddCheck(pThis, SFLBOOTPAGEADVANCED, "jit68k", "68k JIT compiler", 0);
		SflBootAddCheck(pThis, SFLBOOTPAGEADVANCED, "osxjit", "JIT compiler (Mac OS X)", 0);
		item = SflBootAddHeading(pThis, SFLBOOTPAGEADVANCED, "Bootstrap");
		item->pref = "advbootstrapheading";
		SflBootAddHelp(pThis, SFLBOOTPAGEADVANCED, "advbootstraptext", "Select a Mac OS install disc file if you want to redo bootstrapping with another install disc.");
		item = SflBootAddItem(pThis, SFLBOOTPAGEADVANCED, SFLBOOTBUTTONS, "advbootstrapbuttons", NULL);
		SflBootAddButton(pThis, item, "Select install disc", SflBootOnBootstrap, SFLBOOTIDBOOTSTRAP2);
	}
}

static int SflBootCreateWindow(SFLBOOTGUI *pThis)
{
	WSRECT bounds;
	int index;
	if (!WSWndClass_Create(&pThis->windowclass, pThis->app, NULL, SflBootMessages, "SflBootPreferences"))
		return 0;

	WSRect_Construct(&bounds, WSDEFAULTPOSITION, WSDEFAULTPOSITION, SFLBOOTPX(SFLBOOTWIDTH), SFLBOOTPX(SFLBOOTHEIGHT));
	if (!WSWindow_Create(&pThis->window, pThis->app, &bounds, "PocketShaver", NULL, &pThis->windowclass)) {
		WSWndClass_Destroy(&pThis->windowclass, pThis->app);
		return 0;
	}
	WSWindow_SetCon(&pThis->window, pThis);
	WSWindow_SetSize(&pThis->window, SFLBOOTPX(SFLBOOTWIDTH), SFLBOOTPX(SFLBOOTHEIGHT));
	WSRect_Construct(&bounds, 0, 0, SFLBOOTWIDTH, SFLBOOTHEIGHT);
	for (index = 0; index < SFLBOOTPAGES; ++index) {
		if (!WSCtlScrollPanel_Create(&pThis->panels[index], &pThis->window, &bounds, (size_t)pThis->controlid++))
			pThis->failed = 1;
		else
			WSCtlScrollPanel_SetScrollCallback(&pThis->panels[index], SflBootScrollCallback, pThis);
	}
	WSRect_Construct(&bounds, 0, 0, SFLBOOTWIDTH, SFLBOOTTABHEIGHT);
	if (!WSCtlTab_Create(&pThis->tabs, &pThis->window, &bounds, SflBootOnTab, SFLBOOTIDTAB)) {
		pThis->failed = 1;
		return 1;
	}
	WSCtlTab_Insert(&pThis->tabs, SFLBOOTPAGEGENERAL, "General");
	WSCtlTab_Insert(&pThis->tabs, SFLBOOTPAGEGRAPHICS, "Graphics");
	WSCtlTab_Insert(&pThis->tabs, SFLBOOTPAGENETWORK, "Network");
	WSCtlTab_Insert(&pThis->tabs, SFLBOOTPAGEADVANCED, "Advanced");
	if (!WSCtlButton_Create(&pThis->boot, &pThis->window, &bounds, "Boot", SflBootOnBoot, SFLBOOTIDBOOT))
		pThis->failed = 1;
	return 1;
}

static void SflBootDestroyItem(SFLBOOTITEM *item)
{
	int index;
	for (index = 0; index < item->buttoncount; ++index)
		WSCtlButton_Destroy(&item->buttons[index]);
	if (item->hascontrol) {
		switch (item->type) {
		case SFLBOOTCHECK:
			WSCtlCheckBox_Destroy(&item->check);
			break;
		case SFLBOOTPATH:
		case SFLBOOTFIELD:
			WSCtlLine_Destroy(&item->line);
			break;
		case SFLBOOTCOMBO:
			WSCtlComboBox_Destroy(&item->combo);
			break;
		case SFLBOOTLIST:
			WSCtlList_Destroy(&item->list);
			break;
		case SFLBOOTRADIO:
			for (index = 0; index < item->optioncount; ++index)
				WSCtlRadioButton_Destroy(&item->radios[index]);
			break;
		default:
			break;
		}
	}
	if (item->haslabel)
		WSCtlStyledLabel_Destroy(&item->label);
	if (item->wrapped != NULL)
		WSFREE(item->wrapped);
}

static void SflBootInitScale(SFLBOOTGUI *pThis)
{
#if defined(WSWINGUI)
	guipos_t x;
	guipos_t y;
	WSDisplayGetDPI(&x, &y);
	pThis->dpi = (int)y;
	WSDisplayGetBaseDPI(&x, &y);
	pThis->basedpi = (int)y;
#else
	pThis->dpi = 1;
	pThis->basedpi = 1;
#endif
}

static void SflBootDestroyWindow(SFLBOOTGUI *pThis)
{
	int index;
	if (pThis->dropcreated)
		WSDnD_Destroy(&pThis->drop);
	for (index = pThis->count - 1; index >= 0; --index)
		SflBootDestroyItem(&pThis->items[index]);
	for (index = 0; index < pThis->diskcount; ++index)
		WSFREE(pThis->disks[index].path);
	WSCtlButton_Destroy(&pThis->boot);
	WSCtlTab_Destroy(&pThis->tabs);
	for (index = 0; index < SFLBOOTPAGES; ++index)
		WSCtlScrollPanel_Destroy(&pThis->panels[index]);
	if (pThis->fontscreated) {
		WSFont_Destroy(&pThis->headingfont);
		WSFont_Destroy(&pThis->bodyfont);
	}
	WSWindow_Destroy(&pThis->window);
	WSWndClass_Destroy(&pThis->windowclass, pThis->app);
}

int SflBootGuiRun(WSAPP *app)
{
	SFLBOOTGUI *pThis;
	WSEVENTLOOP *loop;
	int accepted;
	pThis = (SFLBOOTGUI *)WSALLOC1(SFLBOOTGUI);
	if (pThis == NULL)
		return 0;
	memset(pThis, 0, sizeof(*pThis));
	pThis->app = app;
	pThis->controlid = SFLBOOTIDCONTROLBASE;
	SflBootInitScale(pThis);
	loop = &app->mEventLoop;
	accepted = 0;
	#if defined(SFL_USEDARKMODE)
		WSDarkMode_Create(&pThis->darkmode);
	#endif /* SFL_USEDARKMODE */
	if (SflBootCreateWindow(pThis)) {
		pThis->fontscreated = SflBootCreateFonts(&pThis->headingfont, &pThis->bodyfont, pThis->dpi, pThis->basedpi);
		SflBootBuildGeneral(pThis);
		SflBootBuildGraphics(pThis);
		SflBootBuildNetwork(pThis);
		SflBootBuildAdvanced(pThis);
		if (!pThis->failed) {
			pThis->dropcreated = WSDnD_Create(&pThis->drop, &pThis->window, SflBootOnDrop);
			SflBootDiskRender(pThis);
			SflBootUpdateRomStatus(pThis);
			WSWindow_Show(&pThis->window);
			pThis->ready = 1;
			SflBootUpdatePortField(pThis);
			SflBootRefreshDynamic(pThis);
			while (!pThis->finished) {
				WSEventLoop_Process(loop);
				WSThreadSleep(SFLBOOTIDLEMS);
			}
			accepted = pThis->accepted;
		}
		SflBootDestroyWindow(pThis);
	}
	#if defined(SFL_USEDARKMODE)
		WSDarkMode_Destroy(&pThis->darkmode);
	#endif /* SFL_USEDARKMODE */
	WSFREE(pThis);
	return accepted;
}
