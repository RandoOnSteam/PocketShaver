#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sysdeps.h"
#include "main.h"
#include "cpu_emulation.h"
#include "emul_op.h"
#include "prefs.h"
#ifdef SHEEPSHAVER
#include "thunks.h"
#endif
#include "pdfgen.h"
#include "pictpdf.h"
#include "printing.h"

#define PRINTTRAPWORD 0xA8FD
#define PRINTTRAPSLOT (0xE00 + 0xFD * 4)
#define PRINTERRLOWMEM 0x944
#define PRINTAPPNAMELOWMEM 0x910
#define PRINTCURRENTA5LOWMEM 0x904
#define PRINTMAINDEVICELOWMEM 0x8A4
#define GDEVICERECT 34

#define PRINTRECORDVERSION 3
#define PRINTRECORDSIZE 120
#define PRINTPORTSIZE 178
#define PRINTDIALOGSIZE 204
#define PRINTDEFAULTRESOLUTION 72
#define PRINTLASTPAGE 9999

#define PRINTNOERR 0
#define PRINTMEMFULLERR -108
#define PRINTIOABORT -27

#define TPRINTVERSION 0
#define TPRINTINFO 2
#define TPRINTPAPER 16
#define TPRINTSTYLE 24
#define TPRINTINFOPT 32
#define TPRINTXINFO 46
#define TPRINTJOB 62
#define TPRINTJOBSIZE 20
#define TPRINTPAPERKIND 82
#define TPRINTLANDSCAPE 84
#define TPRINFOVRES 2
#define TPRINFOHRES 4
#define TPRINFOPAGE 6

#define TPRPORTPICTURE 160
#define TPRPORTPRINT 164
#define TPRPORTJOBHIGH 168
#define TPRPORTJOBLOW 172
#define TPRPORTOURPTR 176

#define TPRDLGFILTER 170
#define TPRDLGITEMPROC 174
#define TPRDLGPRINT 178
#define TPRDLGDOIT 182
#define TPRDLGDONE 183

#define CGRAFPORTRECT 16
#define CGRAFPORTVISRGN 24

#define TRAPNEWPTRCLEAR 0xA31E
#define TRAPNEWHANDLE 0xA122
#define TRAPDISPOSEPTR 0xA01F
#define TRAPGETHANDLESIZE 0xA025
#define TRAPSETTOOLTRAPADDRESS 0xA647
#define TRAPSETPORT 0xA873
#define TRAPOPENCPORT 0xAA00
#define TRAPINITCPORT 0xAA01
#define TRAPCLOSEPORT 0xA87D
#define TRAPSETEMPTYRGN 0xA8DD
#define TRAPOPENPICTURE 0xA8F3
#define TRAPCLOSEPICTURE 0xA8F4
#define TRAPKILLPICTURE 0xA8F5
#define TRAPINITCURSOR 0xA850
#define TRAPNEWDIALOG 0xA97D
#define TRAPGETDIALOGITEM 0xA98D
#define TRAPSETDIALOGITEMTEXT 0xA98F
#define TRAPGETDIALOGITEMTEXT 0xA990
#define TRAPSELECTDIALOGITEMTEXT 0xA97E
#define TRAPSHOWWINDOW 0xA915
#define TRAPCLOSEDIALOG 0xA982
#define TRAPGETCONTROLVALUE 0xA960
#define TRAPSETCONTROLVALUE 0xA963
#define TRAPGETPORT 0xA874
#define TRAPBEGINUPDATE 0xA922
#define TRAPENDUPDATE 0xA923
#define TRAPUPDATEDIALOG 0xA978
#define TRAPPENSIZE 0xA89B
#define TRAPPENNORMAL 0xA89E
#define TRAPFRAMEROUNDRECT 0xA8B0
#define TRAPHILITECONTROL 0xA95D
#define TRAPDELAY 0xA03B

#define EVENTWHAT 0
#define EVENTMESSAGE 2
#define EVENTMODIFIERS 14
#define EVENTNULL 0
#define EVENTKEYDOWN 3
#define EVENTAUTOKEY 5
#define EVENTUPDATE 6
#define EVENTCMDKEY 0x0100
#define WINDOWVISRGN 24

#define DITLBUTTON 4
#define DITLRADIO 6
#define DITLSTATIC (8 + 128)
#define DITLEDIT 16
#define DITLMAXSIZE 1024

#define DIALOGOK 1
#define DIALOGCANCEL 2
#define STYLEPAPERFIRST 5
#define STYLEPORTRAIT 9
#define STYLELANDSCAPE 10
#define JOBALL 5
#define JOBRANGE 6
#define JOBFROM 7
#define JOBTO 9

#define PRGLUEOPENDOC 0x04
#define PRGLUECLOSEDOC 0x08
#define PRGLUEOPENPAGE 0x10
#define PRGLUECLOSEPAGE 0x18
#define PRGLUEPRINTDEFAULT 0x20
#define PRGLUESTLDIALOG 0x2A
#define PRGLUEJOBDIALOG 0x32
#define PRGLUESTLINIT 0x3C
#define PRGLUEJOBINIT 0x44
#define PRGLUEDLGMAIN 0x4A
#define PRGLUEVALIDATE 0x52
#define PRGLUEJOBMERGE 0x58
#define PRGLUEGENERAL 0x70
#define PRGLUEDRVRDCE 0x94
#define PRGLUEDRVRVERS 0x9A
#define PRGLUEERROR 0xBA
#define PRGLUESETERROR 0xC0
#define PRGLUEOPEN 0xC8

#define PRGENERALGETRSL 4
#define PRGENERALSETRSL 5
#define PRGENERALDRAFTBITS 6
#define PRGENERALNODRAFTBITS 7
#define PRGENERALGETROTN 8
#define PRGENERALNOSUCHRSL 1
#define PRGENERALOPNOTIMPL 2

#define PRINTGLUETRAP 0
#define PRINTGLUESTYLEITEM 1
#define PRINTGLUEJOBITEM 2
#define PRINTGLUEFILTER 3
#define PRINTGLUEDIALOGITEM 4

#define PRINTRESOLUTIONCOUNT 4
#define PRINTPAPERCOUNT 3

struct PrintJob
{
	struct pdf_doc *pdf;
	int firstpage;
	int lastpage;
	int pagenumber;
};

class PrintScratch
{
public:
	PrintScratch(uint32 size);
	~PrintScratch();
	uint32 addr(void) const;
private:
#ifdef SHEEPSHAVER
	SheepVar variable;
#else
	uint32 address;
#endif
};

#ifdef SHEEPSHAVER
PrintScratch::PrintScratch(uint32 size) : variable(size)
{
}

PrintScratch::~PrintScratch()
{
}

uint32 PrintScratch::addr(void) const
{
	return variable.addr();
}
#else
PrintScratch::PrintScratch(uint32 size)
{
	M68kRegisters registers;

	memset(&registers, 0, sizeof(registers));
	registers.d[0] = size;
	Execute68kTrap(TRAPNEWPTRCLEAR, &registers);
	address = registers.a[0];
}

PrintScratch::~PrintScratch()
{
	M68kRegisters registers;

	memset(&registers, 0, sizeof(registers));
	registers.a[0] = address;
	Execute68kTrap(TRAPDISPOSEPTR, &registers);
}

uint32 PrintScratch::addr(void) const
{
	return address;
}
#endif

static int PrintResolution(int index)
{
	static const int resolutions[PRINTRESOLUTIONCOUNT] = { 72, 144, 300, 600 };

	return resolutions[index];
}

static bool PrintIsResolution(int resolution)
{
	int index;

	for (index = 0; index < PRINTRESOLUTIONCOUNT; index++)
	{
		if (PrintResolution(index) == resolution)
			return true;
	}
	return false;
}

static void PrintPaperSize(int paper, int *width, int *height)
{
	static const int widths[PRINTPAPERCOUNT] = { 850, 850, 827 };
	static const int heights[PRINTPAPERCOUNT] = { 1100, 1400, 1169 };

	*width = widths[paper];
	*height = heights[paper];
}

static const char *PrintPaperName(int paper)
{
	static const char *const names[PRINTPAPERCOUNT] = { "US Letter (8.5 x 11 in)", "US Legal (8.5 x 14 in)", "A4 (210 x 297 mm)" };

	return names[paper];
}

static uint32 PrintCall68k(uint32 target, uint32 selector, int resultsize, int argumentcount, const uint32 *arguments, const int *argumentsizes)
{
	PrintScratch code(128);
	M68kRegisters registers;
	uint32 cursor;
	int index;

	cursor = code.addr();
	if (resultsize == 4)
	{
		WriteMacInt16(cursor, 0x42A7);
		cursor += 2;
	}
	else if (resultsize == 2)
	{
		WriteMacInt16(cursor, 0x4267);
		cursor += 2;
	}
	for (index = 0; index < argumentcount; index++)
	{
		if (argumentsizes != NULL && argumentsizes[index] == 2)
		{
			WriteMacInt16(cursor, 0x3F3C);
			WriteMacInt16(cursor + 2, (uint16)arguments[index]);
			cursor += 4;
		}
		else
		{
			WriteMacInt16(cursor, 0x2F3C);
			WriteMacInt32(cursor + 2, arguments[index]);
			cursor += 6;
		}
	}
	if (selector)
	{
		WriteMacInt16(cursor, 0x203C);
		WriteMacInt32(cursor + 2, selector);
		cursor += 6;
	}
	if (target >= 0xA000 && target <= 0xAFFF)
	{
		WriteMacInt16(cursor, (uint16)target);
		cursor += 2;
	}
	else
	{
		WriteMacInt16(cursor, 0x207C);
		WriteMacInt32(cursor + 2, target);
		WriteMacInt16(cursor + 6, 0x4E90);
		cursor += 8;
	}
	if (resultsize == 4)
	{
		WriteMacInt16(cursor, 0x201F);
		cursor += 2;
	}
	else if (resultsize == 2)
	{
		WriteMacInt16(cursor, 0x301F);
		cursor += 2;
	}
	WriteMacInt16(cursor, M68K_RTS);
	memset(&registers, 0, sizeof(registers));
	registers.a[5] = ReadMacInt32(PRINTCURRENTA5LOWMEM);
	Execute68k(code.addr(), &registers);
	return registers.d[0];
}

static uint32 PrintCallTrap(uint16 trap, int resultsize, int argumentcount, const uint32 *arguments)
{
	return PrintCall68k(trap, 0, resultsize, argumentcount, arguments, NULL);
}

static uint32 PrintCallTrap1(uint16 trap, int resultsize, uint32 argument)
{
	return PrintCall68k(trap, 0, resultsize, 1, &argument, NULL);
}

static uint32 PrintNewPtrClear(uint32 size)
{
	M68kRegisters registers;

	memset(&registers, 0, sizeof(registers));
	registers.a[5] = ReadMacInt32(PRINTCURRENTA5LOWMEM);
	registers.d[0] = size;
	Execute68kTrap(TRAPNEWPTRCLEAR, &registers);
	if ((int16)registers.d[0] != PRINTNOERR)
		return 0;
	return registers.a[0];
}

static uint32 PrintNewHandle(uint32 size)
{
	M68kRegisters registers;

	memset(&registers, 0, sizeof(registers));
	registers.a[5] = ReadMacInt32(PRINTCURRENTA5LOWMEM);
	registers.d[0] = size;
	Execute68kTrap(TRAPNEWHANDLE, &registers);
	if ((int16)registers.d[0] != PRINTNOERR)
		return 0;
	return registers.a[0];
}

static void PrintDisposePtr(uint32 pointer)
{
	M68kRegisters registers;

	memset(&registers, 0, sizeof(registers));
	registers.a[5] = ReadMacInt32(PRINTCURRENTA5LOWMEM);
	registers.a[0] = pointer;
	Execute68kTrap(TRAPDISPOSEPTR, &registers);
}

static uint32 PrintGetHandleSize(uint32 handle)
{
	M68kRegisters registers;

	memset(&registers, 0, sizeof(registers));
	registers.a[5] = ReadMacInt32(PRINTCURRENTA5LOWMEM);
	registers.a[0] = handle;
	Execute68kTrap(TRAPGETHANDLESIZE, &registers);
	if ((int32)registers.d[0] < 0)
		return 0;
	return registers.d[0];
}

static void PrintSetError(int16 error)
{
	WriteMacInt16(PRINTERRLOWMEM, (uint16)error);
}

static void PrintWriteRect(uint32 address, int top, int left, int bottom, int right)
{
	WriteMacInt16(address, (uint16)top);
	WriteMacInt16(address + 2, (uint16)left);
	WriteMacInt16(address + 4, (uint16)bottom);
	WriteMacInt16(address + 6, (uint16)right);
}

static void PrintWriteInfo(uint32 info, int resolution, int pagewidth, int pageheight)
{
	WriteMacInt16(info, 0);
	WriteMacInt16(info + TPRINFOVRES, (uint16)resolution);
	WriteMacInt16(info + TPRINFOHRES, (uint16)resolution);
	PrintWriteRect(info + TPRINFOPAGE, 0, 0, pageheight, pagewidth);
}

static void PrintBuildRecord(uint32 record, int resolution, int paper, int landscape)
{
	int paperwidth;
	int paperheight;
	int swap;
	int margin;
	int pagewidth;
	int pageheight;

	PrintPaperSize(paper, &paperwidth, &paperheight);
	if (landscape)
	{
		swap = paperwidth;
		paperwidth = paperheight;
		paperheight = swap;
	}
	margin = resolution / 4;
	pagewidth = resolution * paperwidth / 100 - 2 * margin;
	pageheight = resolution * paperheight / 100 - 2 * margin;
	memset(Mac2HostAddr(record), 0, PRINTRECORDSIZE);
	WriteMacInt16(record + TPRINTVERSION, PRINTRECORDVERSION);
	PrintWriteInfo(record + TPRINTINFO, resolution, pagewidth, pageheight);
	PrintWriteRect(record + TPRINTPAPER, -margin, -margin, pageheight + margin, pagewidth + margin);
	WriteMacInt16(record + TPRINTSTYLE + 2, (uint16)paperheight);
	WriteMacInt16(record + TPRINTSTYLE + 4, (uint16)paperwidth);
	PrintWriteInfo(record + TPRINTINFOPT, resolution, pagewidth, pageheight);
	WriteMacInt16(record + TPRINTXINFO, (uint16)(((pagewidth + 15) / 16) * 2));
	WriteMacInt16(record + TPRINTXINFO + 2, (uint16)pageheight);
	WriteMacInt16(record + TPRINTXINFO + 4, (uint16)pagewidth);
	WriteMacInt16(record + TPRINTXINFO + 8, 1);
	WriteMacInt8(record + TPRINTXINFO + 11, 1);
	WriteMacInt8(record + TPRINTXINFO + 12, 1);
	WriteMacInt8(record + TPRINTXINFO + 13, 1);
	WriteMacInt16(record + TPRINTJOB, 1);
	WriteMacInt16(record + TPRINTJOB + 2, PRINTLASTPAGE);
	WriteMacInt16(record + TPRINTJOB + 4, 1);
	WriteMacInt8(record + TPRINTJOB + 7, 1);
	WriteMacInt16(record + TPRINTPAPERKIND, (uint16)paper);
	WriteMacInt16(record + TPRINTLANDSCAPE, (uint16)landscape);
}

static int PrintRecordPaper(uint32 record)
{
	int paper;

	paper = (int16)ReadMacInt16(record + TPRINTPAPERKIND);
	if (paper < 0 || paper >= PRINTPAPERCOUNT)
		paper = 0;
	return paper;
}

static int PrintRecordLandscape(uint32 record)
{
	return ReadMacInt16(record + TPRINTLANDSCAPE) != 0;
}

static int PrintRecordResolution(uint32 record)
{
	return (int16)ReadMacInt16(record + TPRINTINFO + TPRINFOHRES);
}

static bool PrintRecordIsValid(uint32 record)
{
	int resolution;

	resolution = PrintRecordResolution(record);
	return ReadMacInt16(record + TPRINTVERSION) == PRINTRECORDVERSION && PrintIsResolution(resolution) &&
		(int16)ReadMacInt16(record + TPRINTINFO + TPRINFOVRES) == resolution &&
		(int16)ReadMacInt16(record + TPRINTPAPERKIND) >= 0 && (int16)ReadMacInt16(record + TPRINTPAPERKIND) < PRINTPAPERCOUNT;
}

static bool PrintValidate(uint32 printhandle)
{
	uint32 record;

	record = ReadMacInt32(printhandle);
	if (PrintRecordIsValid(record))
		return false;
	PrintBuildRecord(record, PRINTDEFAULTRESOLUTION, 0, 0);
	return true;
}

static void PrintRebuildRecord(uint32 printhandle, int resolution, int paper, int landscape)
{
	uint8 job[TPRINTJOBSIZE];
	uint32 record;

	record = ReadMacInt32(printhandle);
	memcpy(job, Mac2HostAddr(record + TPRINTJOB), TPRINTJOBSIZE);
	PrintBuildRecord(record, resolution, paper, landscape);
	memcpy(Mac2HostAddr(record + TPRINTJOB), job, TPRINTJOBSIZE);
}

static void PrintGeneral(uint32 data)
{
	uint32 records;
	uint32 record;
	int resolution;
	int index;

	WriteMacInt16(data + 2, PRINTNOERR);
	switch (ReadMacInt16(data))
	{
	case PRGENERALGETRSL:
		WriteMacInt16(data + 8, 1);
		WriteMacInt16(data + 10, (uint16)PrintResolution(0));
		WriteMacInt16(data + 12, (uint16)PrintResolution(PRINTRESOLUTIONCOUNT - 1));
		WriteMacInt16(data + 14, (uint16)PrintResolution(0));
		WriteMacInt16(data + 16, (uint16)PrintResolution(PRINTRESOLUTIONCOUNT - 1));
		WriteMacInt16(data + 18, PRINTRESOLUTIONCOUNT);
		records = data + 20;
		for (index = 0; index < PRINTRESOLUTIONCOUNT; index++)
		{
			WriteMacInt16(records + index * 4, (uint16)PrintResolution(index));
			WriteMacInt16(records + index * 4 + 2, (uint16)PrintResolution(index));
		}
		break;
	case PRGENERALSETRSL:
		resolution = (int16)ReadMacInt16(data + 12);
		if (resolution != (int16)ReadMacInt16(data + 14) || !PrintIsResolution(resolution))
			WriteMacInt16(data + 2, PRGENERALNOSUCHRSL);
		else
		{
			PrintValidate(ReadMacInt32(data + 8));
			record = ReadMacInt32(ReadMacInt32(data + 8));
			PrintRebuildRecord(ReadMacInt32(data + 8), resolution, PrintRecordPaper(record), PrintRecordLandscape(record));
		}
		break;
	case PRGENERALDRAFTBITS:
	case PRGENERALNODRAFTBITS:
		break;
	case PRGENERALGETROTN:
		PrintValidate(ReadMacInt32(data + 8));
		WriteMacInt8(data + 12, (uint8)PrintRecordLandscape(ReadMacInt32(ReadMacInt32(data + 8))));
		break;
	default:
		WriteMacInt16(data + 2, PRGENERALOPNOTIMPL);
		break;
	}
}

static void PrintDitlItem(uint8 *ditl, int *length, int type, int top, int left, int bottom, int right, const char *text)
{
	uint8 *item;
	int textlength;

	textlength = (int)strlen(text);
	if (textlength > 255)
		textlength = 255;
	item = ditl + *length;
	memset(item, 0, 4);
	item[4] = (uint8)(top >> 8);
	item[5] = (uint8)top;
	item[6] = (uint8)(left >> 8);
	item[7] = (uint8)left;
	item[8] = (uint8)(bottom >> 8);
	item[9] = (uint8)bottom;
	item[10] = (uint8)(right >> 8);
	item[11] = (uint8)right;
	item[12] = (uint8)type;
	item[13] = (uint8)textlength;
	memcpy(item + 14, text, textlength);
	*length += 14 + textlength + (textlength & 1);
	ditl[1]++;
}

static uint32 PrintNewDialog(uint8 *ditl, int ditllength, int width, int height, uint32 printhandle, uint32 itemproc)
{
	PrintScratch bounds(8);
	PrintScratch title(2);
	uint32 arguments[9];
	int argumentsizes[9];
	uint32 items;
	uint32 storage;
	uint32 device;
	uint32 dialog;
	int screentop;
	int screenleft;
	int screenbottom;
	int screenright;
	int index;

	ditl[1]--;
	items = PrintNewHandle((uint32)ditllength);
	storage = PrintNewPtrClear(PRINTDIALOGSIZE);
	if (items == 0 || storage == 0)
		return 0;
	memcpy(Mac2HostAddr(ReadMacInt32(items)), ditl, ditllength);
	device = ReadMacInt32(ReadMacInt32(PRINTMAINDEVICELOWMEM));
	screentop = (int16)ReadMacInt16(device + GDEVICERECT);
	screenleft = (int16)ReadMacInt16(device + GDEVICERECT + 2);
	screenbottom = (int16)ReadMacInt16(device + GDEVICERECT + 4);
	screenright = (int16)ReadMacInt16(device + GDEVICERECT + 6);
	screentop += (screenbottom - screentop - height) / 3;
	screenleft += (screenright - screenleft - width) / 2;
	PrintWriteRect(bounds.addr(), screentop, screenleft, screentop + height, screenleft + width);
	WriteMacInt16(title.addr(), 0);
	for (index = 0; index < 9; index++)
		argumentsizes[index] = 4;
	arguments[0] = storage;
	arguments[1] = bounds.addr();
	arguments[2] = title.addr();
	arguments[3] = 0;
	argumentsizes[3] = 2;
	arguments[4] = 1;
	argumentsizes[4] = 2;
	arguments[5] = 0xFFFFFFFF;
	arguments[6] = 0;
	argumentsizes[6] = 2;
	arguments[7] = 0;
	arguments[8] = items;
	dialog = PrintCall68k(TRAPNEWDIALOG, 0, 4, 9, arguments, argumentsizes);
	if (dialog == 0)
	{
		PrintDisposePtr(storage);
		return 0;
	}
	WriteMacInt32(dialog + TPRDLGFILTER, PrintThunkAddress() + PRINTFILTEROFFSET);
	WriteMacInt32(dialog + TPRDLGITEMPROC, itemproc);
	WriteMacInt32(dialog + TPRDLGPRINT, printhandle);
	WriteMacInt8(dialog + TPRDLGDOIT, 0);
	WriteMacInt8(dialog + TPRDLGDONE, 0);
	return dialog;
}

static uint32 PrintDialogItemHandle(uint32 dialog, int item)
{
	PrintScratch data(16);
	uint32 arguments[5];
	int argumentsizes[5];

	arguments[0] = dialog;
	argumentsizes[0] = 4;
	arguments[1] = (uint32)item;
	argumentsizes[1] = 2;
	arguments[2] = data.addr();
	argumentsizes[2] = 4;
	arguments[3] = data.addr() + 4;
	argumentsizes[3] = 4;
	arguments[4] = data.addr() + 8;
	argumentsizes[4] = 4;
	PrintCall68k(TRAPGETDIALOGITEM, 0, 0, 5, arguments, argumentsizes);
	return ReadMacInt32(data.addr() + 4);
}

static void PrintSetControl(uint32 dialog, int item, int value)
{
	uint32 arguments[2];
	int argumentsizes[2];

	arguments[0] = PrintDialogItemHandle(dialog, item);
	argumentsizes[0] = 4;
	arguments[1] = (uint32)value;
	argumentsizes[1] = 2;
	PrintCall68k(TRAPSETCONTROLVALUE, 0, 0, 2, arguments, argumentsizes);
}

static int PrintGetControl(uint32 dialog, int item)
{
	return (int16)PrintCallTrap1(TRAPGETCONTROLVALUE, 2, PrintDialogItemHandle(dialog, item));
}

static void PrintSetRadio(uint32 dialog, int first, int last, int selected)
{
	int item;

	for (item = first; item <= last; item++)
	{
		if (item == selected)
			PrintSetControl(dialog, item, 1);
		else
			PrintSetControl(dialog, item, 0);
	}
}

static int PrintGetRadio(uint32 dialog, int first, int last)
{
	int item;

	for (item = first; item <= last; item++)
	{
		if (PrintGetControl(dialog, item))
			return item;
	}
	return first;
}

static void PrintSetItemText(uint32 dialog, int item, const char *text)
{
	PrintScratch string(256);
	uint32 arguments[2];
	int length;

	length = (int)strlen(text);
	if (length > 255)
		length = 255;
	WriteMacInt8(string.addr(), (uint8)length);
	memcpy(Mac2HostAddr(string.addr() + 1), text, length);
	arguments[0] = PrintDialogItemHandle(dialog, item);
	arguments[1] = string.addr();
	PrintCallTrap(TRAPSETDIALOGITEMTEXT, 0, 2, arguments);
}

static int PrintGetItemNumber(uint32 dialog, int item)
{
	PrintScratch string(256);
	uint32 arguments[2];
	int length;
	int index;
	int value;
	int character;

	WriteMacInt8(string.addr(), 0);
	arguments[0] = PrintDialogItemHandle(dialog, item);
	arguments[1] = string.addr();
	PrintCallTrap(TRAPGETDIALOGITEMTEXT, 0, 2, arguments);
	length = ReadMacInt8(string.addr());
	value = 0;
	for (index = 0; index < length && value < PRINTLASTPAGE; index++)
	{
		character = ReadMacInt8(string.addr() + 1 + index);
		if (character >= '0' && character <= '9')
			value = value * 10 + character - '0';
	}
	return value;
}

static void PrintHiliteButton(uint32 dialog, int item, int state)
{
	uint32 arguments[2];
	int argumentsizes[2];

	arguments[0] = PrintDialogItemHandle(dialog, item);
	argumentsizes[0] = 4;
	arguments[1] = (uint32)state;
	argumentsizes[1] = 2;
	PrintCall68k(TRAPHILITECONTROL, 0, 0, 2, arguments, argumentsizes);
}

static void PrintPressButton(uint32 dialog, int item)
{
	M68kRegisters registers;

	PrintHiliteButton(dialog, item, 1);
	memset(&registers, 0, sizeof(registers));
	registers.a[5] = ReadMacInt32(PRINTCURRENTA5LOWMEM);
	registers.a[0] = 8;
	Execute68kTrap(TRAPDELAY, &registers);
	PrintHiliteButton(dialog, item, 0);
}

static void PrintDrawDefaultRing(uint32 dialog)
{
	PrintScratch data(16);
	uint32 arguments[5];
	int argumentsizes[5];
	int index;

	for (index = 0; index < 5; index++)
		argumentsizes[index] = 4;
	arguments[0] = dialog;
	arguments[1] = DIALOGOK;
	argumentsizes[1] = 2;
	arguments[2] = data.addr();
	arguments[3] = data.addr() + 4;
	arguments[4] = data.addr() + 8;
	PrintCall68k(TRAPGETDIALOGITEM, 0, 0, 5, arguments, argumentsizes);
	PrintWriteRect(data.addr() + 8, (int16)ReadMacInt16(data.addr() + 8) - 4, (int16)ReadMacInt16(data.addr() + 10) - 4,
		(int16)ReadMacInt16(data.addr() + 12) + 4, (int16)ReadMacInt16(data.addr() + 14) + 4);
	arguments[0] = 3;
	argumentsizes[0] = 2;
	arguments[1] = 3;
	argumentsizes[1] = 2;
	PrintCall68k(TRAPPENSIZE, 0, 0, 2, arguments, argumentsizes);
	arguments[0] = data.addr() + 8;
	argumentsizes[0] = 4;
	arguments[1] = 16;
	argumentsizes[1] = 2;
	arguments[2] = 16;
	argumentsizes[2] = 2;
	PrintCall68k(TRAPFRAMEROUNDRECT, 0, 0, 3, arguments, argumentsizes);
	PrintCallTrap(TRAPPENNORMAL, 0, 0, NULL);
}

static bool PrintDialogFilter(uint32 dialog, uint32 event, uint32 itemhit)
{
	PrintScratch oldport(4);
	uint32 arguments[2];
	int what;
	int character;

	what = ReadMacInt16(event + EVENTWHAT);
	if (what == EVENTKEYDOWN || what == EVENTAUTOKEY)
	{
		character = ReadMacInt32(event + EVENTMESSAGE) & 0xFF;
		if (character == 13 || character == 3)
		{
			PrintPressButton(dialog, DIALOGOK);
			WriteMacInt16(itemhit, DIALOGOK);
			return true;
		}
		if (character == 27 || (character == '.' && (ReadMacInt16(event + EVENTMODIFIERS) & EVENTCMDKEY)))
		{
			PrintPressButton(dialog, DIALOGCANCEL);
			WriteMacInt16(itemhit, DIALOGCANCEL);
			return true;
		}
		return false;
	}
	if (what == EVENTUPDATE && ReadMacInt32(event + EVENTMESSAGE) == dialog)
	{
		PrintCallTrap1(TRAPGETPORT, 0, oldport.addr());
		PrintCallTrap1(TRAPSETPORT, 0, dialog);
		PrintCallTrap1(TRAPBEGINUPDATE, 0, dialog);
		arguments[0] = dialog;
		arguments[1] = ReadMacInt32(dialog + WINDOWVISRGN);
		PrintCallTrap(TRAPUPDATEDIALOG, 0, 2, arguments);
		PrintDrawDefaultRing(dialog);
		PrintCallTrap1(TRAPENDUPDATE, 0, dialog);
		PrintCallTrap1(TRAPSETPORT, 0, ReadMacInt32(oldport.addr()));
		WriteMacInt16(event + EVENTWHAT, EVENTNULL);
	}
	return false;
}

static void PrintDialogFinish(uint32 dialog, int doit)
{
	WriteMacInt8(dialog + TPRDLGDOIT, (uint8)doit);
	WriteMacInt8(dialog + TPRDLGDONE, 1);
}

static uint32 PrintStyleInit(uint32 printhandle)
{
	uint8 ditl[DITLMAXSIZE];
	uint32 record;
	uint32 dialog;
	int length;
	int paper;

	PrintValidate(printhandle);
	memset(ditl, 0, sizeof(ditl));
	length = 2;
	PrintDitlItem(ditl, &length, DITLBUTTON, 160, 280, 180, 350, "OK");
	PrintDitlItem(ditl, &length, DITLBUTTON, 160, 195, 180, 265, "Cancel");
	PrintDitlItem(ditl, &length, DITLSTATIC, 10, 15, 28, 350, "Page Setup (Save as PDF)");
	PrintDitlItem(ditl, &length, DITLSTATIC, 42, 15, 60, 100, "Paper:");
	for (paper = 0; paper < PRINTPAPERCOUNT; paper++)
		PrintDitlItem(ditl, &length, DITLRADIO, 42 + paper * 20, 105, 60 + paper * 20, 350, PrintPaperName(paper));
	PrintDitlItem(ditl, &length, DITLSTATIC, 112, 15, 130, 100, "Orientation:");
	PrintDitlItem(ditl, &length, DITLRADIO, 112, 105, 130, 215, "Portrait");
	PrintDitlItem(ditl, &length, DITLRADIO, 112, 220, 130, 350, "Landscape");
	dialog = PrintNewDialog(ditl, length, 365, 192, printhandle, PrintThunkAddress() + PRINTSTYLEITEMOFFSET);
	if (dialog == 0)
		return 0;
	record = ReadMacInt32(printhandle);
	PrintSetRadio(dialog, STYLEPAPERFIRST, STYLEPAPERFIRST + PRINTPAPERCOUNT - 1, STYLEPAPERFIRST + PrintRecordPaper(record));
	PrintSetRadio(dialog, STYLEPORTRAIT, STYLELANDSCAPE, STYLEPORTRAIT + PrintRecordLandscape(record));
	return dialog;
}

static void PrintStyleItem(uint32 dialog, int item)
{
	uint32 printhandle;
	uint32 record;

	if (item >= STYLEPAPERFIRST && item < STYLEPAPERFIRST + PRINTPAPERCOUNT)
		PrintSetRadio(dialog, STYLEPAPERFIRST, STYLEPAPERFIRST + PRINTPAPERCOUNT - 1, item);
	else if (item == STYLEPORTRAIT || item == STYLELANDSCAPE)
		PrintSetRadio(dialog, STYLEPORTRAIT, STYLELANDSCAPE, item);
	else if (item == DIALOGOK)
	{
		printhandle = ReadMacInt32(dialog + TPRDLGPRINT);
		record = ReadMacInt32(printhandle);
		PrintRebuildRecord(printhandle, PrintRecordResolution(record),
			PrintGetRadio(dialog, STYLEPAPERFIRST, STYLEPAPERFIRST + PRINTPAPERCOUNT - 1) - STYLEPAPERFIRST,
			PrintGetRadio(dialog, STYLEPORTRAIT, STYLELANDSCAPE) - STYLEPORTRAIT);
		PrintDialogFinish(dialog, 1);
	}
	else if (item == DIALOGCANCEL)
		PrintDialogFinish(dialog, 0);
}

static uint32 PrintJobInit(uint32 printhandle)
{
	uint8 ditl[DITLMAXSIZE];
	char destination[256];
	const char *directory;
	uint32 dialog;
	int length;

	PrintValidate(printhandle);
	directory = PrefsFindString("printpath");
	if (directory == NULL || directory[0] == 0)
		directory = "the emulator folder";
	length = (int)strlen(directory);
	if (length > 60)
		snprintf(destination, sizeof(destination), "...%s", directory + length - 57);
	else
		snprintf(destination, sizeof(destination), "%s", directory);
	memset(ditl, 0, sizeof(ditl));
	length = 2;
	PrintDitlItem(ditl, &length, DITLBUTTON, 150, 280, 170, 350, "Print");
	PrintDitlItem(ditl, &length, DITLBUTTON, 150, 195, 170, 265, "Cancel");
	PrintDitlItem(ditl, &length, DITLSTATIC, 10, 15, 28, 350, "Print (Save as PDF)");
	PrintDitlItem(ditl, &length, DITLSTATIC, 42, 15, 60, 80, "Pages:");
	PrintDitlItem(ditl, &length, DITLRADIO, 42, 85, 60, 150, "All");
	PrintDitlItem(ditl, &length, DITLRADIO, 64, 85, 82, 150, "From:");
	PrintDitlItem(ditl, &length, DITLEDIT, 66, 155, 80, 200, "1");
	PrintDitlItem(ditl, &length, DITLSTATIC, 64, 212, 82, 240, "To:");
	PrintDitlItem(ditl, &length, DITLEDIT, 66, 245, 80, 290, "");
	PrintDitlItem(ditl, &length, DITLSTATIC, 98, 15, 116, 80, "Save to:");
	PrintDitlItem(ditl, &length, DITLSTATIC, 98, 85, 134, 350, destination);
	dialog = PrintNewDialog(ditl, length, 365, 182, printhandle, PrintThunkAddress() + PRINTJOBITEMOFFSET);
	if (dialog == 0)
		return 0;
	PrintSetRadio(dialog, JOBALL, JOBRANGE, JOBALL);
	return dialog;
}

static void PrintJobItem(uint32 dialog, int item)
{
	uint32 record;
	int firstpage;
	int lastpage;

	if (item == JOBALL || item == JOBRANGE)
		PrintSetRadio(dialog, JOBALL, JOBRANGE, item);
	else if (item == JOBFROM || item == JOBTO)
		PrintSetRadio(dialog, JOBALL, JOBRANGE, JOBRANGE);
	else if (item == DIALOGOK)
	{
		firstpage = 1;
		lastpage = PRINTLASTPAGE;
		if (PrintGetRadio(dialog, JOBALL, JOBRANGE) == JOBRANGE)
		{
			firstpage = PrintGetItemNumber(dialog, JOBFROM);
			lastpage = PrintGetItemNumber(dialog, JOBTO);
			if (firstpage < 1)
				firstpage = 1;
			if (lastpage < firstpage)
				lastpage = PRINTLASTPAGE;
		}
		record = ReadMacInt32(ReadMacInt32(dialog + TPRDLGPRINT));
		WriteMacInt16(record + TPRINTJOB, (uint16)firstpage);
		WriteMacInt16(record + TPRINTJOB + 2, (uint16)lastpage);
		WriteMacInt16(record + TPRINTJOB + 4, 1);
		WriteMacInt8(record + TPRINTJOB + 6, 0);
		WriteMacInt8(record + TPRINTJOB + 7, 1);
		PrintDialogFinish(dialog, 1);
	}
	else if (item == DIALOGCANCEL)
		PrintDialogFinish(dialog, 0);
}

static uint32 PrintStartDialog(uint32 dialog)
{
	if (dialog == 0)
		return 0;
	PrintCallTrap(TRAPINITCURSOR, 0, 0, NULL);
	PrintCallTrap1(TRAPSHOWWINDOW, 0, dialog);
	WriteMacInt8(dialog + TPRDLGDONE, 0);
	return dialog;
}

static void PrintDialogItem(uint32 dialog, int item)
{
	uint32 arguments[2];
	int argumentsizes[2];
	uint32 itemproc;

	itemproc = ReadMacInt32(dialog + TPRDLGITEMPROC);
	if (itemproc == PrintThunkAddress() + PRINTSTYLEITEMOFFSET)
		PrintStyleItem(dialog, item);
	else if (itemproc == PrintThunkAddress() + PRINTJOBITEMOFFSET)
		PrintJobItem(dialog, item);
	else if (itemproc != 0)
	{
		arguments[0] = dialog;
		argumentsizes[0] = 4;
		arguments[1] = (uint32)item;
		argumentsizes[1] = 2;
		PrintCall68k(itemproc, 0, 0, 2, arguments, argumentsizes);
	}
	else if (item == DIALOGOK || item == DIALOGCANCEL)
		PrintDialogFinish(dialog, item == DIALOGOK);
}

static void PrintGlueReturn(M68kRegisters *r, uint32 frame)
{
	uint32 result;

	result = frame + 8 + ((ReadMacInt32(frame + 4) >> 8) & 0xFF);
	WriteMacInt32(result - 4, ReadMacInt32(frame));
	r->a[7] = result - 4;
}

static PrintJob *PrintPortJob(uint32 port)
{
	uint64 pointer;

	pointer = ((uint64)ReadMacInt32(port + TPRPORTJOBHIGH) << 32) | ReadMacInt32(port + TPRPORTJOBLOW);
	return (PrintJob *)(uintptr)pointer;
}

static void PrintApplicationName(char *name, int namesize)
{
	int length;
	int index;
	char character;

	length = ReadMacInt8(PRINTAPPNAMELOWMEM);
	if (length > namesize - 1)
		length = namesize - 1;
	for (index = 0; index < length; index++)
	{
		character = (char)ReadMacInt8(PRINTAPPNAMELOWMEM + 1 + index);
		if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
			(character >= '0' && character <= '9') || character == ' ' || character == '-' || character == '_'))
			character = '_';
		name[index] = character;
	}
	name[length] = 0;
	if (length == 0)
		strcpy(name, "Print");
}

static void PrintConfigurePort(uint32 port, uint32 record)
{
	memcpy(Mac2HostAddr(port + CGRAFPORTRECT), Mac2HostAddr(record + TPRINTINFO + TPRINFOPAGE), 8);
	PrintCallTrap1(TRAPSETEMPTYRGN, 0, ReadMacInt32(port + CGRAFPORTVISRGN));
	PrintCallTrap1(TRAPSETPORT, 0, port);
}

static uint32 PrintOpenDoc(uint32 printhandle, uint32 port)
{
	struct pdf_info info;
	PrintJob *job;
	uint32 record;
	uint64 pointer;
	uint8 ourpointer;
	int resolution;

	PrintValidate(printhandle);
	ourpointer = 0;
	if (port == 0)
	{
		port = PrintNewPtrClear(PRINTPORTSIZE);
		ourpointer = 1;
		if (port == 0)
		{
			PrintSetError(PRINTMEMFULLERR);
			return 0;
		}
	}
	else
		memset(Mac2HostAddr(port), 0, PRINTPORTSIZE);
	memset(&info, 0, sizeof(info));
	strcpy(info.creator, "SheepShaver");
	strcpy(info.producer, "SheepShaver");
	PrintApplicationName(info.title, sizeof(info.title));
	record = ReadMacInt32(printhandle);
	resolution = PrintRecordResolution(record);
	job = (PrintJob *)malloc(sizeof(PrintJob));
	if (job != NULL)
	{
		job->pdf = pdf_create((float)(((int16)ReadMacInt16(record + TPRINTPAPER + 6) - (int16)ReadMacInt16(record + TPRINTPAPER + 2)) * 72.0 / resolution),
			(float)(((int16)ReadMacInt16(record + TPRINTPAPER + 4) - (int16)ReadMacInt16(record + TPRINTPAPER)) * 72.0 / resolution), &info);
		if (job->pdf == NULL)
		{
			free(job);
			job = NULL;
		}
	}
	if (job == NULL)
	{
		if (ourpointer)
			PrintDisposePtr(port);
		PrintSetError(PRINTMEMFULLERR);
		return 0;
	}
	job->firstpage = (int16)ReadMacInt16(record + TPRINTJOB);
	job->lastpage = (int16)ReadMacInt16(record + TPRINTJOB + 2);
	job->pagenumber = 0;
	if (job->firstpage < 1)
		job->firstpage = 1;
	if (job->lastpage < job->firstpage)
		job->lastpage = PRINTLASTPAGE;
	pointer = (uint64)(uintptr)job;
	WriteMacInt32(port + TPRPORTPRINT, printhandle);
	WriteMacInt32(port + TPRPORTJOBHIGH, (uint32)(pointer >> 32));
	WriteMacInt32(port + TPRPORTJOBLOW, (uint32)pointer);
	WriteMacInt8(port + TPRPORTOURPTR, ourpointer);
	PrintCallTrap1(TRAPOPENCPORT, 0, port);
	PrintConfigurePort(port, ReadMacInt32(printhandle));
	return port;
}

static void PrintOpenPage(uint32 port, uint32 frame)
{
	PrintScratch rect(8);
	uint32 record;

	PrintPortJob(port)->pagenumber++;
	record = ReadMacInt32(ReadMacInt32(port + TPRPORTPRINT));
	if (frame == 0)
		frame = record + TPRINTINFO + TPRINFOPAGE;
	memcpy(Mac2HostAddr(rect.addr()), Mac2HostAddr(frame), 8);
	PrintCallTrap1(TRAPINITCPORT, 0, port);
	PrintConfigurePort(port, record);
	WriteMacInt32(port + TPRPORTPICTURE, PrintCallTrap1(TRAPOPENPICTURE, 4, rect.addr()));
}

static void PrintClosePage(uint32 port)
{
	PICTPDFMAP map;
	PrintJob *job;
	uint32 picture;
	uint32 record;
	uint32 size;
	double width;

	picture = ReadMacInt32(port + TPRPORTPICTURE);
	if (picture == 0)
		return;
	PrintCallTrap(TRAPCLOSEPICTURE, 0, 0, NULL);
	WriteMacInt32(port + TPRPORTPICTURE, 0);
	job = PrintPortJob(port);
	size = PrintGetHandleSize(picture);
	if (job->pagenumber >= job->firstpage && job->pagenumber <= job->lastpage && pdf_append_page(job->pdf) != NULL)
	{
		record = ReadMacInt32(ReadMacInt32(port + TPRPORTPRINT));
		map.scalex = 72.0 / (int16)ReadMacInt16(record + TPRINTINFO + TPRINFOHRES);
		map.scaley = 72.0 / (int16)ReadMacInt16(record + TPRINTINFO + TPRINFOVRES);
		map.top = (int16)ReadMacInt16(record + TPRINTPAPER);
		map.left = (int16)ReadMacInt16(record + TPRINTPAPER + 2);
		width = ((int16)ReadMacInt16(record + TPRINTPAPER + 6) - map.left) * map.scalex;
		map.pageheight = ((int16)ReadMacInt16(record + TPRINTPAPER + 4) - map.top) * map.scaley;
		pdf_page_set_size(job->pdf, NULL, (float)width, (float)map.pageheight);
		if (size > 0 && PictPdfRenderPage(job->pdf, Mac2HostAddr(ReadMacInt32(picture)), (long)size, &map) < 0)
			printf("Print: page picture of %u bytes was only partly rendered\n", size);
	}
	PrintCallTrap1(TRAPKILLPICTURE, 0, picture);
}

static void PrintSaveDocument(struct pdf_doc *pdf)
{
	char name[64];
	char stamp[64];
	char path[1024];
	const char *directory;
	FILE *existing;
	time_t now;
	int attempt;

	PrintApplicationName(name, 32);
	now = time(NULL);
	strftime(stamp, sizeof(stamp), "%Y-%m-%d %H.%M.%S", localtime(&now));
	directory = PrefsFindString("printpath");
	if (directory == NULL)
		directory = "";
	for (attempt = 1; attempt < 1000; attempt++)
	{
		if (attempt == 1 && directory[0])
			snprintf(path, sizeof(path), "%s/%s %s.pdf", directory, name, stamp);
		else if (attempt == 1)
			snprintf(path, sizeof(path), "%s %s.pdf", name, stamp);
		else if (directory[0])
			snprintf(path, sizeof(path), "%s/%s %s %d.pdf", directory, name, stamp, attempt);
		else
			snprintf(path, sizeof(path), "%s %s %d.pdf", name, stamp, attempt);
		existing = fopen(path, "rb");
		if (existing == NULL)
			break;
		fclose(existing);
	}
	if (pdf_save(pdf, path) < 0)
	{
		printf("Print: could not write %s: %s\n", path, pdf_get_err(pdf, NULL));
		PrintSetError(PRINTIOABORT);
	}
	else
		printf("Print: wrote %s\n", path);
	fflush(stdout);
}

static void PrintCloseDoc(uint32 port)
{
	PrintJob *job;

	PrintClosePage(port);
	PrintCallTrap1(TRAPCLOSEPORT, 0, port);
	job = PrintPortJob(port);
	if (pdf_get_page(job->pdf, 1) != NULL)
		PrintSaveDocument(job->pdf);
	pdf_destroy(job->pdf);
	free(job);
	if (ReadMacInt8(port + TPRPORTOURPTR))
		PrintDisposePtr(port);
}

void PrintWriteThunks(uint8 *host)
{
	static const uint16 glue[] = {
		0x7000, M68K_EMUL_OP_PRGLUE, 0x4a80, 0x6602, M68K_RTS,
		0x2f00, 0x558f,
		0x206f, 0x0002, 0x2f28, 0x00aa, 0x486f, 0x0004, 0xa991,
		0x7004, M68K_EMUL_OP_PRGLUE, 0x4a40, 0x67ea, M68K_RTS
	};
	static const uint16 callbacks[] = {
		0x7001, M68K_EMUL_OP_PRGLUE, M68K_RTS,
		0x7002, M68K_EMUL_OP_PRGLUE, M68K_RTS,
		0x7003, M68K_EMUL_OP_PRGLUE, M68K_RTS
	};
	int index;

	for (index = 0; index < (int)(sizeof(glue) / sizeof(glue[0])); index++)
	{
		host[index * 2] = (uint8)(glue[index] >> 8);
		host[index * 2 + 1] = (uint8)glue[index];
	}
	for (index = 0; index < (int)(sizeof(callbacks) / sizeof(callbacks[0])); index++)
	{
		host[PRINTSTYLEITEMOFFSET + index * 2] = (uint8)(callbacks[index] >> 8);
		host[PRINTSTYLEITEMOFFSET + index * 2 + 1] = (uint8)callbacks[index];
	}
}

void PrintInstall(void)
{
	M68kRegisters registers;
	uint32 thunk;

	thunk = PrintThunkAddress();
	if (thunk == 0 || ReadMacInt32(PRINTTRAPSLOT) == thunk || ReadMacInt16(thunk + 2) != M68K_EMUL_OP_PRGLUE)
		return;
	memset(&registers, 0, sizeof(registers));
	registers.a[5] = ReadMacInt32(PRINTCURRENTA5LOWMEM);
	registers.d[0] = PRINTTRAPWORD;
	registers.a[0] = thunk;
	Execute68kTrap(TRAPSETTOOLTRAPADDRESS, &registers);
}

static void PrintItemGlue(M68kRegisters *r, bool job)
{
	uint32 returnaddress;
	uint32 dialog;
	int item;

	returnaddress = ReadMacInt32(r->a[7]);
	item = (int16)ReadMacInt16(r->a[7] + 4);
	dialog = ReadMacInt32(r->a[7] + 6);
	if (job)
		PrintJobItem(dialog, item);
	else
		PrintStyleItem(dialog, item);
	WriteMacInt32(r->a[7] + 6, returnaddress);
	r->a[7] += 6;
}

static void PrintFilterGlue(M68kRegisters *r)
{
	uint32 returnaddress;
	uint32 stack;

	stack = r->a[7];
	returnaddress = ReadMacInt32(stack);
	if (PrintDialogFilter(ReadMacInt32(stack + 12), ReadMacInt32(stack + 8), ReadMacInt32(stack + 4)))
		WriteMacInt16(stack + 16, 0x0100);
	else
		WriteMacInt16(stack + 16, 0);
	WriteMacInt32(stack + 12, returnaddress);
	r->a[7] = stack + 12;
}

static void PrintDialogItemGlue(M68kRegisters *r)
{
	uint32 stack;
	uint32 dialog;
	uint32 frame;
	uint32 result;
	bool doit;

	stack = r->a[7];
	dialog = ReadMacInt32(stack + 2);
	PrintDialogItem(dialog, (int16)ReadMacInt16(stack));
	r->d[0] = 0;
	if (!ReadMacInt8(dialog + TPRDLGDONE))
		return;
	doit = ReadMacInt8(dialog + TPRDLGDOIT) != 0;
	PrintCallTrap1(TRAPCLOSEDIALOG, 0, dialog);
	PrintDisposePtr(dialog);
	frame = stack + 6;
	result = frame + 8 + ((ReadMacInt32(frame + 4) >> 8) & 0xFF);
	if (doit)
		WriteMacInt16(result, 0x0100);
	else
		WriteMacInt16(result, 0);
	PrintGlueReturn(r, frame);
	r->d[0] = 1;
}

static void PrintTrapGlue(M68kRegisters *r)
{
	uint32 returnaddress;
	uint32 selector;
	uint32 parameters;
	uint32 result;
	uint32 dialog;
	uint32 printhandle;

	dialog = 0;
	returnaddress = ReadMacInt32(r->a[7]);
	selector = ReadMacInt32(r->a[7] + 4);
	parameters = r->a[7] + 8;
	result = parameters + ((selector >> 8) & 0xFF);
	switch (selector >> 24)
	{
	case PRGLUEOPEN:
		PrintSetError(PRINTNOERR);
		break;
	case PRGLUEOPENDOC:
		WriteMacInt32(result, PrintOpenDoc(ReadMacInt32(parameters + 8), ReadMacInt32(parameters + 4)));
		break;
	case PRGLUECLOSEDOC:
		PrintCloseDoc(ReadMacInt32(parameters));
		break;
	case PRGLUEOPENPAGE:
		PrintOpenPage(ReadMacInt32(parameters + 4), ReadMacInt32(parameters));
		break;
	case PRGLUECLOSEPAGE:
		PrintClosePage(ReadMacInt32(parameters));
		break;
	case PRGLUEPRINTDEFAULT:
		PrintBuildRecord(ReadMacInt32(ReadMacInt32(parameters)), PRINTDEFAULTRESOLUTION, 0, 0);
		break;
	case PRGLUESTLDIALOG:
		WriteMacInt16(result, 0);
		dialog = PrintStartDialog(PrintStyleInit(ReadMacInt32(parameters)));
		break;
	case PRGLUEJOBDIALOG:
		WriteMacInt16(result, 0);
		dialog = PrintStartDialog(PrintJobInit(ReadMacInt32(parameters)));
		break;
	case PRGLUESTLINIT:
		WriteMacInt32(result, PrintStyleInit(ReadMacInt32(parameters)));
		break;
	case PRGLUEJOBINIT:
		WriteMacInt32(result, PrintJobInit(ReadMacInt32(parameters)));
		break;
	case PRGLUEDLGMAIN:
		WriteMacInt16(result, 0);
		printhandle = ReadMacInt32(parameters + 4);
		PrintValidate(printhandle);
		if (ReadMacInt32(parameters) != 0)
			dialog = PrintStartDialog(PrintCall68k(ReadMacInt32(parameters), 0, 4, 1, &printhandle, NULL));
		break;
	case PRGLUEVALIDATE:
		if (PrintValidate(ReadMacInt32(parameters)))
			WriteMacInt16(result, 0x0100);
		else
			WriteMacInt16(result, 0);
		break;
	case PRGLUEJOBMERGE:
		memcpy(Mac2HostAddr(ReadMacInt32(ReadMacInt32(parameters)) + TPRINTJOB),
			Mac2HostAddr(ReadMacInt32(ReadMacInt32(parameters + 4)) + TPRINTJOB), TPRINTJOBSIZE);
		break;
	case PRGLUEGENERAL:
		PrintGeneral(ReadMacInt32(parameters));
		break;
	case PRGLUEDRVRDCE:
		WriteMacInt32(result, 0);
		break;
	case PRGLUEDRVRVERS:
		WriteMacInt16(result, 1);
		break;
	case PRGLUEERROR:
		WriteMacInt16(result, ReadMacInt16(PRINTERRLOWMEM));
		break;
	case PRGLUESETERROR:
		WriteMacInt16(PRINTERRLOWMEM, ReadMacInt16(parameters));
		break;
	default:
		break;
	}
	r->d[0] = dialog;
	if (dialog != 0)
		return;
	WriteMacInt32(result - 4, returnaddress);
	r->a[7] = result - 4;
}

void PrintGlue(M68kRegisters *r)
{
	switch (r->d[0] & 0xFF)
	{
	case PRINTGLUETRAP:
		PrintTrapGlue(r);
		break;
	case PRINTGLUESTYLEITEM:
		PrintItemGlue(r, false);
		break;
	case PRINTGLUEJOBITEM:
		PrintItemGlue(r, true);
		break;
	case PRINTGLUEFILTER:
		PrintFilterGlue(r);
		break;
	case PRINTGLUEDIALOGITEM:
		PrintDialogItemGlue(r);
		break;
	}
}
