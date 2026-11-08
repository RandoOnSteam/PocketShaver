#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gui.h"
#include "guictl.h"
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
#define SFLBOOTPATHSIZE 512

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
#define SFLBOOTTEXTLINE 18
#define SFLBOOTCHARWIDTH 7
#define SFLBOOTLABELWIDTH 200
#define SFLBOOTFIELDWIDTH 120
#define SFLBOOTCOMBOWIDTH 240
#define SFLBOOTBROWSEWIDTH 90
#define SFLBOOTBUTTONWIDTH 130
#define SFLBOOTCLOSEWIDTH 26
#define SFLBOOTLISTHEIGHT 130
#define SFLBOOTLISTCOLUMN 620
#define SFLBOOTHELPGREY 96
#define SFLBOOTMINROMTEXT 8
#define SFLBOOTMEGABYTE 1048576L
#define SFLBOOTIDLEMS 10
#define SFLBOOTDISKSIZES 6
#define SFLBOOTDISKBASEMB 32
#define SFLBOOTDISKDEFAULT 3

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
#define SFLBOOTIDCD (SFLBOOTIDBUTTONBASE + 1)
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
	SFLBOOTITEM *radioitems[SFLBOOTMAXRADIOS];
	int radiooptions[SFLBOOTMAXRADIOS];
	int count;
	int radiocount;
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
	WSCTLLABEL label;
	WSCTLLINE line;
	WSCTLCOMBOBOX sizes;
	WSCTLBUTTON accept;
	WSCTLBUTTON cancel;
	char *value;
	int selection;
	int accepted;
	int closed;
} SFLBOOTFORM;

static void SflBootLayout(SFLBOOTGUI *pThis);

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

static void SflBootAddHeading(SFLBOOTGUI *pThis, int page, const char *text)
{
	SFLBOOTITEM *item;
	item = SflBootAddItem(pThis, page, SFLBOOTHEADING, NULL, text);
	SflBootAddLabel(pThis, item, text);
	if (item->haslabel) {
		WSCtlStyledLabel_SetBold(&item->label);
		if (pThis->fontscreated)
			WSCtlStyledLabel_SetFont(&item->label, &pThis->headingfont);
	}
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

static void SflBootAddCheck(SFLBOOTGUI *pThis, int page, const char *pref, const char *caption, int invert)
{
	SFLBOOTITEM *item;
	WSRECT bounds;
	item = SflBootAddItem(pThis, page, SFLBOOTCHECK, pref, caption);
	item->kind = SFLBOOTKINDBOOL;
	item->invert = invert;
	WSRect_Construct(&bounds, 0, 0, 100, SFLBOOTROWHEIGHT);
	if (!WSCtlCheckBox_Create(&item->check, &pThis->window, &bounds, caption, NULL, (size_t)pThis->controlid++)) {
		pThis->failed = 1;
		return;
	}
	item->hascontrol = 1;
	WSCtlCheckBox_SetChecked(&item->check, (SflEmulatorPrefBool(pref) != 0) != invert);
	WSCtlScrollPanel_Add(&pThis->panels[page], WSCtl_GetHandle((WSCTL *)&item->check));
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
	if (strcmp(item->pref, "network") == 0)
		SflBootUpdatePortField(pThis);
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

static void SflBootAddList(SFLBOOTGUI *pThis, int page, const char *pref, const char *caption)
{
	SFLBOOTITEM *item;
	WSRECT bounds;
	const char *value;
	int index;
	item = SflBootAddItem(pThis, page, SFLBOOTLIST, pref, caption);
	WSRect_Construct(&bounds, 0, 0, 100, SFLBOOTLISTHEIGHT);
	if (!WSCtlList_Create(&item->list, &pThis->window, &bounds, NULL, NULL, (size_t)pThis->controlid++, WSCTLLIST_STYLE_REPORT | WSCTLLIST_STYLE_FULLROWSELECT)) {
		pThis->failed = 1;
		return;
	}
	item->hascontrol = 1;
	WSCtlList_InsertColumn(&item->list, 0, caption, WSCTLLIST_ALIGN_LEFT, SFLBOOTLISTCOLUMN);
	index = 0;
	while ((value = SflEmulatorPrefString(pref, index++)) != NULL)
		SflBootAppendList(item, value);
	WSCtlScrollPanel_AddList(&pThis->panels[page], &item->list);
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
	WSRect_Construct(&bounds, x, y, width, SFLBOOTROWHEIGHT);
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
	WSRect_Construct(&bounds, x, y, width, SFLBOOTROWHEIGHT);
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
	WSRect_Construct(&bounds, x, y, width, SFLBOOTROWHEIGHT);
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
	WSRect_Construct(&bounds, x, y, SFLBOOTCOMBOWIDTH, SFLBOOTCOMBOHEIGHT);
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
	WSRect_Construct(&bounds, x, y, width, SFLBOOTROWHEIGHT);
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
	WSRect_Construct(&bounds, x, y, width, SFLBOOTLISTHEIGHT);
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
		SflBootPlaceLabel(pThis, item, SFLBOOTMARGIN, y, width, SFLBOOTTEXTLINE + 2);
		y += SFLBOOTTEXTLINE + SFLBOOTROWGAP;
	}
	cellwidth = width / item->optioncount;
	for (index = 0; index < item->optioncount; ++index)
		SflBootPlaceRadio(pThis, item, index, SFLBOOTMARGIN + index * cellwidth, y, cellwidth);
	return y + SFLBOOTROWHEIGHT + SFLBOOTROWGAP;
}

static int SflBootPlaceItem(SFLBOOTGUI *pThis, SFLBOOTITEM *item, int y, int width)
{
	int lines;
	int index;
	int x;
	if (item->hidden) {
		SflBootPlaceLabel(pThis, item, 0, 0, 1, 1);
		if (item->type == SFLBOOTBUTTONS || item->type == SFLBOOTBANNER)
			for (index = 0; index < item->buttoncount; ++index)
				SflBootPlaceButton(pThis, item, index, 0, 0, 1);
		return y;
	}
	item->documenty = y;
	switch (item->type) {
	case SFLBOOTHEADING:
		y += SFLBOOTHEADINGGAP;
		SflBootPlaceLabel(pThis, item, SFLBOOTMARGIN, y, width, SFLBOOTHEADINGHEIGHT);
		return y + SFLBOOTHEADINGHEIGHT;
	case SFLBOOTHELP:
		lines = SflBootWrapText(item, width / SFLBOOTCHARWIDTH);
		SflBootPlaceLabel(pThis, item, SFLBOOTMARGIN, y, width, lines * SFLBOOTTEXTLINE + 2);
		return y + lines * SFLBOOTTEXTLINE + SFLBOOTROWGAP + 2;
	case SFLBOOTBANNER:
		lines = SflBootWrapText(item, (width - SFLBOOTCLOSEWIDTH) / SFLBOOTCHARWIDTH);
		SflBootPlaceLabel(pThis, item, SFLBOOTMARGIN, y, width - SFLBOOTCLOSEWIDTH - SFLBOOTROWGAP, lines * SFLBOOTTEXTLINE + 2);
		SflBootPlaceButton(pThis, item, 0, SFLBOOTMARGIN + width - SFLBOOTCLOSEWIDTH, y, SFLBOOTCLOSEWIDTH);
		return y + lines * SFLBOOTTEXTLINE + SFLBOOTROWGAP + 8;
	case SFLBOOTCHECK:
		SflBootPlaceCheck(pThis, item, SFLBOOTMARGIN, y, width);
		return y + SFLBOOTROWHEIGHT + SFLBOOTROWGAP;
	case SFLBOOTPATH:
		SflBootPlaceLabel(pThis, item, SFLBOOTMARGIN, y, width, SFLBOOTTEXTLINE + 2);
		y += SFLBOOTTEXTLINE + SFLBOOTROWGAP;
		SflBootPlaceLine(pThis, item, SFLBOOTMARGIN, y, width - SFLBOOTBROWSEWIDTH - SFLBOOTROWGAP);
		SflBootPlaceButton(pThis, item, 0, SFLBOOTMARGIN + width - SFLBOOTBROWSEWIDTH, y, SFLBOOTBROWSEWIDTH);
		return y + SFLBOOTROWHEIGHT + SFLBOOTROWGAP;
	case SFLBOOTFIELD:
		SflBootPlaceLabel(pThis, item, SFLBOOTMARGIN, y + 4, SFLBOOTLABELWIDTH, SFLBOOTTEXTLINE + 2);
		SflBootPlaceLine(pThis, item, SFLBOOTMARGIN + SFLBOOTLABELWIDTH, y, SFLBOOTFIELDWIDTH);
		return y + SFLBOOTROWHEIGHT + SFLBOOTROWGAP;
	case SFLBOOTCOMBO:
		SflBootPlaceLabel(pThis, item, SFLBOOTMARGIN, y + 4, SFLBOOTLABELWIDTH, SFLBOOTTEXTLINE + 2);
		SflBootPlaceCombo(pThis, item, SFLBOOTMARGIN + SFLBOOTLABELWIDTH, y);
		return y + SFLBOOTROWHEIGHT + SFLBOOTROWGAP;
	case SFLBOOTRADIO:
		return SflBootPlaceRadios(pThis, item, y, width);
	case SFLBOOTLIST:
		SflBootPlaceList(pThis, item, SFLBOOTMARGIN, y, width);
		return y + SFLBOOTLISTHEIGHT + SFLBOOTROWGAP;
	case SFLBOOTBUTTONS:
		x = SFLBOOTMARGIN;
		for (index = 0; index < item->buttoncount; ++index) {
			SflBootPlaceButton(pThis, item, index, x, y, SFLBOOTBUTTONWIDTH);
			x += SFLBOOTBUTTONWIDTH + SFLBOOTROWGAP;
		}
		return y + SFLBOOTROWHEIGHT + SFLBOOTROWGAP;
	default:
		return y;
	}
}

static void SflBootLayout(SFLBOOTGUI *pThis)
{
	WSRECT client;
	WSRECT bounds;
	int positions[SFLBOOTPAGES];
	int contentwidth[SFLBOOTPAGES];
	int width;
	int height;
	int index;
	SFLBOOTITEM *item;
	if (!pThis->ready)
		return;
	WSWindow_GetClientRect(&pThis->window, &client);
	width = WSRect_GetWidth(&client);
	height = WSRect_GetHeight(&client);
	WSRect_Construct(&bounds, SFLBOOTMARGIN, SFLBOOTMARGIN, width - 2 * SFLBOOTMARGIN, SFLBOOTTABHEIGHT);
	WSCtlTab_Move(&pThis->tabs, &bounds);
	WSRect_Construct(&bounds, SFLBOOTMARGIN, height - SFLBOOTBOOTHEIGHT - SFLBOOTMARGIN, width - 2 * SFLBOOTMARGIN, SFLBOOTBOOTHEIGHT);
	WSCtlButton_Move(&pThis->boot, &bounds);
	WSRect_Construct(&bounds, SFLBOOTMARGIN, 2 * SFLBOOTMARGIN + SFLBOOTTABHEIGHT, width - 2 * SFLBOOTMARGIN, height - 4 * SFLBOOTMARGIN - SFLBOOTTABHEIGHT - SFLBOOTBOOTHEIGHT);
	for (index = 0; index < SFLBOOTPAGES; ++index) {
		WSCtlScrollPanel_Move(&pThis->panels[index], &bounds);
		contentwidth[index] = WSCtlScrollPanel_GetContentWidth(&pThis->panels[index]) - 2 * SFLBOOTMARGIN;
		positions[index] = SFLBOOTMARGIN;
	}
	for (index = 0; index < pThis->count; ++index) {
		item = &pThis->items[index];
		positions[item->page] = SflBootPlaceItem(pThis, item, positions[item->page], contentwidth[item->page]);
	}
	for (index = 0; index < SFLBOOTPAGES; ++index) {
		WSCtlScrollPanel_SetContentHeight(&pThis->panels[index], positions[index] + SFLBOOTMARGIN);
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

static void SflBootApplyItem(SFLBOOTITEM *item)
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
		SflBootApplyList(item);
		break;
	default:
		break;
	}
}

static void SflBootUpdateRomStatus(SFLBOOTGUI *pThis)
{
	SFLBOOTITEM *rom;
	SFLBOOTITEM *status;
	char *path;
	rom = SflBootFind(pThis, "rom");
	status = SflBootFind(pThis, "romstatus");
	path = NULL;
	WSCtlLine_AllocText(&rom->line, &path);
	if (SflEmulatorRomValid(path))
		SflBootRefreshHelp(status, "A valid ROM was found.");
	else
		SflBootRefreshHelp(status, "No valid ROM. Select a Macintosh ROM file.");
	WSFREE(path);
}

static void SflBootBoot(SFLBOOTGUI *pThis)
{
	int index;
	for (index = 0; index < pThis->count; ++index)
		SflBootApplyItem(&pThis->items[index]);
	SflBootUpdateRomStatus(pThis);
	if (!SflBootIsNativeBoot() && !SflEmulatorRomValid(SflEmulatorPrefString("rom", 0))) {
		SflBootLayout(pThis);
		SflBootError(pThis, SflBootFind(pThis, "rom"), "Select a valid Macintosh ROM file before booting.");
		return;
	}
	if (SflEmulatorPrefString("disk", 0) == NULL && SflEmulatorPrefString("cdrom", 0) == NULL) {
		SflBootError(pThis, SflBootFind(pThis, "disk"), "Add at least one disk before booting.");
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
	memset(&sheet, 0, sizeof(sheet));
	if (!WSWndClass_Create(&windowclass, pThis->app, NULL, SflBootSheetMessages, "SflBootSheet"))
		return;
	WSRect_Construct(&bounds, WSDEFAULTPOSITION, WSDEFAULTPOSITION, 620, 600);
	if (WSWindow_Create(&sheet.window, pThis->app, &bounds, title, NULL, &windowclass)) {
		WSWindow_SetCon(&sheet.window, &sheet);
		WSWindow_SetSize(&sheet.window, 620, 600);
		WSRect_Construct(&bounds, 16, 16, 570, 470);
		if (WSCtlText_CreateScrolled(&sheet.text, &sheet.scrolled, &sheet.window, &bounds, &windowclass)) {
			WSCtlText_SetText(&sheet.text, text, strlen(text));
			WSCtlText_SetEditable(&sheet.text, FALSE);
			WSRect_Construct(&bounds, 16, 500, 570, 34);
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
		form->selection = (int)WSCtlComboBox_GetCurrentIndex(&form->sizes);
	}
	form->accepted = accept;
	form->closed = 1;
	WSCtlButton_Destroy(&form->cancel);
	WSCtlButton_Destroy(&form->accept);
	WSCtlComboBox_Destroy(&form->sizes);
	WSCtlLine_Destroy(&form->line);
	WSCtlLabel_Destroy(&form->label);
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


static int SflBootForm(SFLBOOTGUI *pThis, const char *title, const char *label, const char *initial, int disk, char **value, int *selection)
{
	SFLBOOTFORM form;
	WSWNDCLASS windowclass;
	WSRECT bounds;
	int index;
	int size;
	char number[SFLBOOTVALUESIZE];
	memset(&form, 0, sizeof(form));
	if (!WSWndClass_Create(&windowclass, pThis->app, NULL, SflBootFormMessages, "SflBootForm"))
		return 0;
	WSRect_Construct(&bounds, WSDEFAULTPOSITION, WSDEFAULTPOSITION, 540, 200);
	if (!WSWindow_Create(&form.window, pThis->app, &bounds, title, NULL, &windowclass)) {
		WSWndClass_Destroy(&windowclass, pThis->app);
		return 0;
	}
	WSWindow_SetCon(&form.window, &form);
	WSWindow_SetSize(&form.window, 540, 200);
	WSRect_Construct(&bounds, 16, 16, 490, 22);
	WSCtlLabel_Create(&form.label, &form.window, &bounds, label, 0, 3);
	WSRect_Construct(&bounds, 16, 44, 490, 26);
	WSCtlLine_Create(&form.line, &form.window, &bounds);
	WSCtlLine_SetText(&form.line, initial, strlen(initial));
	WSRect_Construct(&bounds, 16, 80, SFLBOOTCOMBOWIDTH, SFLBOOTCOMBOHEIGHT);
	WSCtlComboBox_Create(&form.sizes, &form.window, &bounds, 4);
	size = SFLBOOTDISKBASEMB;
	for (index = 0; index < SFLBOOTDISKSIZES; ++index) {
		sprintf(number, "%d MB", size);
		WSCtlComboBox_AddString(&form.sizes, number);
		size *= 2;
	}
	WSCtlComboBox_SetCurrentIndex(&form.sizes, SFLBOOTDISKDEFAULT);
	if (!disk)
		WSCtlComboBox_Hide(&form.sizes);
	WSRect_Construct(&bounds, 296, 120, 100, 30);
	WSCtlButton_Create(&form.accept, &form.window, &bounds, "OK", SflBootFormAccept, SFLBOOTIDACCEPT);
	WSRect_Construct(&bounds, 406, 120, 100, 30);
	WSCtlButton_Create(&form.cancel, &form.window, &bounds, "Cancel", SflBootFormCancel, SFLBOOTIDCANCEL);
	WSWindow_Modal(&form.window, &pThis->window, pThis->app);
	WSWndClass_Destroy(&windowclass, pThis->app);
	*value = form.value;
	*selection = form.selection;
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
	SFLBOOTITEM *disks;
	WSCHOOSERFILEITERATOR iterator;
	char *path;
	disks = SflBootFind(pThis, "disk");
	WSChooserFile(&iterator, &pThis->window);
	while (!WSChooserFileIsEnd(&iterator)) {
		WSChooserFileIterator_AllocValue(&iterator, &path);
		SflBootAppendList(disks, path);
		WSFREE(path);
		WSChooserFileIterator_Increment(&iterator);
	}
	WSChooserFileFinish(&iterator);
}

static void SflBootRemoveSelected(SFLBOOTGUI *pThis, const char *pref)
{
	SFLBOOTITEM *item;
	int selected;
	item = SflBootFind(pThis, pref);
	selected = WSCtlList_GetNextItem(&item->list, -1, WSCTLLIST_STATE_SELECTED, WSCTLLIST_GETNEXTITEM_ALL);
	if (selected >= 0)
		WSCtlList_DeleteItem(&item->list, selected);
}

static void SflBootCreateDisk(SFLBOOTGUI *pThis)
{
	FILE *file;
	char *name;
	long size;
	int selection;
	int success;
	if (!SflBootForm(pThis, "Create disk image", "Disk image path and size", "New Disk.img", 1, &name, &selection))
		return;
	file = fopen(name, "rb");
	if (file != NULL) {
		fclose(file);
		WSMessageBox(&pThis->window, "Create disk image", "That file already exists. Choose another name.", WSMESSAGEBOX_OK);
		WSFREE(name);
		return;
	}
	size = ((long)SFLBOOTDISKBASEMB << selection) * SFLBOOTMEGABYTE;
	success = 0;
	file = fopen(name, "wb");
	if (file != NULL) {
		success = fseek(file, size - 1, SEEK_SET) == 0 && fputc(0, file) != EOF;
		if (fclose(file) != 0)
			success = 0;
	}
	if (success)
		SflBootAppendList(SflBootFind(pThis, "disk"), name);
	else
		WSMessageBox(&pThis->window, "Create disk image", "The disk image could not be created.", WSMESSAGEBOX_OK);
	WSFREE(name);
}

static void SflBootAddRedirect(SFLBOOTGUI *pThis)
{
	char *rule;
	int selection;
	if (!SflBootForm(pThis, "Port forwarding", "protocol:host-port:guest-address:guest-port", "tcp:8080:10.0.2.15:80", 0, &rule, &selection))
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
SFLBOOTBUTTONHANDLER(SflBootOnBrowseCd, SflBootBrowsePath(pThis, "cdrom"))
SFLBOOTBUTTONHANDLER(SflBootOnBrowseFolder, SflBootBrowsePath(pThis, "extfs"))
SFLBOOTBUTTONHANDLER(SflBootOnDiskAdd, SflBootAddDisks(pThis))
SFLBOOTBUTTONHANDLER(SflBootOnDiskCreate, SflBootCreateDisk(pThis))
SFLBOOTBUTTONHANDLER(SflBootOnDiskRemove, SflBootRemoveSelected(pThis, "disk"))
SFLBOOTBUTTONHANDLER(SflBootOnRedirAdd, SflBootAddRedirect(pThis))
SFLBOOTBUTTONHANDLER(SflBootOnRedirRemove, SflBootRemoveSelected(pThis, "redir"))
SFLBOOTBUTTONHANDLER(SflBootOnInstructions, SflBootSheet(pThis, "Setup instructions", SflBootInstructionsText()))
SFLBOOTBUTTONHANDLER(SflBootOnLicenses, SflBootSheet(pThis, "Licenses", SflBootLicenseText()))
SFLBOOTBUTTONHANDLER(SflBootOnSetupClose, SflBootDismiss(pThis, "setupbanner", "setupread"))
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
			SflBootAppendList(SflBootFind(pThis, "disk"), path);
			WSFREE(path);
			WSDnDFileDropIterator_Increment(&iterator);
		}
		WSEventDnD_FinishFileDrop(&event, &iterator);
	}
	return WSEVENTDND_HANDLED;
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
	IMPLEMENT_CTLBUTTON_CLICKED_MESSAGE(SflBootOnBrowseCd, SFLBOOTIDCD)
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

static void SflBootBuildGeneral(SFLBOOTGUI *pThis)
{
	SFLBOOTITEM *item;
	item = SflBootAddHelp(pThis, SFLBOOTPAGEGENERAL, "setupbanner", "Read the setup instructions if you want to install Classic Mac OS from scratch. They contain crucial tips on how to not get stuck during installation and how to get audio working.");
	item->type = SFLBOOTBANNER;
	SflBootAddButton(pThis, item, "X", SflBootOnSetupClose, SFLBOOTIDSETUPCLOSE);
	item = SflBootAddItem(pThis, SFLBOOTPAGEGENERAL, SFLBOOTBUTTONS, "setupread", NULL);
	SflBootAddButton(pThis, item, "Read instructions", SflBootOnInstructions, SFLBOOTIDSHEETREAD);
	SflBootAddHeading(pThis, SFLBOOTPAGEGENERAL, "ROM");
	SflBootAddPath(pThis, SFLBOOTPAGEGENERAL, "rom", "Macintosh ROM file", 0, SflBootOnBrowseRom, SFLBOOTIDROM);
	SflBootAddHelp(pThis, SFLBOOTPAGEGENERAL, "romstatus", "");
	SflBootAddHeading(pThis, SFLBOOTPAGEGENERAL, "Disks");
	SflBootAddList(pThis, SFLBOOTPAGEGENERAL, "disk", "Mounted disk images");
	item = SflBootAddItem(pThis, SFLBOOTPAGEGENERAL, SFLBOOTBUTTONS, NULL, NULL);
	SflBootAddButton(pThis, item, "Add...", SflBootOnDiskAdd, SFLBOOTIDDISKADD);
	SflBootAddButton(pThis, item, "Create...", SflBootOnDiskCreate, SFLBOOTIDDISKCREATE);
	SflBootAddButton(pThis, item, "Remove", SflBootOnDiskRemove, SFLBOOTIDDISKREMOVE);
	SflBootAddHeading(pThis, SFLBOOTPAGEGENERAL, "CD-ROM");
	SflBootAddPath(pThis, SFLBOOTPAGEGENERAL, "cdrom", "CD-ROM image", 0, SflBootOnBrowseCd, SFLBOOTIDCD);
	SflBootAddHeading(pThis, SFLBOOTPAGEGENERAL, "Shared folder");
	SflBootAddCheck(pThis, SFLBOOTPAGEGENERAL, "enableextfs", "Enable shared folder", 0);
	SflBootAddPath(pThis, SFLBOOTPAGEGENERAL, "extfs", "Shared folder", 1, SflBootOnBrowseFolder, SFLBOOTIDFOLDER);
	SflBootAddHeading(pThis, SFLBOOTPAGEGENERAL, "Right click");
	SflBootAddRadio(pThis, SFLBOOTPAGEGENERAL, "rightclick", NULL, "Control + click=control|Command + click=command", SFLBOOTKINDSTRING);
	SflBootAddHelp(pThis, SFLBOOTPAGEGENERAL, NULL, "Controls what a right mouse click does.");
	SflBootAddHeading(pThis, SFLBOOTPAGEGENERAL, "Audio");
	SflBootAddCheck(pThis, SFLBOOTPAGEGENERAL, "nosound", "Audio enabled", 1);
	SflBootAddHelp(pThis, SFLBOOTPAGEGENERAL, NULL, "Audio output must be selected in the Sound control panel. See the setup instructions.");
	if (SflBootIsSheepShaver()) {
		SflBootAddHeading(pThis, SFLBOOTPAGEGENERAL, "Mac OS X");
		SflBootAddCombo(pThis, SFLBOOTPAGEGENERAL, "osxboot", "Boot device", "Automatic=|Hard disk=disk|CD-ROM=cdrom", SFLBOOTKINDSTRING);
	}
}

static void SflBootBuildGraphics(SFLBOOTGUI *pThis)
{
	SflBootAddHeading(pThis, SFLBOOTPAGEGRAPHICS, "Display");
	SflBootAddRadio(pThis, SFLBOOTPAGEGRAPHICS, "screenmode", NULL, "Windowed=win|Full screen=dga", SFLBOOTKINDSPECIAL);
	SflBootAddCombo(pThis, SFLBOOTPAGEGRAPHICS, "screensize", "Resolution", "Desktop=0/0|640 x 480=640/480|800 x 600=800/600|1024 x 768=1024/768|1280 x 720=1280/720|1920 x 1080=1920/1080", SFLBOOTKINDSPECIAL);
	SflBootAddHelp(pThis, SFLBOOTPAGEGRAPHICS, NULL, "Resolution made available to Mac OS, along with the standard sizes below it. Desktop uses the size of the host display when full screen.");
	if (SflBootIsSheepShaver()) {
		SflBootAddHeading(pThis, SFLBOOTPAGEGRAPHICS, "Graphics acceleration (experimental)");
		SflBootAddCheck(pThis, SFLBOOTPAGEGRAPHICS, "nqdaccel", "QuickDraw 2D", 0);
		SflBootAddCheck(pThis, SFLBOOTPAGEGRAPHICS, "raveaccel", "QuickDraw 3D + RAVE", 0);
		SflBootAddCheck(pThis, SFLBOOTPAGEGRAPHICS, "glaccel", "OpenGL", 0);
		SflBootAddCheck(pThis, SFLBOOTPAGEGRAPHICS, "dspaccel", "DrawSprocket", 0);
		SflBootAddHelp(pThis, SFLBOOTPAGEGRAPHICS, NULL, "Each accelerator is independent. Mix and match per application to find what runs best.");
	}
	SflBootAddHeading(pThis, SFLBOOTPAGEGRAPHICS, "Rendering filter");
	SflBootAddRadio(pThis, SFLBOOTPAGEGRAPHICS, "scale_nearest", NULL, "Bilinear=0|Nearest neighbor=1", SFLBOOTKINDBOOL);
	SflBootAddHelp(pThis, SFLBOOTPAGEGRAPHICS, NULL, "Bilinear produces a smooth image through interpolation. Nearest neighbor gives a sharp, pixelated look.");
	SflBootAddCheck(pThis, SFLBOOTPAGEGRAPHICS, "scale_integer", "Integer scaling", 0);
	SflBootAddHeading(pThis, SFLBOOTPAGEGRAPHICS, "Gamma ramp");
	SflBootAddRadio(pThis, SFLBOOTPAGEGRAPHICS, "gammaramp", NULL, "Linear=off|OS defined=on|Full screen only=fullscreen", SFLBOOTKINDSTRING);
	SflBootAddHelp(pThis, SFLBOOTPAGEGRAPHICS, NULL, "A linear gamma ramp generally produces a less color distorted image.");
	SflBootAddHeading(pThis, SFLBOOTPAGEGRAPHICS, "Frame rate");
	SflBootAddCombo(pThis, SFLBOOTPAGEGRAPHICS, "frameskip", "Refresh", "Every frame=1|Every 2nd frame=2|Every 4th frame=4|Every 6th frame=6|Every 8th frame=8", SFLBOOTKINDINT);
}

static void SflBootBuildNetwork(SFLBOOTGUI *pThis)
{
	SFLBOOTITEM *item;
	item = SflBootAddHelp(pThis, SFLBOOTPAGENETWORK, "networkbanner", "Network requires Mac OS 9.0 - 9.0.4.");
	item->type = SFLBOOTBANNER;
	SflBootAddButton(pThis, item, "X", SflBootOnNetworkClose, SFLBOOTIDNETWORKCLOSE);
	SflBootAddHeading(pThis, SFLBOOTPAGENETWORK, "Mode");
	SflBootAddRadio(pThis, SFLBOOTPAGENETWORK, "network", NULL, "Off=off|Internet=slirp|UDP tunnel=udptunnel", SFLBOOTKINDSPECIAL);
	SflBootAddHelp(pThis, SFLBOOTPAGENETWORK, NULL, "Internet gives access through SLiRP, without the ability to host. UDP tunnel connects to other emulators on the same network.");
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
	SflBootAddHeading(pThis, SFLBOOTPAGEADVANCED, "Memory");
	SflBootAddCombo(pThis, SFLBOOTPAGEADVANCED, "ramsize", "RAM", "16 MB=16777216|32 MB=33554432|64 MB=67108864|128 MB=134217728|256 MB=268435456|512 MB=536870912|768 MB=805306368|1024 MB=1073741824", SFLBOOTKINDINT);
	SflBootAddHeading(pThis, SFLBOOTPAGEADVANCED, "Clipboard sharing");
	SflBootAddCheck(pThis, SFLBOOTPAGEADVANCED, "noclipconversion", "Convert shared clipboard text", 1);
	if (SflBootIsSheepShaver()) {
		SflBootAddHeading(pThis, SFLBOOTPAGEADVANCED, "Relative mouse mode");
		SflBootAddRadio(pThis, SFLBOOTPAGEADVANCED, "relmouse", NULL, "Manual=manual|Automatic=auto|Always on=always", SFLBOOTKINDSTRING);
		SflBootAddHelp(pThis, SFLBOOTPAGEADVANCED, NULL, "Manual mode toggles with F8. Automatic follows the guest hiding its cursor.");
	} else {
		SflBootAddHeading(pThis, SFLBOOTPAGEADVANCED, "Relative mouse mode");
	}
	SflBootAddCheck(pThis, SFLBOOTPAGEADVANCED, "init_grab", "Boot with relative mouse mode on", 0);
	SflBootAddHeading(pThis, SFLBOOTPAGEADVANCED, "Performance metrics");
	SflBootAddCheck(pThis, SFLBOOTPAGEADVANCED, "showfps", "Show FPS counter", 0);
	SflBootAddCheck(pThis, SFLBOOTPAGEADVANCED, "shownetrate", "Show network transfer rate", 0);
	SflBootAddHeading(pThis, SFLBOOTPAGEADVANCED, "UI options");
	SflBootAddCheck(pThis, SFLBOOTPAGEADVANCED, "notifyip", "Report IP address assignment", 0);
	SflBootAddCheck(pThis, SFLBOOTPAGEADVANCED, "notifyclip", "Notify of clipboard sharing events", 0);
	SflBootAddHeading(pThis, SFLBOOTPAGEADVANCED, "CPU emulation");
	if (SflBootIsSheepShaver()) {
		SflBootAddCheck(pThis, SFLBOOTPAGEADVANCED, "ignoreillegal", "Ignore illegal CPU instructions", 0);
		SflBootAddCheck(pThis, SFLBOOTPAGEADVANCED, "altivec", "AltiVec", 0);
		SflBootAddHelp(pThis, SFLBOOTPAGEADVANCED, NULL, "Advertises a G4 processor so vector-aware software can use faster code paths.");
	}
	SflBootAddCheck(pThis, SFLBOOTPAGEADVANCED, "jit", "JIT compiler", 0);
	SflBootAddHelp(pThis, SFLBOOTPAGEADVANCED, NULL, "Translates guest code into native code as it runs, for a large CPU speedup.");
	if (SflBootIsSheepShaver()) {
		SflBootAddCheck(pThis, SFLBOOTPAGEADVANCED, "jit68k", "68k JIT compiler", 0);
		SflBootAddCheck(pThis, SFLBOOTPAGEADVANCED, "osxjit", "JIT compiler (Mac OS X)", 0);
		SflBootAddHeading(pThis, SFLBOOTPAGEADVANCED, "Mac OS X");
		SflBootAddCheck(pThis, SFLBOOTPAGEADVANCED, "skiposxstartup", "Skip startup screen", 0);
	}
	SflBootAddHeading(pThis, SFLBOOTPAGEADVANCED, "Resources");
	item = SflBootAddItem(pThis, SFLBOOTPAGEADVANCED, SFLBOOTBUTTONS, NULL, NULL);
	SflBootAddButton(pThis, item, "Setup instructions", SflBootOnInstructions, SFLBOOTIDINSTRUCTIONS);
	SflBootAddButton(pThis, item, "Licenses", SflBootOnLicenses, SFLBOOTIDLICENSES);
}

static int SflBootCreateWindow(SFLBOOTGUI *pThis)
{
	WSRECT bounds;
	int index;
	if (!WSWndClass_Create(&pThis->windowclass, pThis->app, NULL, SflBootMessages, "SflBootPreferences"))
		return 0;

	WSRect_Construct(&bounds, WSDEFAULTPOSITION, WSDEFAULTPOSITION, SFLBOOTWIDTH, SFLBOOTHEIGHT);
	if (!WSWindow_Create(&pThis->window, pThis->app, &bounds, "PocketShaver", NULL, &pThis->windowclass)) {
		WSWndClass_Destroy(&pThis->windowclass, pThis->app);
		return 0;
	}
	WSWindow_SetCon(&pThis->window, pThis);
	WSWindow_SetSize(&pThis->window, SFLBOOTWIDTH, SFLBOOTHEIGHT);
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

static void SflBootDestroyWindow(SFLBOOTGUI *pThis)
{
	int index;
	if (pThis->dropcreated)
		WSDnD_Destroy(&pThis->drop);
	for (index = pThis->count - 1; index >= 0; --index)
		SflBootDestroyItem(&pThis->items[index]);
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
	loop = &app->mEventLoop;
	accepted = 0;
	#if defined(SFL_USEDARKMODE)
		WSDarkMode_Create(&pThis->darkmode);
	#endif /* SFL_USEDARKMODE */
	if (SflBootCreateWindow(pThis)) {
		pThis->fontscreated = SflBootCreateFonts(&pThis->headingfont, &pThis->bodyfont);
		SflBootBuildGeneral(pThis);
		SflBootBuildGraphics(pThis);
		SflBootBuildNetwork(pThis);
		SflBootBuildAdvanced(pThis);
		if (!pThis->failed) {
			pThis->dropcreated = WSDnD_Create(&pThis->drop, &pThis->window, SflBootOnDrop);
			SflBootUpdateRomStatus(pThis);
			WSWindow_Show(&pThis->window);
			pThis->ready = 1;
			SflBootLayout(pThis);
			SflBootUpdatePortField(pThis);
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
